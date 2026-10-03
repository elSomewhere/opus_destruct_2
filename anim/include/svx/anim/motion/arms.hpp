// svx_anim — arms and hands of the motion plan: two-bone arm IK to a palm target with a hand
// orientation (the canonical fist frame), and the held weapon: the prop at the rig's weapon
// socket in its carry (relaxed, low ready, shouldered, from the hip, port arms; a pistol lowered,
// at low ready, two-handed or one-handed), recoil and sway, with both hands put on it.
#pragma once

#include <array>
#include <optional>

#include "svx/anim/characters/props.hpp"
#include "svx/anim/ik.hpp"
#include "svx/anim/motion/actions.hpp"
#include "svx/anim/spring.hpp"

namespace svx::anim {

enum class Carry : u8 { Relaxed, Ready, Aim, Hip };
enum class Side : u8 { L, R };

// Where an elbow points in the rest pose (back: the forearm bends forward). The arm IK twists the
// upper arm with it, so the elbow always bends as a hinge (the physical body's elbow can follow
// every planned arm).
constexpr V3 kElbowRest{0, -1, 0};

// The arms of a pose (the plan's pose and its model-space transforms, which it works on).
class ArmRig {
 public:
  ArmRig(SkeletonPtr skeleton, Pose* pose, ModelFK* fk);
  SkeletonPtr skeleton;
  Pose* pose;
  ModelFK* fk;
  // The rotation from a hand's rest frame to the canonical fist frame (knuckles +y, palm -z).
  const Quat& canonical(Side side) const { return canon_[size_t(side)]; }
  // The palm centre relative to the wrist in rest space.
  const V3& palm_offset(Side side) const { return palm_[size_t(side)]; }
  // Puts a hand's palm at `target` (model space), oriented `rot` (model, the canonical fist
  // frame) or left as it is, the elbow towards `pole`, blending the arm by w.
  void hand_ik(Side side, const V3& target, const std::optional<Quat>& rot, const V3& pole, f64 w, f64 soft = 0.03);
  void begin_frame();  // retain the previous arm pose for continuous quaternion blending
  void finish_frame(f64 dt, f64 max_rate = 36); // bound requested joint speed across IK/behaviour handoffs
  // Grip frames: the hand's rotation relative to the prop.
  Quat grip_r(bool pistol) const;
  Quat grip_l(bool pistol) const;

 private:
  std::array<Quat, 2> canon_;
  std::array<V3, 2> palm_;
  std::array<std::array<Quat, 3>, 2> previous_{};
};

// What the weapon hold needs to know each frame.
struct HoldContext {
  Carry carry = Carry::Relaxed;
  std::optional<V3> aim_at;  // the aim target in model space
  bool aiming = false;
  bool moving = false;
  f64 run = 0.0;
  f64 phase = 0.0;
  f64 aim_w = 0.0;  // the aim weight 0..1 (eased)
  f64 k = 1.0;
  // the channels of the posture and one-shot action layers, with their weights
  const ChannelFrame* ch_p = nullptr;
  f64 w_p = 0.0;
  const ChannelFrame* ch_a = nullptr;
  f64 w_a = 0.0;
  bool primary_left = false;
  bool free_left = false;  // let go with the support hand (it has something else to do)
};

// The held weapon: its carry pose at the socket, recoil and sway, and both hands on it. Knives (and
// lowered pistols) sit in the right hand instead.
class WeaponHold {
 public:
  Spring ready_w{6.5, 1.0};
  Spring hip_w{9.0, 1.0};
  Spring sprint_w{6.0, 1.0};
  Spring kick_back{32.0, 0.55};
  Spring kick_pitch{26.0, 0.5};
  // The recoil of one shot.
  void fire(const Prop& prop, f64 strength);
  // Places the socket and the hands. True when the prop is held at the socket (both hands, or the
  // gun hand, busy with it).
  bool hold(f64 dt, ArmRig& arms, const Prop& prop, const HoldContext& c);

 private:
  Spring3 sway_{7.0, 0.7};
};

}  // namespace svx::anim
