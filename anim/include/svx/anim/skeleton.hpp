// svx_anim — skeletons, poses and their world transforms.
//
// Model space: +x right, +y forward, +z up, metres; the origin on the ground between the feet.
// Every bone's rest rotation is the identity, so joint frames are aligned with model space in the
// rest pose, and a bone's rest local translation is its head minus its parent's head. A Pose holds
// each bone's local translation and rotation (relative to its parent; the root's to the character
// origin); a WorldPose each bone's head position and rotation in the world.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "svx/anim/math.hpp"

namespace svx::anim {

struct BoneDef {
  std::string name;
  std::string parent;  // (empty: the root - exactly one, the first; parents come before children)
  V3 head, tail;       // rest positions of the joint and of the bone's end (model space)
};

class Skeleton {
 public:
  explicit Skeleton(const std::vector<BoneDef>& defs);
  i32 count = 0;
  std::vector<std::string> names;
  std::vector<i32> parents;    // (-1: the root)
  std::vector<V3> rest_head, rest_tail;
  std::vector<V3> rest_local;  // head - parent's head (the root's: its head)
  std::vector<std::vector<i32>> children;
  i32 index(const std::string& name) const;  // (-1: none)
  f64 length(i32 i) const { return norm(rest_tail[size_t(i)] - rest_head[size_t(i)]); }
  bool is_below(i32 i, i32 ancestor) const {
    for (i32 b = i; b >= 0; b = parents[size_t(b)])
      if (b == ancestor) return true;
    return false;
  }
};

using SkeletonPtr = std::shared_ptr<const Skeleton>;

// Local joint transforms.
class Pose {
 public:
  explicit Pose(SkeletonPtr sk);
  SkeletonPtr skeleton;
  std::vector<V3> t;
  std::vector<Quat> r;
  Pose& reset();
  Pose& copy_from(const Pose& p);
  // this = lerp(this, p, w) per bone (w in 0..1), optionally weighted per bone
  Pose& blend(const Pose& p, f64 w, const std::vector<f32>* mask = nullptr);
  Pose& rotate_local(i32 i, const Quat& q) {  // (post-multiplied: in the joint's own frame)
    r[size_t(i)] = r[size_t(i)] * q;
    return *this;
  }
  Pose& rotate_parent(i32 i, const Quat& q) {  // (pre-multiplied: in the parent's frame)
    r[size_t(i)] = q * r[size_t(i)];
    return *this;
  }
};

// Bone heads and rotations in the world.
class WorldPose {
 public:
  explicit WorldPose(SkeletonPtr sk);
  SkeletonPtr skeleton;
  std::vector<V3> p;
  std::vector<Quat> q;
  // Forward kinematics of `pose` for a character placed at (root_pos, root_rot).
  WorldPose& compute(const Pose& pose, const V3& root_pos, const Quat& root_rot);
  WorldPose& copy_from(const WorldPose& w);
  // The world position of a point given in bone i's rest model space (its rest tail, ...).
  V3 point_of(i32 i, const V3& rest_point) const { return p[size_t(i)] + rotate(q[size_t(i)], rest_point - skeleton->rest_head[size_t(i)]); }
  V3 tail(i32 i) const { return point_of(i, skeleton->rest_tail[size_t(i)]); }
  // Skin matrices (16 floats per bone, column-major): rest model-space point x -> q_i (x - rest
  // head_i) + p_i.
  void write_skin(f32* out) const;
};

}  // namespace svx::anim
