// svx_anim — movement personality: how a character walks and holds itself. Every character gets
// its own style (seeded), so a crowd never moves in step: long confident strides or short
// hurried ones, bouncy or smooth, swaying hips or stiff ones, big or small arm swing, upright or
// slouched, light or heavy on its feet. Soldiers are upright, heavy (gear) and steady.
#pragma once

#include "svx/anim/math.hpp"

namespace svx::anim {

// (the defaults are the neutral style)
struct GaitStyle {
  f64 stride = 1.0;       // stride length multiplier (the cadence adapts to the speed)
  f64 bounce = 1.0;       // vertical bounce multiplier
  f64 sway = 1.0;         // hip sway and roll multiplier
  f64 arms = 1.0;         // arm swing multiplier
  f64 elbow = 0.0;        // extra elbow bend (rad)
  f64 posture = 0.0;      // -1 slouched (chest and head down) .. 1 upright and proud
  f64 width = 1.0;        // standing width multiplier
  f64 toe_out = 0.12;     // standing toe-out (rad)
  f64 heavy = 0.4;        // 0 light .. 1 heavy: footfall compression, slower to accelerate, more lean
  f64 head_still = 0.5;   // 0 .. 1: how still the head is kept (soldiers, dancers)
  f64 fidget = 0.5;       // fidgetiness when idle 0..1 (how often idles change)
  // Walking/running placement, blended in as speed rises. Width is relative to
  // the standing width; toe-out is per foot, in radians. Wider swagger remains
  // available without making every neutral walk use a broad standing stance.
  f64 move_width = 0.75;  // 0.6..1.5
  f64 move_toe_out = 2.0 * kDeg;  // 0..20 degrees
};

inline constexpr GaitStyle kNeutralStyle{};

enum class StyleKind : u8 { Soldier, Civilian, CivilianFemale, Thug };

// A random style of a kind (deterministic by seed).
GaitStyle random_style(f64 seed, StyleKind kind);

}  // namespace svx::anim
