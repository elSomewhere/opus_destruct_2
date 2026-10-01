// svx_city — nature/caves.hpp (voxel_city nature/caves.js).
#include "nature/caves.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "nature/landcover.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"

namespace svx::city {

namespace {

constexpr int G = 4;  // coarse grid spacing (padded index units)
constexpr double TUBE = 0.14;
constexpr int kNoWater = -2147483647 - 1;  // (compose's NO_WATER)

}  // namespace

Caves::Caves(const World& w)
    : world(&w),
      enabled(w.config["caves"]["enabled"].truthy()),
      depth(w.config["caves"]["depth"].to_number()),
      n_a(derive_seed(w.seed, "cave.a")),
      n_b(derive_seed(w.seed, "cave.b")),
      n_c(derive_seed(w.seed, "cave.cavern")),
      n_mask(derive_seed(w.seed, "cave.mask")) {}

double Caves::region(double x, double y) const {
  const double m = n_mask.fbm2((x * kVoxelSize) / 2600, (y * kVoxelSize) / 2600, 2);
  return js::max(0.0, js::min(1.0, (m - 0.05) * 3));
}

double Caves::field(double x, double y, double z, double d) const {
  const double xm = x * kVoxelSize;
  const double ym = y * kVoxelSize;
  const double zm = z * kVoxelSize;
  const double a = n_a.n3(xm / 46, ym / 46, zm / 26);
  const double b = n_b.n3(xm / 46 + 31.7, ym / 46 - 11.3, zm / 26);
  double f = a * a + b * b - TUBE * TUBE;
  if (d > 20) {
    const double c = n_c.fbm3(xm / 120, ym / 120, zm / 45, 2);
    f = js::min(f, 0.42 - c);
  }
  return f;
}

bool Caves::column_ok(const CaveColumns& tile, int idx) const {
  const int k = tile.kind[idx];
  // natural ground without water; caves do not break through snowfields and glaciers
  return k == 4 && tile.water[idx] == kNoWater && tile.top[idx] != MAT::SNOW && tile.top[idx] != MAT::SNOW_WIND;
}

bool cave_z_range(const World& world, const Rect& rect, int lod, const CaveColumns* tile, double* z0, double* z1) {
  (void)lod;
  const Caves* cv = world.caves.get();
  if (!cv || !cv->enabled || !tile) return false;
  const double cx = (rect.x0 + rect.x1) / 2;
  const double cy = (rect.y0 + rect.y1) / 2;
  if (cv->region(cx, cy) <= 0 && cv->region(rect.x0, rect.y0) <= 0 && cv->region(rect.x1, rect.y1) <= 0) return false;
  // coarse probe of the cave band under the tile
  const double depth = cv->depth * 8;
  double lo = js::kInf;
  for (int j = 2; j < kP; j += 8)
    for (int i = 2; i < kP; i += 8) {
      const int idx = i + j * kP;
      if (!cv->column_ok(*tile, idx)) continue;
      const double x = rect.x0 + ((rect.x1 - rect.x0) * i) / kP;
      const double y = rect.y0 + ((rect.y1 - rect.y0) * j) / kP;
      if (cv->region(x, y) <= 0) continue;
      const double gz = tile->z[idx];
      for (double dz = 0; dz <= depth; dz += 12)
        if (cv->field(x, y, gz - dz, dz / 8) < 0.008) lo = js::min(lo, gz - dz - 24);
    }
  if (lo == js::kInf) return false;
  *z0 = lo;
  *z1 = tile->z_max;
  return true;
}

void cave_rasterize(const World& world, ChunkBuffer& chunk, const CaveColumns* tile) {
  const Caves* cv = world.caves.get();
  if (!cv || !cv->enabled || !tile) return;
  const Box3 box = chunk.world_box();
  const double depth = cv->depth * 8;
  if (box.z0 > tile->z_max + 2 || box.z1 < tile->z_min - depth) return;
  const double cx = (box.x0 + box.x1) / 2;
  const double cy = (box.y0 + box.y1) / 2;
  if (cv->region(cx, cy) <= 0 && cv->region(box.x0, box.y0) <= 0 && cv->region(box.x1, box.y1) <= 0 && cv->region(box.x0, box.y1) <= 0 &&
      cv->region(box.x1, box.y0) <= 0)
    return;
  // coarse grid of field values over the padded chunk (Float32Arrays)
  constexpr int n = (kP - 1 + G - 1) / G + 1;  // Math.ceil((P - 1) / G) + 1
  std::array<float, n * n * n> grid{};
  std::array<float, n * n> region_at{};
  for (int gj = 0; gj < n; ++gj)
    for (int gi = 0; gi < n; ++gi)
      region_at[gi + gj * n] = js::f32(cv->region(chunk.wx(std::min(kP - 1, gi * G)), chunk.wy(std::min(kP - 1, gj * G))));
  bool any = false;
  for (int gk = 0; gk < n; ++gk)
    for (int gj = 0; gj < n; ++gj)
      for (int gi = 0; gi < n; ++gi) {
        const int i = std::min(kP - 1, gi * G);
        const int j = std::min(kP - 1, gj * G);
        const int k = std::min(kP - 1, gk * G);
        const int col = i + j * kP;
        const double r = region_at[gi + gj * n];
        const double d = (tile->z[col] - chunk.wz(k)) / 8;
        double f = 1;
        if (r > 0 && d > -1 && d < cv->depth) f = cv->field(chunk.wx(i), chunk.wy(j), chunk.wz(k), d) + (1 - r) * 0.03;
        grid[gi + gj * n + gk * n * n] = js::f32(f);
        if (f < 0) any = true;
      }
  if (!any) return;
  uint16_t* data = chunk.data.data();
  auto at = [&](int i, int j, int k) {
    const double fi = static_cast<double>(i) / G;
    const double fj = static_cast<double>(j) / G;
    const double fk = static_cast<double>(k) / G;
    const int i0 = std::min(n - 2, static_cast<int>(std::floor(fi)));
    const int j0 = std::min(n - 2, static_cast<int>(std::floor(fj)));
    const int k0 = std::min(n - 2, static_cast<int>(std::floor(fk)));
    const double tx = fi - i0;
    const double ty = fj - j0;
    const double tz = fk - k0;
    auto g = [&](int a, int b, int c) -> double { return grid[a + b * n + c * n * n]; };
    const double c00 = g(i0, j0, k0) * (1 - tx) + g(i0 + 1, j0, k0) * tx;
    const double c10 = g(i0, j0 + 1, k0) * (1 - tx) + g(i0 + 1, j0 + 1, k0) * tx;
    const double c01 = g(i0, j0, k0 + 1) * (1 - tx) + g(i0 + 1, j0, k0 + 1) * tx;
    const double c11 = g(i0, j0 + 1, k0 + 1) * (1 - tx) + g(i0 + 1, j0 + 1, k0 + 1) * tx;
    return (c00 * (1 - ty) + c10 * ty) * (1 - tz) + (c01 * (1 - ty) + c11 * ty) * tz;
  };
  const double seed = world.seed;
  for (int j = 0; j < kP; ++j) {
    for (int i = 0; i < kP; ++i) {
      const int col = i + j * kP;
      if (!cv->column_ok(*tile, col)) continue;
      const double gz = tile->z[col];
      bool carved = false;
      for (int k = 0; k < kP; ++k) {
        const double z = chunk.wz(k);
        if (z > gz || gz - z > depth) continue;
        const int idx = col + k * kP2;
        if (!is_solid(data[idx])) continue;
        if (at(i, j, k) < 0) {
          data[idx] = 0;
          carved = true;
        }
      }
      if (!carved) continue;
      // decorate: floors, stalactites / stalagmites, crystals; moss round the mouth only where
      // plants grow (above the tree line it is scree)
      const double x = chunk.wx(i);
      const double y = chunk.wy(j);
      const bool mossy = world.land_cover->climate(x, y, gz / 8).t > kTreeline;
      for (int k = 1; k < kP - 1; ++k) {
        const int idx = col + k * kP2;
        const uint16_t below = data[idx - kP2];
        const uint16_t m = data[idx];
        if (m == 0 && is_solid(below)) {
          const double z = chunk.wz(k);
          const uint32_t h = hash32(seed, x, y, z);
          const bool near_mouth = gz - z < 24;
          data[idx - kP2] = near_mouth ? (mossy ? MAT::CAVE_MOSS : MAT::GRAVEL) : (h & 7u) < 2 ? MAT::GRAVEL : MAT::CAVE_FLOOR;
          if (chunk.lod == 0) {
            if (h % 997u < 3) {
              const uint16_t c = ((h >> 12) & 1u) != 0 ? MAT::CRYSTAL_CYAN : MAT::CRYSTAL_VIOLET;
              for (int q = 0; q < 1 + static_cast<int>((h >> 8) & 3u) && k + q < kP; ++q)
                if (data[idx + q * kP2] == 0) data[idx + q * kP2] = c;
            } else if (h % 211u < 4) {
              for (int q = 0; q < 1 + static_cast<int>((h >> 9) & 3u) && k + q < kP; ++q)
                if (data[idx + q * kP2] == 0) data[idx + q * kP2] = MAT::ROCK;
            }
          }
        } else if (m == 0 && k + 1 < kP && is_solid(data[idx + kP2]) && chunk.lod == 0) {
          const uint32_t h = hash32(seed, x, y, chunk.wz(k));  // (JS: hash32(seed, x, y, wz, 5): a fifth argument hash32 ignores)
          if (h % 173u < 5)
            for (int q = 0; q < 1 + static_cast<int>((h >> 7) & 3u) && k - q > 0; ++q)
              if (data[idx - q * kP2] == 0) data[idx - q * kP2] = MAT::ROCK;
        }
      }
    }
  }
}

}  // namespace svx::city
