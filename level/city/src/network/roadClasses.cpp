// svx_city — network/roadClasses.hpp (voxel_city network/roadClasses.js).
#include "network/roadClasses.hpp"

#include "core/js.hpp"
#include "core/math.hpp"

namespace svx::city {

const std::array<const char*, 10> kRoadClasses = {"highway", "arterial", "collector", "local", "village", "alley", "lane", "pedestrian", "rural", "path"};

double class_rank(std::string_view cls) {
  struct Rank {
    std::string_view cls;
    double rank;
  };
  static constexpr Rank kRanks[] = {{"highway", 6}, {"arterial", 5}, {"collector", 4}, {"rural", 3}, {"local", 3},
                                    {"village", 3}, {"alley", 1},    {"lane", 1},      {"pedestrian", 2}, {"path", 0}};
  for (const Rank& r : kRanks)
    if (r.cls == cls) return r.rank;
  return js::kNaN;
}

RoadSpecs::RoadSpecs(const Value& config) {
  for (const Value::Member& m : config["roads"].members()) {
    const Value& c = m.second;
    const double travel = c["lanes"].to_number() * c["laneWidth"].to_number();
    const double carriage = travel + c["median"].to_number() + 2 * c["parking"].to_number() + 2 * c["shoulder"].num(0);
    const double hc = js::round(vx(carriage) / 2);
    const double hr = hc + vx(c["sidewalk"].to_number());
    RoadSpec s;
    s.cls = m.first;
    s.lanes = c["lanes"].to_number();
    s.lane = vx(c["laneWidth"].to_number());
    s.median = vx(c["median"].to_number());
    s.parking = vx(c["parking"].to_number());
    s.shoulder = vx(c["shoulder"].num(0));
    s.sidewalk = vx(c["sidewalk"].to_number());
    s.hc = hc;
    s.hr = hr;
    s.corner = vx(c["cornerRadius"].to_number());
    s.paved = true;  // (cls !== "rural" || true)
    // (an object's key set once: a later key of the same name replaces it in place)
    bool replaced = false;
    for (RoadSpec& o : specs_)
      if (o.cls == s.cls) o = s, replaced = true;
    if (!replaced) specs_.push_back(s);
  }
  // footpaths inside parks
  RoadSpec path;
  path.cls = "path";
  path.sidewalk = vx(2.5);
  path.hr = vx(1.25);
  path.corner = vx(1);
  path.paved = true;
  bool replaced = false;
  for (RoadSpec& o : specs_)
    if (o.cls == "path") o = path, replaced = true;
  if (!replaced) specs_.push_back(path);
}

const RoadSpec* RoadSpecs::get(std::string_view cls) const {
  for (const RoadSpec& s : specs_)
    if (s.cls == cls) return &s;
  return nullptr;
}

}  // namespace svx::city
