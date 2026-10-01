// svx_city — small numeric helpers (voxel_city core/math.js) and units (core/units.js).
#pragma once

#include "core/js.hpp"

namespace svx::city {

// ---- core/units.js: the whole generator plans in integer voxel coordinates, one voxel 12.5 cm.
constexpr double kVoxelSize = 0.125;
constexpr double kVoxelsPerMeter = 1 / kVoxelSize;
// metres -> voxels (rounded)
inline double vx(double meters) { return js::round(meters * kVoxelsPerMeter); }
// voxels -> metres
inline double vm(double voxels) { return voxels * kVoxelSize; }
// chunk edge in voxels at every LOD, and padded (a one-voxel apron on every side)
constexpr int kChunk = 32;
constexpr int kPChunk = kChunk + 2;

// ---- core/math.js
inline double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
inline double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double inv_lerp(double a, double b, double v) { return a == b ? 0 : (v - a) / (b - a); }
inline double remap(double v, double a0, double a1, double b0, double b1) { return lerp(b0, b1, inv_lerp(a0, a1, v)); }
inline double fract(double v) { return v - std::floor(v); }
// ((a % n) + n) % n
inline double mod(double a, double n) { return std::fmod(std::fmod(a, n) + n, n); }
inline double sq(double v) { return v * v; }
inline double smoothstep(double e0, double e1, double v) {
  const double t = clamp01((v - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}
// polynomial smooth minimum; k the blend radius
inline double smin(double a, double b, double k) {
  if (k <= 0) return js::min(a, b);
  const double h = clamp01(0.5 + (0.5 * (b - a)) / k);
  return lerp(b, a, h) - k * h * (1 - h);
}
// circular smooth minimum (an exact fillet of radius k for perpendicular SDFs)
inline double smin_circular(double a, double b, double k) {
  if (k <= 0) return js::min(a, b);
  const double ua = js::max(k - a, 0.0);
  const double ub = js::max(k - b, 0.0);
  return js::max(k, js::min(a, b)) - js::hypot(ua, ub);
}
inline double floor_div(double a, double b) { return std::floor(a / b); }
inline double snap(double v, double step) { return js::round(v / step) * step; }
inline double snap_down(double v, double step) { return std::floor(v / step) * step; }

}  // namespace svx::city
