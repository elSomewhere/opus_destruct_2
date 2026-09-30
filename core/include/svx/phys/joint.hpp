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

// A muscle (docs/MOTION.md §6): a drive of a ball's or a hinge's relative rotation towards a
// target - a spring and a damper of limited torque between its two ends, solved as a soft
// constraint (implicit: stable at any stiffness). A character's joints track the pose its motion
// means to have with them; tone scales their stiffness. A hinge's is its turn about the axis.
struct JointMuscle {
  Quat target;             // b's rotation relative to a's that it drives to (body frames: qb = qa target)
  V3 target_rate;          // how fast that target turns (b relative to a, in a's body frame, rad/s):
                           // the damping works towards it, so a joint follows a moving target
  f64 stiffness = 0.0;     // N m/rad (0: no spring)
  f64 damping = 0.0;       // N m s/rad (0: no damper)
  f64 max_torque = 0.0;    // N m: the most it gives (0: unlimited)
  // The inertia the joint really moves (kg m^2: b's whole limb about the joint; 0: its two
  // bodies'). The damper slows the relative spin at the rate it would slow that limb: between two
  // light bodies alone it would lock them together within a substep.
  f64 inertia = 0.0;
  V3 feed;                 // N m (world): a torque on b, a the reaction (a limb's weight held up)
  bool on() const { return stiffness > 0.0 || damping > 0.0; }
};

// An end of a joint, as the solver sees it.
struct JointEnd {
  i64 body = 0;  // a rigid body (Body::id); 0: fixed (the static world)
  // The anchor, and the joint's axis and reference direction (square to the axis: a hinge's angle
  // is b's reference turned from a's) at this end: a body's in its shape frame (the anchor relative
  // to its centre of mass: in the world x + R p), a fixed end's in the world. The two make the
  // joint's frame at the end: z the axis, x the reference (a ball's cone and twist are in it).
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
  // (hinge) A latch: it is held shut - turns not at all from where it was made - until the torque
  // about its axis passes this (N m; 0: none), then swings within its limits (a door, a lid).
  f64 latch = 0.0;
  bool latched = false;
  Quat rel;  // (fixed, slider) b's rotation relative to a's that it holds: conj(qa) qb
  // (ball) An anatomical cone: how far b's axis may tilt from a's, towards a's +x (its end's ref),
  // -x, +y (axis x ref) and -y (rad; an elliptical cone between them); and b's twist about its
  // axis relative to a's (a swing-twist decomposition) within [twist_lower, twist_upper]. A
  // shoulder swings far forward and little back.
  bool swing_limited = false;
  f64 swing[4] = {0.0, 0.0, 0.0, 0.0};  // +x, -x, +y, -y
  bool twist_limited = false;
  f64 twist_lower = 0.0, twist_upper = 0.0;
  JointMuscle muscle;  // (ball, hinge) a drive of its relative rotation
  // (an articulation's) Past a limit it turns back at most kSuppleStep (0.025 rad) a step - its
  // velocity and position passes alike, the XPBD bodies' LIMIT_STEP: a body folded far past its
  // range comes out of it over a few steps, it is not flung back within one.
  bool supple = false;

  // the solver's state
  V3 lin, ang;                  // accumulated impulses on b (warm start): point / square rows, angular rows
  f64 axial = 0.0, limit = 0.0;  // ... along the axis (distance: its row) and the limit's
  f64 motor = 0.0;              // ... the drive's
  f64 swing_imp = 0.0, twist_imp = 0.0;  // ... the cone's and the twist limit's
  V3 muscle_imp;                // ... the muscle's (world)
  // the last substep: the force and torque b received through it (a: the opposite; N, N m), its
  // hinge angle / slider offset / distance, and whether it gave way
  V3 force, torque;
  f64 value = 0.0;
  bool broken = false;
};

}  // namespace svx
