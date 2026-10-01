// svx_city — nature/farmland.hpp (voxel_city nature/farmland.js).
#include "nature/farmland.hpp"

#include <cmath>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"
#include "world/season.hpp"

namespace svx::city {

namespace {

constexpr double BLOCK = 2400;  // 300 m
constexpr double MARGIN = 6;    // grass strip along every field edge (0.75 m)

enum class Crop { Wheat, Barley, Rapeseed, Potato, Maize, Hay, Pasture, Fallow };
constexpr std::string_view kCropNames[] = {"wheat", "barley", "rapeseed", "potato", "maize", "hay", "pasture", "fallow"};

struct CropWeight {
  Crop kind;
  double w;
};
// Crops by climate: [kind, weight].
constexpr CropWeight MILD[] = {{Crop::Wheat, 5}, {Crop::Barley, 3}, {Crop::Rapeseed, 2}, {Crop::Potato, 1.5},
                               {Crop::Maize, 1}, {Crop::Hay, 1.5},  {Crop::Pasture, 2},  {Crop::Fallow, 0.6}};
constexpr CropWeight HOT[] = {{Crop::Maize, 4}, {Crop::Pasture, 3}, {Crop::Potato, 1}, {Crop::Fallow, 1}, {Crop::Hay, 0.5}};
constexpr CropWeight COOL[] = {{Crop::Barley, 2}, {Crop::Hay, 4}, {Crop::Pasture, 3}, {Crop::Potato, 1.5}, {Crop::Fallow, 0.5}};

template <size_t N>
Crop pick(const CropWeight (&list)[N], double r) {
  double total = 0;
  for (const CropWeight& c : list) total += c.w;
  double acc = r * total;
  for (const CropWeight& c : list) {
    acc -= c.w;
    if (acc < 0) return c.kind;
  }
  return list[N - 1].kind;
}

Crop crop_of(double key, double t) {
  const double r = js::to_uint32(key) / 4294967296.0;
  return t < 0.42 ? pick(COOL, r) : t > 0.66 ? pick(HOT, r) : pick(MILD, r);
}

// h01(...a): (hash32(...a) >>> 0) / 2^32 (the reference passes a fifth argument hash32 ignores)
double h01(double a, double b, double c, double d) { return hash32(a, b, c, d) / 4294967296.0; }

}  // namespace

Farmland::Farmland(const World& world) : seed(world.seed), wrap(wrap_of(world.config)), season(season_of(world.config).id) {}

Field Farmland::field_at(double x0, double y0) const {
  const double x = wrap.vi(x0);
  const double y = wrap.vi(y0);
  const double bi = std::floor(x / BLOCK);
  const double bj = std::floor(y / BLOCK);
  const double lx = x - bi * BLOCK;
  const double ly = y - bj * BLOCK;
  const uint32_t hb = hash32(seed, bi, bj, 0x6a1);
  const bool along_x = (hb & 1u) == 1u;
  const double across = along_x ? ly : lx;
  const double along = along_x ? lx : ly;
  // strips of varying width across the block
  const int n = 2 + static_cast<int>((hb >> 1) % 5u);
  double total = 0;
  double w[6];
  for (int k = 0; k < n; ++k) {
    const double v = 0.45 + h01(seed, bi, bj, k);  // (JS: h01(seed, bi, bj, k, 0x6a2))
    w[k] = v;
    total += v;
  }
  double s0 = 0;
  int k = 0;
  for (; k < n - 1; ++k) {
    const double s1 = s0 + (w[k] / total) * BLOCK;
    if (across < s1) break;
    s0 += (w[k] / total) * BLOCK;
  }
  const double s1 = k == n - 1 ? BLOCK : s0 + (w[k] / total) * BLOCK;
  // now and then a strip is cut once across (JS: hash32(seed, bi, bj, k, 0x6a3))
  const uint32_t hs = hash32(seed, bi, bj, k);
  const double cut = (hs & 3u) == 0 ? js::round(BLOCK * (0.3 + 0.4 * (static_cast<double>((hs >> 2) & 1023u) / 1023))) : -1;
  const double part = cut >= 0 && along >= cut ? 1 : 0;
  const double t0 = part != 0 ? cut : 0;
  const double t1 = cut >= 0 && part == 0 ? cut : BLOCK;
  const double dA = js::min(across - s0, s1 - across);
  const double dB = js::min(along - t0, t1 - along);
  // (a block's own edges are keyed by the line, so both blocks agree on a hedge or a wall)
  auto line = [&](bool vertical, double i, double j) { return static_cast<double>(hash32(seed, i, j, vertical ? 0x6b1 : 0x6b2)); };
  double edge;
  double edge_key;
  if (dA <= dB) {
    edge = dA;
    const int side = across - s0 < s1 - across ? 0 : 1;
    if (side == 0 && k == 0)
      edge_key = along_x ? line(false, bi, bj) : line(true, bi, bj);
    else if (side == 1 && k == n - 1)
      edge_key = along_x ? line(false, bi, bj + 1) : line(true, bi + 1, bj);
    else
      edge_key = hash32(seed, bi, bj, k + side);  // (JS: hash32(seed, bi, bj, k + side, 0x6a5))
  } else {
    edge = dB;
    const int side = along - t0 < t1 - along ? 0 : 1;
    if (side == 0 && t0 == 0)
      edge_key = along_x ? line(true, bi, bj) : line(false, bi, bj);
    else if (side == 1 && t1 == BLOCK)
      edge_key = along_x ? line(true, bi + 1, bj) : line(false, bi, bj + 1);
    else
      edge_key = hash32(seed, bi, bj, k);  // (JS: hash32(seed, bi, bj, k, 0x6a8))
  }
  Field f;
  f.key = hash32(seed, bi, bj, k);  // (JS: hash32(seed, bi, bj, k, part, 0x6a9))
  f.along_x = along_x;
  f.a = across - s0;
  f.b = along - t0;
  f.edge = edge;
  f.edge_key = edge_key;
  return f;
}

std::string_view Farmland::crop(double key, double t) const { return kCropNames[static_cast<int>(crop_of(key, t))]; }

FieldGround Farmland::ground(const Field& f, double t, double x, double y) const {
  const bool cold = t < 0.42;
  const double ek = js::to_uint32(f.edge_key) / 4294967296.0;
  if (f.edge < MARGIN) {
    // a stone wall along some boundaries in the north, grass margins elsewhere
    if (cold && ek < 0.3 && f.edge < 4 && (hash32(seed, js::sar(wrap.vi(x), 5), js::sar(wrap.vi(y), 5), 0x6aa) & 15u) != 0) {
      const uint32_t hj = hash32(seed, wrap.vi(x), wrap.vi(y), 0x6ab) & 7u;
      return {hj < 2 ? MAT::LICHEN : hj < 4 ? MAT::GRANITE : MAT::STONE, MAT::STONE, f.edge < 2 ? 5.0 : 3.0};
    }
    return {f.edge < 2 ? MAT::GRASS_TALL : MAT::GRASS, MAT::DIRT, 0};
  }
  const Crop kind = crop_of(f.key, t);
  const bool row = (js::to_int32(std::floor(f.a / 6)) & 1) != 0;
  const std::string& s = season;
  const uint32_t h = hash32(seed, wrap.vi(x), wrap.vi(y), 0x6ac) & 15u;
  switch (kind) {
    case Crop::Wheat:
    case Crop::Barley:
      if (s == "spring") return {row ? MAT::SOIL_BED : MAT::CROP_GREEN, MAT::DIRT, 0};
      if (s == "autumn")
        return {((js::to_uint32(f.key) >> 5) & 1u) != 0 ? (row ? MAT::SOIL_BED : MAT::DIRT) : row ? MAT::GRASS_STRAW : MAT::DIRT, MAT::DIRT, 0};
      return {kind == Crop::Wheat ? (row && h < 12 ? MAT::CROP_WHEAT : MAT::HAY) : row ? MAT::GRASS_STRAW : MAT::HAY, MAT::DIRT, 0};
    case Crop::Rapeseed:
      if (s == "spring") return {h < 13 ? MAT::CROP_RAPE : MAT::CROP_GREEN, MAT::DIRT, 0};
      if (s == "autumn") return {row ? MAT::SOIL_BED : MAT::DIRT, MAT::DIRT, 0};
      return {row ? MAT::GRASS_DRY : MAT::CROP_GREEN, MAT::DIRT, 0};
    case Crop::Maize:
      if (s == "summer" || s == "autumn") return {row ? (s == "autumn" ? MAT::GRASS_STRAW : MAT::GRASS_TALL) : MAT::SOIL_BED, MAT::DIRT, 0};
      return {row ? MAT::SOIL_BED : MAT::DIRT, MAT::DIRT, 0};
    case Crop::Potato:
      if (s == "summer") return {row ? MAT::GRASS_DARK : MAT::SOIL_BED, MAT::DIRT, 0};
      return {row ? MAT::SOIL_BED : MAT::DIRT, MAT::DIRT, 0};
    case Crop::Hay: {
      // mown in wide swaths in summer
      const bool swath = (js::to_int32(std::floor(f.a / 24)) & 1) != 0;
      if (s == "summer") return {swath ? MAT::GRASS_STRAW : MAT::GRASS, MAT::DIRT, 0};
      return {h < 3 ? MAT::GRASS_DRY : MAT::GRASS, MAT::DIRT, 0};
    }
    case Crop::Fallow:
      return {h < 5 ? MAT::GRASS_DRY : h < 6 ? MAT::FLOWER_YELLOW : MAT::GRASS, MAT::DIRT, 0};
    default:
      // pasture: tufted grass
      return {h < 3 ? MAT::GRASS_DARK : MAT::GRASS, MAT::DIRT, 0};
  }
}

double Farmland::hedge(const Field& f, double t) const {
  if (f.edge > 12) return 0;
  const double ek = js::to_uint32(f.edge_key) / 4294967296.0;
  const double share = t < 0.42 ? 0.12 : 0.38;
  // (in the north the walls take the edges the hedges leave)
  if (t < 0.42 ? ek < 0.3 || ek > 0.3 + share : ek > share) return 0;
  return 1 - f.edge / 12;
}

}  // namespace svx::city
