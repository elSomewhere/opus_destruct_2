// svx_city — the World's caches (World.js's LRUs, createWorld's dressing cache): products by
// structural key, shared across threads (core/cache.hpp). Sizes are the reference's counts, raised
// where several threads generate at once.
#pragma once

#include <cstdint>
#include <string>

#include "core/cache.hpp"
#include "world/World.hpp"

namespace svx::city {

// A cell's key (i, j: integers below 2^31 in magnitude).
inline uint64_t cell_key(double i, double j) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(i))) << 32) | static_cast<uint32_t>(static_cast<int32_t>(j));
}
// A column tile's key: (lod, cx, cy), cx and cy below 2^28 in magnitude.
inline uint64_t tile_key(int lod, double cx, double cy) {
  return (static_cast<uint64_t>(lod) << 58) | ((static_cast<uint64_t>(static_cast<int64_t>(cx)) & 0x1FFFFFFFull) << 29) |
         (static_cast<uint64_t>(static_cast<int64_t>(cy)) & 0x1FFFFFFFull);
}

struct World::Caches {
  MemoCache<uint64_t, CellNet> cell_nets{256};
  MemoCache<uint64_t, RoadView> road_views{128};
  MemoCache<uint64_t, CellPlan> cell_plans{128};
  MemoCache<std::string, BuildingPlan> building_plans{384};  // `${env.id}@${env.R.x0},${env.R.y0}`
  MemoCache<uint64_t, GroundTile> ground_tiles{3072};
  MemoCache<uint64_t, Dressing> dressings{128};
};

}  // namespace svx::city
