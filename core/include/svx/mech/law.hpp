// structvox — bond damage / rupture law (port of the prototype's continuum_regularized_v1).
//
// Per component i the softening progress is q_i = clamp((|d_i| - onset_i) /
// (break_i - onset_i), 0, 1) (the axial component uses the opening or closing thresholds by
// sign); progress p = min(1, ||q||_2). Damage only increases: d = max(d_committed, D(p)),
// with D(p) = p (reference law) or the triangular-softening secant (game law, so the work
// to rupture equals G_f * A at any cell size). Secant stiffness scale s(d) = r + (1-r)(1-d).
// A bond is a rupture candidate when d >= 0.999 (equivalently progress reaches 1).
// Candidates are ordered like the prototype: score = max(d, margin, phi) desc, then d desc,
// then id asc (oriented_interface_core_base.js:1717-1730).
#pragma once

#include "svx/mech/bond.hpp"

namespace svx {

struct LawEval {
  f64 progress = 0.0;  // p in [0, 1]
  f64 damage = 0.0;    // trial damage (>= committed)
  f64 scale = 1.0;     // secant stiffness scale s(d)
  f64 margin = -1.0;   // kappa - kappa_break (>= 0 => rupture)
  f64 phi = 0.0;       // equivalent demand (diagnostics / ordering)
  bool candidate = false;
};

// delta: elastic generalized jump in svx order (physical units: m / rad).
LawEval evaluate_law(const BondModel& m, const Vec6& delta, f64 d_committed, bool game_law);

// Secant stiffness scale for a committed damage value. Floored at kMinSecant so a fully
// softened bond of a zero-residual law never makes the operator singular before it ruptures.
constexpr f64 kMinSecant = 1e-4;
inline f64 secant_scale(const BondModel& m, f64 d) {
  const f64 dc = d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
  const f64 s = m.residual + (1.0 - m.residual) * (1.0 - dc);
  return s > kMinSecant ? s : kMinSecant;
}

}  // namespace svx
