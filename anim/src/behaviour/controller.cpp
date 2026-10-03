#include "svx/anim/behaviour/controller.hpp"

#include <algorithm>

namespace svx::anim {

namespace {

constexpr f64 G = 9.81;
f64 g_arms_at_ease = 0.82;
constexpr i32 kParentOf[kBodyCount] = {-1, B::pelvis, B::spine, B::chest, B::chest, B::upperarmL, B::forearmL, B::chest, B::upperarmR, B::forearmR, B::pelvis, B::thighL, B::shinL, B::pelvis, B::thighR, B::shinR};

f64 wrap_pi(f64 a) {
  f64 x = std::fmod(a, kPi * 2.0);
  if (x > kPi) x -= kPi * 2.0;
  else if (x <= -kPi) x += kPi * 2.0;
  return x;
}

size_t region_index(Region r) { return static_cast<size_t>(r); }

bool primary_occupied(const MotionPlan& p) {
  const auto item = p.props.held();
  return item && item->point == AttachPoint::RightHand;
}

}  // namespace

const char* body_mode_name(BodyMode m) {
  switch (m) {
    case BodyMode::Animated: return "animated";
    case BodyMode::Reacting: return "reacting";
    case BodyMode::Falling: return "falling";
    case BodyMode::Lying: return "lying";
    case BodyMode::Rising: return "rising";
    case BodyMode::Dying: return "dying";
    case BodyMode::Dead: return "dead";
  }
  return "animated";
}

void set_arms_at_ease(f64 t) { g_arms_at_ease = t; }

Quat palm_frame(const V3& palm, const V3& fingers) {
  const V3 z = vnorm(palm * -1.0);
  V3 y = fingers - z * dot(fingers, z);
  if (norm(y) < 1e-4) y = V3{0, 1, 0} - z * z.y;
  y = vnorm(y);
  const V3 x = cross(y, z);
  return qfrom_basis(x, y, z);
}

Behaviours::Behaviours(MotionPlan& p, HumanoidBody& b, f64 seed) : plan(p), body(b), rng_(seed * 977 + 5) { writhe_seed_ = rng_.next() * 100.0; }

bool Behaviours::needs_physics() const {
  if (mode != BodyMode::Animated) return true;
  if (brace) return true;
  for (i32 i = 0; i < kBodyCount; ++i)
    if (stun_[size_t(i)] > 0.05f) return true;
  return upset_ > 0.0;
}

void Behaviours::set_mode(BodyMode m) {
  if (m == mode) return;
  mode = m;
  mode_time = 0.0;
  if (m == BodyMode::Reacting) {
    lost_for_ = 0.0;
    steps = 0;
    balanced_for_ = 0.0;
    react_t_ = 0.0;
  }
  if (m == BodyMode::Animated) {
    plan.feet_planner.stepping = false;
    brace.reset();
  }
  if (m == BodyMode::Falling) landed_[0] = landed_[1] = std::nullopt;
  // (what knocked the body over is spent: it gets up from the ground afresh)
  if (m == BodyMode::Falling || m == BodyMode::Lying || m == BodyMode::Rising) force_react_ = false;
}

// ---- events ------------------------------------------------------------------------------------

Zone Behaviours::hit(const HitInfo& info, i32 part, const WorldPose& pose) {
  if (part < 0 || part >= kBodyCount) return Zone::Chest;
  const Zone zone = zone_of_part(part);
  if (!std::isfinite(info.force) || info.force <= 0.0 || !std::isfinite(norm(info.point)) ||
      !std::isfinite(norm(info.dir)) || norm(info.dir) < 1e-9 || !std::isfinite(info.impulse_ns)) return zone;
  const f64 f = clamp(info.force, 0.0, 8.0);
  const V3 d = vnorm(info.dir);
  const f64 J = info.impulse_ns >= 0.0 ? clamp(info.impulse_ns, 0.0, 500.0) : default_hit_impulse(info.kind, f);
  body.system.wake();
  // a light part cannot take it all: what it cannot passes up the limb to its parent
  const f64 dv_max = info.kind == HitKind::Blunt ? 7.0 : info.kind == HitKind::Blast ? 10.0 : 4.0;
  f64 left = J;
  i32 p = part;
  for (i32 hop = 0; hop < 3 && left > 0.0 && p >= 0; ++hop) {
    RigidBody& pb = *body.parts[size_t(p)];
    const f64 take = std::min(left, dv_max * pb.mass * (hop == 0 ? 1.0 : 1.5));
    const V3 r = hop == 0 ? info.point - pb.x : V3{};
    pb.update_inertia();
    pb.apply_impulse(d * take, r);
    left -= take;
    p = p == B::pelvis ? -1 : kParentOf[p];
  }
  // Preserve the requested total momentum. Any impulse a light limb cannot
  // absorb is shared by the connected body; no extra linear or angular kick.
  if (left > 0.0) body.shove(d * (left / body.total_mass));
  if (J > .05) impact_yield_ = std::max(impact_yield_, .12 + .08 * clamp(J / 10, 0.0, 1.0));
  if (!alive) return zone;
  if (part <= B::head) {
    const V3 local_dir = rotate(conj(plan.root_rot()), d);
    const V3 local_point = rotate(conj(plan.root_rot()), info.point - pose.p[H::spine]);
    const f64 response = std::min(1.0, J / 5.0) * std::min(1.5, f);
    impact_reflex_.kick(V3{-local_dir.y, local_dir.x, clamp(local_point.x * local_dir.y * 3.0, -0.5, 0.5)} * (7.0 * response));
    const f64 speed = norm(impact_reflex_.v);
    if (speed > 12.0) impact_reflex_.v = impact_reflex_.v * (12.0 / speed);
  }
  if ((zone == Zone::LegL || zone == Zone::LegR) && info.kind == HitKind::Bullet && f > 0.7)
    stun_part(part, clamp(0.6 + 0.15 * f, 0.0, 0.95));
  // the struck part and its neighbours go slack for a moment (the trunk less: it carries the body)
  const bool trunk_part = part == B::pelvis || part == B::spine || part == B::chest;
  const f64 s = clamp(0.35 + 0.3 * f, 0.0, trunk_part ? 0.65 : 0.95);
  stun_part(part, s);
  // the whole body goes slack for a moment with the shock of it (it answers the blow with its own
  // weight: the arms swing, the head lolls), then the muscles take over again
  shock_ = std::max(shock_, clamp(0.3 + 0.25 * f, 0.0, 0.8) * (info.kind == HitKind::Blade ? 0.5 : part == B::head ? 0.6 : 1.0));
  if (part > 0) stun_part(kParentOf[part], s * 0.5);
  // a blow to the head dazes (hard ones knock out)
  if (zone == Zone::Head && info.kind == HitKind::Blunt) daze_ = std::max(daze_, clamp(0.25 * f, 0.0, 0.9));
  if (info.kind == HitKind::Blast) daze_ = std::max(daze_, clamp(0.4 * f, 0.0, 0.85));
  // a wound to hold
  if (info.kind == HitKind::Bullet || info.kind == HitKind::Blade) {
    const size_t bone = size_t(kBodyBone[size_t(part)]);
    const Quat q = pose.q[bone];
    const V3 local = rotate(conj(q), info.point - pose.p[bone]) - body.com_local[size_t(part)];
    const V3 nrm = vnorm(rotate(conj(q), d * -1.0));
    damage.reaction(part, local, nrm, info.kind, f);
  }
  // the body flinches from the blow (the eyes close, the shoulders come up), a beat after the blow
  // itself has shown
  threats_.push_back(Threat{info.point - d * 0.6, clamp(0.25 + 0.15 * f, 0.0, 0.7), -0.18, 0.1});
  plan.interrupt(f > 0.8);
  // then a look for where it came from
  V3 from = info.point - d * 6.0;
  from.z = std::max(from.z, info.point.z);
  hit_from_ = from;
  hit_at_ = time_;
  upset_ = 0.4;
  // knocked off the plan: a blow that moves the whole body (a blow to the head mostly snaps the
  // head), a leg that gives, a daze
  const f64 body_dv = J / body.total_mass;
  if (body_dv > 0.55 || ((zone == Zone::LegL || zone == Zone::LegR) && f > 0.7 && info.kind != HitKind::Blunt) || daze_ > 0.4) force_react_ = true;
  return zone;
}

void Behaviours::bumped(f64 j) {
  // (on the ground or getting up, bumps are part of it)
  if (!alive || (mode != BodyMode::Animated && mode != BodyMode::Reacting)) return;
  // (a brush of hands or shoulders in passing is nothing; a body knocked into is)
  const f64 dv = j / body.total_mass;
  if (dv > 0.1) upset_ = std::max(upset_, std::min(0.5, 2.0 * dv));
  if (dv > 0.35) force_react_ = true;
}

void Behaviours::perceive(const Perception& p) {
  if (!alive || !conscious) return;
  const V3 eyes = plan.eyes();
  const f64 d = hypot3(p.point.x - eyes.x, p.point.y - eyes.y, p.point.z - eyes.z);
  const f64 reach = p.kind == PerceptionKind::Blast ? 12.0 : p.kind == PerceptionKind::Whiz ? 1.8 : 4.5;
  const f64 amount = clamp(p.strength * pow(std::max(0.0, 1.0 - d / reach), 0.6) * (1.0 + 0.6 * nerves), 0.0, 1.3);
  if (amount < 0.08) return;
  // a round smacking into the ground at the feet: a startled step away from it
  if (p.kind == PerceptionKind::Impact && mode == BodyMode::Animated && physical && time_ > startle_at_ + 0.8) {
    const V3 pel = plan.world.p[H::pelvis];
    const f64 dx = pel.x - p.point.x, dy = pel.y - p.point.y;
    const f64 dh = hypot2(dx, dy);
    auto& fs = plan.feet_planner.feet;
    if (dh < 1.6 && dh > 0.05 && p.point.z - ground_z < 0.5 && plan.stance == Stance::Stand && !plan.busy() && fs[0].planted && fs[1].planted) {
      startle_at_ = time_;
      const i32 i = hypot2(fs[0].pos.x - p.point.x, fs[0].pos.y - p.point.y) < hypot2(fs[1].pos.x - p.point.x, fs[1].pos.y - p.point.y) ? 0 : 1;
      const auto& f = fs[size_t(i)];
      const f64 step = (0.22 + 0.2 * (1.0 - dh / 1.6)) * k();
      plan.feet_planner.step(i, V3{f.pos.x + (dx / dh) * step, f.pos.y + (dy / dh) * step, f.pos.z}, 0.18);
    }
  }
  threats_.push_back(Threat{p.point, amount, 0.0, 0.1 + 0.25 * amount});
  if (threats_.size() > 4) threats_.erase(threats_.begin());
  nerves = std::min(1.0, nerves + 0.12 * amount);
  upset_ = std::max(upset_, 0.25 * amount);
}

void Behaviours::push(const V3& dv, f64 stun_all) {
  static const f64 share[kBodyCount] = {0.8, 1, 1.1, 1.15, 1.05, 1, 0.9, 1.05, 1, 0.9, 0.7, 0.55, 0.45, 0.7, 0.55, 0.45};
  body.shove(dv, share);
  if (stun_all > 0.0) daze_ = std::max(daze_, stun_all);
  upset_ = 0.5;
  if (hypot2(dv.x, dv.y) > 0.3) force_react_ = true;
}

void Behaviours::trip() {
  if (!alive || mode == BodyMode::Falling || mode == BodyMode::Lying || mode == BodyMode::Rising) return;
  const auto& feet = plan.feet_planner.feet;
  const i32 i = !feet[0].planted && !feet[0].held ? 0 : !feet[1].planted && !feet[1].held ? 1 : -1;
  if (i >= 0 && feet[size_t(i)].swing < 0.8) catch_foot(i);
  else trip_pending_ = 0.6;
}

void Behaviours::catch_foot(i32 i) {
  const auto& f = plan.feet_planner.feet[size_t(i)];
  const V3 at = physical ? sole_pos(i) : f.pos;
  plan.feet_planner.plant_now(i, at);
  snag_until_[size_t(i)] = time_ + 0.24;
  trip_pending_ = 0.0;
  // the legs stop, the rest carries on: the trunk pitches forward over the stance foot
  const V3 v = physical ? com_vel : plan.velocity;
  const f64 sp = hypot2(v.x, v.y);
  if (sp > 0.2 && physical) {
    const V3 ax{-v.y / sp, v.x / sp, 0.0};
    const f64 pitch = 1.1 + 0.5 * sp;
    const std::pair<i32, f64> parts[3] = {{B::chest, 1.0}, {B::spine, 0.8}, {B::head, 1.1}};
    for (const auto& [b, kk] : parts) {
      V3& w = body.parts[size_t(b)]->w;
      w.x -= ax.x * pitch * kk;
      w.y -= ax.y * pitch * kk;
    }
    RigidBody& chest = *body.parts[B::chest];
    chest.v.x += (v.x / sp) * 0.3;
    chest.v.y += (v.y / sp) * 0.3;
  }
  upset_ = 0.6;
  force_react_ = true;
}

void Behaviours::lose_limb(i32 part) {
  auto mark = [&](auto&& self, i32 i) -> void {
    if (lost[size_t(i)]) return;
    lost[size_t(i)] = true;
    body.parts[size_t(i)]->lose();
    damage.lost(i);
    for (i32 j = 1; j < kBodyCount; ++j)
      if (kParentOf[j] == i) self(self, j);
  };
  mark(mark, part);
  const bool leg = part >= B::thighL;
  if (leg) {
    legless = (lost[B::thighL] || lost[B::shinL]) && (lost[B::thighR] || lost[B::shinR]);
    damage.lost(part);
    if (alive) {
      writhing = false;
      collapse(30.0);
    }
  }
}

void Behaviours::collapse(f64 seconds) {
  if (!alive || writhing) return;
  writhing = true;
  down_until_ = time_ + seconds;
  daze_ = std::max(daze_, 0.8);
  upset_ = 1.0;
  force_react_ = true;
}

void Behaviours::knock_out(f64 seconds) {
  conscious = false;
  daze_ = 1.0;
  down_until_ = time_ + seconds;
  upset_ = 1.0;
  force_react_ = true;
}

void Behaviours::die(f64 collapse_s) {
  if (!alive) return;
  alive = false;
  dying_for_ = std::max(0.05, collapse_s);
  dying_head_ = collapse_s < 0.2;
  threats_.clear();
  die_z_ = plan.world.p[H::pelvis].z;
  set_mode(BodyMode::Dying);
  // the legs give way at once in a quick death: standing takes muscle, and without it the knees
  // fold (a body does not topple like a plank)
  if (physical && collapse_s < 0.7) {
    const Quat pq = body.parts[B::pelvis]->q;
    const V3 f = rotate(pq, V3{0, 1, 0});
    const f64 give = 1.4 * (1.0 - collapse_s);
    for (i32 s : {B::shinL, B::shinR}) {
      RigidBody& b = *body.parts[size_t(s)];
      b.v.x += f.x * give;
      b.v.y += f.y * give;
    }
    body.parts[B::pelvis]->v.z -= 0.6 * give;
  }
}

void Behaviours::stun_part(i32 part, f64 s) { stun_[size_t(part)] = static_cast<f32>(std::max(static_cast<f64>(stun_[size_t(part)]), s)); }

// ---- sensing -----------------------------------------------------------------------------------

void Behaviours::sense(const WorldPose& pose) {
  const f64 kk = k();
  if (physical) {
    const f64 px = com.x, py = com.y, pz = com.z;
    com = body.com();
    if (sense_dt_ > 0.0 && had_com_ && upset_ < 0.35) {
      // (from the frame's motion: the last substep's velocities carry the contacts' jitter)
      const f64 a = 1.0 - exp(-sense_dt_ * 30.0);
      com_vel.x += ((com.x - px) / sense_dt_ - com_vel.x) * a;
      com_vel.y += ((com.y - py) / sense_dt_ - com_vel.y) * a;
      com_vel.z += ((com.z - pz) / sense_dt_ - com_vel.z) * a;
    } else {
      com_vel = body.com_velocity();
    }
    had_com_ = true;
  } else {
    had_com_ = false;
    // (animated only: the plan's pelvis stands for the centre of mass)
    const V3 p = plan.world.p[H::pelvis];
    com = V3{p.x, p.y, p.z + 0.08 * kk};
    com_vel = plan.velocity;
  }
  const std::optional<f64> g = plan.collision->ground_height(com.x, com.y, com.z, com.z - 2.2 * kk);
  ground_z = g ? *g : plan.root_pos.z;
  const f64 h = std::max(0.45 * kk, com.z - ground_z);
  const f64 w0 = std::sqrt(G / h);
  w0_ = w0;
  capture = V3{com.x + com_vel.x / w0, com.y + com_vel.y / w0, ground_z};
  // the support: planted feet (their soles), a hand holding on
  std::vector<f64>& pts = pts_;
  pts.clear();
  const auto& feet = plan.feet_planner.feet;
  for (i32 fi = 0; fi < 2; ++fi) {
    const auto& f = feet[size_t(fi)];
    if (!f.planted || capabilities().legs[size_t(fi)].support < .12 || time_ < snag_until_[size_t(fi)]) continue;
    const f64 c = cos(f.yaw), s = sin(f.yaw);
    const f64 corners[4][2] = {{-0.07, -0.045}, {-0.07, 0.045}, {0.18, -0.04}, {0.18, 0.04}};
    for (const auto& ab : corners) {
      const f64 fx = ab[0] * kk, sx = ab[1] * kk;
      pts.push_back(f.pos.x + c * fx + s * sx);
      pts.push_back(f.pos.y + s * fx - c * sx);
    }
  }
  if (brace && brace->holding) {
    const V3 t = brace->target;
    // a hand on a wall props the body up towards it
    pts.push_back(t.x - brace->normal.x * 0.05);
    pts.push_back(t.y - brace->normal.y * 0.05);
  }
  if (physical && !pts.empty()) {
    // so does the trunk or a shoulder leaning on a wall
    for (i32 i = 0; i < 10; ++i) {
      const RigidBody& b = *body.parts[size_t(i)];
      if (!b.contact || std::abs(b.contact_normal.z) > 0.5 || b.contact_point.z - ground_z < 0.4 * kk) continue;
      pts.push_back(b.contact_point.x);
      pts.push_back(b.contact_point.y);
    }
  }
  support.hull(pts);
  // (both feet off the ground: a running stride, a jump; judged by where the feet will land)
  airborne = support.empty();
  if (airborne) {
    for (size_t i = 0; i < 2; ++i) {
      if (capabilities().legs[i].support < .12) continue;
      const auto& f = feet[i];
      pts.push_back(f.target.x);
      pts.push_back(f.target.y);
    }
    support.hull(pts);
  }
  balance_error = support.empty() ? 0.0 : support.distance(capture.x, capture.y);
  // the root under the body, and its heading (the pelvis's forward)
  const Quat pq = pose.q[H::pelvis];
  const V3 fwd = rotate(pq, V3{0, 1, 0});
  const V3 up = rotate(pq, V3{0, 0, 1});
  // (lying, the pelvis's forward points up or down: its up axis says where the head is)
  const f64 hx = std::abs(fwd.z) > 0.7 ? up.x * sign(fwd.z) * -1.0 : fwd.x;
  const f64 hy = std::abs(fwd.z) > 0.7 ? up.y * sign(fwd.z) * -1.0 : fwd.y;
  if (hypot2(hx, hy) > 0.2) body_yaw = atan2(hy, hx);
  body_root = V3{com.x, com.y, ground_z};
}

V3 Behaviours::sole_pos(i32 i) const {
  const RigidBody& f = *body.parts[i == 0 ? B::footL : B::footR];
  V3 out = f.point(body.feet[size_t(i)]->local);
  out.z -= plan.feet_planner.dims.ankle_h;
  return out;
}

// ---- the frame, before the plan ------------------------------------------------------------------

void Behaviours::prepare(f64 dt, const WorldPose& pose) {
  time_ += dt;
  mode_time += dt;
  const f64 kk = k();
  PlanControl& ctl = plan.control;
  i32 pressed_part = -1;
  if (const auto& care = capabilities().care; care && care->from_bone && conscious) {
    const auto bone = size_t(kBodyBone[size_t(care->part)]);
    const V3 wound = pose.p[bone] + rotate(pose.q[bone], care->local);
    for (size_t hand = 0; hand < 2; ++hand)
      if (ctl.arms[hand] && ctl.arms[hand]->weight > .5 && vdist(ctl.arms[hand]->target, wound) < .07 * k() &&
          vdist(pose.p[hand == 0 ? H::handL : H::handR], wound) < .13 * k())
        pressed_part = care->part;
  }
  ctl.reset();
  impact_reflex_.update(V3{}, dt);
  if ((alive && conscious && (mode == BodyMode::Animated || mode == BodyMode::Reacting)) ||
      (mode == BodyMode::Dying && !dying_head_)) {
    ctl.spine += impact_reflex_.x * 0.35;
    ctl.chest += impact_reflex_.x * 0.65;
  }
  grip_[0] = grip_[1] = 0.0;
  damage.update(dt, pressed_part);
  plan.capabilities = capabilities();
  // Resolve the host's requested stance before recovery reads it. Hosts may
  // keep asking to stand every tick, even after both legs have been lost.
  if (capabilities().mobility == Mobility::Kneel) plan.input.stance = Stance::Kneel;
  if (capabilities().mobility == Mobility::Crawl || capabilities().mobility == Mobility::Immobile) {
    plan.input.stance = Stance::Prone;
    if ((mode == BodyMode::Animated || mode == BodyMode::Reacting) && plan.stance != Stance::Prone) collapse(2);
  }
  for (i32 i = 0; i < kBodyCount; ++i) {
    const f64 s = stun_[size_t(i)];
    stun_[size_t(i)] = static_cast<f32>(std::max(0.0, s - dt * (0.9 + 0.8 * s)));
  }
  daze_ = std::max(0.0, daze_ - dt * (conscious ? 0.9 : 0.0));
  shock_ = std::max(0.0, shock_ - dt * 2.2);
  impact_yield_ = std::max(0.0, impact_yield_ - dt);
  nerves = std::max(0.0, nerves - dt * 0.05);
  upset_ = std::max(0.0, upset_ - dt);
  step_cooldown_ = std::max(0.0, step_cooldown_ - dt);
  if (trip_pending_ > 0.0) {
    trip_pending_ -= dt;
    const auto& fs = plan.feet_planner.feet;
    for (i32 i = 0; i < 2; ++i)
      if (!fs[size_t(i)].planted && !fs[size_t(i)].held && fs[size_t(i)].swing > 0.15) {
        catch_foot(i);
        break;
      }
  }
  for (Threat& t : threats_) t.age += dt;
  threats_.erase(std::remove_if(threats_.begin(), threats_.end(), [](const Threat& t) { return !(t.age < t.hold + 1.2); }), threats_.end());
  sense_dt_ = dt;
  if (dt > 0.0) {
    const V3 pp = plan.world.p[H::pelvis];
    const f64 a = 1.0 - exp(-dt * 30.0);
    V3& v = plan_pelvis_vel_;
    const V3& q = plan_pelvis_prev_;
    v.x += ((pp.x - q.x) / dt - v.x) * a;
    v.y += ((pp.y - q.y) / dt - v.y) * a;
    v.z += ((pp.z - q.z) / dt - v.z) * a;
    plan_pelvis_prev_ = pp;
  }
  sense(pose);
  if (!root_init_) {
    root_init_ = true;
    body_root = plan.root_pos;
  }
  // what is around (walls to hold on to), now and then, more often when it may matter
  const bool wants =
      mode != BodyMode::Animated || capabilities().pain > 0.3 || (1 - capabilities().legs[0].control) + (1 - capabilities().legs[1].control) > 0.3;
  surroundings.age += dt;
  if (wants && surroundings.age > (mode == BodyMode::Animated ? 0.4 : 0.12)) surroundings.probe(*plan.collision, body_root_or_plan(), kk, 1.05);

  // ---- modes ----
  modes(dt, pose);

  // ---- the plan's controls ----
  // injuries: a limp, pain, a hunch
  ctl.limp[0] = clamp((1 - capabilities().legs[0].control) * 1.1, 0.0, 1.0);
  ctl.limp[1] = clamp((1 - capabilities().legs[1].control) * 1.1, 0.0, 1.0);
  ctl.pain = capabilities().pain;
  ctl.fold += (1 - capabilities().trunk) * 0.22 * smoothstep(0.12, 0.4, time_ - hit_at_) + (mode == BodyMode::Dying ? 0.25 : 0.0);
  ctl.crouch += nerves * 0.25;
  ctl.care = clamp(1.0 - 0.6 * smoothstep(2.2, 5.0, hypot2(plan.velocity.x, plan.velocity.y)) - 0.4 * (plan.input.mood == Mood::Panic ? 1.0 : 0.0) -
                       0.3 * capabilities().pain,
                   0.1, 1.0);
  if (mode != BodyMode::Animated) ctl.busy = true;
  // the body leads: the plan's pelvis is where the physics has it, its root under the body
  if (physical && (mode == BodyMode::Reacting || mode == BodyMode::Falling || mode == BodyMode::Dying || mode == BodyMode::Dead)) {
    ctl.pelvis_pos = pose.p[H::pelvis];
    ctl.pelvis_rot = pose.q[H::pelvis];
    ctl.pelvis_weight = mode == BodyMode::Reacting ? 0.85 : 1.0;
    ctl.pelvis_height = mode != BodyMode::Reacting && mode != BodyMode::Dying;
    ctl.hold_feet = true;
    carry_root();
  }
  if (mode == BodyMode::Lying || mode == BodyMode::Rising) ctl.hold_feet = true;
  if (mode == BodyMode::Falling) {
    // the legs carry nothing now: loose, bent; the trunk curls a little
    ctl.relax_legs = smoothstep(0.05, 0.35, mode_time);
    ctl.fold += 0.15;
  }

  // writhing on the ground: curling up and stretching out, rocking, the head coming up
  if (mode == BodyMode::Lying && writhing && alive) {
    const f64 t = time_ + writhe_seed_;
    // spasms of pain come and go: curling up, rocking, the head coming up, easing off
    const f64 spasm = pow(0.5 + 0.5 * sin(t * 0.9) * sin(t * 0.31 + 1.0), 1.5);
    const f64 a = 0.5 + 0.5 * sin(t * 1.7);
    ctl.relax_legs = 0.25 + 0.75 * spasm;
    ctl.spine.z += (0.25 + 0.4 * spasm) * sin(t * 1.3);
    ctl.spine.y += 0.35 * spasm * sin(t * 0.8 + 2.0);
    ctl.chest.x -= 0.35 * spasm * a;
    ctl.neck.x -= 0.45 * spasm * std::max(0.0, sin(t * 1.1));
    ctl.fold += 0.45 * spasm;
  }

  // ---- reflexes and behaviours ----
  if (alive || mode == BodyMode::Dying) {
    flail_want_ = 0.0;
    if (mode == BodyMode::Reacting) balance(dt);
    flail_ += (flail_want_ - flail_) * (1.0 - exp(-dt * (flail_want_ > flail_ ? 9.0 : 2.2)));
    if (flail_ > 0.02 && (mode == BodyMode::Reacting || mode == BodyMode::Animated)) arms_out(flail_, dt);
    if (mode == BodyMode::Falling || (mode == BodyMode::Dying && mode_time > dying_for_ * 0.4)) catch_fall(dt, pose);
    bracing(dt, pose);
    if (conscious) flinch(dt, pose);
    hold_wound(dt, pose);
    // after the flinch and a glance at the wound: where did that come from?
    const f64 since = time_ - hit_at_;
    if (hit_from_ && conscious && alive && !ctl.look && since > 0.45 && since < 3.0) {
      ctl.look = *hit_from_;
      ctl.look_weight = 0.85 * smoothstep(0.45, 0.8, since) * (1.0 - smoothstep(2.2, 3.0, since));
    }
  }
}

V3 Behaviours::body_root_or_plan() const { return mode == BodyMode::Animated ? plan.root_pos : body_root; }

void Behaviours::carry_root() {
  const V3 r = body_root;
  const f64 dx = r.x - plan.root_pos.x, dy = r.y - plan.root_pos.y;
  root_motion.x += dx;
  root_motion.y += dy;
  const f64 z = mode == BodyMode::Reacting ? plan.root_pos.z + clamp(ground_z - plan.root_pos.z, -0.05, 0.05) : ground_z;
  root_motion.z += z - plan.root_pos.z;
  // (the heading follows the pelvis while reacting; lying, the plan's own)
  plan.carry_root(V3{r.x, r.y, z}, mode == BodyMode::Reacting ? plan.root_yaw + clamp(wrap_pi(body_yaw - plan.root_yaw), -0.05, 0.05) : plan.root_yaw);
}

V3 Behaviours::take_root_motion() {
  const V3 out = root_motion;
  root_motion = V3{};
  return out;
}

void Behaviours::modes(f64 dt, const WorldPose& pose) {
  const f64 kk = k();
  if (alive && capabilities().mobility == Mobility::Immobile &&
      (mode == BodyMode::Animated || mode == BodyMode::Reacting || mode == BodyMode::Rising)) {
    // Exhaustion can arrive after a crawl has begun, or halfway through getting
    // up. Release the locomotion assists in either case and settle where it is.
    set_mode(BodyMode::Falling);
  }
  switch (mode) {
    case BodyMode::Animated: {
      if (!physical) {
        if (force_react_) force_react_ = false;
        break;
      }
      // knocked off the plan: too far off its pelvis, or moving other than it means to
      const V3 pp = plan.world.p[H::pelvis];
      const V3 bp = pose.p[H::pelvis];
      const f64 ex = bp.x - pp.x, ey = bp.y - pp.y;
      const f64 dev = hypot2(ex, ey);
      // (a host that moves its character faster than a body can follow: the body is carried after
      // the plan rather than left behind)
      if (upset_ <= 0.0 && !force_react_ && dev > 0.3 * kk) carry_body(ex, ey, pp.z - bp.z, dev - 0.3 * kk);
      // (lagging behind a hurried start is not a fall; being pushed sideways is)
      const f64 vs = hypot2(plan.velocity.x, plan.velocity.y);
      const f64 lateral = vs > 0.3 ? std::abs(ex * plan.velocity.y - ey * plan.velocity.x) / vs : dev;
      // (against the pelvis target's own motion, smoothed: the plan's velocity lags a start)
      const V3& pv = plan_pelvis_vel_;
      const f64 dv = hypot2(com_vel.x - pv.x, com_vel.y - pv.y);
      const bool standing = plan.stance == Stance::Stand && plan.stance_progress() >= 1.0 && !plan.down();
      // (out of balance beyond what the plan means: a lunge into a punch is not a fall)
      const f64 px = pp.x + com.x - bp.x + pv.x / w0_, py = pp.y + com.y - bp.y + pv.y / w0_;
      const f64 planned = support.empty() ? 0.0 : std::max(0.0, support.distance(px, py));
      // (only after something happened to the body: lagging a hurried plan is not a fall)
      const bool knocked = upset_ > 0.0 && (lateral > 0.1 * kk || dev > 0.22 * kk || dv > 1.4 || (vs < 0.3 && balance_error > 0.06 * kk + planned));
      if (standing && (force_react_ || knocked)) {
        force_react_ = false;
        set_mode(BodyMode::Reacting);
      } else if (!standing && upset_ > 0.0 && (dev > 0.25 * kk || daze_ > 0.6)) {
        // knocked over from a kneel, a seat, the ground
        set_mode(BodyMode::Falling);
      }
      force_react_ = false;
      if (daze_ > 0.6 && standing) set_mode(BodyMode::Reacting);
      break;
    }
    case BodyMode::Reacting: {
      // (already knocked: another knock is dealt with here)
      force_react_ = false;
      react_t_ += dt;
      const f64 legs = leg_strength();
      const f64 tl = tilt(pose);
      const f64 max_step = 1.1 * plan.leg_len * clamp(legs, 0.45, 1.0);
      // (a step in the air still has its landing to catch the body with)
      const auto& fp = plan.feet_planner.feet;
      const bool stepping = (!fp[0].planted && fp[0].forced) || (!fp[1].planted && fp[1].forced);
      const f64 reach = max_step * (stepping ? 1.75 : 1.3) + 0.1 * kk;
      // (lost for a moment, not a passing jolt - unless far gone)
      const bool beyond = tl > 0.9 || (!airborne && balance_error > reach);
      lost_for_ = beyond ? lost_for_ + dt : 0.0;
      const bool gone = beyond && (lost_for_ > 0.14 || tl > 1.3 || balance_error > 1.8 * reach);
      // (thrown up off the feet: no step catches that)
      const bool thrown = airborne && com_vel.z - std::max(0.0, plan_pelvis_vel_.z) > 1.3;
      const bool lost_it = gone || thrown || legs < 0.25 || steps > 12 || react_t_ > 6.0 || daze_ > 0.75 || !conscious;
      if (lost_it) {
        lost_why = thrown ? "thrown" : tl > 0.9 ? "tilt" : balance_error > reach ? "reach" : legs < 0.25 ? "legs" : steps > 12 ? "steps" : react_t_ > 6.0 ? "time" : "daze";
        set_mode(BodyMode::Falling);
        break;
      }
      const bool slow = hypot2(com_vel.x, com_vel.y) < 0.3;
      const bool still = !plan.feet_planner.feet[0].forced && !plan.feet_planner.feet[1].forced;
      // (in balance, or at its edge and still: leaning on something)
      // (a sway about the edge of the support, as a body collects itself, is settled too)
      const bool settled = balance_error < 0.02 * kk || (balance_error < 0.06 * kk && hypot2(com_vel.x, com_vel.y) < 0.2);
      balanced_for_ = settled && slow && still ? balanced_for_ + dt : std::max(0.0, balanced_for_ - 2.0 * dt);
      if (balanced_for_ > 0.3 && mode_time > 0.35) set_mode(BodyMode::Animated);
      break;
    }
    case BodyMode::Falling: {
      const f64 pelvis_h = pose.p[H::pelvis].z - ground_z;
      const bool slow = hypot3(com_vel.x, com_vel.y, com_vel.z) < 0.7;
      low_for_ = pelvis_h < 0.42 * kk && slow ? low_for_ + dt : 0.0;
      // (down - the hips and the chest on the ground - is lying, whatever still slides)
      const f64 chest_h = pose.p[H::chest].z - ground_z;
      const bool down = pelvis_h < 0.38 * kk && chest_h < 0.42 * kk;
      if (low_for_ > 0.3 || (mode_time > 0.3 && down) || (mode_time > 1.4 && pelvis_h < 0.45 * kk) || mode_time > 3.0) enter_lying(pose);
      break;
    }
    case BodyMode::Lying: {
      align_lying(pose, true);
      if (!conscious && time_ >= down_until_) conscious = true;
      // A body with usable arms can turn over and crawl; an incapacitated body
      // stays down instead of retrying the get-up sequence every few seconds.
      const bool crawl = (writhing || legless || capabilities().mobility == Mobility::Crawl) && plan.input.stance == Stance::Prone && mode_time > 1.5;
      if (alive && conscious && capabilities().mobility != Mobility::Immobile && mode_time > 0.8 &&
          ((time_ >= down_until_ && !legless) || crawl)) set_mode(BodyMode::Rising);
      break;
    }
    case BodyMode::Rising: {
      writhing = false;
      // gather for a moment, then get up through the stances: briskly unhurt, slowly hurt
      const f64 hurt =
          clamp(capabilities().pain + 0.6 * std::max((1 - capabilities().legs[0].control), (1 - capabilities().legs[1].control)) + 0.5 * daze_, 0.0, 1.0);
      if (mode_time > 0.3 + 0.5 * hurt && plan.down() && plan.stance == Stance::Down && plan.stance_progress() >= 1.0) plan.get_up(lerp(1.45, 0.7, hurt));
      // (up to the host's stance: standing, or prone to crawl away)
      if (mode_time > 0.6 && !plan.down() && plan.stance != Stance::Down && plan.stance == plan.stance_target() && plan.stance_progress() >= 1.0) set_mode(BodyMode::Animated);
      break;
    }
    case BodyMode::Dying: {
      if (mode_time > dying_for_ + 0.4) set_mode(BodyMode::Dead);
      break;
    }
    case BodyMode::Dead:
      break;
  }
}

void Behaviours::carry_body(f64 ex, f64 ey, f64 ez, f64 amount) {
  f64 d = hypot2(ex, ey);
  if (d == 0.0) d = 1.0;
  const f64 kk = amount / d;
  for (RigidBody* b : body.parts) {
    b->x.x -= ex * kk;
    b->x.y -= ey * kk;
    b->x.z += ez * kk * 0.5;
  }
  had_com_ = false;
}

void Behaviours::enter_lying(const WorldPose& pose) {
  set_mode(BodyMode::Lying);
  align_lying(pose);
  if (down_until_ < time_) down_until_ = time_ + 1.2 + rng_.next() * 1.4 + 2.0 * capabilities().pain;
}

void Behaviours::align_lying(const WorldPose& pose, bool only_if_changed) {
  const Quat pq = pose.q[H::pelvis];
  const V3 fwd = rotate(pq, V3{0, 1, 0});
  if (only_if_changed) {
    const bool flipped = plan.lying_on_back() ? fwd.z < -0.5 : fwd.z > 0.5;
    const V3 pp = pose.p[H::pelvis];
    const V3 r = plan.root_pos;
    if (!flipped && hypot2(pp.x - r.x, pp.y - r.y) < 0.35 * k()) return;
  }
  const bool back = fwd.z > 0.0;
  // the plan's lying pose: on the back the feet point along +y from the head, face down the head does
  const V3 head = pose.p[H::chest] - pose.p[H::pelvis];
  const f64 yaw = back ? atan2(-head.y, -head.x) : atan2(head.y, head.x);
  const V3 pp = pose.p[H::pelvis];
  const V3 before = plan.root_pos;
  plan.lie(V3{pp.x, pp.y, ground_z}, yaw, back);
  // the host is told where the body ended up
  const V3 r = plan.root_pos;
  root_motion = root_motion + (r - before);
}

f64 Behaviours::leg_strength() const {
  const f64 legs = 1.0 - 0.5 * std::max((1 - capabilities().legs[0].control), (1 - capabilities().legs[1].control)) -
                   0.2 * std::min((1 - capabilities().legs[0].control), (1 - capabilities().legs[1].control));
  const f64 stun = std::max({static_cast<f64>(stun_[B::thighL]), static_cast<f64>(stun_[B::shinL]), static_cast<f64>(stun_[B::thighR]), static_cast<f64>(stun_[B::shinR])});
  return legless ? 0.0 : clamp(std::max(capabilities().legs[0].support, capabilities().legs[1].support) * legs * (1.0 - 0.6 * stun) * (1.0 - daze_), 0.0, 1.0);
}

f64 Behaviours::tilt(const WorldPose& pose) const {
  const V3 up = rotate(pose.q[H::chest], V3{0, 0, 1});
  return acos(clamp(up.z, -1.0, 1.0));
}

// ---- balance -------------------------------------------------------------------------------------

// Stepping to stay up: where the capture point runs out of the support, a foot goes there (the
// one it suits, on its own side of the other), quicker the worse it is; a step already swinging
// is re-aimed where the capture point will be when it lands. Arms go out.
void Behaviours::balance(f64 /*dt*/) {
  const f64 kk = k();
  auto& fp = plan.feet_planner;
  auto& feet = fp.feet;
  const f64 err = balance_error;
  const f64 h = std::max(0.45 * kk, com.z - ground_z);
  const f64 w0 = std::sqrt(G / h);
  const f64 legs = leg_strength();
  const f64 max_step = 1.1 * plan.leg_len * clamp(legs, 0.45, 1.0);
  // (a stride the gait had begun becomes the balance's step)
  const i32 swinging = !feet[0].planted && !feet[0].held && capabilities().legs[0].support >= .12 ? 0
                       : !feet[1].planted && !feet[1].held && capabilities().legs[1].support >= .12 ? 1 : -1;
  // (just outside, a weight shift does it: no step)
  const f64 margin = 0.05 * kk;
  const V3 right{sin(plan.root_yaw), -cos(plan.root_yaw), 0.0};
  auto planned_step = [&](i32 i, f64 T) {
    const auto& stance = feet[size_t(1 - i)];
    // where the capture point will be when the foot lands (it runs away from the stance foot's
    // centre of pressure)
    const f64 px = stance.planted ? stance.pos.x : com.x, py = stance.planted ? stance.pos.y : com.y;
    const f64 grow = exp(w0 * T);
    f64 tx = px + (capture.x - px) * grow;
    f64 ty = py + (capture.y - py) * grow;
    // a little past it, to catch the body rather than just stop
    const f64 dx = tx - com.x, dy = ty - com.y;
    f64 dl = hypot2(dx, dy);
    if (dl == 0.0) dl = 1.0;
    tx += (dx / dl) * 0.06 * kk;
    ty += (dy / dl) * 0.06 * kk;
    // on its own side of the other foot (a cross-over step at most a little)
    if (stance.planted) {
      const f64 lat = (tx - stance.pos.x) * right.x + (ty - stance.pos.y) * right.y;
      const f64 side = static_cast<f64>(feet[size_t(i)].side);
      const f64 want = side * 0.13 * kk;
      const f64 need = side > 0.0 ? std::max(0.0, want - lat) : std::min(0.0, want - lat);
      tx += right.x * need * 0.8;
      ty += right.y * need * 0.8;
      // within reach of the stance foot
      const f64 sx = tx - stance.pos.x, sy = ty - stance.pos.y;
      const f64 sl = hypot2(sx, sy);
      if (sl > max_step) {
        tx = stance.pos.x + (sx / sl) * max_step;
        ty = stance.pos.y + (sy / sl) * max_step;
      }
    }
    return V3{tx, ty, ground_z};
  };
  if (swinging >= 0) {
    // re-aim the swing (and hurry it if things got worse)
    const auto& f = feet[size_t(swinging)];
    const f64 urgency = clamp(err / (0.3 * kk), 0.0, 1.0);
    const f64 left = std::min((1.0 - f.swing) / std::max(1e-3, f.swing_rate), (1.0 - f.swing) * lerp(0.3, 0.14, urgency) / std::max(0.6, legs));
    fp.step(swinging, planned_step(swinging, left), left);
  } else if (err > margin && step_cooldown_ <= 0.0 && time_ >= snag_until_[0] && time_ >= snag_until_[1]) {
    // (a snagged foot: the other one has the weight and cannot go; the snag frees first)
    // which foot: the one behind the way the body goes (the other has the weight), unless that
    // would cross the legs
    const V3 dir = vnorm(V3{capture.x - com.x, capture.y - com.y, 0.0}, V3{cos(plan.root_yaw), sin(plan.root_yaw), 0.0});
    i32 best = -1;
    f64 score = -kInf;
    for (i32 i = 0; i < 2; ++i) {
      const auto& f = feet[size_t(i)];
      if (!f.planted || f.held || capabilities().legs[size_t(i)].support < .12 || time_ < snag_until_[size_t(i)]) continue;
      const f64 behind = -((f.pos.x - com.x) * dir.x + (f.pos.y - com.y) * dir.y);
      const f64 side_fit = static_cast<f64>(f.side) * dot(dir, right);
      const f64 sc = behind + 0.12 * side_fit - (f.since < 0.12 ? 1.0 : 0.0);
      if (sc > score) {
        score = sc;
        best = i;
      }
    }
    if (best >= 0) {
      const f64 urgency = clamp(err / (0.3 * kk), 0.0, 1.0);
      const f64 T = lerp(0.3, 0.15, urgency) / std::max(0.6, legs);
      fp.step(best, planned_step(best, T), T, plan.root_yaw - static_cast<f64>(feet[size_t(best)].side) * plan.style.toe_out);
      ++steps;
      step_cooldown_ = 0.05;
    }
  }
  // arms out for balance (a windmill when it is bad): quick to go out, slow to come down
  flail_want_ = smoothstep(0.04 * kk, 0.4 * kk, err + (swinging >= 0 ? 0.08 * kk : 0.0));
}

void Behaviours::arms_out(f64 w, f64 /*dt*/) {
  const f64 kk = k();
  const WorldPose& wp = plan.world;
  const Quat chest_q = wp.q[H::chest];
  for (i32 i = 0; i < 2; ++i) {
    if (plan.control.arms[size_t(i)]) continue;
    // a long gun stays in the right hand
    if (!plan.props.free_hand(i == 0)) continue;
    const f64 side = i == 0 ? -1.0 : 1.0;
    const V3 sh = wp.p[size_t(i == 0 ? H::upperarmL : H::upperarmR)];
    // out to the side, a little forward, circling (no higher than the shoulder)
    const f64 ph = time_ * 4.5 + i * 1.7;
    const V3 circle{0.0, cos(ph) * 0.06 * kk * w, sin(ph) * 0.05 * kk * w};
    const V3 off = rotate(chest_q, V3{side * 0.44 * kk, 0.12 * kk + circle.y, -0.08 * kk + circle.z});
    ArmTask t;
    t.target = sh + off;
    t.rot = chest_q * qeuler(0.0, side * 1.4, 0.0);
    t.pole = rotate(chest_q, V3{side * 0.3, -0.5, -0.8});
    t.weight = w * 0.8;
    plan.control.arms[size_t(i)] = t;
  }
}

// ---- catching a fall -------------------------------------------------------------------------------

// Hands out where the body will land (the ground, or a wall in the way), the head kept up. A hand
// that lands stays where it landed and gives, so the arms fold as the body comes down on them;
// once the body is down the arms let go.
void Behaviours::catch_fall(f64 dt, const WorldPose& pose) {
  const f64 kk = k();
  PlanControl& ctl = plan.control;
  const f64 tone = mode == BodyMode::Dying ? clamp(1.0 - mode_time / (dying_for_ + 0.2), 0.0, 1.0) * (dying_head_ ? 0.0 : 0.8) : 1.0;
  const bool low = pose.p[H::pelvis].z - ground_z < 0.38 * kk && pose.p[H::chest].z - ground_z < 0.45 * kk;
  if (tone < 0.05 || low) {
    landed_[0] = landed_[1] = std::nullopt;
    return;
  }
  const V3 v = com_vel;
  V3 dir{v.x, v.y, 0.0};
  if (norm(dir) < 0.3) {
    // slow: the way the trunk leans
    const V3 up = rotate(pose.q[H::chest], V3{0, 0, 1});
    dir = V3{up.x, up.y, 0.0};
  }
  dir = vnorm(dir, V3{cos(body_yaw), sin(body_yaw), 0.0});
  const V3 fwd = rotate(pose.q[H::pelvis], V3{0, 1, 0});
  const bool back = dir.x * fwd.x + dir.y * fwd.y < -0.3;
  const std::optional<Surface> wall = surroundings.wall_toward(dir, 0.7, 1.0 * kk);
  const V3 chest = pose.p[H::chest];
  for (i32 i = 0; i < 2; ++i) {
    if (ctl.arms[size_t(i)] && ctl.arms[size_t(i)]->weight > 0.5) continue;
    const f64 side = i == 0 ? -1.0 : 1.0;
    const V3 sh = pose.p[size_t(i == 0 ? H::upperarmL : H::upperarmR)];
    const V3 across{dir.y * side, -dir.x * side, 0.0};
    const RigidBody& hand = *body.parts[i == 0 ? B::handL : B::handR];
    V3 target;
    const std::optional<V3> landed = landed_[size_t(i)];
    if (landed) {
      target = *landed;
    } else if (wall) {
      target = wall->point + (wall->normal * (0.04 * kk) + across * (0.18 * kk));
      target.z = chest.z + 0.05 * kk;
    } else {
      target = sh + (dir * (back ? 0.3 * kk : 0.5 * kk) + across * (0.12 * kk));
      target.z = ground_z + 0.06 * kk;
    }
    // a hand that touches down stays there and gives
    if (!landed && hand.contact && physical) {
      const V3 at = hand.point(body.hands[size_t(i)]->local);
      landed_[size_t(i)] = at;
      target = at;
    }
    catch_t_[size_t(i)] = landed_[size_t(i)] ? std::min(1.0, catch_t_[size_t(i)] + dt * 2.5) : std::max(0.0, catch_t_[size_t(i)] - dt);
    const Quat palm_rot = palm_frame(V3{0, 0, -1}, dir);
    ArmTask t;
    t.target = target;
    if (!back) t.rot = palm_rot;
    t.pole = dir * (back ? 0.5 : -0.6) + V3{0, 0, -0.6};
    t.weight = tone * (1.0 - 0.6 * catch_t_[size_t(i)]);
    ctl.arms[size_t(i)] = t;
    // (guided out while in the air; landed, only the arm's own muscles hold)
    grip_[size_t(i)] = landed_[size_t(i)] ? 0.0 : 0.2 * tone;
  }
  // the head: up off the ground falling forward, chin tucked falling back
  if (back) ctl.neck.x += 0.45 * tone;
  else ctl.neck.x -= 0.35 * tone;
  ctl.crouch += 0.5 * tone;
}

// ---- holding on to a wall -----------------------------------------------------------------------

void Behaviours::bracing(f64 dt, const WorldPose& pose) {
  const f64 kk = k();
  PlanControl& ctl = plan.control;
  // why a hand would go to a wall now (and a reason once found holds a while: a hand does not come
  // and go with every wobble)
  std::optional<BraceWhy> why;
  std::optional<V3> dir;
  const std::optional<Brace>& prev = brace;
  if (mode == BodyMode::Reacting && (balance_error > 0.03 * kk || (prev && prev->why == BraceWhy::Balance && (prev->age < 0.9 || balanced_for_ < 0.25)))) {
    why = BraceWhy::Balance;
    dir = vnorm(V3{capture.x - com.x, capture.y - com.y, 0.0}, V3{0, 0, 0});
  } else if (mode == BodyMode::Dying && mode_time < dying_for_ * 0.9 && !dying_head_) {
    why = BraceWhy::Slump;
    dir = vnorm(V3{com_vel.x, com_vel.y, 0.0}, V3{cos(body_yaw), sin(body_yaw), 0.0});
  } else if (mode == BodyMode::Animated && alive &&
             (capabilities().pain > 0.45 || std::max((1 - capabilities().legs[0].control), (1 - capabilities().legs[1].control)) > 0.4) &&
             hypot2(plan.velocity.x, plan.velocity.y) < 0.25 && plan.stance == Stance::Stand && !plan.busy()) {
    why = BraceWhy::Lean;
  }
  if (!why) {
    brace.reset();
    return;
  }
  // find (or keep) the wall
  std::optional<Brace> b = brace;
  if (b && b->why != *why) b.reset();
  if (!b) {
    const std::optional<Surface> s = dir && norm(*dir) > 0.1 ? surroundings.wall_toward(*dir, 1.1, 0.95 * kk) : surroundings.nearest(0.75 * kk);
    if (!s) {
      brace.reset();
      return;
    }
    const V3 to_wall = vnorm(V3{s->point.x - pose.p[H::pelvis].x, s->point.y - pose.p[H::pelvis].y, 0.0});
    const V3 right{sin(body_yaw), -cos(body_yaw), 0.0};
    i32 hand = dot(to_wall, right) > 0.0 ? 1 : 0;
    // (a right hand on a long gun: the left one goes)
    if (!plan.props.free_hand(hand == 0)) hand = 1 - hand;
    if (!plan.props.free_hand(hand == 0)) return;
    const f64 height = *why == BraceWhy::Lean ? 1.25 : *why == BraceWhy::Slump ? 1.05 : 1.2;
    V3 t{s->point.x + s->normal.x * 0.035 * kk, s->point.y + s->normal.y * 0.035 * kk, ground_z + height * kk};
    // (along the wall, towards the hand's side)
    const V3 along{-s->normal.y, s->normal.x, 0.0};
    const f64 sgn = (dot(along, right) >= 0.0 ? 1.0 : -1.0) * (hand == 1 ? 1.0 : -1.0);
    t.x += along.x * sgn * 0.12 * kk;
    t.y += along.y * sgn * 0.12 * kk;
    Brace nb;
    nb.surface = *s;
    nb.hand = hand;
    nb.target = t;
    nb.normal = s->normal;
    nb.why = *why;
    nb.holding = false;
    nb.age = 0.0;
    b = nb;
  }
  b->age += dt;
  const RigidBody& hand_body = *body.parts[b->hand == 0 ? B::handL : B::handR];
  const V3 palm = physical ? hand_body.point(body.hands[size_t(b->hand)]->local) : plan.palm(b->hand == 0 ? Side::L : Side::R);
  if (!b->holding) {
    // it holds on where it gets there, or wherever the palm meets the wall first
    const V3 n0 = hand_body.contact_normal;
    if (norm(palm - b->target) < 0.12 * kk) {
      b->holding = true;
    } else if (physical && hand_body.contact && n0.x * b->normal.x + n0.y * b->normal.y > 0.6) {
      b->holding = true;
      b->target = palm + b->normal * (0.01 * kk);
    }
  }
  brace = b;
  // a palm flat on the wall, fingers up
  const V3 n = b->normal;
  const V3 side = vnorm(cross(V3{0, 0, 1}, n));
  const Quat rot = palm_frame(n * -1.0, V3{0, 0, 1});
  const f64 w = smoothstep(0.0, 0.25, b->age);
  ArmTask t;
  t.target = b->target;
  t.rot = rot;
  t.pole = V3{-n.x * 0.3 + (b->hand == 0 ? 0.3 : -0.3) * side.x, -n.y * 0.3, -1.0};
  t.weight = w;
  ctl.arms[size_t(b->hand)] = t;
  grip_[size_t(b->hand)] = b->holding ? 1.0 : 0.4;
  if (*why == BraceWhy::Lean || *why == BraceWhy::Slump) {
    // the weight goes towards the wall: the trunk leans onto the arm
    const V3 to_wall = vnorm(V3{-n.x, -n.y, 0.0});
    const V3 local = rotate(conj(qz(plan.root_yaw - kPi / 2.0)), to_wall);
    ctl.spine.y += -local.x * 0.12 * w;
    ctl.chest.y += -local.x * 0.1 * w;
    if (*why == BraceWhy::Lean) ctl.look.reset();
  }
}

// ---- flinching --------------------------------------------------------------------------------------

void Behaviours::flinch(f64 dt, const WorldPose& pose) {
  const f64 kk = k();
  PlanControl& ctl = plan.control;
  const V3 head = pose.p[H::head];
  // what threatens now, pooled: how much, and from where (weighted by each one's envelope)
  f64 sum = 0.0, peak = 0.0;
  V3 dir;
  for (const Threat& t : threats_) {
    const f64 env = t.age < 0.0 ? 0.0 : t.age < 0.07 ? t.age / 0.07 : t.age < 0.07 + t.hold ? 1.0 : exp(-(t.age - 0.07 - t.hold) / 0.3);
    const f64 w = env * t.amount;
    if (w < 1e-3) continue;
    const V3 d = vnorm(t.point - head, V3{0, 1, 0});
    dir = dir + d * w;
    sum += w;
    peak = std::max(peak, w);
  }
  // the response is one smooth thing, not a jerk per round: quick to come, slow to go, and it
  // turns towards where the danger is over a moment
  const f64 want = clamp(peak + 0.25 * (sum - peak), 0.0, 1.2);
  flinch_i_ += (want - flinch_i_) * (1.0 - exp(-dt * (want > flinch_i_ ? 22.0 : 3.0)));
  if (sum > 1e-3) {
    const V3 d = vnorm(dir, flinch_dir_);
    const f64 a = 1.0 - exp(-dt * (flinch_i_ < 0.05 ? 60.0 : 7.0));
    flinch_dir_ = vnorm(flinch_dir_ + (d - flinch_dir_) * a, d);
  }
  // under fire for a while: into cover (ducked low, both arms over the head)
  under_fire_ = want > 0.25 ? std::min(2.0, under_fire_ + dt) : std::max(0.0, under_fire_ - dt * 0.7);
  const f64 cover = smoothstep(0.5, 1.4, under_fire_);
  // once the flinch has passed: a look back at what it was
  for (const Threat& t : threats_) {
    const f64 after = t.age - 0.07 - t.hold - 0.2;
    if (after <= 0.0 || after > 1.1 || t.amount < 0.2) continue;
    if (flinch_i_ < 0.3 && (!ctl.look || ctl.look_weight < 0.3)) {
      ctl.look = t.point;
      ctl.look_weight = 0.75 * smoothstep(0.0, 0.25, after) * (1.0 - smoothstep(0.7, 1.1, after));
    }
    break;
  }
  const f64 w = flinch_i_;
  if (w < 0.02) {
    tension_ = std::max(0.0, tension_ - dt * 2.0);
    return;
  }
  tension_ += (std::max(tension_, w) - tension_) * (1.0 - exp(-dt * 12.0));
  const V3 away = flinch_dir_ * -1.0;
  // in the body's frame: which side the danger is on (it changes sides only clearly)
  const Quat inv = conj(qz(plan.root_yaw - kPi / 2.0));
  const V3 lm = rotate(inv, flinch_dir_);
  if (std::abs(lm.x) > 0.3) flinch_side_ = lm.x >= 0.0 ? 1.0 : -1.0;
  const f64 side_of = flinch_side_;
  // duck and turn away
  ctl.look = head + (away * 2.0 + V3{0, 0, -1.2 - 0.8 * cover});
  ctl.look_weight = std::max(ctl.look_weight, 0.8 * clamp(w, 0.0, 1.0));
  ctl.neck.x += (0.45 + 0.2 * cover) * w;
  ctl.head.y += side_of * 0.2 * w * (1.0 - 0.5 * cover);
  // (the face turns from it at once, before the eyes have found anything to look at)
  ctl.neck.z += side_of * 0.35 * w * (1.0 - 0.5 * cover);
  ctl.head.z += side_of * 0.45 * w * (1.0 - 0.5 * cover);
  ctl.shrug = std::max(ctl.shrug, clamp(w, 0.0, 1.0));
  ctl.spine.x -= (0.14 + 0.12 * cover) * w;
  ctl.chest.x -= (0.1 + 0.1 * cover) * w;
  ctl.crouch += (0.42 + 0.3 * cover) * w;
  // a hand up between the face and the danger (a two-handed gun: the body hunches over it); under
  // fire both arms cover the head
  const bool lg = primary_occupied(plan);
  if (!lg || w > 0.9) {
    const i32 hand = lg ? 0 : side_of > 0.0 ? 1 : 0;
    if (!ctl.arms[size_t(hand)] || ctl.arms[size_t(hand)]->weight < 0.5) {
      const V3 shield = head + (away * (-0.16 * kk * (1.0 - 0.4 * cover)) + V3{0, 0, (0.02 + 0.08 * cover) * kk});
      // (the palm to the danger; covering, over the head)
      const Quat rot = palm_frame(vnorm(away * -(1.0 - cover) + V3{0, 0, -cover}, away * -1.0), V3{0, 0, 1});
      ArmTask t;
      t.target = shield;
      t.rot = rot;
      t.pole = V3{0, 0, -1};
      t.weight = clamp(w * 1.1, 0.0, 1.0);
      ctl.arms[size_t(hand)] = t;
    }
    const f64 both = std::max(clamp((w - 0.7) * 3.0, 0.0, 1.0), cover * clamp(w * 2.0, 0.0, 1.0));
    if (both > 0.02 && !lg) {
      const i32 other = 1 - hand;
      if (!ctl.arms[size_t(other)]) {
        const V3 shield = head + (away * (-0.14 * kk * (1.0 - 0.4 * cover)) + V3{0, 0, (-0.06 + 0.12 * cover) * kk});
        ArmTask t;
        t.target = shield;
        t.pole = V3{0, 0, -1};
        t.weight = both;
        ctl.arms[size_t(other)] = t;
      }
    }
  }
}

// ---- holding a wound ----------------------------------------------------------------------------

void Behaviours::hold_wound(f64 /*dt*/, const WorldPose& pose) {
  const CareTarget* inj = capabilities().care ? &*capabilities().care : nullptr;
  PlanControl& ctl = plan.control;
  if (!inj || (!conscious && mode != BodyMode::Dying)) return;
  // These hands carry the trunk while getting up or pulling it along the
  // ground. At rest they can press the wound again.
  const bool on_ground = plan.stance == Stance::Prone || plan.stance_target() == Stance::Prone;
  if (mode == BodyMode::Rising || (on_ground && (plan.transitioning() || hypot2(plan.velocity.x, plan.velocity.y) > .04))) return;
  const f64 kk = k();
  const f64 tone = mode == BodyMode::Dying ? clamp(1.0 - mode_time / std::max(0.3, dying_for_), 0.0, 1.0) : 1.0;
  if (tone < 0.1 || dying_head_) return;
  // the free hand, or the other one for an arm
  const bool lg = primary_occupied(plan);
  const size_t bone = size_t(kBodyBone[size_t(inj->part)]);
  const Quat q = pose.q[bone];
  const V3 wound = pose.p[bone] + rotate(q, (inj->from_bone ? V3{} : body.com_local[size_t(inj->part)]) + inj->local);
  const V3 n = rotate(q, inj->normal);
  i32 hand;
  if ((inj->part >= B::upperarmL && inj->part <= B::handL)) {
    hand = 1;
  } else if ((inj->part >= B::upperarmR && inj->part <= B::handR)) {
    hand = 0;
  } else {
    const V3 right{sin(body_yaw), -cos(body_yaw), 0.0};
    hand = dot(wound - pose.p[H::pelvis], right) > 0.0 ? 1 : 0;
    if (lg && hand == 1) hand = 0;
  }
  if (hand == 1 && lg && (inj->part >= B::upperarmL && inj->part <= B::handL)) {
    // the gun hand lets go of nothing: the wounded arm just hangs
    return;
  }
  auto can_press = [&](i32 side) {
    const auto& arm = capabilities().arms[size_t(side)];
    const i32 first = side == 0 ? B::upperarmL : B::upperarmR;
    return plan.props.free_hand(side == 0) && arm.strength > .2 && arm.control > .2 &&
           !(inj->part >= first && inj->part <= first + 2);
  };
  if (!can_press(hand)) hand = 1 - hand;
  if (!can_press(hand)) return;
  if (ctl.arms[size_t(hand)] && ctl.arms[size_t(hand)]->weight > 0.6) return;
  // a moment to react, then the hand presses on it
  const f64 w = smoothstep(0.12, 0.4, inj->age) * (1.0 - smoothstep(inj->hold_until - 0.6, inj->hold_until, inj->age)) * tone;
  if (w < 0.02) return;
  const V3 target = wound + n * (0.03 * kk);
  ArmTask t;
  t.target = target;
  t.rot = palm_frame(n * -1.0, V3{0, 0, 1});
  t.weight = w;
  ctl.arms[size_t(hand)] = t;
  grip_[size_t(hand)] = 0.35 * w;
  // reaching a leg: the body bends to it
  if (inj->part >= B::thighL) {
    const f64 reach = clamp((pose.p[H::chest].z - target.z - 0.5 * kk) / (0.5 * kk), 0.0, 1.0);
    ctl.fold += 0.5 * reach * w;
    ctl.crouch += 0.3 * reach * w;
  }
  // a glance at it at first
  if (inj->age < 1.2 && !ctl.look) {
    ctl.look = wound;
    ctl.look_weight = 0.7 * w * (1.0 - smoothstep(0.6, 1.2, inj->age));
  }
}

// ---- the frame, after the plan: the muscles and the body -------------------------------------------

void Behaviours::drive(f64 dt, WorldPose& out) {
  drive_pre(dt);
  body.system.step(dt);
  drive_post(dt, out);
}

// (at ease only while they hang: hands held up - on the head in a panic, raised to surrender, over
// the face - are held there firmly, not left to flap)
f64 Behaviours::arm_held(i32 i) const {
  if (mode != BodyMode::Animated) return 0.0;
  const WorldPose& target = plan.world;
  const f64 kk = k();
  const f64 hz = target.p[size_t(i == 0 ? H::handL : H::handR)].z - target.p[size_t(i == 0 ? H::upperarmL : H::upperarmR)].z;
  return std::max(smoothstep(-0.42 * kk, -0.18 * kk, hz), plan.input.mood == Mood::Normal ? 0.0 : 0.8);
}

void Behaviours::drive_pre(f64 dt) {
  HumanoidBody& bd = body;
  const f64 kk = k();
  const f64 m = bd.total_mass;
  const WorldPose& target = plan.world;
  const WorldPose& prev_t = plan.prev_world;
  const bool ground_motion = mode == BodyMode::Falling || mode == BodyMode::Lying || mode == BodyMode::Rising ||
      plan.stance == Stance::Prone || plan.stance == Stance::Down;
  // Ground recovery must keep the same dissipative body as the landing. Raising
  // the spin allowance from 14 to 80 on entering Lying let a trapped wrist whirl.
  if (mode == BodyMode::Lying && mode_time > .3) bd.system.spin_cap = 4;
  else if (ground_motion || mode == BodyMode::Dead || mode == BodyMode::Dying) bd.system.spin_cap = 14;
  else if (mode == BodyMode::Reacting) bd.system.spin_cap = 24;
  else bd.system.spin_cap = plan.busy() ? 60 : 36;
  if (alive) bd.system.angular_drag = ground_motion && (bd.parts[B::pelvis]->contact || bd.parts[B::chest]->contact) ? .08 : .9;
  // A broad trunk contact resists spinning about the floor normal. The sphere
  // contacts supply sliding friction but have no contact-patch torsion of their own.
  if (ground_motion) for (i32 part : {B::pelvis, B::spine, B::chest}) {
    auto& b = *bd.parts[size_t(part)];
    if (!b.contact || b.contact_normal.z < .45) continue;
    const V3 n = b.contact_normal;
    const f64 inverse = dot(n, b.inv_inertia_mul(n));
    if (inverse > 0) b.torque -= n * clamp(dot(b.w, n) * 18 / inverse, -b.mass * G * .12, b.mass * G * .12);
  }
  bd.track(target, plan.pose, &prev_t, dt, ground_motion ? 7.0 : 30.0);
  const f64 idt = dt > 0.0 ? 1.0 / dt : 0.0;

  // ---- tone ----
  f64 base = 1.0;
  f64 legs = 1.0, arms = 1.0, neck = 1.0, trunk = 1.0;
  switch (mode) {
    case BodyMode::Animated:
      // at ease the arms and the head are carried loosely (they swing and settle with the body's
      // motion); what an action moves is firmer (see limb_t)
      arms = g_arms_at_ease;
      neck = 0.8;
      break;
    case BodyMode::Reacting:
      arms = 0.8;
      break;
    case BodyMode::Falling:
      // (braced for the ground, not fighting it)
      legs = 0.35;
      trunk = 0.55;
      neck = 0.8;
      arms = 0.9;
      break;
    case BodyMode::Lying:
      base = !conscious ? 0.06 : writhing ? 0.75 : 0.22;
      break;
    case BodyMode::Rising:
      base = lerp(0.25, 1.0, smoothstep(0.1, 0.9, mode_time));
      break;
    case BodyMode::Dying: {
      // the knees go first, the arms drop, the trunk and the neck keep their shape longest
      const f64 u = clamp(mode_time / dying_for_, 0.0, 1.0);
      base = dying_head_ ? 0.02 : 0.9 * pow(1.0 - u, 0.8) + 0.02;
      // (and a body tipping over goes limp in the legs: it collapses, it does not fall like a plank
      // and swing its legs up over itself)
      legs = 0.0;
      arms = pow(1.0 - u, 0.7) * 0.8;
      break;
    }
    case BodyMode::Dead:
      base = 0.0;
      break;
  }
  const f64 dz = (1.0 - 0.85 * daze_) * (1.0 - 0.6 * shock_);
  const f64 tense = 1.0 + 0.35 * tension_;
  // Recruit quickly for a held pose, an action or a turn, then release gradually.
  // The hand still needs support while returning to rest after an action ends.
  const f64 turn = clamp(norm(qerror(prev_t.q[H::chest], target.q[H::chest])) * idt / 4.0, 0.0, 1.0);
  for (size_t i = 0; i < 2; ++i) {
    f64 load = 0;
    if (const auto held = plan.props.at(i == 0 ? AttachPoint::LeftHand : AttachPoint::RightHand))
      load = clamp(held->archetype->mass / 8.0, .2, 1.0);
    else if (const auto held = plan.props.held(); held && held->style == WieldStyle::TwoHands)
      load = clamp(held->archetype->mass / 16.0, .2, 1.0);
    const f64 want = mode == BodyMode::Animated ? std::max({arm_held(i32(i)), plan.effort[i], turn, load}) : 0.0;
    arm_activation_[i] += (want - arm_activation_[i]) * (1.0 - exp(-(want > arm_activation_[i] ? 24.0 : 4.0) * dt));
  }
  auto arm_base = [&](i32 i) { return lerp(arms, std::max(arms, 1.15), arm_activation_[size_t(i)]); };
  std::array<f64, kRegionCount> region_t{};
  region_t[region_index(Region::Trunk)] = base * trunk * dz * tense * (1.0 - 0.3 * (1 - capabilities().trunk));
  region_t[region_index(Region::Neck)] = base * neck * dz * tense * (1.0 - 0.3 * (1 - capabilities().neck));
  region_t[region_index(Region::ArmL)] = base * arm_base(0) * dz * (1.0 - 0.65 * (1 - capabilities().arms[0].strength)) * (1.0 + 0.8 * tension_);
  region_t[region_index(Region::ArmR)] = base * arm_base(1) * dz * (1.0 - 0.65 * (1 - capabilities().arms[1].strength)) * (1.0 + 0.8 * tension_);
  region_t[region_index(Region::LegL)] = base * legs * dz * (1.0 - 0.45 * (1 - capabilities().legs[0].control));
  region_t[region_index(Region::LegR)] = base * legs * dz * (1.0 - 0.45 * (1 - capabilities().legs[1].control));
  region_tone = region_t;
  // a limb that strikes is thrown hard (tensed), one an action moves is firmer
  auto limb_t = [&](Region r) {
    const i32 i = r == Region::ArmL ? 0 : r == Region::ArmR ? 1 : r == Region::LegL ? 2 : r == Region::LegR ? 3 : -1;
    if (i < 0) return 1.0;
    return 1.0 + plan.effort[size_t(i)] * (plan.striking[size_t(i)] ? 2.5 : 0.6);
  };
  // (a limb that strikes passes through the body it strikes: the host deals the blow)
  for (i32 i = 0; i < 4; ++i) {
    const bool g = plan.striking[size_t(i)] && alive;
    const i32 a = i == 0 ? B::forearmL : i == 1 ? B::forearmR : i == 2 ? B::shinL : B::shinR;
    const i32 b = i == 0 ? B::handL : i == 1 ? B::handR : i == 2 ? B::footL : B::footR;
    bd.parts[size_t(a)]->ghost = g;
    bd.parts[size_t(b)]->ghost = g;
  }
  const auto& feet = plan.feet_planner.feet;
  for (i32 i = 1; i < kBodyCount; ++i) {
    const Region r = kRegion[size_t(i)];
    const f64 t = region_t[region_index(r)] * limb_t(r) * (1.0 - 0.85 * stun_[size_t(i)]) * capabilities().muscle[size_t(i)];
    bd.tone[size_t(i)] = static_cast<f32>(t);
    // standing legs are held by the ground, hanging limbs by the muscles
    const bool leg = i >= B::thighL;
    const bool planted = leg && feet[i < B::thighR ? 0 : 1].planted && (mode == BodyMode::Animated || mode == BodyMode::Reacting || mode == BodyMode::Rising);
    bd.hold_weight[size_t(i)] = planted || mode == BodyMode::Lying ? 0.0f : 1.0f;
  }
  if (auto held = plan.props.held(); held && held->style == WieldStyle::TwoHands && !held->archetype->has("firearm")) {
    const bool left = held->point == AttachPoint::RightHand;
    const size_t side = left ? 0 : 1;
    if (plan.striking[1 - side]) {
      bd.parts[left ? B::forearmL : B::forearmR]->ghost = true;
      bd.parts[left ? B::handL : B::handR]->ghost = true;
    }
    if (!plan.control.arms[side] || plan.control.arms[side]->weight < .35) {
      // The second arm follows the shared grip; it must not fight it with a
      // separate, strongly driven wrist trajectory during the follow-through.
      for (i32 part : {left ? B::upperarmL : B::upperarmR, left ? B::forearmL : B::forearmR, left ? B::handL : B::handR}) bd.tone[size_t(part)] *= .15f;
    }
  }
  bd.apply_tone();
  bd.compensate_gravity();

  // ---- assists: how much the legs hold the body up and where ----
  const V3 pel = target.p[H::pelvis];
  const V3 vel_t = plan.velocity;
  Attachment& sup = *bd.support;
  Attachment& steer = *bd.steer;
  Orienter& up = *bd.upright;
  const bool standing = plan.stance == Stance::Stand && plan.stance_progress() >= 1.0 && !plan.down();
  const f64 legs_s = leg_strength();
  sup.enabled = steer.enabled = up.enabled = false;
  bd.chest_turn->enabled = false;
  sup.target.z = pel.z;
  sup.target_vel.z = (pel.z - prev_t.p[H::pelvis].z) * idt;
  if (ground_motion) sup.target_vel.z = clamp(sup.target_vel.z, -1.2, 1.2);
  steer.target.x = pel.x;
  steer.target.y = pel.y;
  // (the pelvis target's own velocity: the plan's smoothed velocity lags a hurried start)
  const V3 pel_prev = prev_t.p[H::pelvis];
  steer.target_vel.x = (pel.x - pel_prev.x) * idt;
  steer.target_vel.y = (pel.y - pel_prev.y) * idt;
  if (ground_motion && norm(steer.target_vel) > 1.5) steer.target_vel = vnorm(steer.target_vel) * 1.5;
  up.target = target.q[H::pelvis];
  up.tilt_only = false;
  const f64 rise = mode == BodyMode::Rising ? smoothstep(0.15, 1.0, mode_time) : 1.0;
  if (mode == BodyMode::Animated || mode == BodyMode::Rising) {
    sup.enabled = true;
    sup.stiffness = m * 900.0 * rise;
    sup.damping = m * 55.0 * rise;
    sup.max_force = m * G * 2.2 * rise;
    steer.enabled = true;
    steer.stiffness = m * 700.0 * rise;
    steer.damping = m * 50.0 * rise;
    // (legs push harder on the move: starts, stops, turns)
    const f64 sp = hypot2(vel_t.x, vel_t.y);
    steer.max_force = m * G * (standing ? 0.8 + 0.7 * std::min(1.0, sp / 3.0) : 1.2) * rise;
    const f64 yield = 1 - .95 * smoothstep(0.0, .06, impact_yield_);
    steer.stiffness *= yield;
    steer.damping *= yield;
    steer.max_force *= yield;
    up.enabled = true;
    up.stiffness = 5000.0 * rise;
    up.damping = 450.0 * rise;
    up.max_torque = (standing ? 380.0 : ground_motion ? 160.0 : 900.0) * rise;
    if (ground_motion) {
      up.stiffness *= .25;up.damping *= .5;
      steer.max_force = std::min(steer.max_force, m * G * .45 * rise);
    }
  } else if (mode == BodyMode::Reacting) {
    // the legs hold the body up (as strong as they are) but no longer steer it
    sup.enabled = true;
    sup.stiffness = m * 700.0;
    sup.damping = m * 45.0;
    sup.max_force = m * G * lerp(0.7, 1.9, legs_s);
    up.enabled = true;
    up.tilt_only = true;
    up.stiffness = 1500.0 * legs_s;
    up.damping = 220.0;
    up.max_torque = 200.0 * legs_s;
    centre_of_pressure(legs_s);
  } else if (mode == BodyMode::Dying) {
    const f64 u = clamp(mode_time / dying_for_, 0.0, 1.0);
    // (the legs hold at first, then give all at once; a trunk tipping over is not held up by its
    // hips - the body would pivot on them and swing its legs up)
    const f64 up_z = rotate(bd.parts[B::chest]->q, V3{0, 0, 1}).z;
    const f64 hold = dying_head_ ? 0.0 : legs_s * (1.0 - u * u) * smoothstep(0.45, 0.85, up_z);
    if (hold > 0.02) {
      // the knees buckle: the legs hold less and less, lower and lower
      sup.enabled = true;
      sup.target.z = die_z_ - 0.35 * kk * u;
      sup.stiffness = m * 400.0 * hold;
      sup.damping = m * 30.0;
      sup.max_force = m * G * 1.05 * hold;
      // the ground's push through failing legs: the body topples as one over its feet (it does not
      // fold over hips held up in the air)
      if (!support.empty()) centre_of_pressure(0.5 * hold);
    }
    // a seat holds a dying body where it sits
    if (plan.stance == Stance::Sit) {
      sup.enabled = true;
      sup.target.z = pel.z;
      sup.stiffness = m * 600.0;
      sup.damping = m * 40.0;
      sup.max_force = m * G * 1.5;
      steer.enabled = true;
      steer.stiffness = m * 200.0;
      steer.damping = m * 30.0;
      steer.max_force = m * G * 0.5;
    }
  }

  // ---- feet: planted ones pinned, swinging ones led (an obstacle can stop them) ----
  for (i32 i = 0; i < 2; ++i) {
    const auto& f = feet[size_t(i)];
    Attachment& pin = *bd.feet[size_t(i)];
    Orienter& turn = *bd.feet_turn[size_t(i)];
    const size_t foot_bone = size_t(i == 0 ? H::footL : H::footR);
    const bool on_feet = mode == BodyMode::Animated || mode == BodyMode::Reacting || (mode == BodyMode::Rising && standing) ||
                         (mode == BodyMode::Dying && mode_time < dying_for_ * 0.55 && !dying_head_);
    pin.enabled = turn.enabled = false;
    if (!on_feet || !(plan.stance == Stance::Stand || plan.stance_target() == Stance::Stand)) continue;
    const V3 a = target.p[foot_bone];
    pin.target = a;
    pin.local = bd.com_local[size_t(i == 0 ? B::footL : B::footR)] * -1.0;
    turn.target = target.q[foot_bone];
    pin.enabled = turn.enabled = true;
    const f64 fm = bd.parts[i == 0 ? B::footL : B::footR]->mass;
    if (f.planted && !f.held) {
      if (!f.held && mode == BodyMode::Animated && plan.feet_planner.stepping) {
        // Hold the point touching the ground, so the ankle can roll over it.
        // Pinning a moving ankle with zero target velocity resisted the roll
        // and let the heel/toe drift, especially during brisk walking. Recovery
        // keeps the ankle support expected by its centre-of-pressure controller.
        const FeetDims& d = plan.feet_planner.dims;
        const V3 pivot{0, f.pitch < 0.0 ? d.ball_fwd : -d.heel_back, -d.ankle_h};
        pin.local += pivot;
        pin.target = a + rotate(target.q[foot_bone], pivot);
      }
      // (stiff, not rigid: a foot that lands a little off its spot is drawn in, not snapped)
      pin.stiffness = fm * 16000.0;
      pin.max_force = m * G * 2.5;
      pin.damping = fm * 250.0;
      pin.target_vel = V3{};
      turn.stiffness = 2500.0;
      turn.max_torque = 400.0;
      turn.damping = 60.0;
    } else if (f.held) {
      // a kick: the foot is driven to where the action puts it
      const f64 e = plan.effort[size_t(2 + i)];
      pin.stiffness = fm * 3000.0 * e;
      pin.max_force = 80.0 + 600.0 * e * (plan.striking[size_t(2 + i)] ? 1.0 : 0.6);
      pin.damping = fm * 60.0;
      const V3 pa = prev_t.p[foot_bone];
      pin.target_vel = (a - pa) * idt;
      turn.stiffness = 200.0;
      turn.max_torque = 60.0;
      turn.damping = 12.0;
    } else {
      // (the leg's muscles swing it; this only guides the foot to its spot, gently, so the whole
      // body is not dragged by it and an obstacle can stop it; up a stair the knee is lifted with a
      // will)
      const f64 upf = clamp(std::max(f.target.z - f.lift.z, f.clear) / (0.25 * kk), 0.0, 1.0);
      pin.stiffness = fm * (500.0 + 1500.0 * upf);
      pin.max_force = (mode == BodyMode::Reacting ? 90.0 : 45.0) + 250.0 * upf;
      pin.damping = fm * 40.0;
      const V3 pa = prev_t.p[foot_bone];
      pin.target_vel = (a - pa) * idt;
      turn.stiffness = 120.0;
      turn.max_torque = 40.0;
      turn.damping = 4.0;
    }
  }

  // A broken or paralysed leg cannot pin itself to the plan with a foot assist.
  for (size_t side = 0; side < 2; ++side) {
    const f64 control = capabilities().legs[side].control;
    const f64 support = feet[side].planted ? capabilities().legs[side].support : 1;
    bd.feet[side]->max_force *= control * support;
    bd.feet[side]->stiffness *= control * support;
    bd.feet_turn[side]->max_torque *= control;
    bd.feet_turn[side]->stiffness *= control;
  }

  // ---- hands: on the weapon, or where a behaviour sends them ----
  for (i32 i = 0; i < 2; ++i) {
    Attachment& att = *bd.hands[size_t(i)];
    Orienter& turn = *bd.hands_turn[size_t(i)];
    att.enabled = turn.enabled = false;
    att.reference = nullptr;
    att.local = bd.com_local[i == 0 ? B::handL : B::handR] * -1;
    const size_t hand_bone = size_t(i == 0 ? H::handL : H::handR);
    const auto& arm = capabilities().arms[size_t(i)];
    const f64 control = clamp(std::min(arm.strength, arm.control) * capabilities().consciousness, 0.0, 1.0);
    const f64 tone = region_t[region_index(i == 0 ? Region::ArmL : Region::ArmR)] * control;
    const std::optional<ArmTask>& task = plan.control.arms[size_t(i)];
    f64 grip = grip_[size_t(i)];
    // the weapon: the gun hand keeps its aim, the support hand its hold
    const auto held = plan.props.held();
    const bool on_weapon = held && (held->style == WieldStyle::TwoHands || attachment_bone(held->point) == i32(hand_bone)) && (!task || task->weight < .35);
    if (on_weapon && tone > 0.3 && (mode == BodyMode::Animated || mode == BodyMode::Reacting || mode == BodyMode::Rising)) grip = std::max(grip, i == 1 ? 0.8 : 0.6);
    // (an action's hand: a fist thrown at a jaw, a hand on a magazine)
    if (mode == BodyMode::Animated && plan.effort[size_t(i)] > 0.05) grip = std::max(grip, plan.effort[size_t(i)] * (plan.striking[size_t(i)] ? 1.0 : 0.5));
    // (hands held up - on the head, raised, over the face - are held where they are meant)
    grip = std::max(grip, 0.5 * arm_held(i));
    if (mode == BodyMode::Animated) grip = std::max(grip, 0.4 * arm_activation_[size_t(i)]);
    const V3 w = target.p[hand_bone];
    const V3 wp = prev_t.p[hand_bone];
    // (the hand's planned velocity, smoothed: frame differences are noisy)
    V3& hv = hand_vel_[size_t(i)];
    const f64 fv = 1.0 - exp(-dt * 20.0);
    hv.x += ((w.x - wp.x) * idt - hv.x) * fv;
    hv.y += ((w.y - wp.y) * idt - hv.y) * fv;
    hv.z += ((w.z - wp.z) * idt - hv.z) * fv;
    const f64 hand_speed = ground_motion ? 2.0 : 12.0;
    if (norm(hv) > hand_speed) hv = vnorm(hv) * hand_speed;
    if (grip <= 0.01 || tone < 0.05 || lost[i == 0 ? B::handL : B::handR]) continue;
    att.target = w;
    att.target_vel = hv;
    turn.target = target.q[hand_bone];
    // The support hand follows the physical grip, including the primary arm's
    // lag under load. Following only the plan would let it slip off a heavy bat.
    const auto* action = action_def(plan.action_name());
    const bool hand_task = action && !action->two_hands && action->drives(i == 0 ? Channel::HandL : Channel::HandR);
    const bool support_grip = on_weapon && held->style == WieldStyle::TwoHands && attachment_bone(held->point) != i32(hand_bone) && !hand_task;
    if (support_grip) {
      if (const auto* socket = held->archetype->socket("secondary")) {
        const Side side = i == 0 ? Side::L : Side::R;
        auto& primary = *bd.parts[size_t(HumanoidBody::body_of_bone(attachment_bone(held->point)))];
        turn.target = held->rotation * socket->rotation * plan.arms.canonical(side);
        att.local += plan.arms.palm_offset(side);
        att.target = held->pos + rotate(held->rotation, socket->point);
        att.reference = &primary;
        att.reference_local = rotate(conj(primary.q), att.target - primary.x);
        att.target_vel = primary.v + cross(primary.w, att.target - primary.x);
        grip = std::max(grip, .9);
      }
    }
    const f64 hm = bd.parts[i == 0 ? B::handL : B::handR]->mass;
    att.enabled = true;
    att.stiffness = hm * 2500.0 * grip * std::min(1.0, tone);
    att.damping = hm * 60.0 * std::min(1.0, tone);
    att.max_force = 30.0 + 420.0 * grip * std::min(1.0, tone);
    turn.enabled = true;
    turn.stiffness = 30.0 * grip * control;
    turn.max_torque = 12.0 * grip * control;
    turn.damping = 1.5 * grip * control;
    if (support_grip) {
      att.stiffness *= 24;
      att.damping *= 3;
      att.max_force *= 8;
      turn.stiffness *= 2;
      turn.max_torque *= 2;
      turn.damping *= 2;
    }
  }

  // writhing on the back: the pain rolls the body from side to side about its length
  if (mode == BodyMode::Lying && writhing && alive && conscious && plan.lying_on_back()) {
    const f64 t = time_ + writhe_seed_;
    const f64 spasm = pow(0.5 + 0.5 * sin(t * 0.9) * sin(t * 0.31 + 1.0), 1.5);
    RigidBody& pb = *bd.parts[B::pelvis];
    RigidBody& ch = *bd.parts[B::chest];
    const V3 ax = vnorm(ch.x - pb.x);
    const f64 roll = 55.0 * spasm * sin(t * 0.75 + 0.6);
    for (RigidBody* b : {&pb, &ch}) b->torque = b->torque + ax * roll;
  }
}

void Behaviours::drive_post(f64 dt, WorldPose& out) {
  HumanoidBody& bd = body;
  bd.write_pose(out);
  // bumped into (by) someone, something: the balance has to answer it (a brush in passing is
  // nothing; being barged moves the body)
  f64 bump = 0.0;
  for (const RigidBody* p : bd.parts) bump += p->bumped;
  const f64 bdv = bump / bd.total_mass;
  if (bdv > 0.12 && alive && (mode == BodyMode::Animated || mode == BodyMode::Reacting)) {
    upset_ = std::max(upset_, std::min(0.5, 2.5 * bdv));
    if (bdv > 0.22) force_react_ = true;
  }
  detect_trips(out, dt);
  // (a body gone limp is heavy: no part of it whirls about on an impact)
  // Soft tissue dissipates a trunk landing; inert limbs should not spring the body upright.
  if ((mode == BodyMode::Dying || mode == BodyMode::Dead) && (bd.parts[B::pelvis]->contact || bd.parts[B::chest]->contact)) {
    bd.system.linear_drag = .002;
    bd.system.angular_drag = .004;
  }
  // the dead settle (a body at rest does not keep rocking on its contacts) and sleep
  if (mode == BodyMode::Dead) {
    const bool slow = bd.system.last_speed < 0.25 || mode_time > 2.5 || bd.parts[B::pelvis]->contact || bd.parts[B::chest]->contact;
    bd.system.linear_drag = slow ? 0.05 : 0.98;
    bd.system.angular_drag = slow ? 0.02 : 0.9;
    // A corpse landing on its trunk has no active rebound. Dissipate the normal
    // component across the connected mass, including limbs transferring a landing impulse.
    if (bd.parts[B::pelvis]->contact || bd.parts[B::chest]->contact) {
      V3 normal = bd.parts[B::chest]->contact ? bd.parts[B::chest]->contact_normal : bd.parts[B::pelvis]->contact_normal;
      normal = vnorm(normal, V3{0, 0, 1});
      V3 momentum;
      f64 mass = 0;
      for (const auto* p : bd.parts)
        if (!p->gone) {
          momentum += p->v * p->mass;
          mass += p->mass;
        }
      const f64 rebound = dot(momentum, normal) / std::max(1.0, mass);
      if (rebound > .05)
        for (auto* p : bd.parts)
          if (!p->gone) p->v -= normal * (rebound * .9);
    }
    // (once down, the limbs lie where they lie: keeping them out of the trunk would only squeeze a
    // trapped arm out from under the body)
    bd.system.pairs_enabled =
        bd.system.pairs_enabled && !(bd.parts[B::pelvis]->contact || bd.parts[B::chest]->contact || (mode_time > 1.2 && bd.system.last_speed < 0.6));
    if (mode_time > 1.0) bd.system.try_sleep(0.5);
  }
}

// The ground's push through the feet (reacting): the centre of pressure is kept under the feet;
// the force m w0^2 (c - p) brakes the body when the capture point is inside the support and lets
// it topple when it is not.
void Behaviours::centre_of_pressure(f64 legs) {
  const f64 kk = k();
  const f64 m = body.total_mass;
  const f64 h = std::max(0.45 * kk, com.z - ground_z);
  const f64 w2 = G / h;
  const f64 w0 = std::sqrt(w2);
  if (support.empty()) return;
  f64 cen[2] = {0, 0};
  support.centroid(cen);
  // aim the centre of pressure so the capture point comes back to the middle of the feet
  const f64 gain = 2.2 * legs;
  f64 p[2] = {capture.x + ((capture.x - cen[0]) * gain) / w0, capture.y + ((capture.y - cen[1]) * gain) / w0};
  const f64 want[2] = {p[0], p[1]};
  const f64 out = support.distance(p[0], p[1], p);
  // the hip strategy (a swing of the trunk, the arms) puts the effective centre of pressure a
  // little past the feet's edge
  if (out > 0.0) {
    const f64 ext = std::min(out, 0.07 * kk * legs);
    const f64 dx = want[0] - p[0], dy = want[1] - p[1];
    f64 dl = hypot2(dx, dy);
    if (dl == 0.0) dl = 1.0;
    p[0] += (dx / dl) * ext;
    p[1] += (dy / dl) * ext;
  }
  f64 fx = m * w2 * (com.x - p[0]);
  f64 fy = m * w2 * (com.y - p[1]);
  // friction and leg strength bound it
  const f64 max = m * G * 0.9 * clamp(legs, 0.3, 1.0);
  const f64 f = hypot2(fx, fy);
  if (f > max) {
    fx *= max / f;
    fy *= max / f;
  }
  RigidBody& pel = *body.parts[B::pelvis];
  pel.force.x += fx;
  pel.force.y += fy;
}

// A swinging foot that hits something on its way stops there: a trip.
void Behaviours::detect_trips(const WorldPose& pose, f64 dt) {
  if (mode != BodyMode::Animated && mode != BodyMode::Reacting) return;
  auto& feet = plan.feet_planner.feet;
  const f64 kk = k();
  // (a hurried body trips at a touch; one picking its way lifts the foot over and goes on)
  const f64 hurry = clamp((hypot2(plan.velocity.x, plan.velocity.y) - 1.2) / 2.3, 0.0, 1.0);
  for (i32 i = 0; i < 2; ++i) {
    auto& f = feet[size_t(i)];
    // (still rolling off its toes, a foot against a riser is not caught: it lifts)
    if (f.planted || f.held || f.swing < 0.2 || f.swing > 0.85) {
      blocked_for_[size_t(i)] = 0.0;
      continue;
    }
    const RigidBody& fb = *body.parts[i == 0 ? B::footL : B::footR];
    const V3 planned = plan.world.p[size_t(i == 0 ? H::footL : H::footR)];
    const V3 actual = pose.p[size_t(i == 0 ? H::footL : H::footR)];
    const f64 lag = hypot2(planned.x - actual.x, planned.y - actual.y);
    const f64 sx = f.target.x - f.lift.x, sy = f.target.y - f.lift.y;
    f64 sl = hypot2(sx, sy);
    if (sl == 0.0) sl = 1.0;
    const V3 n = fb.contact_normal;
    // (the foot, or the shin: a body lying in the way catches a runner at the knee)
    const RigidBody& sb = *body.parts[i == 0 ? B::shinL : B::shinR];
    const V3 sn = sb.contact_normal;
    const bool blocked = (fb.contact && (n.x * sx + n.y * sy) / sl < -0.35) || (sb.bumped > 1.5 && (sn.x * sx + sn.y * sy) / sl < -0.35);
    // (coming down onto something - a body, a lump of rubble - late in the swing: the foot lands
    // on it, a step sooner than meant, and the gait goes on)
    if (!blocked && fb.contact && n.z > 0.6 && f.swing > 0.5 && lag > 0.08 * kk) {
      plan.feet_planner.plant_now(i, sole_pos(i));
      blocked_for_[size_t(i)] = 0.0;
      continue;
    }
    // (blocked by something in the way; merely scuffing the ground is not a trip unless the foot
    // is hopelessly behind)
    if ((blocked && lag > 0.05 * kk) || (fb.contact && lag > 0.4 * kk)) {
      // the stumble reflex: the first touch lifts the foot higher; caught for longer than a careful
      // step allows (or hopelessly behind), it trips
      if (blocked_for_[size_t(i)] == 0.0) f.clear = std::max(0.0, f.clear) + 0.1 * kk * (1.0 - 0.7 * hurry);
      blocked_for_[size_t(i)] += dt;
      if (blocked_for_[size_t(i)] > lerp(0.16, 0.02, hurry) || lag > 0.5 * kk) {
        catch_foot(i);
        if (mode == BodyMode::Animated) set_mode(BodyMode::Reacting);
        return;
      }
    } else {
      blocked_for_[size_t(i)] = std::max(0.0, blocked_for_[size_t(i)] - dt);
    }
  }
}

}  // namespace svx::anim
