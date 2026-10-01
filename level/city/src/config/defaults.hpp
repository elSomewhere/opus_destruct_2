// svx_city — the generator's configuration (voxel_city config/defaults.js): a Value tree of
// the reference's DEFAULT_CONFIG with a world's overrides merged on top.
#pragma once

#include "core/value.hpp"

namespace svx::city {

// DEFAULT_CONFIG.
const Value& default_config();
// makeConfig(overrides): the defaults, the world mode's defaults (an island's terrain), then the
// overrides, deeply merged; a wrapping world's lattices fitted to its size. Idempotent.
Value make_config(const Value& overrides);
// world/island.js islandTerrain(config): an island's terrain scales (its size and peak).
Value island_terrain(const Value& config);

}  // namespace svx::city
