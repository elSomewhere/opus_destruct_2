// structvox — procedural test worlds (docs/API.md loadProcedural: 'rooms' | 'city' | 'tower').
//
// All worlds sit on an anchored ground layer (z < 0); structural voxels are reinforced
// concrete. Dimensions are in voxels of pitch h (default 0.125 m).
#pragma once

#include <array>
#include <string>

#include "svx/world/grid.hpp"

namespace svx {

struct ProcWorld {
  VoxelGrid grid;
  std::array<f64, 3> spawn_pos{0, 0, 0};  // feet, metres
  std::array<f64, 3> spawn_dir{1, 0, 0};
};

// kind: "rooms", "city" or "tower". Unknown kinds fall back to "rooms".
ProcWorld make_procedural(const std::string& kind, u64 seed, f64 h = 0.125);

}  // namespace svx
