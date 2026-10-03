#include "svx/anim/motion/feet.hpp"

#include <algorithm>
#include <limits>

namespace svx::anim {

FootPlanner::FootPlanner(const FeetDims& dims_, const CollisionWorld* collision_, const std::array<FootBones, 2>& bones)
    : dims(dims_), collision(collision_) {
  for (size_t i = 0; i < 2; ++i) {
    Foot& f = feet[i];
    f.side = i == 0 ? -1 : 1;
    f.thigh = bones[i].thigh;
    f.shin = bones[i].shin;
    f.foot = bones[i].foot;
    f.toe = bones[i].toe;
    f.offset = f.side < 0 ? 0.0 : 0.5;
  }
}

V3 FootPlanner::nominal(const Foot& f, const V3& root, f64 yaw, f64 crouch, const GaitStyle& style) const {
  const FeetDims& d = dims;
  const f64 x = f.side * d.foot_x * style.width * (1.0 + 0.45 * crouch);
  const V3 o = rotate(qz(yaw - kPi / 2.0), V3{x, -0.02 * d.k, 0});
  return V3{root.x + o.x, root.y + o.y, root.z};
}

f64 FootPlanner::ground(f64 x, f64 y, f64 z_ref, f64 fallback) const {
  const f64 k = dims.k;
  const std::optional<f64> g = collision->ground_height(x, y, z_ref + 0.6 * k, z_ref - 0.9 * k);
  return g ? *g : fallback;
}

void FootPlanner::reset(const V3& root_in, f64 yaw, f64 crouch, const GaitStyle& style) {
  settle_layout_ = false;
  const V3 root = root_in;
  for (Foot& f : feet) {
    f.pos = nominal(f, root, yaw, crouch, style);
    f.pos.z = ground(f.pos.x, f.pos.y, root.z, root.z);
    f.yaw = yaw - f.side * style.toe_out;
    f.planted = true;
    f.held = false;
    f.forced = false;
    f.unloaded = false;
    f.swing = 0.0;
    f.pitch = 0.0;
    f.clear = 0.0;
    f.target = f.pos;
    f.ankle = ankle_from_plant(f.pos, f.yaw, 0.0);
  }
}

void FootPlanner::place_at(const std::array<V3, 2>& soles, const std::array<f64, 2>& yaws) {
  settle_layout_ = false;
  for (size_t i = 0; i < 2; ++i) {
    Foot& f = feet[i];
    f.pos = soles[i];
    f.yaw = yaws[i];
    f.planted = true;
    f.forced = false;
    f.held = false;
    f.swing = 0.0;
    f.pitch = 0.0;
    f.target = f.pos;
    f.ankle = ankle_from_plant(f.pos, f.yaw, 0.0);
  }
  stepping = false;
}

void FootPlanner::step(i32 i, const V3& target_in, f64 duration, std::optional<f64> yaw) {
  Foot& f = feet[size_t(i)];
  if (f.held || f.unloaded) return;
  const V3 target = target_in;
  if (!f.planted) {
    // re-aim a swing under way (the gait's becomes the balance's, from where the foot is)
    if (!f.forced) {
      f.forced = true;
      f.lift = f.ankle - ankle_from_plant(V3{}, f.yaw, f.pitch);
      f.lift_root = f.lift;
      f.lift_yaw = f.yaw;
      f.lift_pitch = f.pitch;
      f.swing = 0.0;
      f.swing_rate = 1.0 / std::max(0.1, duration);
      f.clear = 0.0;
    }
    f.target = target;
    f.target.z = ground(target.x, target.y, target.z, target.z);
    if (yaw) f.target_yaw = *yaw;
    // the rest of the swing in the time left
    f.swing_rate = std::max(f.swing_rate, (1.0 - f.swing) / std::max(0.06, duration));
    return;
  }
  f.planted = false;
  f.forced = true;
  f.lift = f.pos;
  f.lift_root = f.pos;
  f.lift_yaw = f.yaw;
  f.lift_pitch = f.pitch;
  f.swing = 0.0;
  f.swing_rate = 1.0 / std::max(0.12, duration);
  f.target = target;
  f.target.z = ground(target.x, target.y, target.z, target.z);
  f.target_yaw = yaw ? *yaw : f.yaw;
  f.clear = clearance(f.lift, f.target, 0.6);
}

void FootPlanner::plant_now(i32 i, const V3& at_in) {
  Foot& f = feet[size_t(i)];
  const V3 at = at_in;
  f.pos = at;
  f.pos.z = ground(at.x, at.y, at.z + 0.1, at.z);
  f.target = f.pos;
  f.planted = true;
  f.forced = false;
  f.swing = 0.0;
  f.since = 0.0;
}

void FootPlanner::on_tread(const Foot& f, V3& tgt, f64 yaw) const {
  const f64 k = dims.k;
  const f64 max_rise = 0.3 * k;
  const f64 lz = f.lift.z;
  const f64 fx = cos(yaw), fy = sin(yaw);
  auto fits = [&](f64 x, f64 y, f64 z) {
    if (std::abs(z - lz) > max_rise) return false;
    // (the toes a little short of the next riser, so they come off it cleanly)
    const f64 tf = dims.ball_fwd * 1.2 + 0.05 * k;
    for (const f64 side : {-0.06 * k, 0.0, 0.06 * k}) {
      const f64 toe = ground(x + fx * tf - fy * side, y + fy * tf + fx * side, z, z - 1.0);
      const f64 heel = ground(x - fx * dims.heel_back - fy * side, y - fy * dims.heel_back + fx * side, z, z - 1.0);
      if (std::abs(toe - z) >= 0.03 * k || std::abs(heel - z) >= 0.03 * k) return false;
    }
    return true;
  };
  if (fits(tgt.x, tgt.y, tgt.z)) return;
  // (flat ground never gets here; only a landing across an edge or too far up or down)
  const f64 ax = f.lift.x, ay = f.lift.y;
  for (f64 t = 0.95; t >= 0.3; t -= 0.05) {
    const f64 x = ax + (tgt.x - ax) * t, y = ay + (tgt.y - ay) * t;
    const f64 z = ground(x, y, lz + 0.4 * k, lz);
    if (!fits(x, y, z)) continue;
    tgt.x = x;
    tgt.y = y;
    tgt.z = z;
    return;
  }
}

f64 FootPlanner::clearance(const V3& a, const V3& b, f64 care) const {
  const f64 top = std::max(a.z, b.z);
  f64 high = top;
  const f64 length = hypot2(b.x - a.x, b.y - a.y);
  const V3 side = length > 1e-6 ? V3{-(b.y - a.y) / length, (b.x - a.x) / length, 0} : V3{1, 0, 0};
  for (int s = 1; s <= 4; ++s) {
    const f64 t = s / 5.0;
    const f64 x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t;
    // Check the boot's width too. A narrower step can pass alongside rubble
    // whose edge catches the boot even though the centre line is clear.
    for (const f64 offset : {-0.06 * dims.k, 0.0, 0.06 * dims.k}) {
      const std::optional<f64> g = collision->ground_height(x + side.x * offset, y + side.y * offset, top + 0.55 * dims.k, top - 0.3 * dims.k);
      if (g && *g > high) high = *g;
    }
    // (a body, a piece of debris on the path)
    for (const Obstacle& o : obstacles) {
      const f64 dx = x - o.c.x, dy = y - o.c.y;
      const f64 h2 = o.r * o.r - dx * dx - dy * dy;
      if (h2 <= 0.0) continue;
      const f64 z = o.c.z + std::sqrt(h2);
      if (z > high && z < top + 0.55 * dims.k) high = z;
    }
  }
  if (high <= top + 0.02) return 0.0;
  // a careful foot clears an obstacle by a hand's width; a hurried one barely
  return high - top + lerp(0.015, 0.08, care) * dims.k;
}

V3 FootPlanner::ankle_from_plant(const V3& plant, f64 yaw, f64 pitch) const {
  const FeetDims& d = dims;
  const V3 fwd{cos(yaw), sin(yaw), 0};
  f64 dy = 0.0;
  f64 dz = d.ankle_h;
  if (pitch < 0.0) {
    // toe-off: the foot rolls about the ball
    const f64 c = cos(pitch), s = sin(pitch);
    const f64 by = -d.ball_fwd, bz = d.ankle_h;
    dy = d.ball_fwd + by * c - bz * s;
    dz = by * s + bz * c;
  } else if (pitch > 0.0) {
    // heel strike: about the heel
    const f64 c = cos(pitch), s = sin(pitch);
    const f64 by = d.heel_back, bz = d.ankle_h;
    dy = -d.heel_back + by * c - bz * s;
    dz = by * s + bz * c;
  }
  return V3{plant.x + fwd.x * dy, plant.y + fwd.y * dy, plant.z + dz};
}

f64 FootPlanner::advance_clock(f64 dt, const FeetContext& c, const std::array<f64, 2>& limp) {
  const GaitParams& g = c.gait;
  const GaitStyle& st = c.style;
  f64 prev = phase;
  for (size_t i = 0; i < 2; ++i) {
    const bool was_unloaded = feet[i].unloaded;
    feet[i].unloaded = c.support[i] < .12 && c.support[1 - i] > .4;
    if (feet[i].unloaded) feet[i].forced = false;
    else if (was_unloaded) {
      feet[i].planted = true;
      step(i32(i), nominal(feet[i], c.root, c.body_yaw, c.crouch, st), .25, c.body_yaw);
    }
  }
  const bool forced = feet[0].forced || feet[1].forced;
  if (forced) {
    // the balance is stepping: the clock waits
    stepping = false;
    settle_layout_ = false;
    return prev;
  }
  if (c.moving) {
    settle_layout_ = true;
    if (!stepping) {
      // start with the foot behind
      const f64 sp = c.speed != 0.0 && !std::isnan(c.speed) ? c.speed : 1.0;
      const V3 dir{c.vel.x / sp, c.vel.y / sp, 0};
      const f64 d0 = (feet[0].pos.x - c.root.x) * dir.x + (feet[0].pos.y - c.root.y) * dir.y;
      const f64 d1 = (feet[1].pos.x - c.root.x) * dir.x + (feet[1].pos.y - c.root.y) * dir.y;
      const Foot& lead = feet[0].unloaded ? feet[1] : feet[1].unloaded ? feet[0] : d0 <= d1 ? feet[0] : feet[1];
      if ((feet[0].planted || feet[0].unloaded) && (feet[1].planted || feet[1].unloaded)) {
        phase = fract(g.duty - 0.02 - lead.offset);
        prev = phase;
      }
      stepping = true;
    }
    // a limp hurries the step off the wounded leg
    const bool in_stance_l = fract(phase) < g.duty;
    const bool in_stance_r = fract(phase + 0.5) < g.duty;
    const f64 hurry = 1.0 + 0.9 * (in_stance_l && !feet[0].unloaded ? limp[0] : 0.0) +
                      0.9 * (in_stance_r && !feet[1].unloaded ? limp[1] : 0.0);
    phase = fract(phase + g.freq * hurry * dt);
  } else if (!c.airborne) {
    bool need = false;
    for (const Foot& f : feet) {
      if (f.held || f.unloaded) continue;
      if (!f.planted) {
        need = true;
      } else {
        const V3 nom = nominal(f, c.root, c.body_yaw, c.crouch, st);
        const f64 err = hypot2(nom.x - f.pos.x, nom.y - f.pos.y);
        const f64 yaw_err = std::abs(wrap_angle(c.body_yaw - f.side * st.toe_out - f.yaw));
        // After travel, step back to the relaxed standing layout. Keep the
        // normal turning dead zone once settled, so a small glance is not a shuffle.
        const f64 lateral = std::abs((nom.x - f.pos.x) * sin(c.body_yaw) - (nom.y - f.pos.y) * cos(c.body_yaw));
        const bool layout = settle_layout_ && (lateral > 0.01 * dims.k || yaw_err > 0.025);
        if (err > 0.16 * dims.k || yaw_err > 0.42 || layout || (stepping && err > 0.07 * dims.k)) need = true;
      }
    }
    // (turning on the spot and settling: unhurried steps)
    if (need) phase = fract(phase + 1.15 * dt);
    else {
      stepping = false;
      settle_layout_ = false;
    }
  }
  return prev;
}

void FootPlanner::update(f64 dt, const FeetContext& c, f64 prev_phase) {
  const FeetDims& d = dims;
  const f64 k = d.k;
  const GaitStyle& st = c.style;
  const GaitParams& g = c.gait;
  landed[0] = landed[1] = false;
  if (was_airborne_ && !c.airborne) {
    for (Foot& f : feet) {
      f.pos = nominal(f, c.root, c.body_yaw, c.crouch, st);
      f.pos.z = ground(f.pos.x, f.pos.y, c.ground_z, c.ground_z);
      f.yaw = c.body_yaw - f.side * st.toe_out;
      f.planted = true;
      f.forced = false;
    }
    landed[0] = landed[1] = true;
  }
  was_airborne_ = c.airborne;
  const f64 D = g.duty;
  const f64 freq = c.moving ? g.freq : 1.5;
  const f64 stance_t = D / freq;
  const f64 move_amt = smoothstep(0.1, 0.9, c.speed);
  const bool forced_any = feet[0].forced || feet[1].forced;
  for (size_t fi = 0; fi < 2; ++fi) {
    Foot& f = feet[fi];
    f.since += dt;
    f.unloaded = c.support[fi] < .12 && c.support[1 - fi] > .4;
    if (f.unloaded && !c.airborne) {
      // A controlled leg can be tucked behind the good one. With little motor
      // control it drags close to the floor, without becoming a support contact.
      V3 sole = nominal(f, c.root, c.body_yaw, c.crouch, st);
      sole -= V3{cos(c.body_yaw), sin(c.body_yaw), 0} * (.12 * k);
      sole.z = ground(sole.x, sole.y, c.ground_z, c.ground_z) + lerp(.015, .16, c.control[fi]) * k;
      f.pitch = -.2 * (1 - c.control[fi]);
      f.yaw += wrap_angle(c.body_yaw - f.yaw) * (1 - exp(-dt * 10));
      f.ankle = vlerp(f.ankle, ankle_from_plant(sole, f.yaw, f.pitch), 1 - exp(-dt * 10));
      f.pos = f.ankle - ankle_from_plant({}, f.yaw, f.pitch);
      f.target = f.pos;
      f.planted = f.forced = false;
      f.swing = 0;
      continue;
    }
    if (f.held) continue;
    const f64 p0 = fract(prev_phase + f.offset);
    const f64 p1 = fract(phase + f.offset);
    const bool advanced = phase != prev_phase;
    if (c.airborne) {
      f.planted = false;
    } else if (f.planted && !forced_any && !c.hold) {
      const bool wrapped = p1 < p0;
      const bool crossed_lift = advanced && ((!wrapped && p0 < D && p1 >= D) || (wrapped && p0 < D));
      // a foot still down late in its swing phase (it landed late) goes now, so the feet keep
      // alternating
      const bool missed_lift = c.moving && advanced && p1 > D + 0.08 && p1 < 0.9;
      // a foot left behind out of reach pushes off early (a long running stride shortens the
      // ground contact; a sudden start); it still lands on the gait's beat below
      const bool far = hypot2(f.pos.x - c.root.x, f.pos.y - c.root.y) > 0.62 * d.leg_len;
      if (crossed_lift || missed_lift || far) {
        f.planted = false;
        f.lift = f.pos;
        f.lift_root = c.root;
        f.lift_yaw = f.yaw;
        f.lift_pitch = f.pitch;
        f.swing = 0.0;
        f.clear = -1.0;
        // land when the gait says this foot lands (phase 1), however it left the ground
        f.swing_rate = crossed_lift ? freq / (1.0 - D) : c.moving ? 1.0 / clamp((1.0 - p1) / freq, 0.12, 1.0 / freq) : 1.0 / 0.2;
      }
    }
    if (!c.airborne && !f.planted) {
      f.swing = std::min(1.0, f.swing + f.swing_rate * dt);
      if (!f.forced) {
        const f64 remain = (1.0 - f.swing) / f.swing_rate;
        const V3 pred{c.root.x + c.vel.x * remain, c.root.y + c.vel.y * remain, c.root.z};
        V3 tgt = nominal(f, pred, c.body_yaw, c.crouch, st);
        const f64 reach = lerp(0.52, 0.34, g.run) * d.leg_len;
        const f64 ahead = stance_t * lerp(0.5, 0.36, g.run);
        f64 hx = c.vel.x * ahead, hy = c.vel.y * ahead;
        const f64 hl = hypot2(hx, hy);
        if (hl > reach) {
          hx *= reach / hl;
          hy *= reach / hl;
        }
        tgt.x += hx;
        tgt.y += hy;
        tgt.z = ground(tgt.x, tgt.y, c.ground_z, c.ground_z);
        on_tread(f, tgt, c.body_yaw);
        f.target = tgt;
        // turning on the spot: a step opens the foot at most ~43 degrees past the other one
        f64 heading = c.body_yaw;
        if (!c.moving) {
          const Foot& o = feet[1 - fi];
          const f64 oh = o.yaw + o.side * st.toe_out;
          heading = oh + clamp(wrap_angle(c.body_yaw - oh), -0.75, 0.75);
        }
        f.target_yaw = heading - f.side * st.toe_out;
        // what lies on the path (sampled once, at lift-off)
        if (f.clear < 0.0) f.clear = clearance(f.lift, f.target, c.care);
      }
      if (f.swing >= 1.0) {
        f.planted = true;
        f.forced = false;
        f.pos = f.target;
        f.yaw = f.target_yaw;
        f.since = 0.0;
        landed[fi] = true;
      }
    }
    if (c.airborne) {
      V3 nom = nominal(f, c.root, c.body_yaw, c.crouch, st);
      nom.z = c.root.z + 0.12 * k + (f.side < 0 ? 0.05 : 0.0) * k;
      f.ankle = nom;
      f.pitch = -0.3;
      f.yaw = c.body_yaw;
      f.pos = nom;
      f.pos.z -= d.ankle_h;
      f.lift = f.pos;
      f.lift_root = c.root;
      continue;
    }
    if (f.planted) {
      const f64 gz = ground(f.pos.x, f.pos.y, f.pos.z + 0.2 * k, f.pos.z);
      if (gz < f.pos.z - 0.01) f.pos.z = std::max(gz, f.pos.z - 3.0 * dt);
      // Contact starts at the actual touchdown, including a late/early step.
      // Using the clock here changed the ankle pivot discontinuously at landing.
      const f64 u = clamp(f.since / stance_t, 0.0, 1.0);
      f64 pitch = 0.0;
      if (c.moving && !forced_any) {
        const f64 hs = lerp(0.28, 0.1, g.run) * move_amt;
        const f64 to = lerp(0.45, 0.65, g.run) * move_amt;
        pitch = hs * (1.0 - smoothstep(0.0, 0.18, u)) - to * smoothstep(0.5, 1.0, u);
      }
      f.pitch = pitch;
      f.ankle = ankle_from_plant(f.pos, f.yaw, pitch);
    } else {
      const f64 s = f.swing;
      const f64 run = f.forced ? 0.0 : g.run;
      const f64 sh = lerp(s, pow(s, 1.6), run);
      const f64 eh = sh * sh * (3.0 - 2.0 * sh);
      // the foot leaves the ground moving with the body (a runner's heel kicks up and comes
      // through under the hip; a walker's foot peels off more slowly)
      const f64 carry = c.moving && !f.forced ? lerp(0.35, 1.0, run) * (1.0 - eh) : 0.0;
      const V3 from{f.lift.x + (c.root.x - f.lift_root.x) * carry, f.lift.y + (c.root.y - f.lift_root.y) * carry, f.lift.z};
      V3 hz = vlerp(from, f.target, eh);
      // (a step up is risen to early, before the toe meets its edge, and cleared with a margin;
      // a step down is kept above until past its edge)
      const f64 rise = f.target.z - from.z;
      if (rise > 0.02 * k) hz.z = from.z + rise * smoothstep(0.0, 0.55, s);
      else if (rise < -0.02 * k) hz.z = from.z + rise * smoothstep(0.35, 1.0, s);
      const f64 peak = sin(kPi * pow(s, lerp(1.0, 0.62, run)));
      const f64 base_lift = f.forced ? 0.07 * k : c.moving ? g.lift : 0.06 * k;
      const f64 step_up = std::abs(rise) > 0.04 * k ? 0.045 * k : 0.0;
      const f64 lift = (base_lift + step_up) * peak + std::max(0.0, rise) * 0.15 * peak + std::max(0.0, f.clear) * sin(kPi * clamp(s * 1.15, 0.0, 1.0));
      hz.z += lift * lerp(.08, 1.0, c.control[f.side < 0 ? 0 : 1]);
      // the sole keeps above what is under and just ahead of the foot on its way (a stair's
      // edge, a kerb): the toe does not stub on it
      if ((step_up > 0.0 || f.clear > 0.0) && s < 0.92) {
        const f64 dx = f.target.x - from.x, dy = f.target.y - from.y;
        const f64 dl = hypot2(dx, dy);
        if (dl > 1e-3) {
          const f64 ux = dx / dl, uy = dy / dl;
          const f64 along = (hz.x - from.x) * ux + (hz.y - from.y) * uy;
          f64 floor_z = -std::numeric_limits<f64>::infinity();
          const f64 alongs[3] = {-d.heel_back, d.ball_fwd * 1.3, d.ball_fwd * 1.3 + 0.12 * k};
          const f64 sides[3] = {0.0, -0.06 * k, 0.06 * k};
          for (const f64 a : alongs) {
            // (not past the landing: the foot comes down onto its tread)
            if (along + a > dl + 0.02 * k) continue;
            // (the foot's width: what is just beside the line catches it too)
            for (const f64 w : sides) {
              const std::optional<f64> gz = collision->ground_height(hz.x + ux * a - uy * w, hz.y + uy * a + ux * w, hz.z + 0.5 * k, hz.z - 0.8 * k);
              if (gz && *gz > floor_z) floor_z = *gz;
            }
          }
          if (floor_z > -std::numeric_limits<f64>::infinity())
            hz.z = std::max(hz.z, floor_z + 0.05 * k * (1.0 - smoothstep(0.65, 1.0, s)));
        }
      }
      const f64 e = s * s * (3.0 - 2.0 * s);
      const f64 yaw = f.lift_yaw + wrap_angle(f.target_yaw - f.lift_yaw) * e;
      const f64 hs = c.moving && !f.forced ? lerp(0.28, 0.1, run) * move_amt : 0.0;
      f.pitch = lerp(f.lift_pitch, hs, smoothstep(0.12, 0.92, s));
      // (up a step the toes come up at once, clear of its edge)
      if (rise > 0.04 * k) f.pitch = lerp(f.pitch, 0.15, smoothstep(0.0, 0.25, s) * (1.0 - smoothstep(0.65, 1.0, s)));
      // The same sole -> ankle transform on both sides of contact. Otherwise
      // heel strike and toe-off teleport the ankle by the foot's roll offset.
      f.ankle = ankle_from_plant(hz, yaw, f.pitch);
      const V3 landing_ankle = f.ankle;
      // within the leg's reach of the hip: a foot left behind rises (the heel kicks up)
      const V3& hip = c.hips[fi];
      const f64 reach = 0.97 * d.leg_len;
      const f64 dx = f.ankle.x - hip.x, dy = f.ankle.y - hip.y;
      const f64 hd = hypot2(dx, dy);
      if (hd < reach) {
        f.ankle.z = std::max(f.ankle.z, hip.z - std::sqrt(reach * reach - hd * hd));
      } else {
        f.ankle.x = hip.x + (dx / hd) * reach * 0.95;
        f.ankle.y = hip.y + (dy / hd) * reach * 0.95;
        f.ankle.z = std::max(f.ankle.z, hip.z - reach * 0.31);
      }
      // Early swing can tuck the foot beneath the hip. Late swing must approach
      // its ground contact; the pelvis reach solve makes room for that landing.
      // Keeping the old hip's reach clamp until contact caused a visible snap.
      f.ankle = vlerp(f.ankle, landing_ankle, smoothstep(0.4, 0.85, s));
      f.yaw = yaw;
    }
  }
}

std::array<FootState, 2> FootPlanner::state() const {
  std::array<FootState, 2> out;
  for (size_t i = 0; i < 2; ++i) out[i] = FootState{feet[i].planted, feet[i].pos, feet[i].ankle, feet[i].yaw, feet[i].forced};
  return out;
}

}  // namespace svx::anim
