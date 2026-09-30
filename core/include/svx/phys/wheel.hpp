// structvox — wheels (docs/MOTION.md §7): a cast wheel on a sprung suspension with a tyre, solved
// with the contacts and joints of the rigid solver (sequential impulses, warm-started).
//
// A wheel hangs from a mount on a body, its carrier (whatever rolls on wheels: a vehicle, a
// trolley, a cart, a machine's undercarriage). Each substep its tyre is cast along the
// suspension against the static grids and the other pieces (samples across its lower arc and its
// width: it rolls onto a step instead of dropping into a voxel's gap); where it touches, three
// rows act between the carrier and what the wheel stands on:
//   - the suspension: a spring and damper along the suspension axis, solved implicitly (a soft
//     row: stable at any stiffness), and a bump stop when it is compressed through its travel;
//   - the tyre, longitudinal and lateral: slip against the ground drives a force, stiff at low
//     speed (a carrier at rest holds on a slope), soft at speed (slip angles, progressive grip), both
//     clamped together by the friction ellipse at grip x the surface's friction x the load, whose
//     peak falls off once the tyre slides (drifts, handbrake turns);
//   - the wheel's spin is a degree of freedom of its own: drive torque spins it up, brake torque
//     holds it, the longitudinal row couples it to the ground.
// Its forces load what it stands on: a static grid's structure (a heavy carrier loads the bridge
// it is on) or a piece (one carrier on another's roof). No transcendental functions: bit-identical on every
// platform at whole-numbered substeps of 1/120 s.
#pragma once

#include <array>

#include "svx/base/vec.hpp"

namespace svx {

struct Wheel {
  u32 id = 0;
  // ---- the mount (set every substep by the owner from its anchor: World)
  i64 body = 0;          // the carrier (Body::id); 0: not mounted now (skipped)
  V3 p;                  // the suspension's top in the carrier's frame, relative to its centre of mass
  V3 down{0, 0, -1};     // suspension axis (carrier's frame, unit)
  V3 axle{0, 1, 0};      // spin axis at zero steer (carrier's frame, unit; the wheel rolls along down x axle)
  // ---- settings
  f64 radius = 0.33, width = 0.22;  // m
  f64 rest = 0.35;       // m: spring length at full droop (no load: the wheel hangs there)
  f64 travel = 0.2;      // m: compression to the bump stop
  f64 stiffness = 35e3;  // N/m
  f64 damping = 3.5e3;   // N s/m
  f64 inertia = 1.2;     // kg m^2 (spin)
  f64 grip = 1.0;        // x the surface's tyre friction
  f64 break_force = 0.0; // N: it comes off beyond (0: never)
  // ---- input (the host's, per tick)
  f64 drive = 0.0;       // N m: drive torque (negative: backwards)
  f64 brake = 0.0;       // N m: brake torque (>= 0)
  f64 steer = 0.0;       // rad: the axle turned about the suspension axis (positive: to the left of down x axle)
  // ---- state
  f64 spin = 0.0;        // rad/s about the axle, relative to the carrier (positive: rolling forward)
  f64 angle = 0.0;       // rad: the spin's angle (in [-pi, pi))
  f64 length = 0.35;     // m: the suspension now
  bool contact = false;
  V3 point, normal;      // the contact (world)
  i64 ground_body = 0;   // the piece it stands on (0: a static grid, or nothing)
  u16 ground_grid = 0;   // the static grid's slot, or (on a piece) the piece's shape, and the voxel
  std::array<i32, 3> ground_voxel{0, 0, 0};
  u8 ground_mat = 0;     // the surface's material id
  f64 load = 0.0;        // N: the suspension's force (last substep)
  V3 force;              // N: what the carrier received at the contact (last substep)
  f64 slip_long = 0.0, slip_lat = 0.0;  // m/s: the tyre sliding over the ground (last substep)
  bool broken = false;   // (its force passed break_force: it comes off)
  // ---- solver (accumulated impulses: suspension, bump stop, longitudinal, lateral, brake)
  f64 ls = 0.0, lbump = 0.0, lx = 0.0, ly = 0.0, lb = 0.0;
};

}  // namespace svx
