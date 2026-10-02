// structvox — articulations (docs/MOTION.md §6): bodies made of links - rigid bodies of no voxels
// that collide as spheres - held together by joints with anatomical limits (a ball's cone and
// twist, a hinge's range) and muscles (drives to a relative rotation), and pulled by targets
// (soft pulls to a point or a rotation of the world). A character's body is one: its host drives
// the muscles and the targets from the pose it means to have, and reads back where the links are
// and what they felt. So is a robot, a creature, a rag doll, a chain of anything.
//
// They are solved with everything else: their links stand on the structures and load them, are
// knocked by what hits them (a falling slab, a vehicle), push what they meet (debris, each other);
// sleeping, they are rubble at rest. One that touches no awake piece is stepped finer on its own
// (RigidParams::link_substeps). They are saved with sessions and, in a streamed world, archived
// with their region when they rest out of range, their host data with them.
#pragma once

#include <utility>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/phys/joint.hpp"
#include "svx/phys/rigid.hpp"
#include "svx/phys/target.hpp"

namespace svx {

// Articulations: ids from 1.
using ArticulationId = u32;

// A link of an articulation.
struct LinkDesc {
  f64 mass = 1.0;                   // kg (0: kinematic - moved by its host at its velocity, never pushed)
  V3 inertia{0.01, 0.01, 0.01};     // kg m^2: principal moments about the centre of mass, along the body frame's axes
  V3 pos;                           // its centre of mass (world)
  Quat rot;                         // body frame -> world
  V3 vel, ang;                      // (world)
  std::vector<BodySphere> spheres;  // (body frame, relative to the centre of mass)
  f64 friction = 0.75;
  // tissue: a spin about this axis (body frame) dies away at twist_damping (1/s) - a light limb
  // does not spin freely about its length
  V3 long_axis{0, 0, 1};
  f64 twist_damping = 0.0;
};

// A joint of an articulation, between its links `parent` (the joint's a) and `child` (b).
struct ArticulationJointDesc {
  u16 parent = 0, child = 0;
  JointType type = JointType::Ball;  // Ball, Hinge or Fixed
  // where, in each link's body frame (relative to its centre of mass): the same point of the world
  // in the pose the links are made in
  V3 anchor_parent, anchor_child;
  // The joint's frame in each link's body frame: z its twist axis (the child's bone), x a hinge's
  // axis. The two frames coincide in the pose the links are made in.
  Quat frame_parent, frame_child;
  // (ball) a cone: how far the child's z may tilt from the parent's towards +x, -x, +y, -y (rad,
  // an elliptical cone between); and the twist about z, in [twist_lower, twist_upper]
  bool swing_limited = false;
  f64 swing[4] = {0.0, 0.0, 0.0, 0.0};
  bool twist_limited = false;
  f64 twist_lower = 0.0, twist_upper = 0.0;
  // (hinge) about x: the child's z turned from the parent's (rad)
  bool hinge_limited = false;
  f64 hinge_lower = 0.0, hinge_upper = 0.0;
};

// A target of an articulation (what it pulls with: TargetDrive, set every tick by its host).
struct ArticulationTargetDesc {
  u16 link = 0;
  Target::Kind kind = Target::Kind::Point;
  V3 local;  // (Point) the point, in the link's body frame relative to its centre of mass
};

struct ArticulationDesc {
  std::vector<LinkDesc> links;
  std::vector<ArticulationJointDesc> joints;
  std::vector<ArticulationTargetDesc> targets;
  std::vector<std::pair<u16, u16>> collide;  // the link pairs that collide (the others of it do not)
  // Host data, saved and archived with it: what it is to its host (a character: its kind, look,
  // wounds). A host finds its articulations again from it after a session is loaded or one comes
  // back from the streaming archive.
  u32 group = 0, tag = 0;
  std::vector<u8> data;
};

// What drives an articulation, set by its host between ticks (World::articulation_control).
struct ArticulationControl {
  std::vector<JointMuscle> muscles;  // per joint of its desc
  std::vector<TargetDrive> targets;  // per target of its desc
  std::vector<V3> force, torque;     // per link: N at its centre of mass, N m - during the next tick only
  std::vector<u8> ghost;             // per link: it passes through other bodies (a limb that strikes)
  // per joint: where it holds on to its parent and its child (their body frames, relative to their
  // centres of mass), for a host that moves it (a shoulder that follows its clavicle); left empty,
  // or not finite: where it was made
  std::vector<V3> anchor_parent, anchor_child;
  // Per point target: a movable attachment point in its link's body frame.
  // Missing or invalid entries leave the point unchanged (heel -> toe in a gait).
  std::vector<V3> target_local;
  f64 max_spin = 80.0;               // rad/s: no link spins faster (a limp body's: much less)
  f64 keep_linear = 0.98, keep_angular = 0.9;  // velocity kept per second (air, tissue; a body at rest settles with less)
  bool self_collide = true;          // its links collide with each other as its desc says
  bool can_sleep = false;            // it may sleep once still (at rest; alive, its host keeps it awake)
};

// A link now (World::articulation_state).
struct LinkState {
  V3 pos;       // centre of mass
  Quat rot;     // body frame -> world
  V3 vel, ang;
  f64 mass = 0.0;
  bool gone = false;
  // what it felt over the last tick: it touched something (the hardest touch's normal, towards
  // the link, and where), its largest contact impulse, how hard other bodies pushed it sideways
  bool contact = false;
  V3 contact_normal{0, 0, 1}, contact_point;
  f64 impact = 0.0, bumped = 0.0;
};

struct ArticulationState {
  std::vector<LinkState> links;
  std::vector<V3> target_applied;  // per target: what it gave its link in the last substep (N, N m)
  bool asleep = false;
  u32 group = 0, tag = 0;
};

}  // namespace svx
