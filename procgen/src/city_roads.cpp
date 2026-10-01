// structvox procgen — the city generator's road network in the game (svx/procgen/city_roads.hpp).
#include "svx/procgen/city_roads.hpp"

#include "svx/base/dmath.hpp"

namespace svx {

namespace {

V3 v3(const std::array<double, 3>& p) { return V3{p[0], p[1], p[2]}; }

Lane lane_of(const city::RoadLane& l) {
  Lane out;
  out.id = l.id;
  out.a = v3(l.a);
  out.b = v3(l.b);
  out.width = l.width;
  out.speed = l.speed;
  return out;
}

Walk walk_of(const city::RoadWalk& w) {
  Walk out;
  out.id = w.id;
  out.a = v3(w.a);
  out.b = v3(w.b);
  out.inset = v3(w.inset);
  out.width = w.width;
  out.crossing = w.crossing;
  return out;
}

}  // namespace

CityRoadNetwork::CityRoadNetwork(std::shared_ptr<const city::World> world, const city::RoadNetworkOptions& options)
    : net_(std::move(world), options) {}

void CityRoadNetwork::lanes_in(const V3& lo, const V3& hi, std::vector<Lane>& out) const {
  for (const city::RoadLane& l : net_.lanes_in({lo.x, lo.y}, {hi.x, hi.y})) out.push_back(lane_of(l));
}

bool CityRoadNetwork::lane(u64 id, Lane* out) const {
  const std::optional<city::RoadLane> l = net_.lane(id);
  if (!l) return false;
  if (out) *out = lane_of(*l);
  return true;
}

void CityRoadNetwork::next(u64 lane, std::vector<std::pair<u64, int>>& out) const {
  for (const city::RoadTurn& t : net_.next(lane)) out.push_back(t);
}

bool CityRoadNetwork::green(u64 lane, f64 time) const { return net_.green(lane, time); }

void CityRoadNetwork::parking_in(const V3& lo, const V3& hi, std::vector<ParkingSpot>& out) const {
  for (const city::RoadParking& p : net_.parking_in({lo.x, lo.y}, {hi.x, hi.y})) {
    ParkingSpot s;
    s.pos = v3(p.pos);
    s.yaw = dm::atan2(p.heading[1], p.heading[0]);
    s.id = p.id;
    out.push_back(s);
  }
}

void CityRoadNetwork::walks_in(const V3& lo, const V3& hi, std::vector<Walk>& out) const {
  for (const city::RoadWalk& w : net_.walks_in({lo.x, lo.y}, {hi.x, hi.y})) out.push_back(walk_of(w));
}

bool CityRoadNetwork::walk(u64 id, Walk* out) const {
  const std::optional<city::RoadWalk> w = net_.walk(id);
  if (!w) return false;
  if (out) *out = walk_of(*w);
  return true;
}

void CityRoadNetwork::walk_next(u64 id, int end, std::vector<std::pair<u64, int>>& out) const {
  for (const city::RoadTurn& t : net_.walk_next(id, end)) out.push_back(t);
}

bool CityRoadNetwork::walk_open(u64 id, f64 time) const { return net_.walk_open(id, time); }

std::unique_ptr<CityRoadNetwork> make_city_roads(std::shared_ptr<const city::World> world, const city::RoadNetworkOptions& options) {
  return std::make_unique<CityRoadNetwork>(std::move(world), options);
}

}  // namespace svx
