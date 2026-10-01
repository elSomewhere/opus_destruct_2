// structvox game — streamed levels: a chunk source (world/source.hpp) with what a game needs on
// top: where the player starts and a coarse view for the far render tier.
#pragma once

#include <memory>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/game/appearance.hpp"
#include "svx/game/roads.hpp"
#include "svx/world/source.hpp"

namespace svx {

class GameSource : public ChunkSource {
 public:
  virtual V3 spawn_pos() const = 0;  // feet, metres
  virtual V3 spawn_dir() const = 0;
  // Its roads, for traffic (nullptr: none).
  virtual const RoadNetwork* roads() const { return nullptr; }
  // Its looks (nullptr: its materials' colours): the game then has a regenerable "look" layer
  // (LayerSpec::regenerable, values from generate_layer(chunk, "look", ...)) and meshes a face of
  // (material, look) with the table's appearance (svx/game/appearance.hpp). Its oriented grids
  // carry their looks in their own "look" layer (generate_grid).
  virtual std::shared_ptr<const AppearanceTable> appearances() const { return nullptr; }
  // Far render tier: the voxels of n coarse cells of `factor`^3 voxels each from voxel `lo`
  // (index (x * n1 + y) * n2 + z). Thin solids must survive (a cell is solid where any solid
  // covers part of it; air only where it covers the cell). Sources without a cheap coarse view
  // return false (no far tier).
  virtual bool coarse(const IVec3& lo, const IVec3& n, i32 factor, std::vector<Vox>& out) const {
    (void)lo, (void)n, (void)factor, (void)out;
    return false;
  }
};

// Far render tier: coarse meshes of tile x tile chunk columns (all heights) at `factor` voxels
// per cell, for tiles wholly beyond the stream's evict radius and within `radius`.
struct FarConfig {
  f64 radius = 320.0;
  i32 tile = 8;
  i32 factor = 8;
  int tiles_per_tick = 1;
};

}  // namespace svx
