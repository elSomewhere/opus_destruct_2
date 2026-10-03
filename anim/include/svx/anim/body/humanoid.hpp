// svx_anim — the physical humanoid: the rig as 16 rigid bodies joined by 15 muscled joints
// (physics/rigid.hpp), the body a character is made of.
//
// - Bodies: pelvis, abdomen (spine), chest (with the clavicles), head (with the neck), upper
//   arms, forearms, hands, thighs, shins, feet. Masses follow anthropometric segment fractions
//   (Dempster/Winter) of the character's weight, with centres of mass along the segments, box and
//   rod inertias, and collision spheres shaped like the voxel body.
// - Joints have human ranges: the spine and neck bend further forward than back, hips flex far
//   forward and little back, shoulders swing wide but not far behind, knees and elbows are hinges
//   that bend one way, ankles and wrists give a little.
// - Muscles: every joint drives towards the relative rotation of a target pose (the motion plan),
//   with a stiffness set from the inertia it moves (so every joint answers at a chosen frequency),
//   scaled by a per-joint `tone` a controller sets (a shot leg goes weak, a dying body goes limp),
//   and gravity compensation (muscles hold the limbs' weight, so low tone still tracks the pose;
//   without tone the limbs drop).
// - Assists: the pelvis held up (vertical), steered (horizontal) and turned (the balance a
//   controller grants the legs), planted feet pinned, hands pulled to targets (a grip, a wall).
//
// A body's frame is its primary bone's frame (every bone's rest rotation is the identity), its
// position the centre of mass: bone head = x - q (com - rest head).
//
// Where it is simulated is the character's business: stepped on its own (system.step: the
// shallow path) or as a core articulation (physics/core_binding.hpp: the deep path).
#pragma once

#include <array>
#include <vector>

#include "svx/anim/physics/rigid.hpp"
#include "svx/anim/rig.hpp"
#include "svx/anim/skeleton.hpp"

namespace svx::anim {

// Body indices.
namespace B {
enum : i32 {
  pelvis = 0,
  spine = 1,
  chest = 2,
  head = 3,
  upperarmL = 4,
  forearmL = 5,
  handL = 6,
  upperarmR = 7,
  forearmR = 8,
  handR = 9,
  thighL = 10,
  shinL = 11,
  footL = 12,
  thighR = 13,
  shinR = 14,
  footR = 15,
};
}  // namespace B
constexpr i32 kBodyCount = 16;

// The rig bone each body moves, and its name ("left upper arm").
extern const std::array<i32, kBodyCount> kBodyBone;
extern const std::array<const char*, kBodyCount> kBodyName;
// Each body's parent body (-1: the pelvis). Joint i connects kBodyParent[i] and body i (i >= 1).
extern const std::array<i32, kBodyCount> kBodyParent;

// Body regions, for tone.
enum class Region : u8 { Trunk, Neck, ArmL, ArmR, LegL, LegR };
constexpr i32 kRegionCount = 6;
extern const std::array<Region, kBodyCount> kRegion;

struct HumanoidBodyOptions {
  f64 mass = 0.0;   // total mass (kg); 0: 75 kg scaled with the rig's height cubed
  f64 girth = 1.0;  // body thickness (collision radii)
  // the arm muscles {natural frequency rad/s, damping ratio}: shoulder, elbow, wrist (CharacterProfile)
  std::array<std::array<f64, 2>, 3> arm_muscles{{{16, 1.05}, {19, 1.05}, {28, 1.15}}};
  bool solver_clamps = true;  // (CharacterProfile::solver_clamps)
};

class HumanoidBody {
 public:
  HumanoidBody(SkeletonPtr sk, const CollisionWorld* collision, const HumanoidBodyOptions& o = {});
  HumanoidBody(const HumanoidBody&) = delete;
  HumanoidBody& operator=(const HumanoidBody&) = delete;

