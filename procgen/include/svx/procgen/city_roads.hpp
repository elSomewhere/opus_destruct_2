// structvox procgen — the city generator's road network in the game (docs/VEHICLES.md, the city's
// road network): svx_city's export of voxel_city's streets, highways, walkways and kerbside parking
// (svx/city/roads.hpp: metres in the export's frame, the game's conventions) as the game's
// RoadNetwork (svx/game/roads.hpp), which its traffic drives and its people walk. The city's
// GameSource returns it from roads() once the city's chunks stream (a later step).
#pragma once

#include <memory>

#include "svx/city/roads.hpp"
#include "svx/game/roads.hpp"

namespace svx {

// A city world's roads (city::make_world). Every answer is the city network's (ids, records in id
// order); a parking place's yaw is its heading's angle. Thread-safe.
class CityRoadNetwork final : public RoadNetwork {
 public:
  explicit CityRoadNetwork(std::shared_ptr<const city::World> world, const city::RoadNetworkOptions& options = {});

  void lanes_in(const V3& lo, const V3& hi, std::vector<Lane>& out) const override;
  bool lane(u64 id, Lane* out) const override;
  void next(u64 lane, std::vector<std::pair<u64, int>>& out) const override;
  bool green(u64 lane, f64 time) const override;
  void parking_in(const V3& lo, const V3& hi, std::vector<ParkingSpot>& out) const override;
  void walks_in(const V3& lo, const V3& hi, std::vector<Walk>& out) const override;
  bool walk(u64 id, Walk* out) const override;
  void walk_next(u64 id, int end, std::vector<std::pair<u64, int>>& out) const override;
  bool walk_open(u64 id, f64 time) const override;

  // The city's own network (its records' keys, signals, walk kinds).
  const city::RoadNetwork& city() const { return net_; }

 private:
  city::RoadNetwork net_;
};

std::unique_ptr<CityRoadNetwork> make_city_roads(std::shared_ptr<const city::World> world, const city::RoadNetworkOptions& options = {});

}  // namespace svx
