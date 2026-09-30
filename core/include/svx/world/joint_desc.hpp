// structvox — ids of grids and joints, and a joint as a host or a chunk source describes it
// (World::add_joint, ChunkSource::joints; docs/MOTION.md).
#pragma once

#include "svx/base/vec.hpp"
#include "svx/phys/joint.hpp"

namespace svx {

// Grids (docs/GRIDS.md): the world grid is kWorldGrid; oriented grids have ids from 1.
using GridId = u32;
constexpr GridId kWorldGrid = 0;

// Joints (docs/MOTION.md): ids from 1. (JointType, JointDrive: phys/joint.hpp)
using JointId = u32;

// What an end of a joint holds on to (World::add_joint).
struct JointAnchor {
  enum class Kind : u8 {
    World,  // a point fixed in the world
    Grid,   // a voxel of a grid (kWorldGrid too): its structure holds it, and it goes with the piece
            // it breaks off in
    Piece,  // a voxel of a piece: it goes with the part it is in when the piece breaks
    Link,   // a link of an articulation (id: World::link_body): a point that moves with it
  };
  Kind kind = Kind::World;
  u64 id = 0;  // the grid or piece
  V3 point;    // where, in the world now (Grid, Piece: in a solid voxel of it, or next to one)
};

struct JointDesc {
  JointType type = JointType::Ball;
  JointAnchor a, b;
  V3 axis{0, 0, 1};  // (hinge, slider) in the world now
  // (distance) its length: a rope's longest (it only pulls), a rod's (held both ways); < 0: the
  // ends' distance now
  f64 length = -1.0;
  bool rope = true;
  // (distance) how it stretches beyond its length: a spring (N/m) with damping (N s/m); 0: rigid.
  // A wire rope of a crane stretches (a ball stopped by a wall pulls on the jib over the time
  // it takes, not in one substep); a bungee much more.
  f64 stiffness = 0.0, damping = 0.0;
  // limits: (hinge) b's turn about the axis from now (rad); (slider) b's move along it from now (m)
  bool limited = false;
  f64 lower = 0.0, upper = 0.0;
  // a drive of that motion (phys/joint.hpp): a motor of limited strength at a speed, or to a
  // target that may follow a program of the world's clock (a machine)
  JointDrive drive;
  f64 break_force = 0.0, break_torque = 0.0;  // it gives way beyond (N, N m; 0: never)
  // Whether its two ends' pieces collide with each other (a car's door welded into its frame:
  // they do not, until the weld gives way).
  bool collide = true;
  // (hinge) It gives way once turned this far from where it was made, either way (rad; 0: never):
  // a plastic hinge's rotation capacity, a door torn off its hinge.
  f64 break_angle = 0.0;
  // (hinge) A latch holds it shut - it does not turn at all - until the torque about its axis
  // passes this (N m; 0: none); then it swings within its limits (a car's door, its bonnet). What
  // the latch holds is not its hinge's to break on.
  f64 latch = 0.0;
};

// Wheels (docs/VEHICLES.md): ids from 1.
using WheelId = u32;

// A wheel as a host describes it (World::add_wheel): a cast wheel on a sprung suspension with a
// tyre, hung from a chassis (phys/wheel.hpp).
struct WheelDesc {
  // What it hangs from, and where: the top of its suspension. A Grid anchor holds a voxel of a
  // grid (the chassis, dropped in as a grid of free voxels) and goes with the piece it becomes; a
  // Piece anchor a voxel of a piece. The wheel comes off when that voxel is gone (crushed, carved)
  // or its force passes break_force: it turns into a wheel-shaped piece (WheelDetached).
  JointAnchor mount;
  V3 down{0, 0, -1};  // the suspension's axis (in the world now)
  V3 axle{0, 1, 0};   // its spin axis at zero steer (in the world now): it rolls along down x axle
  f64 radius = 0.33, width = 0.22;  // m
  f64 rest = 0.35;       // m: the suspension's length at full droop (the spring's natural length)
  f64 travel = 0.2;      // m: how far it compresses to the bump stop
  f64 stiffness = 35e3;  // N/m
  f64 damping = 3.5e3;   // N s/m
  f64 inertia = 1.2;     // kg m^2: the wheel's (and its driveline's) spin inertia
  f64 grip = 1.0;        // x the surface's tyre friction
  f64 break_force = 0.0; // N: it comes off beyond (0: never)
  // Host data, saved with it: which vehicle it belongs to (its group) and what it is to the host
  // (its tag: front left, driven, ...). A host finds its vehicles again from them after a
  // session is loaded or a vehicle comes back from the streaming archive.
  u32 group = 0, tag = 0;
};

}  // namespace svx
