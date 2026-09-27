// structvox game — procedural test levels (docs/API.md loadProcedural: 'rooms' | 'city' | 'tower').
//
// All worlds sit on an anchored ground layer (z < 0); structural voxels are reinforced
// concrete. Dimensions are in voxels of pitch h (default 0.125 m).
#pragma once

#include <array>
#include <string>

#include "svx/base/vec.hpp"
#include "svx/world/grid.hpp"

namespace svx {

struct ProcWorld {
  VoxelGrid grid;
  V3 spawn_pos{0, 0, 0};  // feet, metres
  V3 spawn_dir{1, 0, 0};
};

// kind: "rooms", "city", "tower", "yard" (wood, stone, glass, steel, reinforced concrete) and the
// physics tests "slab", "chimney", "bridge". Unknown kinds fall back to "rooms".
ProcWorld make_procedural(const std::string& kind, u64 seed, f64 h = 0.125);

}  // namespace svx
