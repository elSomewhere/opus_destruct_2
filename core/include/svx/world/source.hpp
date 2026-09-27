// structvox — streamed worlds: chunk sources that generate any 32^3 chunk deterministically on
// demand, so a large world is resident only around the focus points (World::enable_streaming).
#pragma once

#include <vector>

#include "svx/world/grid.hpp"

namespace svx {

class ChunkSource {
 public:
  virtual ~ChunkSource() = default;
  // Voxels of one chunk (kChunkVox, Chunk::v order). Returns false if the chunk is all air.
  // A pure function of the chunk: called from several threads at once.
  // Generated structures should stand under their own weight; the world designs (strengthens)
  // the members that do not when it first touches them.
  virtual bool generate(const IVec3& chunk, std::vector<Vox>& out) const = 0;
  // World extent in chunks: [lo, hi). Outside it the world is air.
  virtual IVec3 chunk_lo() const = 0;
  virtual IVec3 chunk_hi() const = 0;
};

struct StreamConfig {
  f64 load_radius = 96.0;    // m (horizontal): chunks within are resident
  f64 evict_radius = 128.0;  // m: chunks beyond are evicted (hysteresis)
  int chunks_per_tick = 6;   // generation budget per tick
  // byte budget: above this much resident grid memory, chunks beyond load_radius are evicted
  // farthest first (checked every 30 ticks; 0 = the radii alone)
  f64 max_resident_mb = 0.0;
};

}  // namespace svx
