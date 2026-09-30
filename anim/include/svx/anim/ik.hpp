// svx_anim — inverse kinematics on a Pose, in the character's model space (root at the origin,
// facing +y): an analytic two-bone solver (legs, arms) with a pole direction for the middle joint,
// and helpers that set a bone's model-space rotation or aim it. ModelFK keeps the model-space
// transforms of a pose up to date for the bones the solvers touch.
#pragma once

#include "svx/anim/skeleton.hpp"

namespace svx::anim {

// Model-space bone transforms of a Pose (root at the origin, identity rotation).
class ModelFK {
 public:
  explicit ModelFK(SkeletonPtr sk);
  SkeletonPtr skeleton;
  std::vector<V3> p;
  std::vector<Quat> q;
  ModelFK& update(const Pose& pose, i32 from = 0);  // (bones from `from` to the end)
  void update_bone(const Pose& pose, i32 i);
  void update_subtree(const Pose& pose, i32 i);     // (a bone and everything below it)
};

// Sets bone i's local rotation so that its model-space rotation becomes q.
void set_model_rotation(Pose& pose, ModelFK& fk, i32 i, const Quat& q);
// The rotation taking the frame (a, b) to (a2, b2): a exactly to a2, b to b2's part square to a2.
Quat frame_rotation(const V3& a, const V3& b, const V3& a2, const V3& b2);

struct TwoBoneResult {
  V3 mid, end;     // model-space middle and end joints after solving
  f64 reach = 0.0; // target distance / chain length (>= 1: out of reach, straight)
};

// Rotates `upper` and `lower` so that `end` reaches `target`, bending the middle joint towards
// `pole` (model space). `soft` eases the last part of the reach (no knee pop). `rest_pole`: where
// the joint points in the rest pose (a knee: forward) - the whole limb twists towards the pole;
// null: the pole is its own rest reference.
TwoBoneResult solve_two_bone(Pose& pose, ModelFK& fk, i32 upper, i32 lower, i32 end, const V3& target, const V3& pole, f64 soft = 0.03,
                             const V3* rest_pole = nullptr);
// Turns bone i (model space) so that its rest `axis` points along `dir`, keeping its rest `up` as
// near `up_wanted` as it can; weight blends from the current rotation.
void aim_bone(Pose& pose, ModelFK& fk, i32 i, const V3& axis, const V3& up, const V3& dir, const V3& up_wanted, f64 weight = 1.0);

}  // namespace svx::anim
