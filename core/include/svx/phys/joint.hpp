// structvox — joints between rigid bodies (docs/MOTION.md): ball, hinge, slider, fixed and
// distance (rope, rod) constraints, with limits, drives (motors and servos: a machine's program)
// and a breaking strength. They are solved with the contacts (sequential impulses, warm-started;
// their position error by split impulse, like the contacts' penetration: it adds no energy).
//
// An end of a joint is a rigid body, or fixed: the static world, which no impulse moves. The
// World fills the ends each substep from what they are anchored to (world/world.hpp: add_joint).
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

// A joint's drive: a motor of limited strength moving a hinge's turn (rad) or a slider's move (m),
// at a speed or to a target (a servo: it asks for the target's own rate and `stiffness` times the
// error, at most `speed`). A target that moves with time is a machine's program: the lift that
// rises and falls, the drawbridge, the crane's swing - as the world's clock runs, the same in
// every session.
struct JointDrive {
  enum class Kind : u8 {
    Off,        // free (a limit may still bear)
    Speed,      // at `speed`
    Target,     // to `target`, held there
    Oscillate,  // from `target` to `target2` and back every `period` s (eased), `phase` s in
  };
  Kind kind = Kind::Off;
  f64 speed = 1.0;      // rad/s, m/s: its speed (Speed), the fastest it goes (Target, Oscillate)
  f64 max = 0.0;        // N m, N: the most it gives (0: nothing)
  f64 target = 0.0, target2 = 0.0;
  f64 period = 10.0, phase = 0.0;
  f64 stiffness = 4.0;  // 1/s
  // Its target and the target's rate at time t (s of the world's clock).
  void goal(f64 t, f64* x, f64* rate) const;
};

// An end of a joint, as the solver sees it.
struct JointEnd {
  i64 body = 0;  // a rigid body (Body::id); 0: fixed (the static world)
  // The anchor, and the joint's axis and reference direction (square to the axis: a hinge's angle
  // is b's reference turned from a's) at this end: a body's in its shape frame (the anchor relative
  // to its centre of mass: in the world x + R p), a fixed end's in the world.
  V3 p;
  V3 axis{0, 0, 1}, ref{1, 0, 0};
  Quat q;         // (fixed) its frame's rotation in the world (a body's is Body::q)
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
  // axis from a's anchor, m) and a drive of that motion.
  bool limited = false;
  f64 lower = 0.0, upper = 0.0;
  JointDrive drive;
  f64 break_force = 0.0, break_torque = 0.0;  // it gives way beyond (N, N m; 0: never)
  f64 break_angle = 0.0;  // (hinge) it gives way turned this far from its start, either way (rad; 0: never)
  bool collide = true;    // its ends' bodies collide with each other (a welded part: not)
  Quat rel;  // (fixed, slider) b's rotation relative to a's that it holds: conj(qa) qb

  // the solver's state
  V3 lin, ang;                  // accumulated impulses on b (warm start): point / square rows, angular rows
  f64 axial = 0.0, limit = 0.0;  // ... along the axis (distance: its row) and the limit's
  f64 motor = 0.0;              // ... the drive's
  // the last substep: the force and torque b received through it (a: the opposite; N, N m), its
  // hinge angle / slider offset / distance, and whether it gave way
  V3 force, torque;
  f64 value = 0.0;
  bool broken = false;
};

}  // namespace svx
