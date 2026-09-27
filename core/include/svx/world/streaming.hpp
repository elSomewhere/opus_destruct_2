// structvox — streamed worlds (plan §B8, Phase 6): chunk sources that generate any 32^3 chunk
// deterministically on demand, so a large world is resident only around the viewer.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "svx/world/grid.hpp"

namespace svx {

class ChunkSource {
 public:
  virtual ~ChunkSource() = default;
  // Voxels of one chunk (kChunkVox, Chunk::v order). Returns false if the chunk is all air.
  // A pure function of the chunk: called from several threads at once.
  virtual bool generate(const IVec3& chunk, std::vector<Vox>& out) const = 0;
  // World extent in chunks: [lo, hi).
  virtual IVec3 chunk_lo() const = 0;
  virtual IVec3 chunk_hi() const = 0;
  virtual std::array<f64, 3> spawn_pos() const = 0;
  virtual std::array<f64, 3> spawn_dir() const = 0;
  // Far render tier (plan Phase 6 "far render LOD"): the voxels of n coarse cells of
  // `factor`^3 voxels each from voxel `lo` (index (x * n1 + y) * n2 + z). Thin solids must
  // survive (a cell is solid where any solid covers part of it; air only where it covers the
  // cell). Sources without a cheap coarse view return false (no far tier).
  virtual bool coarse(const IVec3& lo, const IVec3& n, i32 factor, std::vector<Vox>& out) const {
    (void)lo, (void)n, (void)factor, (void)out;
    return false;
  }
};

// A procedural city: a grid of blocks (streets between them) with column / slab buildings of
// seeded height and layout, on an anchored ground slab. extent_m: side of the square world.
std::unique_ptr<ChunkSource> make_city_source(u64 seed, f64 extent_m = 1000.0, f64 h = 0.125);

struct StreamConfig {
  f64 load_radius = 96.0;    // m (horizontal): chunks within are resident
  f64 evict_radius = 128.0;  // m: chunks beyond are evicted (hysteresis)
  int chunks_per_tick = 6;   // generation budget per tick
  // byte budget (plan §B8): above this much resident grid memory, chunks beyond load_radius
  // are evicted farthest first (checked every 30 ticks; 0 = the radii alone)
  f64 max_resident_mb = 0.0;
  // far render tier: coarse meshes of far_tile x far_tile chunk columns (all heights) at
  // far_factor voxels per cell, for tiles wholly beyond evict_radius and within far_radius
  f64 far_radius = 320.0;
  i32 far_tile = 8;
  i32 far_factor = 8;
  int far_tiles_per_tick = 1;
};

}  // namespace svx
