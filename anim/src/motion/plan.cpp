#include "svx/anim/motion/plan.hpp"

#include <algorithm>

#include "svx/anim/rig.hpp"

namespace svx::anim {

void PlanControl::reset() {
  arms[0].reset();
  arms[1].reset();
  spine = V3{};
  chest = V3{};
  neck = V3{};
  head = V3{};
  fold = 0.0;
  crouch = 0.0;
  shrug = 0.0;
  look.reset();
  look_weight = 0.0;
  pelvis_pos.reset();
  pelvis_weight = 0.0;
  pelvis_height = false;
  hold_feet = false;
  relax_legs = 0.0;
  care = 1.0;
  busy = false;
}

namespace {

// Channels by side (0 left, 1 right).
constexpr Channel kHandCh[2] = {Channel::HandL, Channel::HandR};
constexpr Channel kHandW[2] = {Channel::HandLw, Channel::HandRw};
constexpr Channel kHandRot[2] = {Channel::HandLrot, Channel::HandRrot};
constexpr Channel kElbowCh[2] = {Channel::ElbowL, Channel::ElbowR};
constexpr Channel kStrikeCh[2] = {Channel::StrikeL, Channel::StrikeR};
constexpr Channel kFootCh[2] = {Channel::FootL, Channel::FootR};
constexpr Channel kFootW[2] = {Channel::FootLw, Channel::FootRw};
constexpr Channel kStrikeFoot[2] = {Channel::StrikeFootL, Channel::StrikeFootR};

// A number as a 32-bit integer, as JavaScript's bit operations take it (value noise seeds).
i32 to_i32(f64 x) { return static_cast<i32>(Rng::to_u32(x)); }

FeetDims feet_dims(const Skeleton& sk, f64 k, f64 leg_len) {
  const std::vector<V3>& rh = sk.rest_head;
  FeetDims d;
  d.k = k;
  d.leg_len = leg_len;
  d.ankle_h = rh[H::footL].z;
  d.ball_fwd = rh[H::toeL].y - rh[H::footL].y;
  d.heel_back = 0.06 * k;
  d.foot_x = std::abs(rh[H::footL].x);
  return d;
}

}  // namespace

MotionPlan::MotionPlan(SkeletonPtr skeleton_, const CollisionWorld* collision_, f64 seed)
    : skeleton(std::move(skeleton_)),
      rest_pelvis_z(skeleton->rest_head[H::pelvis].z),
      k(rest_pelvis_z / 0.97),
      leg_len(vdist(skeleton->rest_head[H::thighL], skeleton->rest_head[H::shinL]) + vdist(skeleton->rest_head[H::shinL], skeleton->rest_head[H::footL])),
      pose(skeleton),
      fk(skeleton),
      world(skeleton),
      prev_world(skeleton),
      collision(collision_),
      gait(gait_for(0.0, 0.0)),
      feet_planner(feet_dims(*skeleton, k, leg_len), collision_,
                   {FootBones{H::thighL, H::shinL, H::footL, H::toeL}, FootBones{H::thighR, H::shinR, H::footR, H::toeR}}),
      arms(skeleton, &pose, &fk),
      seed_(seed),
      rng_(seed * 31.0 + 7.0) {
  const std::vector<V3>& rh = skeleton->rest_head;
  dims_ = Dims{k, rh[H::footL].z, std::abs(rh[H::footL].x)};
}

void MotionPlan::place(const V3& pos_in, f64 yaw) {
  const V3 pos = pos_in;
  root_pos = pos;
  last_pos_ = pos;
  root_yaw = yaw;
  last_yaw_ = yaw;
  vis_z_.x = pos.z;
  vis_z_.v = 0.0;
  vel_spring_.reset();
  velocity = V3{};
  feet_planner.collision = collision;
  feet_planner.reset(root_pos, yaw, crouch_s_.x, style);
  pelvis_z_.x = rest_pelvis_z;
  placed_ = true;
  update(0.0);
  prev_world.copy_from(world);
}

void MotionPlan::set_root(const V3& pos, f64 yaw) {
  if (!placed_ || vdist(pos, root_pos) > 2.0) {
    place(pos, yaw);
    return;
  }
  root_pos = pos;
  root_yaw = yaw;
}

void MotionPlan::carry_root(const V3& pos_in, f64 yaw) {
  const V3 pos = pos_in;
  const f64 dx = pos.x - root_pos.x, dy = pos.y - root_pos.y;
  root_pos = pos;
  last_pos_.x += dx;
  last_pos_.y += dy;
  last_pos_.z = pos.z;
  last_yaw_ += wrap_angle(yaw - root_yaw);
  root_yaw = yaw;
  if (std::abs(pos.z - vis_z_.x) > 0.3 * k) vis_z_.x = pos.z;
}

bool MotionPlan::play(std::string_view name, std::optional<V3> target, f64 rate) {
  const ActionDef* def = action_def(name);
  if (!def) return false;
  if (stance == Stance::Down || lying_ || stance_p_ < 1.0) return false;
  if (def->layer == ActionLayer::Pose) {
    set_pose_action(def);
    return true;
  }
  if (act_) act_->stop();
  act_.emplace(def, target, rate);
  if (target && def->reach != 0.0) step_in(*def, *target, rate);
  return true;
}

// A punch at a target out of reach steps into it: the lead foot goes forward with the lunge (the
// hips drive over it, not out past the feet).
void MotionPlan::step_in(const ActionDef& def, const V3& target, f64 rate) {
  if (def.drives(Channel::StrikeFootR) || def.drives(Channel::StrikeFootL) || stance != Stance::Stand || hypot2(velocity.x, velocity.y) > 0.3) return;
  const f64 dx = target.x - root_pos.x, dy = target.y - root_pos.y;
  const f64 h = hypot2(dx, dy);
  const f64 need = clamp(h - def.reach * k, 0.0, 0.36 * k) * 0.6;
  if (need < 0.05 * k || h < 1e-3) return;
  f64 strike_at = 0.2;
  for (const ActionEvent& e : def.events) {
    if (e.name != "strike") continue;
    strike_at = e.t;
    break;
  }
  // the lead foot: the one nearer the target
  const std::array<Foot, 2>& f = feet_planner.feet;
  const f64 d0 = hypot2(f[0].pos.x - target.x, f[0].pos.y - target.y);
  const f64 d1 = hypot2(f[1].pos.x - target.x, f[1].pos.y - target.y);
  const i32 i = d0 <= d1 ? 0 : 1;
  const V3 p = f[size_t(i)].pos;
  feet_planner.step(i, V3{p.x + (dx / h) * need * 1.15, p.y + (dy / h) * need * 1.15, p.z}, std::max(0.12, strike_at / rate));
}

void MotionPlan::interrupt(bool hard) {
  if (hard && act_ && !act_->def->name.starts_with("block")) act_->stop();
  if (pose_act_ && pose_act_->def->name != "guard" && pose_act_->def->name != "knifeGuard") pose_act_->stop();
  idle_time_ = 0.0;
  next_idle_pose_ = 4.0 + rng_.next() * 4.0;
}

void MotionPlan::aim_action(const V3& target) {
  if (act_) act_->target = target;
}

void MotionPlan::set_pose_action(const ActionDef* def) {
  if (pose_act_ && def && pose_act_->def == def && !pose_act_->done()) return;
  if (pose_act_) pose_act_->stop();
  if (def) pose_act_.emplace(def);
}

std::vector<AnimEvent> MotionPlan::take_events() {
  std::vector<AnimEvent> out;
  out.swap(events);
  return out;
}

void MotionPlan::take_events(std::vector<AnimEvent>& out) {
  for (AnimEvent& e : events) out.push_back(std::move(e));
  events.clear();
}

void MotionPlan::fire(f64 strength) {
  if (!weapon) return;
  hold.fire(*weapon, strength);
  recoil_.kick((weapon->kind == PropKind::Pistol ? 0.15 : 0.35) * strength);
}

void MotionPlan::lie(const V3& root, f64 yaw, bool back) {
  stance_queue_.clear();
  if (act_) act_->stop();
  if (pose_act_) pose_act_->stop();
  down_back_ = back;
  stance = Stance::Down;
  stance_to_ = Stance::Down;
  stance_p_ = 1.0;
  lying_ = true;
  get_up_run_ = false;
  carry_root(root, yaw);
  vel_spring_.reset();
  velocity = V3{};
}

void MotionPlan::fall(bool back) {
  stance_queue_.clear();
  if (act_) act_->stop();
  down_back_ = back;
  lying_ = true;
  get_up_run_ = false;
  begin_transition(Stance::Down);
}

void MotionPlan::get_up(f64 rate) {
  if (!lying_) return;
  lying_ = false;
  get_up_run_ = true;
  get_up_rate_ = clamp(rate, 0.3, 2.0);
}

// ---- stances --------------------------------------------------------------------------------

void MotionPlan::begin_transition(Stance to) {
  // (a transition interrupted by another starts from where the first was going)
  const Stance from = stance_p_ >= 1.0 ? stance : stance_to_;
  stance = from;
  stance_to_ = to;
  stance_p_ = 0.0;
  stance_dur_ = transition_time(from, to) / (get_up_run_ ? get_up_rate_ : 1.0);
}

void MotionPlan::update_stance(f64 dt) {
  if (stance_p_ < 1.0) {
    stance_p_ = std::min(1.0, stance_p_ + dt / stance_dur_);
    if (stance_p_ >= 1.0) {
      stance = stance_to_;
      if (!stance_queue_.empty()) {
        const Stance next = stance_queue_.front();
        stance_queue_.erase(stance_queue_.begin());
        begin_transition(next);
      } else {
        get_up_run_ = false;
      }
    }
    return;
  }
  if (lying_) return;
  Stance want = input.stance;
  if (want == Stance::Sit && !input.seat) want = Stance::Stand;
  if (want == Stance::Down) want = Stance::Stand;
  if (want != stance) {
    // up from lying: sit up (on the back) or push up (face down), then kneel
    std::vector<Stance> route;
    if (stance == Stance::Down) {
      const Stance up = down_back_ ? Stance::Ground : Stance::Prone;
      route.push_back(up);
      for (const Stance s : stance_route(up, want)) route.push_back(s);
    } else {
      route = stance_route(stance, want);
    }
    if (!route.empty()) {
      stance_queue_.insert(stance_queue_.end(), route.begin() + 1, route.end());
      begin_transition(route.front());
    }
  }
}

f64 MotionPlan::weight_of(Stance s) const {
  // (falling accelerates; the rest ease in and out)
  const f64 p = stance_p_;
  const f64 e = stance_to_ == Stance::Down ? p * p : p * p * (3.0 - 2.0 * p);
  if (p >= 1.0) return s == stance ? 1.0 : 0.0;
  return (s == stance_to_ ? e : 0.0) + (s == stance ? 1.0 - e : 0.0);
}

StanceSample& MotionPlan::sample_stance(Stance s, StanceSample& out, const StanceSample& standing) {
  const Dims& d = dims_;
  switch (s) {
    case Stance::Stand:
      return blend_samples(standing, standing, 0.0, out);
    case Stance::Kneel:
      return kneel_sample(d, out);
    case Stance::Prone: {
      const f64 sp = hypot2(velocity.x, velocity.y);
      const f64 crawl = smoothstep(0.05, 0.3, sp);
      return prone_sample(d, crawl, crawl_phase_, out);
    }
    case Stance::Sit: {
      const std::optional<SeatInfo>& seat = input.seat;
      SeatModel m;
      m.pos = seat ? to_model(seat->pos) : V3{0, -0.38 * k, 0.46 * k};
      const std::optional<f64> desk = seat ? seat->desk_height : std::nullopt;
      m.variant = seat && seat->variant ? *seat->variant : desk ? SitVariant::Desk : SitVariant::Upright;
      m.backrest = seat ? seat->backrest : false;
      if (desk) m.desk = *desk + (vis_z_.x - root_pos.z);
      return sit_sample(d, m, time + seed_, out);
    }
    case Stance::Ground:
      // (getting up from the back: sitting up with the knees drawn in)
      return ground_sample(d, get_up_run_ ? GroundVariant::KneesUp : input.ground_variant, time + seed_, out);
    case Stance::Down:
      return down_sample(d, down_back_, time, out);
  }
  return out;
}

// ---- update ---------------------------------------------------------------------------------

void MotionPlan::update(f64 dt_in) {
  const f64 dt = std::min(0.05, std::max(0.0, dt_in));
  prev_world.copy_from(world);
  time += dt;
  const MotionInput& inp = input;
  const PlanControl& ctl = control;
  const GaitStyle& st = style;

  // ---- motion estimate ----------------------------------------------------------------------
  if (dt > 0.0) {
    V3 raw = (root_pos - last_pos_) * (1.0 / dt);
    raw.z = 0.0;
    const V3 before = vel_spring_.x;
    vel_spring_.omega = lerp(11.0, 6.5, st.heavy);
    vel_spring_.update(raw, dt);
    const V3 a = (vel_spring_.x - before) * (1.0 / dt);
    for (int i = 0; i < 3; ++i) accel_[i] = lerp(accel_[i], a[i], 1.0 - exp(-dt * 6.0));
    const f64 yr = wrap_angle(root_yaw - last_yaw_) / dt;
    yaw_rate_ = lerp(yaw_rate_, yr, 1.0 - exp(-dt * 8.0));
  }
  velocity = vel_spring_.x;
  last_pos_ = root_pos;
  last_yaw_ = root_yaw;
  if (std::abs(root_pos.z - vis_z_.x) > 0.6 * k) vis_z_.x = root_pos.z;
  vis_z_.update(root_pos.z, dt);

  // ---- stance, actions ----------------------------------------------------------------------
  update_stance(dt);
  update_actions(dt);
  const ChannelFrame* ch_p = pose_act_ && !pose_act_->done() ? &ch_pose_ : nullptr;
  const ChannelFrame* ch_a = act_ && !act_->done() ? &ch_act_ : nullptr;
  const f64 w_p = ch_p ? pose_act_->weight() : 0.0;
  const f64 w_a = ch_a ? act_->weight() : 0.0;
  auto add = [&](Channel c, int i) { return (ch_p ? ch_p->at(c, i) : 0.0) * w_p + (ch_a ? ch_a->at(c, i) : 0.0) * w_a; };
  // the limbs the actions drive, and those that strike
  for (int i = 0; i < 4; ++i) {
    const size_t si = size_t(i % 2);
    const bool hand = i < 2;
    auto w = [&](const ChannelFrame* ch, f64 wt) {
      if (!ch) return 0.0;
      if (hand) return ch->has(kHandCh[si]) ? (ch->has(kHandW[si]) ? ch->at(kHandW[si]) : 1.0) * wt : 0.0;
      return ch->at(kFootW[si]) * wt;
    };
    effort[size_t(i)] = clamp(std::max(w(ch_p, w_p), w(ch_a, w_a)), 0.0, 1.0);
    const bool sc = ch_a && ch_a->has(hand ? kStrikeCh[si] : kStrikeFoot[si]);
    striking[size_t(i)] = sc && w_a > 0.1;
  }

  const V3 vel = velocity;
  const f64 speed = hypot2(vel.x, vel.y);
  // the ground ahead: a climb (stairs, a ramp) is leaned into on bent knees
  {
    f64 want = 0.0;
    if (speed > 0.3 && !inp.airborne) {
      const f64 run = 0.7 * k;
      const std::optional<f64> h0 = collision->ground_height(root_pos.x, root_pos.y, root_pos.z + 0.4 * k, root_pos.z - 0.6 * k);
      const f64 g0 = h0 ? *h0 : root_pos.z;
      const f64 ax = root_pos.x + (vel.x / speed) * run, ay = root_pos.y + (vel.y / speed) * run;
      const std::optional<f64> g1 = collision->ground_height(ax, ay, g0 + 0.6 * k, g0 - 0.6 * k);
      if (g1) want = clamp((*g1 - g0) / run, -0.6, 0.6);
    }
    slope_s_.update(want, dt);
  }
  const f64 climb = clamp(slope_s_.x, -0.6, 0.6);
  const f64 crouch_in = inp.mood == Mood::Cower ? 1.0 : inp.crouch;
  crouch_s_.update(clamp(crouch_in + add(Channel::Crouch, 0) + ctl.crouch + std::max(0.0, climb) * 0.35, 0.0, 1.0), dt);
  const f64 crouch = clamp(crouch_s_.x, 0.0, 1.0);
  const Quat root_q = root_rot();
  const Quat inv = conj(root_q);
  inv_ = inv;
  const V3 v_local = rotate(inv, vel);
  const V3 a_local = rotate(inv, accel_);
  const bool armed = weapon && weapon->kind != PropKind::Knife;
  const bool aiming = armed && (inp.carry == Carry::Aim || inp.carry == Carry::Hip) && inp.aim_at.has_value();
  const f64 stand_w = weight_of(Stance::Stand);

  // the lower body's heading relative to the facing: bladed when shouldering a rifle, towards
  // the motion when strafing
  f64 lower = 0.0;
  if (speed > 0.3) {
    const f64 th = atan2(-v_local.x, v_local.y);
    if (std::abs(th) < 1.75) lower = clamp(th, -1.0, 1.0) * 0.65;
    else lower = clamp(wrap_angle(th - kPi), -1.0, 1.0) * 0.65;
  } else if (aiming && weapon->kind != PropKind::Pistol) {
    lower = -0.42;
  }
  lower_yaw_.update(lower, dt);

  // ---- gait ---------------------------------------------------------------------------------
  const f64 aim_move = aim_w_.x;
  GaitParams g = gait_for(speed, crouch, k);
  // personal style, the tactical walk while aiming, a limp, pain
  const f64 tactical = clamp(aim_move, 0.0, 1.0) * (1.0 - g.run);
  const f64 limp_l = ctl.limp[0], limp_r = ctl.limp[1];
  const f64 pain = ctl.pain;
  g.freq = g.freq / pow(st.stride * (1.0 - 0.15 * tactical), 0.7);
  g.bob = g.bob * st.bounce * (1.0 - 0.7 * tactical);
  g.sway = g.sway * st.sway * (1.0 - 0.4 * tactical);
  g.hip_yaw = g.hip_yaw * st.sway * (1.0 - 0.5 * tactical);
  g.hip_roll = g.hip_roll * st.sway * (1.0 - 0.3 * tactical);
  g.arm_swing = g.arm_swing * st.arms * (1.0 - 0.35 * pain);
  g.elbow = g.elbow + st.elbow;
  g.sink = g.sink + 0.055 * k * tactical + 0.02 * k * pain;
  g.lean = g.lean * (1.0 + 0.4 * st.heavy) + 0.05 * tactical;
  gait = g;
  FootPlanner& feet = feet_planner;
  feet.collision = collision;
  const bool moving = speed > 0.12 && !inp.airborne && stand_w > 0.99 && !ctl.hold_feet;
  const f64 body_yaw = root_yaw + lower_yaw_.x;
  f64 prev_phase = feet.phase;
  FeetContext fctx;
  fctx.root = root_pos;
  fctx.body_yaw = body_yaw;
  fctx.vel = vel;
  fctx.speed = speed;
  fctx.gait = g;
  fctx.moving = moving;
  fctx.airborne = inp.airborne;
  fctx.crouch = crouch;
  fctx.style = st;
  fctx.hips = {world.p[H::thighL], world.p[H::thighR]};
  fctx.care = ctl.care;
  fctx.ground_z = root_pos.z;
  fctx.hold = ctl.hold_feet;
  if (stand_w < 0.999) {
    // another stance: the feet wait at the standing stance's spots
    feet.reset(root_pos, body_yaw, crouch, st);
    feet.stepping = false;
  } else {
    if (!ctl.hold_feet || feet.feet[0].forced || feet.feet[1].forced) prev_phase = feet.advance_clock(dt, fctx, ctl.limp);
    feet.update(dt, fctx, prev_phase);
    // footfall: the body settles onto the leg (heavier characters and faster gaits more), the
    // head nods with it unless it is held still
    for (size_t i = 0; i < 2; ++i) {
      if (!feet.landed[i]) continue;
      const f64 sp = moving ? std::min(1.4, speed / 2.5) : 0.3;
      impact_.kick(-(0.18 + 0.4 * st.heavy) * sp);
      nod_.kick(-0.5 * sp * (1.0 - st.head_still));
    }
  }

  // the crawling clock (prone)
  crawl_phase_ = fract(crawl_phase_ + dt * std::min(1.2, speed / 0.35));

  // ---- the standing sample ------------------------------------------------------------------
  origin_ = V3{root_pos.x, root_pos.y, vis_z_.x};
  const V3 origin = origin_;
  const f64 ph = feet.phase;
  const f64 D = g.duty;
  const f64 run = g.run;
  const f64 move_amt = smoothstep(0.1, 0.9, speed);
  const f64 idle = 1.0 - move_amt;
  const f64 walk_bob = moving ? g.bob * (1.0 - 2.0 * run) * cos(kTau * 2.0 * (ph - D / 2.0)) : 0.0;
  const f64 sway_x = moving ? -g.sway * cos(kTau * (ph - D / 2.0)) : 0.0;
  const f64 hip_yaw_osc = moving ? -g.hip_yaw * cos(kTau * ph) : 0.0;
  f64 hip_roll = moving ? -g.hip_roll * cos(kTau * (ph - (1.0 + D) / 2.0)) : 0.0;
  mood_kind_ = inp.mood;
  mood_w_.update(inp.mood == Mood::Normal ? 0.0 : 1.0, dt);
  air_w_.update(inp.airborne ? 1.0 : 0.0, dt);
  const f64 breathe = sin(time * (1.9 + 1.2 * pain) + seed_);
  // idle weight shift (contrapposto): the hips settle over one leg, the free knee bends
  const f64 shift_target =
      idle > 0.5 && !aiming && !inp.guard ? (std::fmod(std::floor((time + seed_ * 3.7) / (5.0 + 4.0 * (1.0 - st.fidget))), 2.0) == 0.0 ? 1.0 : -1.0) : 0.0;
  // (a hurt leg carries no weight: the hips settle over the good one)
  const f64 favour = limp_l - limp_r;
  shift_.update(std::abs(favour) > 0.15 ? sign(favour) : shift_target, dt);
  const f64 shift_x = shift_.x * 0.035 * k * std::max(idle, std::min(1.0, std::abs(favour) * 2.0));
  hip_roll += shift_.x * 0.07 * idle;
  const f64 crouch_drop = crouch * 0.4 * k;
  // limp: the pelvis dips over the wounded leg while it bears weight
  const f64 limp_dip = moving ? (fract(ph) < D ? limp_l : 0.0) * 0.05 * k + (fract(ph + 0.5) < D ? limp_r : 0.0) * 0.05 * k : 0.0;
  f64 pz = rest_pelvis_z - g.sink - crouch_drop + walk_bob - 0.012 * k * idle - limp_dip;
  // the hips: standing, they stay with the planted feet (the trunk and the head turn first, the
  // feet follow with steps); walking, they turn towards the motion
  f64 hips_want = lower_yaw_.x;
  const bool on_feet = !feet.feet[0].held && !feet.feet[1].held;
  if (!moving && on_feet && !inp.airborne && stand_w > 0.99) {
    f64 sx = 0.0, cy = 0.0;
    for (const Foot& f : feet.feet) {
      const f64 rel = f.yaw + f.side * st.toe_out - root_yaw;
      sx += sin(rel);
      cy += cos(rel);
    }
    hips_want = lerp(clamp(atan2(sx, cy), -1.2, 1.2), lower_yaw_.x, 0.3);
  }
  hips_yaw_.update(hips_want, dt);
  const f64 pelvis_yaw = hips_yaw_.x + hip_yaw_osc;
  // banking into the turns of the path (its lateral acceleration, not the body's yaw) and a
  // spring-loaded lean into (de)acceleration
  bank_.update(clamp(a_local.x * 0.025, -0.12, 0.12) * move_amt, dt);
  trunk_lean_.omega = lerp(7.5, 5.0, st.heavy);
  trunk_lean_.update(clamp(a_local.y * (0.03 + 0.02 * st.heavy), -0.2, 0.24), dt);
  const Quat pelvis_rot = qz(pelvis_yaw) * qeuler(-(0.1 * crouch + 0.05 * g.lean), hip_roll + bank_.x * 0.6, 0.0);
  const f64 px = sway_x + shift_x;
  const f64 py = -0.07 * crouch * k;
  const f64 lowest = rest_pelvis_z - 0.2 * k - crouch_drop;
  for (const Foot& f : feet.feet) {
    const V3 a = to_model(f.ankle);
    const V3 hip_off = rotate(pelvis_rot, skeleton->rest_head[size_t(f.thigh)] - skeleton->rest_head[H::pelvis]);
    const f64 dx = px + hip_off.x - a.x;
    const f64 dy = py + hip_off.y - a.y;
    const f64 reach = 0.995 * leg_len;
    const f64 reach2 = reach * reach - dx * dx - dy * dy;
    f64 max_z = (reach2 > 0.0 ? std::sqrt(reach2) : 0.0) + a.z - hip_off.z;
    if (!f.planted) max_z = lerp(pz, max_z, smoothstep(0.45, 0.9, f.swing));
    if (f.held) max_z = pz;
    if (pz > max_z) pz = std::max(max_z, lowest);
  }
  if (inp.airborne) pz = rest_pelvis_z - 0.04 * k;
  pelvis_z_.update(pz, dt);
  impact_.update(0.0, dt);
  nod_.update(0.0, dt);
  recoil_.update(0.0, dt);
  const f64 pz_s = std::min(pelvis_z_.x, pz + 0.01 * k);
  StanceSample& s0 = s_a_;
  s0.pelvis_pos = V3{px, py, pz_s};
  s0.pelvis_rot = pelvis_rot;
  const f64 lean = g.lean + trunk_lean_.x + crouch * 0.38 + climb * 0.35 + (mood_kind_ == Mood::Panic ? mood_w_.x * 0.12 : 0.0) +
                   (mood_kind_ == Mood::Cower ? mood_w_.x * 0.25 : 0.0);
  // posture: slouched (chest and neck forward) .. upright (chest up); pain hunches
  const f64 posture = st.posture * 0.07 - pain * 0.12;
  const f64 counter = -hip_yaw_osc * 1.6 * (1.0 - 0.8 * tactical);
  s0.spine = qeuler(-lean * 0.5 + (0.1 * crouch + 0.05 * g.lean) + posture, -hip_roll * 0.6 - bank_.x * 0.3, counter * 0.4);
  s0.chest = qeuler(-lean * 0.45 + 0.02 * breathe * (1.0 - 0.5 * move_amt) + posture * 0.8, -hip_roll * 0.4 - bank_.x * 0.2, counter * 0.6);
  s0.neck = qx(posture * 0.6);
  s0.head = qx(0.0);
  for (size_t i = 0; i < 2; ++i) {
    const Foot& f = feet.feet[i];
    FootPose& fp = s0.feet[i];
    fp.ankle = to_model(f.ankle);
    const f64 fy = wrap_angle(f.yaw - root_yaw);
    const V3 foot_fwd{-sin(fy), cos(fy), 0};
    fp.pole = vnorm(foot_fwd + V3{f.side * (0.15 + 0.22 * crouch), 0, 0.1});
    fp.rot = qz(fy) * qx(f.pitch);
    fp.toe = f.planted && f.pitch < 0.0 ? -f.pitch * 0.9 : air_w_.x * 0.2;
  }
  s0.hands = {std::nullopt, std::nullopt};
  s0.turn = 1.0;

  // ---- blend with the other stance ------------------------------------------------------------
  StanceSample* S = &s0;
  if (stand_w < 0.999) {
    std::optional<Stance> other;
    if (stance_p_ >= 1.0) other = stance;
    else if (stance == Stance::Stand) other = stance_to_;
    else if (stance_to_ == Stance::Stand) other = stance;
    if (other && *other != Stance::Stand) {
      const StanceSample& so = sample_stance(*other, s_b_, s0);
      S = &blend_samples(s0, so, 1.0 - stand_w, s_out_);
    } else {
      const StanceSample& a = sample_stance(stance, s_b_, s0);
      const StanceSample& b = sample_stance(stance_to_, s_c_, s0);
      S = &blend_samples(a, b, weight_of(stance_to_), s_out_);
    }
    // people lean forward while getting down or up
    if (stance_p_ < 1.0 && (stance_to_ == Stance::Sit || stance == Stance::Sit || stance_to_ == Stance::Kneel || stance == Stance::Kneel)) {
      const f64 bend = sin(kPi * stance_p_) * 0.35;
      S->spine = qx(-bend) * S->spine;
      S->chest = qx(-bend * 0.6) * S->chest;
    }
  }

  // ---- pose: pelvis and trunk -----------------------------------------------------------------
  pose.reset();
  const f64 impact_z = impact_.x;
  // a strike at a target out of reach steps into it: the pelvis drives forward with the blow
  f64 lunge = 0.0;
  f64 lunge_lean = 0.0;
  V3 lunge_dir;
  if (ch_a && act_->target && act_->def->reach != 0.0) {
    const V3 tm = to_model(*act_->target);
    const f64 h = hypot2(tm.x, tm.y);
    if (h > 1e-3) {
      const bool kick = ch_a->has(Channel::StrikeFootR) || ch_a->has(Channel::StrikeFootL);
      const f64 s = clamp(std::max(std::max(ch_a->at(Channel::StrikeR), ch_a->at(Channel::StrikeL)), std::max(ch_a->at(Channel::StrikeFootR), ch_a->at(Channel::StrikeFootL))),
                          0.0, 1.0);
      const f64 need = clamp(h - act_->def->reach * k, 0.0, 0.36 * k) * s * w_a;
      // (a punch: the lead foot steps in with it and the shoulders lean, the hips follow less)
      lunge = need * (kick ? 1.0 : 0.55);
      lunge_lean = kick ? 0.0 : need * 1.7;
      lunge_dir.x = tm.x / h;
      lunge_dir.y = tm.y / h;
    }
  }
  pose.t[H::pelvis] = V3{S->pelvis_pos.x + add(Channel::Pelvis, 0) * k + lunge_dir.x * lunge, S->pelvis_pos.y + add(Channel::Pelvis, 1) * k + lunge_dir.y * lunge,
                         S->pelvis_pos.z + add(Channel::Pelvis, 2) * k + impact_z - lunge * 0.15};
  const V3 pr{add(Channel::PelvisRot, 0), add(Channel::PelvisRot, 1), add(Channel::PelvisRot, 2)};
  pose.r[H::pelvis] = S->pelvis_rot * qeuler(pr.x * kDeg, pr.y * kDeg, pr.z * kDeg);
  // the pelvis where the physics has it (the body reacting, falling, lying)
  if (ctl.pelvis_pos && ctl.pelvis_weight > 0.0) {
    const f64 w = clamp(ctl.pelvis_weight, 0.0, 1.0);
    V3 pm = to_model(*ctl.pelvis_pos);
    if (!ctl.pelvis_height) pm.z = pose.t[H::pelvis].z;
    pose.t[H::pelvis] = vlerp(pose.t[H::pelvis], pm, w);
    pose.r[H::pelvis] = qnlerp(pose.r[H::pelvis], inv * ctl.pelvis_rot, w);
  }

  // aim and look: the yaw and pitch of the target seen from the chest (model space)
  f64 want_yaw = 0.0;
  f64 want_pitch = 0.0;
  std::optional<V3> tgt;
  if (aiming) tgt = inp.aim_at;
  else if (inp.look_at) tgt = inp.look_at;
  else if (armed && inp.carry != Carry::Relaxed) tgt = inp.aim_at;
  const V3 chest_rest = skeleton->rest_head[H::chest];
  if (tgt) {
    const V3 m = to_model(*tgt);
    const V3 dd = m - V3{0, 0, chest_rest.z + 0.2 * k};
    want_yaw = clamp(atan2(-dd.x, dd.y), -1.9, 1.9);
    want_pitch = clamp(atan2(dd.z, hypot2(dd.x, dd.y)), -1.1, 1.1);
  }
  aim_yaw_.update(want_yaw, dt);
  aim_pitch_.update(want_pitch, dt);
  aim_w_.update(aiming ? 1.0 : 0.0, dt);
  // peeking round a corner is done standing (or crouched) still: walking, the body is upright
  lean_s_.update(clamp(inp.lean, -1.0, 1.0) * (1.0 - smoothstep(0.25, 0.7, speed)), dt);
  const f64 aw = aim_w_.x;
  const bool rifle = weapon && weapon->kind != PropKind::Pistol && weapon->kind != PropKind::Knife;
  // a long gun bladed: the trunk turns off the target (from the hip more, so the support hand
  // reaches the handguard)
  const f64 blade = aw * (rifle ? (inp.carry == Carry::Aim ? 0.3 : inp.carry == Carry::Hip ? 0.5 : 0.0) : 0.0);
  const f64 turn = S->turn;
  const f64 toward = (aiming || (tgt && inp.carry != Carry::Relaxed)) && tgt ? aim_yaw_.x - blade : 0.0;
  const f64 trunk_yaw = clamp(toward - hips_yaw_.x * stand_w, -0.95, 0.95) * turn;
  const f64 trunk_pitch = (tgt && (aiming || inp.carry != Carry::Relaxed) ? aim_pitch_.x * lerp(0.55, 0.75, aw) : 0.0) * turn;
  const f64 peek = lean_s_.x * 0.3;
  const f64 fold = ctl.fold + pain * 0.12;
  const V3 sp{add(Channel::Spine, 0) * kDeg + ctl.spine.x, add(Channel::Spine, 1) * kDeg + ctl.spine.y, add(Channel::Spine, 2) * kDeg + ctl.spine.z};
  const V3 ch{add(Channel::Chest, 0) * kDeg + ctl.chest.x - recoil_.x * 0.1, add(Channel::Chest, 1) * kDeg + ctl.chest.y,
              add(Channel::Chest, 2) * kDeg + ctl.chest.z};
  pose.r[H::spine] = qeuler(trunk_pitch * 0.3 - fold * 0.55 - lunge_lean * 0.5 * lunge_dir.y, peek * 0.55 + lunge_lean * 0.5 * lunge_dir.x, trunk_yaw * 0.35) *
                     S->spine * qeuler(sp.x, sp.y, sp.z);
  pose.r[H::chest] = qeuler(trunk_pitch * 0.45 - fold * 0.45 - lunge_lean * 0.5 * lunge_dir.y, peek * 0.45 + lunge_lean * 0.5 * lunge_dir.x, trunk_yaw * 0.45) *
                     S->chest * qeuler(ch.x, ch.y, ch.z);
  if (inp.lean != 0.0 || std::abs(lean_s_.x) > 0.01) pose.t[H::pelvis].x += lean_s_.x * 0.07 * k;
  fk.update(pose, 0);

  // ---- head ---------------------------------------------------------------------------------
  f64 look_w = 1.0;
  if (ch_a && ch_a->has(Channel::Look)) look_w = lerp(1.0, ch_a->at(Channel::Look), w_a);
  else if (ch_p && ch_p->has(Channel::Look)) look_w = lerp(1.0, ch_p->at(Channel::Look), w_p);
  look_w = clamp(look_w, 0.0, 1.0);
  f64 look_yaw = 0.0;
  f64 look_pitch = -0.06 - lean * 0.3;
  if (tgt) {
    look_yaw = aim_yaw_.x;
    look_pitch = aim_pitch_.x;
  } else if (idle > 0.5 && mood_kind_ == Mood::Normal && inp.talk == Talk::None) {
    look_yaw = (value_noise(time * 0.18, 3.1, seed_, 1.0, to_i32(seed_ + 9.0)) * 2.0 - 1.0) * 1.0;
    look_pitch = (value_noise(time * 0.13, 7.7, seed_, 1.0, to_i32(seed_ + 3.0)) * 2.0 - 1.0) * 0.25;
  } else if (mood_kind_ == Mood::Panic) {
    look_yaw = sin(time * 2.7 + seed_) * 0.7;
  }
  if (mood_kind_ == Mood::Cower) look_pitch = lerp(look_pitch, -0.7, mood_w_.x);
  // a behaviour's look (a wound, a threat) takes over
  look_w_.update(ctl.look ? clamp(ctl.look_weight, 0.0, 1.0) : 0.0, dt);
  if (ctl.look) {
    const V3 m = to_model(*ctl.look);
    const V3 dd = m - V3{0, 0, chest_rest.z + 0.3 * k};
    const f64 w = look_w_.x;
    look_yaw = lerp(look_yaw, clamp(atan2(-dd.x, dd.y), -1.9, 1.9), w);
    look_pitch = lerp(look_pitch, clamp(atan2(dd.z, hypot2(dd.x, dd.y)), -1.2, 1.1), w);
  }
  // saccades: the head turns fast to a new target, then settles
  const bool far = std::abs(look_yaw - head_yaw_.x) > 0.5;
  head_yaw_.omega = far ? 8.0 : 5.0;
  head_yaw_.update(look_yaw, dt);
  head_pitch_.update(look_pitch, dt);
  const Quat chest_q = fk.q[H::chest];
  const V3 chest_fwd = rotate(chest_q, V3{0, 1, 0});
  const f64 chest_yaw_now = atan2(-chest_fwd.x, chest_fwd.y);
  const f64 chest_pitch_now = asin(clamp(chest_fwd.z, -1.0, 1.0));
  const f64 rel_yaw = clamp(wrap_angle(head_yaw_.x - chest_yaw_now), -1.3, 1.3) * look_w;
  const f64 rel_pitch = clamp(head_pitch_.x - chest_pitch_now, -0.9, 0.8) * look_w;
  const V3 nk{add(Channel::Neck, 0) * kDeg + ctl.neck.x, add(Channel::Neck, 1) * kDeg + ctl.neck.y, add(Channel::Neck, 2) * kDeg + ctl.neck.z};
  const V3 hd{add(Channel::Head, 0) * kDeg + ctl.head.x + nod_.x * 0.12, add(Channel::Head, 1) * kDeg + ctl.head.y, add(Channel::Head, 2) * kDeg + ctl.head.z};
  pose.r[H::neck] = qeuler(rel_pitch * 0.4 + nod_.x * 0.06, -peek * 0.4, rel_yaw * 0.4) * S->neck * qeuler(nk.x, nk.y, nk.z);
  pose.r[H::head] = qeuler(rel_pitch * 0.6, -0.12 * aw * (rifle ? 1.0 : 0.0) - peek * 0.4, rel_yaw * 0.6) * S->head * qeuler(hd.x, hd.y, hd.z);
  fk.update(pose, H::neck);

  // ---- legs (last: every pelvis motion bends the knees, the feet stay) ------------------------
  for (size_t i = 0; i < 2; ++i) {
    Foot& f = feet.feet[i];
    const FootPose& fp = S->feet[i];
    V3 target = fp.ankle;
    Quat rot = fp.rot;
    V3 pole = fp.pole;
    const f64 fw = clamp((ch_a ? ch_a->at(kFootW[i]) : 0.0) * w_a, 0.0, 1.0);
    f.held = fw > 0.02;
    if (fw > 0.02 && ch_a) {
      const f64* fc = ch_a->get(kFootCh[i]);
      V3 a = fc ? V3{fc[0], fc[1], fc[2]} : target;
      a = V3{a.x * k, a.y * k, a.z * k};
      const f64 s = ch_a->at(kStrikeFoot[i]);
      // the foot's pitch in the air: pointed, or toes up for a push kick at the strike
      const f64 kick_pitch = act_ && act_->def->kick_pitch ? *act_->def->kick_pitch : -0.6;
      const Quat kick_rot = qx(lerp(-0.6, kick_pitch, clamp(s, 0.0, 1.0)));
      if (s > 0.0 && act_ && act_->target) {
        // the striking part of the foot (the toes' end) meets the target
        const V3 toe_off = rotate(kick_rot, skeleton->rest_tail[size_t(f.toe)] - skeleton->rest_head[size_t(f.foot)]);
        a = vlerp(a, to_model(*act_->target) - toe_off, clamp(s, 0.0, 1.2));
      }
      target = vlerp(target, a, fw);
      pole = vlerp(pole, V3{0.1 * f.side, 1, 0.4}, fw);
      rot = qnlerp(rot, kick_rot, fw);
    }
    solve_two_bone(pose, fk, f.thigh, f.shin, f.foot, target, pole, 0.02, &kKneeRest);
    set_model_rotation(pose, fk, f.foot, rot);
    pose.r[size_t(f.toe)] = qx(fp.toe);
    if (ctl.relax_legs > 0.0) {
      // legs that carry nothing (falling, thrown): loosely bent at hip and knee, whatever the
      // feet were doing
      const f64 w = clamp(ctl.relax_legs, 0.0, 1.0);
      const f64 sway = f.side * 0.12;
      pose.r[size_t(f.thigh)] = qnlerp(pose.r[size_t(f.thigh)], qeuler(0.55, sway, -sway), w);
      pose.r[size_t(f.shin)] = qnlerp(pose.r[size_t(f.shin)], qx(-1.0), w);
      pose.r[size_t(f.foot)] = qnlerp(pose.r[size_t(f.foot)], qx(0.35), w);
      fk.update_subtree(pose, f.thigh);
    }
    fk.update_bone(pose, f.toe);
  }

  // ---- arms ---------------------------------------------------------------------------------
  arm_swing(dt, g, moving, speed, breathe, idle);
  fk.update(pose, H::clavicleL);
  // stance hands (seated on the thighs, prone on the elbows...)
  rest_hands(*S, 1.0 - stand_w);
  // the support hand lets go of the weapon when a behaviour needs it
  task_w_[0].update(ctl.arms[0] ? clamp(ctl.arms[0]->weight, 0.0, 1.0) : 0.0, dt);
  task_w_[1].update(ctl.arms[1] ? clamp(ctl.arms[1]->weight, 0.0, 1.0) : 0.0, dt);
  const bool free_left = task_w_[0].x > 0.35;
  bool held_prop = false;
  if (weapon && weapon->kind != PropKind::Knife) {
    HoldContext hc;
    hc.carry = inp.carry;
    if (inp.aim_at) hc.aim_at = to_model(*inp.aim_at);
    hc.aiming = aiming;
    hc.moving = moving;
    hc.run = run;
    hc.phase = feet.phase;
    hc.aim_w = aw;
    hc.k = k;
    hc.ch_p = ch_p;
    hc.w_p = w_p;
    hc.ch_a = ch_a;
    hc.w_a = w_a;
    hc.free_left = free_left;
    held_prop = hold.hold(dt, arms, *weapon, hc);
  }
  if (!held_prop && mood_w_.x > 0.01 && mood_kind_ != Mood::Normal) mood_arms(mood_w_.x, feet.phase);
  // action hands: the posture layer, then one-shots on top
  if (ch_p && w_p > 0.0) action_hands(*ch_p, w_p, *pose_act_, held_prop);
  if (ch_a && w_a > 0.0) action_hands(*ch_a, w_a, *act_, held_prop);
  // clavicle channels (shrugs)
  struct Clavicle {
    Channel c;
    i32 b;
    f64 s;
  };
  constexpr Clavicle kClavicles[2] = {{Channel::ClavR, H::clavicleR, 1.0}, {Channel::ClavL, H::clavicleL, -1.0}};
  for (const Clavicle& cv : kClavicles) {
    const V3 e{add(cv.c, 0) * kDeg, add(cv.c, 1) * kDeg - cv.s * ctl.shrug * 0.35, add(cv.c, 2) * kDeg};
    if (e.x != 0.0 || e.y != 0.0 || e.z != 0.0) {
      pose.rotate_local(cv.b, qeuler(e.x, e.y, e.z));
      fk.update_subtree(pose, cv.b);
    }
  }
  // behaviour tasks on top (a hand to a wound, on a wall, out to break a fall); what a behaviour
  // asks of a hand is followed smoothly (its target on a critically damped spring, its turn and
  // its elbow eased), and when it stops asking the hand fades back from where the task had it,
  // not in one frame
  for (size_t i = 0; i < 2; ++i) {
    const std::optional<ArmTask>& task = ctl.arms[i];
    const f64 w = task_w_[i].x;
    TaskState& ts = task_s_[i];
    if (task) {
      if (!ts.on || w < 0.02) {
        ts.pos = task->target;
        ts.vel = V3{};
        ts.rot = task->rot;
        ts.pole = task->pole;
        ts.on = true;
      } else {
        const f64 om = 14.0;
        for (int c = 0; c < 3; ++c) {
          const f64 a = om * om * (task->target[c] - ts.pos[c]) - 2.0 * om * ts.vel[c];
          ts.vel[c] += a * dt;
          ts.pos[c] += ts.vel[c] * dt;
        }
        const f64 f = 1.0 - exp(-dt * 12.0);
        if (!task->rot) ts.rot.reset();
        else ts.rot = ts.rot ? qnlerp(*ts.rot, *task->rot, f) : *task->rot;
        if (!task->pole) ts.pole.reset();
        else ts.pole = ts.pole ? vnorm(vlerp(*ts.pole, *task->pole, f)) : *task->pole;
      }
    }
    if (!ts.on || w < 0.01) {
      if (w < 0.01) ts.on = false;
      continue;
    }
    const Side side = i == 0 ? Side::L : Side::R;
    // (a right hand holding a long gun takes the gun with it: the gun is let go of meanwhile)
    const Quat chest_qn = fk.q[H::chest];
    const V3 pole = ts.pole ? rotate(inv, *ts.pole) : rotate(chest_qn, vnorm(V3{side == Side::R ? 0.8 : -0.8, -0.35, -0.5}));
    std::optional<Quat> rot;
    if (ts.rot) rot = inv * *ts.rot;
    arms.hand_ik(side, to_model(ts.pos), rot, pole, clamp(w, 0.0, 1.0));
  }
  fk.update(pose, H::clavicleL);

  // ---- props, world, events -------------------------------------------------------------------
  world.compute(pose, origin, root_q);
  weapon_in_hand = false;
  if (weapon) {
    if (weapon->kind == PropKind::Knife || !held_prop || task_w_[1].x > 0.5) {
      prop_in_hand(H::handR);
      weapon_in_hand = true;
    } else {
      weapon_pos = world.p[H::weapon];
      weapon_rot = world.q[H::weapon];
    }
  }
  flush_events();
}

// ---- actions --------------------------------------------------------------------------------

void MotionPlan::update_actions(f64 dt) {
  const MotionInput& inp = input;
  const f64 speed = hypot2(velocity.x, velocity.y);
  const bool free = stance == Stance::Stand && stance_p_ >= 1.0 && inp.mood == Mood::Normal && !control.busy;
  const bool armed = weapon && weapon->kind != PropKind::Knife;
  // (a pause with the weapon down: standing still, not aiming)
  armed_idle_ = armed && free && speed < 0.15 && inp.carry != Carry::Aim && inp.carry != Carry::Hip ? armed_idle_ + dt : 0.0;
  // the posture layer: guard, talk, idle poses
  std::string_view want;
  if (inp.guard && free) {
    want = weapon && weapon->kind == PropKind::Knife ? "knifeGuard" : "guard";
  } else if (inp.talk == Talk::Speak && free && !armed) {
    want = "talk";
  } else if (inp.talk == Talk::Listen && free && !armed) {
    want = idle_pose_for(true);
  } else if (inp.idle && free && !armed && speed < 0.15 && !inp.aim_at) {
    idle_time_ += dt;
    if (idle_time_ > next_idle_pose_) {
      next_idle_pose_ = idle_time_ + 7.0 + rng_.next() * 12.0 * (1.2 - style.fidget);
      idle_choice_ = rng_.chance(0.3) ? std::string_view() : idle_pose_for(false);
    }
    want = idle_time_ > 2.0 ? idle_choice_ : std::string_view();
  } else {
    idle_time_ = 0.0;
    next_idle_pose_ = 2.0 + rng_.next() * 2.0;
  }
  if (!want.empty()) set_pose_action(action_def(want));
  else if (pose_act_ && !pose_act_->done()) pose_act_->stop();
  // one-shots the plan starts itself: gestures and nods in conversation, fidgets
  if (!busy() && free) {
    if (inp.talk == Talk::Speak) {
      next_gesture_ -= dt;
      if (next_gesture_ <= 0.0) {
        next_gesture_ = 1.2 + rng_.next() * 2.8;
        static constexpr std::array<std::string_view, 2> kNods = {"nod", "shakeHead"};
        if (rng_.chance(0.25)) play(rng_.pick(kNods));
        else play(rng_.pick(kGestures));
      }
    } else if (inp.talk == Talk::Listen) {
      next_gesture_ -= dt;
      if (next_gesture_ <= 0.0) {
        next_gesture_ = 1.5 + rng_.next() * 3.0;
        const f64 r = rng_.next();
        play(r < 0.65 ? "nod" : r < 0.8 ? "laugh" : r < 0.9 ? "shakeHead" : "shrug");
      }
    } else if (inp.idle && !armed && speed < 0.15 && idle_time_ > 3.0 && !inp.aim_at) {
      next_fidget_ -= dt;
      if (next_fidget_ <= 0.0) {
        next_fidget_ = 5.0 + rng_.next() * 10.0 * (1.3 - style.fidget);
        if (rng_.chance(0.7)) play(rng_.pick(kFidgets));
      }
    } else if (inp.idle && armed && armed_idle_ > 2.5) {
      // an armed body in a pause: the helmet, the brow, the shoulders, the weapon, a look round
      next_fidget_ -= dt;
      if (next_fidget_ <= 0.0) {
        next_fidget_ = 6.0 + rng_.next() * 9.0 * (1.3 - style.fidget);
        if (rng_.chance(0.75)) play(rng_.pick(kArmedFidgets));
      }
    }
  }
  // advance and sample
  std::optional<ActionPlayer>* players[2] = {&pose_act_, &act_};
  ChannelFrame* frames[2] = {&ch_pose_, &ch_act_};
  for (int l = 0; l < 2; ++l) {
    std::optional<ActionPlayer>& p = *players[l];
    if (!p) continue;
    crossed_.clear();
    p->advance(dt, &crossed_);
    for (const ActionEvent* e : crossed_) pending_events_.push_back(PendingEvent{e, p->def, p->target});
    frames[l]->clear();
    if (!p->done()) p->sample(*frames[l]);
  }
  if (pose_act_ && pose_act_->done()) pose_act_.reset();
  if (act_ && act_->done()) act_.reset();
}

// An idle posture that suits the character (stable per character).
std::string_view MotionPlan::idle_pose_for(bool listening) const {
  Rng r(seed_ * 131.0 + (listening ? 7.0 : std::floor(time / 15.0)));
  static constexpr std::array<std::string_view, 5> kListening = {"armsCrossed", "pockets", "handsFolded", "handsBehind", "handsOnHips"};
  return listening ? r.pick(kListening) : r.pick(kIdlePoses);
}

void MotionPlan::flush_events() {
  for (const PendingEvent& pe : pending_events_) {
    AnimEvent ev;
    ev.pos = limb_pos(pe.e->limb);
    ev.name = pe.e->name;
    ev.action = pe.def->name;
    ev.limb = pe.e->limb;
    ev.target = pe.target;
    events.push_back(std::move(ev));
  }
  pending_events_.clear();
}

V3 MotionPlan::limb_pos(Limb limb) const {
  const WorldPose& w = world;
  const Skeleton& sk = *skeleton;
  switch (limb) {
    case Limb::HandR:
    case Limb::HandL: {
      const i32 b = limb == Limb::HandR ? H::handR : H::handL;
      return w.point_of(b, vlerp(sk.rest_head[size_t(b)], sk.rest_tail[size_t(b)], 0.55));
    }
    case Limb::FootR:
    case Limb::FootL: {
      const i32 b = limb == Limb::FootR ? H::toeR : H::toeL;
      return w.point_of(b, sk.rest_tail[size_t(b)]);
    }
    case Limb::Blade:
    case Limb::Muzzle:
      if (weapon) return prop_point(weapon->muzzle);
      return w.point_of(H::handR, sk.rest_tail[H::handR]);
    default:
      return w.p[H::chest];
  }
}

// ---- arms -----------------------------------------------------------------------------------

// Arms with the gait (or relaxed), as local joint rotations; mirrored for the right side.
void MotionPlan::arm_swing(f64 dt, const GaitParams& g, bool moving, f64 speed, f64 breathe, f64 idle) {
  const f64 ph = feet_planner.phase;
  const f64 run = g.run;
  // the swing dies out over a few swings when stopping (inertia), rather than at once
  swing_amp_.update(moving ? g.arm_swing : 0.0, dt);
  const f64 swing = swing_amp_.x;
  const f64 elbow = lerp(0.18 + 0.03 * breathe + style.elbow * 0.6, g.elbow, smoothstep(0.1, 1.0, speed));
  for (const i32 side : {-1, 1}) {
    const f64 c = cos(kTau * (ph - (side < 0 ? 0.5 : 0.0) - 0.04));
    const f64 flex = swing * c + run * 0.25 + 0.02 * breathe * idle;
    const f64 adduct = -0.2 + run * 0.02;
    Quat ua = qeuler(flex, adduct, -0.1 * run);
    Quat fa = qeuler(elbow + std::max(0.0, c) * swing * 0.35, 0.0, 0.3 * run);
    Quat hd = qeuler(0.12, 0.0, 0.0);
    Quat cl = qeuler(0.0, 0.0, -0.05 * c * swing);
    if (side > 0) {
      ua = qmirror_x(ua);
      fa = qmirror_x(fa);
      hd = qmirror_x(hd);
      cl = qmirror_x(cl);
    }
    pose.r[side < 0 ? H::upperarmL : H::upperarmR] = ua;
    pose.r[side < 0 ? H::forearmL : H::forearmR] = fa;
    pose.r[side < 0 ? H::handL : H::handR] = hd;
    pose.r[side < 0 ? H::clavicleL : H::clavicleR] = cl;
  }
}

// Free hands of a stance (thighs, desk, ground, elbows) with weight w.
void MotionPlan::rest_hands(const StanceSample& s, f64 w) {
  if (w <= 0.01) return;
  for (size_t i = 0; i < 2; ++i) {
    if (!s.hands[i]) continue;
    const V3 h = *s.hands[i];
    const V3 pole = rotate(fk.q[H::chest], vnorm(V3{i == 0 ? -0.8 : 0.8, -0.5, -0.5}));
    arms.hand_ik(i == 0 ? Side::L : Side::R, h, std::nullopt, pole, w);
  }
}

// Hand channels of an action layer.
void MotionPlan::action_hands(const ChannelFrame& ch, f64 w, const ActionPlayer& player, bool held_prop) {
  const Quat chest_q = fk.q[H::chest];
  const V3 chest_p = fk.p[H::chest];
  for (const Side side : {Side::R, Side::L}) {
    const size_t si = side == Side::L ? 0 : 1;
    const f64* c = ch.get(kHandCh[si]);
    if (!c) continue;
    // a hand on the held weapon stays there unless the action says otherwise
    if (held_prop && side == Side::R && !ch.has(Channel::StrikeR) && player.def->name != "riflePush") continue;
    const f64 cw = ch.has(kHandW[si]) ? ch.at(kHandW[si]) : 1.0;
    const f64 ww = clamp(w * cw, 0.0, 1.0);
    if (ww <= 0.01) continue;
    V3 palm = chest_p + rotate(chest_q, V3{c[0] * k, c[1] * k, c[2] * k});
    const f64 s = ch.at(kStrikeCh[si]);
    if (s != 0.0 && player.target) palm = vlerp(palm, to_model(*player.target), clamp(s, -0.3, 1.1));
    const f64* r = ch.get(kHandRot[si]);
    std::optional<Quat> rot;
    if (r) rot = chest_q * qeuler(r[0] * kDeg, r[1] * kDeg, r[2] * kDeg);
    const f64* pc = ch.get(kElbowCh[si]);
    const V3 pole = rotate(chest_q, vnorm(pc ? V3{pc[0], pc[1], pc[2]} : V3{side == Side::R ? 0.7 : -0.7, -0.3, -0.7}));
    arms.hand_ik(side, palm, rot, pole, ww);
  }
}

// Mood arm poses by IK (blended over the swing pose by weight w).
void MotionPlan::mood_arms(f64 w, f64 phase) {
  const V3 head_p = fk.p[H::head];
  const Quat head_q = fk.q[H::head];
  const Quat chest_q = fk.q[H::chest];
  const f64 t = time;
  for (const i32 side : {-1, 1}) {
    const i32 ua = side < 0 ? H::upperarmL : H::upperarmR;
    V3 target, pole;
    Quat rot;
    if (mood_kind_ == Mood::Cower) {
      target = head_p + rotate(head_q, V3{side * 0.06 * k, -0.05 * k, 0.16 * k});
      pole = rotate(chest_q, vnorm(V3{side * 0.6, 0.8, 0.1}));
      rot = head_q * qeuler(-60 * kDeg, side * -90 * kDeg, 0.0);
    } else if (mood_kind_ == Mood::Surrender) {
      const V3 sh = fk.p[size_t(ua)];
      target = sh + rotate(chest_q, V3{side * 0.2 * k, 0.06 * k, 0.5 * k + 0.02 * sin(t * 3.0 + side)});
      pole = rotate(chest_q, vnorm(V3{f64(side), -0.2, -0.4}));
      rot = chest_q * qeuler(90 * kDeg, 0.0, 0.0);
    } else {
      // (panic: the hands to the head)
      const f64 b = sin(kTau * phase + (side < 0 ? 0.0 : kPi)) * 0.06 * k;
      target = head_p + rotate(chest_q, V3{side * 0.16 * k, 0.1 * k, 0.12 * k + b});
      pole = rotate(chest_q, vnorm(V3{f64(side), 0.2, -0.5}));
      rot = chest_q * qeuler(70 * kDeg, side * -90 * kDeg, 0.0);
    }
    arms.hand_ik(side < 0 ? Side::L : Side::R, target, rot, pole, w);
  }
}

// ---- props ----------------------------------------------------------------------------------

// A prop held in the hand (a knife, a lowered pistol): along the knuckles, the grip in the palm.
void MotionPlan::prop_in_hand(i32 b) {
  const WorldPose& w = world;
  const Side side = b == H::handL ? Side::L : Side::R;
  const Quat q = w.q[size_t(b)] * conj(arms.canonical(side));
  const V3 palm = w.p[size_t(b)] + rotate(w.q[size_t(b)], arms.palm_offset(side));
  weapon_pos = palm;
  // a knife points out of the fist along the knuckles; a pistol hangs muzzle down-forward; a long
  // gun hangs from the hand
  const bool pistol = weapon && weapon->kind == PropKind::Pistol;
  const bool knife = weapon && weapon->kind == PropKind::Knife;
  weapon_rot = pistol ? q * qx(-0.3) : knife ? q : q * qx(-0.9);
}

V3 MotionPlan::eyes() const {
  const V3& h = skeleton->rest_head[H::head];
  return world.point_of(H::head, V3{h.x, h.y + 0.09 * k, h.z + 0.12 * k});
}

V3 MotionPlan::palm(Side side) const {
  const i32 b = side == Side::L ? H::handL : H::handR;
  return world.point_of(b, skeleton->rest_head[size_t(b)] + arms.palm_offset(side));
}

}  // namespace svx::anim
