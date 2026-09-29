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
};

}  // namespace svx
