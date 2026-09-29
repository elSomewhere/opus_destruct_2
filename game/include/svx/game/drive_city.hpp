// structvox game — an endless procedural city to drive through (docs/VEHICLES.md).
//
// A grid of streets and avenues - lanes with their markings, kerbs and sidewalks, street lamps
// and trees, signals at the junctions, parking along the streets - and on its blocks houses,
// apartment blocks, shops with glass fronts, office towers, warehouses, parks and car parks, all
// destructible (masonry, reinforced concrete, glass, timber, steel), painted. It goes on in
// every direction as far as the world's voxel keys reach (some 130 km each way), generated
// chunk by chunk as the viewer goes. Its roads are a RoadNetwork for traffic.
#pragma once

#include <memory>

#include "svx/game/source.hpp"

namespace svx {

std::unique_ptr<GameSource> make_drive_city(u64 seed, f64 h = 0.125);

}  // namespace svx
