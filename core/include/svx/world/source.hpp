// structvox — streamed worlds: chunk sources that generate any 32^3 chunk deterministically on
// demand, so a large world is resident only around the focus points (World::enable_streaming).
#pragma once

#include <string>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/world/grid.hpp"
#include "svx/world/joint_desc.hpp"

namespace svx {

// An oriented grid a source places in its world (docs/GRIDS.md): a voxel lattice with a frame of
// its own (its voxel p is centred at origin + rot (h p)).
struct SourceGrid {
  // unique and stable (not 0, below 2^30): the same grid gets the same id every time (a streamed
  // world gives the grids made in play - add_grid - ids of 2^30 and up: never one of these)
  u32 id = 0;
  V3 origin;
  Quat rot;
  f64 voxel_size = 0.0;  // its voxel size (m; 0: the world's)
  i32 priority = 0;      // (overlaps: GridDesc::priority)
};

// A joint a source places in its world: its id (unique and stable, not 0, below 2^31: the world
// gives it the joint id 2^31 + id) and what it holds together.
struct SourceJoint {
  u32 id = 0;
  JointDesc desc;
};

// The tallest content a column of a streamed world has (chunks: ChunkSource::column_range).
constexpr i32 kMaxColumnChunks = 1024;

// (Its calls must not throw: the core is built without exceptions, so one escaping into it is
// not contained - a host catching it finds the world mid-tick.)
class ChunkSource {
 public:
  virtual ~ChunkSource() = default;
  // Voxels of one chunk (kChunkVox, Chunk::v order). Returns false if the chunk is all air.
  // A pure function of the chunk: called from several threads at once.
  // Generated structures should stand under their own weight; the world designs (strengthens)
  // the members that do not when it first touches them.
  virtual bool generate(const IVec3& chunk, std::vector<Vox>& out) const = 0;
  // World extent in chunks: [lo, hi). Outside it the world is air. (The world holds it within
  // the voxel key range - kVoxelLimit.)
  virtual IVec3 chunk_lo() const = 0;
  virtual IVec3 chunk_hi() const = 0;
  // The chunks of column (cx, cy) that hold content: [*z_lo, *z_hi) (at most kMaxColumnChunks; the
  // world holds them within the extent). Below z_lo the column is uniformly *below (anchored rock,
  // say), above z_hi it is air: the world generates only the content, and the chunk under it as
  // its fill (a floor, where below is solid), and keeps the rest implicit - solid below (a support
  // to what reaches into it, as the unknown world is), air above - until something changes it
  // there (a carve, an edit: it is made then from the fill). Default: the whole extent, air below
  // (every chunk of the column generated). A pure function of the column, called from the world's
  // thread; generate() is never asked for a chunk outside the range.
  virtual void column_range(i32 cx, i32 cy, i32* z_lo, i32* z_hi, Vox* below) const {
    (void)cx, (void)cy;
    *z_lo = chunk_lo()[2];
    *z_hi = chunk_hi()[2];
    *below = kAir;
  }
  // The region a chunk's changes are remembered and forgotten with (StreamConfig::archive_mb):
  // a unit that should come back whole, like a city block, so that no building returns in half.
  // Default: 8 x 8 chunk columns (32 m at the default voxel size), all heights.
  virtual u64 region(const IVec3& chunk) const { return key3(chunk[0] >> 3, chunk[1] >> 3, 0); }
  // The values of a layer (World::add_layer) the source makes, e.g. "water" for its lakes and
  // seas: kChunkVox values in Chunk::v order; false: none (all zero). Called from the world's
  // thread after generate().
  virtual bool generate_layer(const IVec3& chunk, const std::string& layer, std::vector<u8>& out) const {
    (void)chunk, (void)layer, (void)out;
    return false;
  }
  // Oriented grids of the source's (docs/GRIDS.md): the grids at home in a chunk - generated
  // whole when that chunk is, evicted with it (their changes archived with its region, forgotten
  // with it). A grid is at home in one chunk only (e.g. the chunk of its origin). Called from the
  // world's thread after generate().
  virtual std::vector<SourceGrid> grids(const IVec3& chunk) const {
    (void)chunk;
    return {};
  }
  // The voxels of one of those grids, in its own coordinates (false: none).
  virtual bool generate_grid(u32 id, VoxelGrid& out) const {
    (void)id, (void)out;
    return false;
  }
  // The joints at home in a chunk (docs/MOTION.md: a machine's, a hanging part's): made when the
  // chunk's grids are, their anchors on those grids' voxels or the world grid's. A machine that
  // went out of range is archived as it was, with its joints, and comes back so: its source's
  // joints are made again only when it was forgotten. Called from the world's thread.
  virtual std::vector<SourceJoint> joints(const IVec3& chunk) const {
    (void)chunk;
    return {};
  }
};

struct StreamConfig {
  f64 load_radius = 96.0;    // m (horizontal): chunks within are resident (at most 256 chunks)
  f64 evict_radius = 128.0;  // m: chunks beyond are evicted (hysteresis)
  int chunks_per_tick = 6;   // generation budget per tick (1 .. 4096)
  // byte budget: above this much resident grid memory, chunks beyond load_radius are evicted
  // farthest first (checked every 30 ticks; 0 = the radii alone)
  f64 max_resident_mb = 0.0;
  // The changes of chunks that are not resident (the change archive): a fixed arena of this
  // many MB, allocated once. When it is full, the region seen least recently (and not resident
  // now) is forgotten: its chunks come back from the source as they were generated, and are
  // designed again when first touched. 0 = keep every change (a bounded level streamed from a
  // file: its changes are bounded by its size). At most 4096 (1024 on a 32-bit build).
  f64 archive_mb = 64.0;
  // Regions out of range for this long are forgotten even with room left: the world heals out
  // of sight (0 = only when the archive is full).
  f64 forget_after_s = 0.0;
};

}  // namespace svx
