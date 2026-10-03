#include "svx/anim/motion/arms.hpp"

#include "svx/anim/rig.hpp"

namespace svx::anim {

ArmRig::ArmRig(SkeletonPtr skeleton_, Pose* pose_, ModelFK* fk_) : skeleton(std::move(skeleton_)), pose(pose_), fk(fk_) {
  const Skeleton& sk = *skeleton;
  for (const Side side : {Side::L, Side::R}) {
    const i32 b = side == Side::L ? H::handL : H::handR;
    const V3 dir = sk.rest_tail[size_t(b)] - sk.rest_head[size_t(b)];
    canon_[size_t(side)] = frame_rotation(dir, V3{side == Side::L ? 1.0 : -1.0, 0, 0}, V3{0, 1, 0}, V3{0, 0, -1});
    palm_[size_t(side)] = dir * 0.42;
  }
}

void ArmRig::begin_frame() {
  for (size_t side = 0; side < 2; ++side) for (size_t j = 0; j < 3; ++j)
    previous_[side][j] = pose->r[(side == 0 ? H::upperarmL : H::upperarmR) + j];
}

void ArmRig::finish_frame(f64 dt, f64 max_rate) {
  if (dt <= 0.0) return;
  // Limits the intended pose, never the simulated body's response to a hit.
  // Elbows may extend faster than shoulders and wrists during a strike.
  constexpr f64 speed[3] = {30.0, 36.0, 30.0};
  for (size_t side = 0; side < 2; ++side) for (size_t j = 0; j < 3; ++j) {
    Quat& goal = pose->r[(side == 0 ? H::upperarmL : H::upperarmR) + j];
    const Quat before = previous_[side][j];
    const V3 delta = qerror(goal, before);
    const f64 angle = norm(delta), limit = std::min(speed[j], max_rate) * dt;
    if (angle > limit) goal = qnormalize(qexp(delta * (limit / angle)) * before);
  }
}

void ArmRig::hand_ik(Side side, const V3& target, const std::optional<Quat>& rot, const V3& pole, f64 w, f64 soft) {
  Pose& p = *pose;
  ModelFK& f = *fk;
  const i32 ua = side == Side::L ? H::upperarmL : H::upperarmR;
  const i32 fa = side == Side::L ? H::forearmL : H::forearmR;
  const i32 hand = side == Side::L ? H::handL : H::handR;
  const bool blend = w < 0.999;
  const Quat saved[3] = {p.r[size_t(ua)], p.r[size_t(fa)], p.r[size_t(hand)]};
  const Quat hand_q = rot ? *rot * canonical(side) : f.q[size_t(hand)];
  const V3 palm = rotate(hand_q, palm_offset(side));
  solve_two_bone(p, f, ua, fa, hand, target - palm, pole, soft, &kElbowRest);
  if (rot) set_model_rotation(p, f, hand, hand_q);
  if (blend) {
    for (size_t j = 0; j < 3; ++j) {
      const Quat a = saved[j], b = p.r[size_t(ua) + j];
      Quat result = qnlerp(a, b, w);
      // Near a half-turn, the two interpolation arcs are equally short. Keep
      // the arc continuous with the preceding frame instead of flipping it.
      if (std::abs(qdot(a, b)) < 0.3) {
        const f64 sign = qdot(a, b) < 0.0 ? 1.0 : -1.0;
        const Quat other = qnormalize(Quat{a.x * (1 - w) + sign * b.x * w, a.y * (1 - w) + sign * b.y * w,
                                          a.z * (1 - w) + sign * b.z * w, a.w * (1 - w) + sign * b.w * w});
        if (std::abs(qdot(other, previous_[size_t(side)][j])) > std::abs(qdot(result, previous_[size_t(side)][j]))) result = other;
      }
      p.r[size_t(ua) + j] = result;
    }
  }
  f.update_subtree(p, ua);
}

Quat ArmRig::grip_r(bool pistol) const {
  const Skeleton& sk = *skeleton;
  const V3 dir = sk.rest_tail[H::handR] - sk.rest_head[H::handR];
  return frame_rotation(dir, V3{-1, 0, 0}, vnorm(pistol ? V3{0, -0.2, -1} : V3{0, -0.3, -1}), V3{-1, 0, 0});
}

Quat ArmRig::grip_l(bool pistol) const {
  const Skeleton& sk = *skeleton;
  const V3 dir = sk.rest_tail[H::handL] - sk.rest_head[H::handL];
  // a pistol's support hand wraps the gun hand from the left
  if (pistol) return frame_rotation(dir, V3{1, 0, 0}, vnorm(V3{0.4, -0.1, -1}), vnorm(V3{1, 0.2, 0.1}));
  return frame_rotation(dir, V3{1, 0, 0}, vnorm(V3{0.35, 0.8, 0.1}), vnorm(V3{0.3, 0, 1}));
}

void WeaponHold::fire(const Prop& prop, f64 strength) {
  const bool pistol = prop.has("handgun");
  kick_back.kick((pistol ? 0.8 : 1.1) * strength);
  kick_pitch.kick((pistol ? 12.0 : 7.0) * strength);
}

bool WeaponHold::hold(f64 dt, ArmRig& arms, const Prop& prop, const HoldContext& c) {
  Pose& pose = *arms.pose;
  ModelFK& fk = *arms.fk;
  const f64 k = c.k;
  const bool pistol = prop.has("handgun");
  ready_w.update(c.carry == Carry::Relaxed ? 0.0 : 1.0, dt);
  hip_w.update(c.carry == Carry::Hip ? 1.0 : 0.0, dt);
  sprint_w.update(c.moving && !c.aiming ? c.run : 0.0, dt);
  kick_back.update(0.0, dt);
  kick_pitch.update(0.0, dt);
  // a lowered pistol hangs in the hand
  if (pistol && ready_w.x < 0.05 && !c.aiming) return false;
  const Quat chest_q = fk.q[H::chest];
  const V3 chest_p = fk.p[H::chest];
  const f64 handed = c.primary_left ? -1 : 1;
  const V3 shoulder = fk.p[c.primary_left ? H::upperarmL : H::upperarmR];
  const V3 pocket = shoulder + rotate(chest_q, V3{-handed * 0.06 * k, 0.07 * k, -0.035 * k});
  const V3 cf = rotate(chest_q, V3{0, 1, 0});
  const Quat chest_yaw_q = frame_rotation(V3{0, 1, 0}, V3{0, 0, 1}, vnorm(V3{cf.x, cf.y, 0}), V3{0, 0, 1});
  const V3 eyes = fk.p[H::head] + rotate(fk.q[H::head], V3{0, 0.09 * k, 0.1 * k});
  V3 aim_dir = vnorm(V3{cf.x, cf.y, 0});
  if (c.aim_at) aim_dir = vnorm(*c.aim_at - (pistol ? eyes : pocket));
  const Quat aim_rot = frame_rotation(V3{0, 1, 0}, V3{0, 0, 1}, aim_dir, V3{0, 0, 1});
  const f64 rw = clamp(ready_w.x, 0.0, 1.0);
  const f64 aw = clamp(c.aim_w, 0.0, 1.0);
  const f64 hw = clamp(hip_w.x, 0.0, 1.0);
  Quat rot;
  V3 pos;
  bool two_handed = !c.free_left;
  if (pistol) {
    // lowered (in the hand) -> low ready (two hands, muzzle down) -> aimed (two hands at eye
    // level) or one-handed (arm out at the target)
    const Quat ready_rot = aim_rot * qeuler(-0.7, 0, 0);
    const V3 ready_grip = chest_p + rotate(chest_yaw_q, V3{handed * 0.03 * k, 0.34 * k, 0.02 * k});
    const V3 aim_grip = eyes + (aim_dir * (0.47 * k) + V3{0, 0, -0.07 * k});
    const V3 one_grip = shoulder + aim_dir * (0.56 * k);
    rot = qnlerp(ready_rot, aim_rot, aw);
    pos = vlerp(ready_grip, vlerp(aim_grip, one_grip, c.free_left ? 1.0 : hw), aw);
    two_handed = two_handed && hw < 0.5;
  } else {
    const Quat relaxed_rot = chest_yaw_q * qeuler(-0.95, 0.15, 0.55);
    const V3 relaxed_grip = chest_p + rotate(chest_yaw_q, V3{handed * 0.13 * k, 0.2 * k, -0.2 * k});
    // (a heavy machine gun is carried across the body, lower)

    const Quat ready_rot = aim_rot * qeuler(prop.ready_pitch, 0, prop.ready_roll);
    const V3 ready_stock = pocket + rotate(chest_q, prop.ready_stock_offset * k);
    const V3 ready_grip = ready_stock - rotate(ready_rot, prop.stock);
    const V3 aim_grip = pocket - rotate(aim_rot, prop.stock);
    // hip fire: the stock under the arm, level at the target (machine guns on the move)
    const Quat hip_rot = qnlerp(aim_rot, frame_rotation(V3{0, 1, 0}, V3{0, 0, 1}, vnorm(V3{aim_dir.x, aim_dir.y, aim_dir.z * 0.5}), V3{0, 0, 1}), 0.5);
    const V3 hip_grip = chest_p + rotate(chest_yaw_q, V3{handed * 0.13 * k, 0.2 * k, -0.2 * k});
    const Quat port_rot = chest_yaw_q * frame_rotation(V3{0, 1, 0}, V3{0, 0, 1}, vnorm(V3{-0.55, 0.3, 0.78}), vnorm(V3{0.1, 1, 0.1}));
    const V3 port_grip = chest_p + rotate(chest_yaw_q, V3{handed * 0.12 * k, 0.2 * k, -0.08 * k});
    rot = qnlerp(relaxed_rot, ready_rot, rw);
    pos = vlerp(relaxed_grip, ready_grip, rw);
    const f64 sw = clamp(sprint_w.x, 0.0, 1.0) * (1.0 - aw);
    rot = qnlerp(rot, port_rot, sw);
    pos = vlerp(pos, port_grip, sw);
    const Quat aim_r = qnlerp(aim_rot, hip_rot, hw);
    const V3 aim_p = vlerp(aim_grip, hip_grip, hw);
    rot = qnlerp(rot, aim_r, aw);
    pos = vlerp(pos, aim_p, aw);
    if (c.free_left) {
      // one hand on a long gun: it hangs lower, muzzle down
      rot = qnlerp(rot, chest_yaw_q * qeuler(-1.15, 0.1, 0.35), 0.7);
      pos = vlerp(pos, chest_p + rotate(chest_yaw_q, V3{handed * .2 * k, 0.12 * k, -0.28 * k}), 0.7);
    }
  }
  // recoil, weapon sway
  const V3 fwd = rotate(rot, V3{0, 1, 0});
  pos = pos + fwd * (-(pistol ? 0.03 : 0.035) * kick_back.x);
  rot = rot * qx((pistol ? 0.07 : 0.05) * kick_pitch.x);
  const f64 bob = c.moving ? sin(kTau * 2.0 * c.phase) * 0.008 * k * (1.0 - aw * 0.7) : 0.0;
  pos = pos + sway_.update(V3{0, 0, bob}, dt);
  // action offsets of the weapon (reloads, a rifle jab)
  const ChannelFrame* layers[2] = {c.ch_p, c.ch_a};
  const f64 weights[2] = {c.w_p, c.w_a};
  for (int l = 0; l < 2; ++l) {
    const ChannelFrame* ch = layers[l];
    const f64 w = weights[l];
    if (!ch || w <= 0.0) continue;
    const f64* wr = ch->get(Channel::WeaponRot);
    const f64* wpos = ch->get(Channel::WeaponPos);
    if (wr) rot = rot * qeuler(wr[0] * kDeg * w, wr[1] * kDeg * w, wr[2] * kDeg * w);
    if (wpos) pos = pos + rotate(chest_q, V3{wpos[0] * k * w, wpos[1] * k * w, wpos[2] * k * w});
  }
  pose.t[H::weapon] = pos;
  pose.r[H::weapon] = rot;
  fk.update_bone(pose, H::weapon);

  if (c.primary_left) {
    const Quat right = rot * arms.grip_r(pistol) * conj(arms.canonical(Side::R));
    arms.hand_ik(Side::L, pos, right, rotate(chest_q, V3{-.7, -.3, -.75}), 1, .02);
    if (two_handed) {
      const Quat left = rot * arms.grip_l(pistol) * conj(arms.canonical(Side::L));
      arms.hand_ik(Side::R, pos + rotate(rot, prop.support), left, rotate(chest_q, V3{.7, .1, -.8}), 1, .04);
    }
    return true;
  }
  // hands on the prop
  const Quat hand_r = rot * arms.grip_r(pistol);
  const V3 wrist_r = pos - rotate(hand_r, arms.palm_offset(Side::R));
  const V3 pole_r = rotate(chest_q, vnorm(pistol ? V3{0.5, -0.2, -0.9} : V3{0.7, -0.3, -0.75}));
  pose.r[H::clavicleR] = qeuler(0, 0, (pistol ? 0.08 : 0.12) * rw);
  pose.r[H::clavicleL] = qeuler(0, 0, -(pistol ? 0.08 : 0.18) * rw);
  fk.update(pose, H::clavicleL);
  solve_two_bone(pose, fk, H::upperarmR, H::forearmR, H::handR, wrist_r, pole_r, 0.02, &kElbowRest);
  set_model_rotation(pose, fk, H::handR, hand_r);
  if (two_handed) {
    const V3 support = pos + rotate(rot, prop.support);
    const Quat hand_l = rot * arms.grip_l(pistol);
    const V3 wrist_l = support - rotate(hand_l, arms.palm_offset(Side::L));
    const V3 pole_l = rotate(chest_q, vnorm(pistol ? V3{-0.5, -0.2, -0.9} : V3{-0.7, 0.1, -0.8}));
    solve_two_bone(pose, fk, H::upperarmL, H::forearmL, H::handL, wrist_l, pole_l, 0.04, &kElbowRest);
    set_model_rotation(pose, fk, H::handL, hand_l);
  }
  return true;
}

}  // namespace svx::anim
