// structvox game — streamed levels: a chunk source (world/source.hpp) with what a game needs on
// top: where the player starts and a coarse view for the far render tier.
#pragma once

#include <climits>
#include <memory>
#include <string>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/env/fire.hpp"
#include "svx/game/appearance.hpp"
#include "svx/game/roads.hpp"
#include "svx/world/source.hpp"

namespace svx {

// Something a source places that the game makes rather than its voxels (an entity): a vehicle at
// its station - a fire engine in its bay, an ambulance at a hospital's porch, a police car in its
// garage, a lorry in a yard, a stronghold's trucks - with its pose and what it belongs to.
struct SpawnRecord {
  u64 id = 0;             // stable: the same record has the same id
  std::string kind;       // "car", "van", "pickup", "truck", "lorry", "fire_truck", "ambulance", "police", "tank", ...
  V3 pos;                 // where it stands: on the ground, world metres
  f64 yaw = 0.0;          // its heading (radians, from +x)
  std::string context;    // what it belongs to (a building's, a site's id; "": none)
};

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
  // The entities it places in a box (x, y; z ignored), in a stable order (none by default). The
  // game makes them as it makes parked cars (with its traffic: near the viewer, out of sight, and
  // gone again when out of range untouched - they come back where they were).
  virtual void spawns_in(const V3& lo, const V3& hi, std::vector<SpawnRecord>& out) const {
    (void)lo, (void)hi, (void)out;
  }
  // The fire facets of its own materials (a city's roofing, furnishings, plants): set on the
  // game's fire when it loads the source (ids no other world uses: they stay set).
  virtual void fire_materials(FireSystem& fire) const { (void)fire; }
  // Far render tier: the voxels of n coarse cells of `factor`^3 voxels each from voxel `lo`
  // (index (x * n1 + y) * n2 + z). Thin solids must survive (a cell is solid where any solid
  // covers part of it; air only where it covers the cell). Sources without a cheap coarse view
  // return false (no far tier).
  virtual bool coarse(const IVec3& lo, const IVec3& n, i32 factor, std::vector<Vox>& out) const {
    (void)lo, (void)n, (void)factor, (void)out;
    return false;
  }
  // Far render tier: the open water (sea, lakes) over the same n[0] x n[1] columns of `factor`^2
  // voxels from `lo`: the level of its surface in each (the z of its top water voxel; kNoWater:
  // none; index x * n1 + y). False: none anywhere (no water in the far tier).
  static constexpr i32 kNoWater = INT32_MIN;
  virtual bool coarse_water(const IVec3& lo, const IVec3& n, i32 factor, std::vector<i32>& out) const {
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
