#include "svx/mech/law.hpp"

#include <algorithm>
#include <cmath>

namespace svx {

namespace {

inline f64 progress_of(f64 mag, f64 on, f64 br) {
  if (mag <= on) return 0.0;
  const f64 q = (mag - on) / std::max(br - on, 1e-12);
  return q > 1.0 ? 1.0 : q;
}

}  // namespace

LawEval evaluate_law(const BondModel& m, const Vec6& delta, f64 d_committed, bool game_law) {
  LawEval e;
  f64 q[6];
  f64 ratio[6];  // break / onset ratio of each component (game law secant)
  const f64 d0 = delta[0];
  if (d0 >= 0.0) {
    q[0] = progress_of(d0, m.onset.open, m.brk.open);
    ratio[0] = m.brk.open / std::max(m.onset.open, 1e-300);
  } else {
    q[0] = progress_of(-d0, m.onset.comp, m.brk.comp);
    ratio[0] = m.brk.comp / std::max(m.onset.comp, 1e-300);
  }
  for (int i = 1; i < 6; ++i) {
    q[i] = progress_of(std::abs(delta[i]), m.onset.comp6[i], m.brk.comp6[i]);
    ratio[i] = m.brk.comp6[i] / std::max(m.onset.comp6[i], 1e-300);
  }
  f64 sum = 0.0;
  int gov = 0;
  for (int i = 0; i < 6; ++i) {
    sum += q[i] * q[i];
    if (q[i] > q[gov]) gov = i;
  }
  const f64 p = std::min(1.0, std::sqrt(sum));
  e.progress = p;
  f64 dp = p;
  if (game_law && p > 0.0) {
    // Triangular softening from the onset peak to zero force at break: the secant scale
    // at progress p is (1-p) / (1 + p (rho - 1)).
    const f64 rho = std::max(1.0, ratio[gov]);
    const f64 sec = (1.0 - p) / (1.0 + p * (rho - 1.0));
    dp = 1.0 - sec;
  }
  e.damage = std::max(d_committed, dp);
  e.scale = secant_scale(m, e.damage);
  // kappa = max(kappa_c, 1 + progress) with kappa_break = 2 (compiled archetype).
  e.margin = std::max(d_committed, p) - 1.0;
  // equivalent demand phi (constitutive.js equivalentDemand)
  const f64 open = d0 > 0.0 ? d0 / m.onset.open : 0.0;
  const f64 comp = d0 < 0.0 ? -d0 / m.onset.comp : 0.0;
  f64 phi2 = open * open + comp * comp;
  for (int i = 1; i < 6; ++i) {
    const f64 r = delta[i] / std::max(m.onset.comp6[i], 1e-300);
    phi2 += r * r;
  }
  e.phi = std::sqrt(phi2);
  e.candidate = e.damage >= 0.999 || e.margin >= 0.0;
  return e;
}

}  // namespace svx
