// svx_city — furniture of civic buildings and shops (voxel_city buildings/interior/civicPrefabs.js):
// hospitals, police stations, museums and galleries, venues and cinemas, supermarkets, shops, fire
// stations, hotels, warehouses. The same frame and records as prefabs.hpp: a along the wall, b
// into the room, z above the finished floor; build(rng, opt) returns boxes (m = 0 carves).
#pragma once

#include <string_view>
#include <vector>

#include "buildings/interior/prefabs.hpp"

namespace svx::city {

// CIVIC_PREFABS, in the reference's key order.
const std::vector<Prefab>& civic_prefabs();
// CIVIC_PREFABS[key], or null.
const Prefab* civic_prefab(std::string_view key);

}  // namespace svx::city
