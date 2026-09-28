// structvox — joints between rigid bodies (docs/MOTION.md §2): ball, hinge, slider, fixed and
// distance (rope, rod) constraints, with limits, motors and a breaking strength. They are solved
// with the contacts (sequential impulses, warm-started; their position error by split impulse,
// like the contacts' penetration: it adds no energy).
//
// An end of a joint is a rigid body, or a frame that moves by itself: the static world, or a
// kinematic body (its velocity field), which no impulse moves. The World fills the ends each
// substep from what they are anchored to (world/world.hpp: add_joint).
#pragma once

#include "svx/base/vec.hpp"

namespace svx {

enum class JointType : u8 {
  Ball,      // the ends' points coincide (turns every way)
  Hinge,     // ... and their axes stay aligned (turns about the axis)
  Slider,    // the ends keep their relative rotation, b moves along a's axis only
  Fixed,     // the ends keep their relative place and rotation
  Distance,  // the points keep a distance in a range: a rope [0, L] (pulls only), a rod [L, L]
};

// An end of a joint, as the solver sees it.
struct JointEnd {
  i64 body = 0;  // a rigid body (Body::id); 0: a frame (the static world, a kinematic body)
  // The anchor, and the joint's axis and reference direction (square to the axis: a hinge's angle
  // is b's reference turned from a's) at this end: a body's in its shape frame (the anchor relative
  // to its centre of mass: in the world x + R p), a frame's in the world, now.
  V3 p;
  V3 axis{0, 0, 1}, ref{1, 0, 0};
  Quat q;         // (a frame) its rotation in the world now (a body's is Body::q)
  V3 v, w, c;     // (a frame) its velocity field: v + w x (X - c)
};

struct Joint {
  u32 id = 0;
  JointType type = JointType::Ball;
  JointEnd a, b;
  f64 min_length = 0.0, max_length = 0.0;  // (distance)
  // (distance) a rope or rod that stretches: a spring of this stiffness (N/m) and damping
  // (N s/m) beyond its range (0: rigid). A shock (a ball stopped by a wall) is spread over the
  // time it stretches, not passed on in one substep.
  f64 stiffness = 0.0, damping = 0.0;
  // Limits (hinge: b's turn about the axis from a's reference, rad; slider: b's offset along a's
  // axis from a's anchor, m) and a motor driving that motion (rad/s, N m; m/s, N).
  bool limited = false;
  f64 lower = 0.0, upper = 0.0;
  bool motor = false;
  f64 motor_speed = 0.0, motor_max = 0.0;
  f64 break_force = 0.0, break_torque = 0.0;  // it gives way beyond (N, N m; 0: never)
  Quat rel;  // (fixed, slider) b's rotation relative to a's that it holds: conj(qa) qb

  // the solver's state
  V3 lin, ang;                  // accumulated impulses on b (warm start): point / square rows, angular rows
  f64 axial = 0.0, limit = 0.0;  // ... along the axis (distance: its row) and the limit's
  f64 drive = 0.0;              // ... the motor's
  // the last substep: the force and torque b received through it (a: the opposite; N, N m), its
  // hinge angle / slider offset / distance, and whether it gave way
  V3 force, torque;
  f64 value = 0.0;
  bool broken = false;
};

}  // namespace svx
