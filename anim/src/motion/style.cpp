#include "svx/anim/motion/style.hpp"

namespace svx::anim {

GaitStyle random_style(f64 seed, StyleKind kind) {
  Rng r(seed * 7919.0 + 13.0);
  // (a value about c, spread s: the two draws in order)
  auto n = [&r](f64 c, f64 s) {
    const f64 a = r.next();
    const f64 b = r.next();
    return c + (a + b - 1.0) * s;
  };
  GaitStyle g;
  if (kind == StyleKind::Soldier) {
    g.stride = n(1.02, 0.05);
    g.bounce = n(0.85, 0.12);
    g.sway = n(0.75, 0.12);
    g.arms = n(0.9, 0.1);
    g.elbow = n(0.1, 0.05);
    g.posture = n(0.55, 0.2);
    g.width = n(1.15, 0.06);
    g.toe_out = n(0.14, 0.04);
    g.move_width = 0.9;
    g.move_toe_out = g.toe_out * 0.4;
    g.heavy = n(0.75, 0.1);
    g.head_still = n(0.8, 0.1);
    g.fidget = n(0.3, 0.15);
    return g;
  }
  if (kind == StyleKind::Thug) {
    // a swagger: wide, rolling shoulders, arms swinging out, heavy on the feet, restless
    g.stride = n(1.02, 0.05);
    g.bounce = n(1.15, 0.12);
    g.sway = n(1.35, 0.15);
    g.arms = n(1.3, 0.12);
    g.elbow = n(0.28, 0.06);
    g.posture = n(0.15, 0.3);
    g.width = n(1.22, 0.06);
    g.toe_out = n(0.22, 0.04);
    g.move_width = 1.0;
    g.move_toe_out = g.toe_out * 0.75;
    g.heavy = n(0.65, 0.1);
    g.head_still = n(0.35, 0.1);
    g.fidget = n(0.75, 0.1);
    return g;
  }
  const bool female = kind == StyleKind::CivilianFemale;
  // a few archetypes, then jitter: brisk commuter, stroller, sloucher, swaggerer, shuffler
  static constexpr GaitStyle kBase[5] = {
      {1.05, 0.9, 1.0, 0.9, 0.15, 0.4, 1.0, 0.1, 0.4, 0.6, 0.4},
      {0.95, 1.1, 1.2, 1.1, 0.05, 0.1, 1.0, 0.14, 0.3, 0.4, 0.6},
      {0.9, 0.8, 0.8, 0.6, 0.1, -0.7, 1.05, 0.16, 0.55, 0.3, 0.7},
      {1.1, 1.25, 1.35, 1.35, 0.25, 0.6, 1.2, 0.22, 0.45, 0.4, 0.5},
      {0.82, 0.7, 0.7, 0.55, 0.2, -0.4, 0.95, 0.08, 0.6, 0.5, 0.35},
  };
  const GaitStyle& base = kBase[r.integer(0, 4)];
  g.stride = n(base.stride, 0.05);
  g.bounce = n(base.bounce, 0.15);
  g.sway = n(base.sway, 0.15) * (female ? 1.35 : 1.0);
  g.arms = n(base.arms, 0.12);
  g.elbow = n(base.elbow, 0.08) + (female ? 0.12 : 0.0);
  g.posture = n(base.posture, 0.25);
  g.width = n(base.width, 0.06) * (female ? 0.8 : 1.0);
  g.toe_out = n(base.toe_out, 0.04) * (female ? 0.6 : 1.0);
  g.move_width = base.width > 1.1 ? 0.95 : 0.75;
  g.move_toe_out = g.toe_out * (base.width > 1.1 ? 0.65 : 0.3);
  g.heavy = n(base.heavy, 0.1);
  g.head_still = n(base.head_still, 0.12);
  g.fidget = n(base.fidget, 0.15);
  return g;
}

}  // namespace svx::anim