  SkeletonPtr skeleton;
  RigidSystem system;
  std::array<RigidBody*, kBodyCount> parts{};
  std::array<Joint*, kBodyCount> joints{};  // joints[i] connects kBodyParent[i] and body i (joints[0]: none)
  std::array<f32, kBodyCount> tone{};         // muscle tone per body's joint (0 limp .. 1 normal .. >1 tense)
  std::array<f32, kBodyCount> hold_weight{};  // gravity compensation per joint (0..1): the muscles hold the limb's weight
  // base stiffness (N m/rad) and damping (N m s/rad) per joint at tone 1
  std::array<f32, kBodyCount> base_stiffness{}, base_damping{};
  std::array<f32, kBodyCount> inertia_at{};  // inertia each joint moves (its subtree about the joint, kg m^2)
  // How much of the plan's acceleration the muscles supply ahead of the error (0..1). Off:
  // differentiated twice from frame to frame (and from a plan the physics feeds back into), it is
  // mostly noise, and the body trembles with it.
  f64 feed_forward = 0.0;
  std::array<V3, kBodyCount> com_local{};  // centre of mass relative to the primary bone's head, per body (rest model space)
  f64 total_mass = 0.0;
  f64 k = 1.0;
  // assists
  Attachment* support = nullptr;
  Attachment* steer = nullptr;
  Orienter* upright = nullptr;
  Orienter* chest_turn = nullptr;
  std::array<Attachment*, 2> feet{};
  std::array<Orienter*, 2> feet_turn{};
  std::array<Attachment*, 2> hands{};
  std::array<Orienter*, 2> hands_turn{};

  std::vector<i32> subtree(i32 i) const;  // bodies below (and including) body i
  V3 joint_point(i32 i) const;            // the rest position of joint i (model space)
  f64 sole_drop() const;                  // sole height below the ankle at rest

  // ---- pose <-> bodies
  // Places the bodies on a pose (`world`), with velocities from the pose `dt` seconds earlier
  // (`prev`), so a body taking over from animation keeps its momentum.
  void set_from_pose(const WorldPose& world, const WorldPose* prev, f64 dt);
  V3 body_at(const WorldPose& world, i32 i) const;  // where body i's centre of mass is in a pose
  // Muscle targets from a target pose (world rotations of the bones; only relative rotations
  // matter) and, from the target `dt` seconds before, how fast they move; and the local rotations
  // the physics lacks (clavicles, the neck's share, toes).
  void track(const WorldPose& target, const Pose& local, const WorldPose* prev = nullptr, f64 dt = 0.0, f64 max_rate = 30.0);
  void refresh_mass();
  void apply_tone();          // muscle stiffness and damping from tone
  void compensate_gravity();  // feed-forward muscle torques that hold each limb's weight (scaled by hold_weight)
  // Writes the bodies' pose into `out` (all 23 bones): bodies give their bones; the neck takes a
  // share of the head's turn; clavicles, toes and the root follow their parents with the plan's
  // local rotations.
  void write_pose(WorldPose& out) const;

  // ---- sensing
  V3 com() const;           // centre of mass (world)
  V3 com_velocity() const;  // its velocity
  // Adds a velocity change `dv` to every body (a shove that moves the whole body), shared per body.
  void shove(const V3& dv, const f64* share = nullptr);
  // The body's collision spheres in the world (appended to `out`), from the bodies or, when given,
  // from a pose (a body resting on its plan): what others bump into and trip over. `owner` marks
  // them as this body's.
  void spheres_of(const WorldPose* pose, std::vector<Obstacle>& out, i32 owner) const;
  i32 nearest_part(const V3& p) const;  // the body whose collision spheres are nearest to a world point
  static i32 body_of_bone(i32 bone);    // the body that moves a rig bone (the neck and clavicles belong to their neighbours)
  i64 memory_bytes() const;

 private:
  std::array<std::array<f64, 2>, kBodyCount> muscle_{};  // per joint: natural frequency (rad/s), damping ratio
  // the plan's relative angular acceleration per joint (parent frame, rad/s^2)
  std::array<V3, kBodyCount> target_acc_{};
  // local rotations the physics does not have (neck share, clavicles, toes), from the plan
  Pose extras_;
};

}  // namespace svx::anim
