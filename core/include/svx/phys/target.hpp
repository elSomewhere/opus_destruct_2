// structvox — targets (docs/MOTION.md §6): a body's point pulled towards a point of the world,
// or the body turned towards a rotation of the world, by a spring and a damper of limited force:
// the "hand of god" a character's balance is held with (the legs' support of the pelvis, a
// planted foot, a hand on a wall), a grab, a magnet. Soft constraints (implicit: stable at any
// stiffness), solved with the contacts and joints; the targets move each tick as their host
// says.
#pragma once

#include "svx/base/vec.hpp"

namespace svx {

// What a target pulls with (the host's, per tick).
struct TargetDrive {
  bool on = false;
  // Point: where the body's point is pulled to (world), and how fast that moves (the damping
  // works towards it); the world axes it acts on (support: z alone; steering: x and y).
  V3 pos, vel;
  bool axes[3] = {true, true, true};
  // Rotation: the world rotation the body is turned to; tilt_only: only its `up` axis (body
  // frame) is turned onto the target's, the heading is free (an upright trunk).
  Quat rot;
  bool tilt_only = false;
  V3 up{0, 0, 1};
  f64 stiffness = 0.0;  // N/m, N m/rad (<= 0: none; a very large one is rigid)
  f64 damping = 0.0;    // N s/m, N m s/rad
  f64 max = 0.0;        // N, N m: the most its spring gives, and its damper (0: unlimited)
};

struct Target {
  enum class Kind : u8 {
    Point,     // a point of the body to a point of the world
    Rotation,  // the body to a rotation of the world
  };
  u32 id = 0;
  i64 body = 0;       // the body (Body::id; not found: skipped)
  Kind kind = Kind::Point;
  V3 local;           // (Point) the point in the body frame, relative to its centre of mass
  TargetDrive drive;
  i64 reference_body = 0;  // point target attached to another body; zero means world
  V3 reference_local;
  // the solver's state: the accumulated impulses of its spring and its damper (world: N s, N m s;
  // warm start), and what its spring gave the body in the last substep (N, N m)
  V3 imp, imp_d;
  f64 step = 0.0;     // (a link's) the step (s) they are of: a fine step's or a substep's
  V3 applied;
};

}  // namespace svx
