// svx_anim — the humanoid rig: a 23-bone skeleton (fixed bone indices, H) scaled by a build.
//
// Rest pose (model space): a relaxed A-pose, arms about 17 degrees out from the body, feet a hand
// apart, so sculpted limbs stay clear of the torso. `weapon` is a socket (no voxels): props are
// separate models drawn at its transform, the hands put on them by IK.
#pragma once

#include "svx/anim/skeleton.hpp"

namespace svx::anim {

namespace H {
enum : i32 {
  root = 0,
  pelvis = 1,
  spine = 2,
  chest = 3,
  neck = 4,
  head = 5,
  clavicleL = 6,
  upperarmL = 7,
  forearmL = 8,
  handL = 9,
  clavicleR = 10,
  upperarmR = 11,
  forearmR = 12,
  handR = 13,
  thighL = 14,
  shinL = 15,
  footL = 16,
  toeL = 17,
  thighR = 18,
  shinR = 19,
  footR = 20,
  toeR = 21,
  weapon = 22,
  count = 23,
};
}  // namespace H

// Left/right bone pairs (for mirroring).
extern const i32 kHMirror[8][2];

struct HumanoidBuild {
  f64 height = 1.0;     // overall scale; 1 = 1.78 m
  f64 shoulders = 1.0;  // shoulder width scale
  f64 hips = 1.0;       // hip width scale
  f64 girth = 1.0;      // body thickness (sculpting only)
};

SkeletonPtr humanoid_skeleton(const HumanoidBuild& build = HumanoidBuild{});
// A one-bone skeleton for props (weapons): the prop's origin is its grip.
SkeletonPtr prop_skeleton();

}  // namespace svx::anim
