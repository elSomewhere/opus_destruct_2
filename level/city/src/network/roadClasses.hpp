// svx_city — road cross sections (voxel_city network/roadClasses.js), in voxels, derived from the
// metric config:
//
//   |sidewalk|parking|lanes...|median|...lanes|parking|sidewalk|
//   hr = half right-of-way, hc = half carriageway (curb to curb)
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "core/value.hpp"

namespace svx::city {

// ROAD_CLASSES
extern const std::array<const char*, 10> kRoadClasses;
// CLASS_RANK[cls]; NaN for a class it does not list (JS: undefined; callers write `?? 0`).
double class_rank(std::string_view cls);

struct RoadSpec {
  std::string cls;
  double lanes = 0, lane = 0, median = 0, parking = 0, shoulder = 0, sidewalk = 0, hc = 0, hr = 0, corner = 0;
  bool paved = true;
};

// roadSpecs(config): a spec per class of config.roads (in its key order), then `path` (footpaths
// inside parks).
class RoadSpecs {
 public:
  explicit RoadSpecs(const Value& config);
  // specs[cls], or null (JS: undefined).
  const RoadSpec* get(std::string_view cls) const;
  const std::vector<RoadSpec>& all() const { return specs_; }

 private:
  std::vector<RoadSpec> specs_;
};

// (the reference caches the specs per config object; they are a few numbers per class, made
// when asked: hoist them out of loops)
inline RoadSpecs road_specs(const Value& config) { return RoadSpecs(config); }

}  // namespace svx::city
