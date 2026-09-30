#include "svx/anim/motion/gait.hpp"

#include <algorithm>

namespace svx::anim {

namespace {

constexpr f64 kSpeeds[] = {0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.5, 7.5};
constexpr f64 kCadence[] = {0.75, 0.74, 0.86, 0.95, 1.04, 1.16, 1.28, 1.36, 1.45, 1.55};
constexpr int kRows = 10;

f64 table(const f64* xs, const f64* ys, f64 x) {
  if (x <= xs[0]) return ys[0];
  for (int i = 1; i < kRows; ++i) {
    if (x <= xs[i]) {
      const f64 t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
      return lerp(ys[i - 1], ys[i], t);
    }
  }
  return ys[kRows - 1];
}

}  // namespace

GaitParams gait_for(f64 speed, f64 crouch, f64 scale) {
  const f64 v = std::max(0.0, speed) / std::sqrt(scale);
  const f64 c = clamp(crouch, 0.0, 1.0);
  const f64 run = smoothstep(1.9, 2.9, v) * (1.0 - c);
  f64 freq = table(kSpeeds, kCadence, v) * std::sqrt(1.0 / scale);
  // crouched strides are short: at most ~0.9 m per cycle
  if (c > 0.0) freq = lerp(freq, std::max(std::max(freq, (v * std::sqrt(scale)) / 0.9), 0.8), c);
  GaitParams g;
  g.freq = freq;
  g.duty = lerp(lerp(0.62, 0.56, smoothstep(0.5, 2.0, v)), lerp(0.36, 0.26, smoothstep(3.0, 6.5, v)), run);
  g.run = run;
  g.lift = lerp(lerp(0.06, 0.1, smoothstep(0.5, 2.0, v)), lerp(0.2, 0.3, smoothstep(3.0, 6.0, v)), run) * scale * (1.0 - 0.35 * c);
  g.bob = lerp(lerp(0.008, 0.02, smoothstep(0.3, 1.8, v)), 0.035, run) * scale * (1.0 - 0.5 * c);
  g.sway = lerp(0.022, 0.008, run) * smoothstep(0.05, 0.6, v) * scale;
  g.hip_yaw = lerp(0.08, 0.14, run) * smoothstep(0.1, 1.2, v);
  g.hip_roll = lerp(0.07, 0.04, run) * smoothstep(0.1, 1.2, v);
  g.lean = lerp(lerp(0.04, 0.08, smoothstep(0.5, 2.0, v)), lerp(0.16, 0.26, smoothstep(3.0, 6.5, v)), run);
  g.arm_swing = lerp(lerp(0.1, 0.32, smoothstep(0.3, 2.0, v)), lerp(0.55, 0.8, smoothstep(3.0, 6.5, v)), run);
  g.elbow = lerp(lerp(0.2, 0.35, smoothstep(0.3, 2.0, v)), 1.45, run);
  g.sink = lerp(lerp(0.012, 0.03, smoothstep(0.3, 2.0, v)), 0.07, run) * scale;
  return g;
}

}  // namespace svx::anim
