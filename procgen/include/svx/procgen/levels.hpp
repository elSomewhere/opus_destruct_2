// structvox procgen — procedural test levels (docs/API.md loadProcedural: 'rooms' | 'city' | 'tower').
//
// All worlds sit on an anchored ground layer (z < 0); structural voxels are reinforced
// concrete. Dimensions are in voxels of pitch h (default 0.125 m).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/game/level.hpp"

namespace svx {

// kind: "rooms", "city", "tower", "yard" (wood, stone, glass, steel, reinforced concrete), the
// physics tests "slab", "chimney", "bridge", "angles" (structures in oriented grids: a turned
// tower, a diagonal bridge deck, a ramp, a braced portal, masonry walls, crates and a leaning
// monolith) and "machines" (pieces on joints to structures: a lift, a turntable, a drawbridge
// and a crane with a wrecking ball on driven joints, a pendulum, a hinged door, a chain). Unknown
// kinds fall back to "rooms".
Level make_procedural(const std::string& kind, u64 seed, f64 h = 0.125);

}  // namespace svx
