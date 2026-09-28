// structvox game — procedural test levels (docs/API.md loadProcedural: 'rooms' | 'city' | 'tower').
//
// All worlds sit on an anchored ground layer (z < 0); structural voxels are reinforced
// concrete. Dimensions are in voxels of pitch h (default 0.125 m).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/world/grid.hpp"
#include "svx/world/world.hpp"

namespace svx {

// A grid of the world's placed at its own origin and rotation (docs/GRIDS.md).
struct ProcGrid {
  GridFrame frame;
  VoxelGrid grid;
};

struct ProcWorld {
  VoxelGrid grid;
  std::vector<ProcGrid> grids;  // added after the world grid, in order (World::add_grid: ids 1, 2, ...)
  V3 spawn_pos{0, 0, 0};  // feet, metres
  V3 spawn_dir{1, 0, 0};
};

// kind: "rooms", "city", "tower", "yard" (wood, stone, glass, steel, reinforced concrete), the
// physics tests "slab", "chimney", "bridge", and "angles" (structures in oriented grids: a turned
// tower, a diagonal bridge deck, a ramp, a braced portal, masonry walls, crates and a leaning
// monolith). Unknown kinds fall back to "rooms".
ProcWorld make_procedural(const std::string& kind, u64 seed, f64 h = 0.125);

// Adds a world's oriented grids to the world it was loaded into (after World::load or Game::load
// took its grid), in order.
void add_grids(World& world, std::vector<ProcGrid>&& grids);

}  // namespace svx
