// svx_city — nature/rivers.hpp (voxel_city nature/rivers.js).
#include "nature/rivers.hpp"

#include <cmath>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/chart.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city {

namespace {

constexpr double SCALE = 5200;     // meander wavelength (m)
constexpr double RING = 18 * 8;    // terrain samples 18 m around for the water level

}  // namespace

Rivers::Rivers(const World& w)
    : world(&w),
      n(derive_seed(w.seed, "river.path")),
      warp(derive_seed(w.seed, "river.warp")),
      mask(derive_seed(w.seed, "river.mask")),
      enabled(w.config["rivers"]["enabled"].truthy()),
      max_half_width(w.config["rivers"]["maxHalfWidth"].to_number()) {}

double Rivers::field(double fx, double fy, double fz, double fw) const {
  const double w = 900 * warp.fbmP(fx / 3000, fy / 3000, fz / 3000, fw / 3000, 2);
  return n.fbmP((fx + w) / SCALE, (fy - w) / SCALE, fz / SCALE, fw / SCALE, 3);
}

std::optional<RiverInfo> Rivers::at(double x, double y) const {
  if (!enabled) return std::nullopt;
  // island: rivers end at the shore
  const IslandPlan* isl = world->fields->island.get();
  if (isl && isl->coast(x / 8, y / 8) < -12) return std::nullopt;
  const Chart& chart = *world->chart;
  const FieldPoint f = chart.to_field(x * kVoxelSize, y * kVoxelSize);
  const double v = field(f.x, f.y, f.z, f.w);
  if (std::fabs(v) > 0.06) return std::nullopt;  // > ~150 m from any centerline
  const double e = 12;
  double gx;
  double gy;
  if (f.w != f.w) {  // (JS: fw === undefined, a 3D chart)
    gx = (field(f.x + e, f.y, f.z) - v) / e;
    gy = (field(f.x, f.y + e, f.z) - v) / e;
  } else {
    // torus chart: step along the surface
    const double R = chart.R;
    gx = (field(f.x - (e * f.y) / R, f.y + (e * f.x) / R, f.z, f.w) - v) / e;
    gy = (field(f.x, f.y, f.z - (e * f.w) / R, f.w + (e * f.z) / R) - v) / e;
  }
  const double g = js::or_(js::hypot(gx, gy), 1e-6);
  const double d = std::fabs(v) / g;
  // rivers fade in and out (springs, mouths) and stay out of the mountains
  const double m = smoothstep(0.05, 0.4, mask.fbmP(f.x / 16000, f.y / 16000, f.z / 16000, f.w / 16000, 2));
  if (m <= 0) return std::nullopt;
  const double mtn = world->fields->mountainness(x, y);
  const double half = max_half_width * m * (1 - smoothstep(0.05, 0.3, mtn));
  if (half < 2) return std::nullopt;
  const double urban = smoothstep(0.2, 0.4, world->fields->urban(x, y).u);
  const double bank = 22 * (1 - urban);
  if (d > half + bank + 1) return std::nullopt;
  RiverInfo o;
  o.d = d;
  o.half = half;
  o.urban = urban;
  o.bank = bank;
  o.water = water_level(x, y);
  const double depth_m = d < half ? 1.2 + 2.2 * (1 - js::pow(d / half, 2)) : 0;
  o.bed = js::round(o.water - depth_m * 8);
  return o;
}

double Rivers::water_level(double x, double y) const {
  const Terrain& t = *world->terrain;
  double lo = t.sample(x, y).h;
  constexpr double kRing[4][2] = {{RING, 0}, {-RING, 0}, {0, RING}, {0, -RING}};
  for (const auto& dd : kRing) lo = js::min(lo, t.sample(x + dd[0], y + dd[1]).h);
  const bool urban = world->fields->urban(x, y).u > 0.3;
  // quantize in 2-voxel steps so the surface is flat over long reaches
  const double level = std::floor((lo - (urban ? 22 : 12)) / 2) * 2;
  // (an island's rivers meet the sea at sea level)
  return world->fields->island ? js::max(level, js::round(world->config["world"]["seaLevel"].to_number() * 8)) : level;
}

double Rivers::ground_at(const RiverInfo& info, double h) const {
  if (info.d < info.half) return js::min(h, info.bed);
  if (info.urban > 0.5) return h;  // quay wall at the channel edge
  const double t = (info.d - info.half) / js::max(1.0, info.bank);
  return js::min(h, js::round(info.water + 2 + t * t * (h - info.water)));
}

double Rivers::channel_gap(double x, double y) const {
  const IslandPlan* isl = world->fields->island.get();
  if (isl && isl->coast(x / 8, y / 8) < -12) return js::kInf;
  const Chart& chart = *world->chart;
  const FieldPoint f = chart.to_field(x * kVoxelSize, y * kVoxelSize);
  const double v = field(f.x, f.y, f.z, f.w);
  if (std::fabs(v) > 0.06) return js::kInf;
  const double e = 12;
  double gx;
  double gy;
  if (f.w != f.w) {
    gx = (field(f.x + e, f.y, f.z) - v) / e;
    gy = (field(f.x, f.y + e, f.z) - v) / e;
  } else {
    const double R = chart.R;
    gx = (field(f.x - (e * f.y) / R, f.y + (e * f.x) / R, f.z, f.w) - v) / e;
    gy = (field(f.x, f.y, f.z - (e * f.w) / R, f.w + (e * f.z) / R) - v) / e;
  }
  const double d = std::fabs(v) / js::or_(js::hypot(gx, gy), 1e-6);
  const double m = smoothstep(0.05, 0.4, mask.fbmP(f.x / 16000, f.y / 16000, f.z / 16000, f.w / 16000, 2));
  if (m <= 0) return js::kInf;
  const double half = max_half_width * m * (1 - smoothstep(0.05, 0.3, world->fields->mountainness(x, y)));
  if (half < 2) return js::kInf;
  return d - half;
}

bool Rivers::hits_rect(const Rect& r, double margin_m) const {
  if (!enabled) return false;
  const double step = 80;
  for (double y = r.y0; y <= r.y1 + step - 1; y += step) {
    for (double x = r.x0; x <= r.x1 + step - 1; x += step) {
      if (channel_gap(js::min(x, r.x1), js::min(y, r.y1)) < margin_m + (step / 8) * 0.71) return true;
    }
  }
  return false;
}

}  // namespace svx::city
