// svx_city — natural caves under open country (voxel_city nature/caves.js):
//
//   worm tunnels  where two 3D noise fields are both near zero (the intersection of two
//                 isosurfaces is a winding tube); where a tube meets the surface it opens as a
//                 cave mouth
//   caverns       deeper chambers where a low-frequency 3D noise is high
//   decoration    gravel / moss floors, stalactites and stalagmites, rare glowing crystal clusters
//
// Caves appear in cave regions (a 2D mask), within `caves.depth` metres of the surface, never
// under towns, lots, sites, roads or open water. Noise is sampled on a coarse grid and
// interpolated.
//
// The feature source (caveSource: id "caves", order 1, up to LOD 1) reads the column tile of
// voxel/compose (its kind, water, top and z columns and its z range); cave_z_range and
// cave_rasterize take those columns as a CaveColumns view, so compose's FeatureSource wraps them
// once its GroundTile exists. Immutable: any thread may use them.
#pragma once

#include <cstdint>

#include "core/noise.hpp"
#include "core/rect.hpp"

namespace svx::city {

class World;
class ChunkBuffer;

// The columns of a ground tile (voxel/compose's tile) the caves read: kP x kP padded columns,
// index i + j kP. kind: 4 natural ground; water: the water surface (INT32_MIN: none); top: the
// surface material; z: the ground height (voxels); z_min, z_max: the tile's ground range.
struct CaveColumns {
  const uint8_t* kind = nullptr;
  const int32_t* water = nullptr;
  const uint16_t* top = nullptr;
  const int32_t* z = nullptr;
  double z_min = 0, z_max = 0;
};

// The view of a tile that has the reference's fields (kind, water, top, z as contiguous arrays;
// z_min, z_max).
template <class Tile>
CaveColumns cave_columns(const Tile& t) {
  return {t.kind.data(), t.water.data(), t.top.data(), t.z.data(), static_cast<double>(t.z_min), static_cast<double>(t.z_max)};
}

class Caves {
 public:
  explicit Caves(const World& world);
  Caves(const Caves&) = delete;
  Caves& operator=(const Caves&) = delete;

  const World* world;
  // config.caves (read once): enabled (truthiness), depth (m)
  bool enabled = false;
  double depth = 0;
  SimplexNoise n_a, n_b, n_c, n_mask;

  // 0..1 cave-region strength at a column.
  double region(double x, double y) const;
  // Cave field at a voxel (x, y, z) at depth d (m) below the surface: negative inside a cave.
  double field(double x, double y, double z, double d) const;
  // Can caves exist under this ground-tile column? Natural ground without water; caves do not
  // break through snowfields and glaciers.
  bool column_ok(const CaveColumns& tile, int idx) const;
};

// caveSource
constexpr const char* kCaveSourceId = "caves";
constexpr double kCaveSourceOrder = 1;
constexpr int kCaveSourceMaxLod = 1;  // (cave interiors are seen from inside or through a mouth up close)

// caveSource.zRange: the z range [*z0, *z1] (LOD 0 voxels) caves may take under a column tile
// whose padded rect is `rect` (a coarse probe of the cave band), or false. (A null tile: false.)
bool cave_z_range(const World& world, const Rect& rect, int lod, const CaveColumns* tile, double* z0, double* z1);
// caveSource.rasterize: carves and decorates the caves of a chunk. (A null tile: nothing.)
void cave_rasterize(const World& world, ChunkBuffer& chunk, const CaveColumns* tile);

}  // namespace svx::city
