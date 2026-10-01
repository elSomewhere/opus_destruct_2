// svx_city — urban districts, the land-use zones (voxel_city city/districts.js and the DISTRICTS
// registry of world/registry.js).
//
// Each district bundles its local street pattern and target block sizes (m), weighted block
// programs, the lot subdivision mode and frontage widths (m), weighted building archetypes, a
// floor range before downtown-ness scaling and weighted facade styles. A new zone is a new
// registration (sites register theirs: sites/*.cpp) plus a rule in classify_district.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "world/registry.hpp"

namespace svx::city {

// A weighted list of registry ids ([["tower", 5], ["office", 3]]): Rng::weighted picks from it.
using WeightedIds = std::vector<std::pair<std::string, double>>;

// (registrations initialize these by member designators: every member has a default)
struct DistrictStreets {
  std::string pattern{};                         // "grid", "subdivide", "organic", "none"
  std::array<std::array<double, 2>, 2> block{};  // [[w0, w1], [d0, d1]] (m)
  double pedestrian_chance = 0, merge_chance = 0;
  std::string local_class{};  // "local", "village", "rural"
  std::string lane_class{};   // ("" undefined)
  std::optional<double> lane_chance{};
  std::string paving{};  // ("" undefined)
};

struct DistrictLots {
  std::string mode{};
  std::array<double, 2> width{};  // (m)
  double alley_chance = 0;
};

struct District {
  std::string id{}, label{}, color{};
  bool port = false;  // (yards facing the water get a quay with cranes and moored ships)
  DistrictStreets streets{};
  WeightedIds block_use{};
  DistrictLots lots{};
  WeightedIds archetypes{};
  std::array<double, 2> floors{};
  WeightedIds styles{};
  // (the chance per district that a flat-roofed block gets a pitched roof: oldcore's, or a
  // flavor's through flavored_district)
  std::optional<double> pitched{};
};

// DISTRICTS: read-only once register_all() has run; _mut for registration only.
const Registry<District>& district_registry();
Registry<District>& district_registry_mut();

// city/districts.js's registrations (register_all calls it, in the reference's order).
void register_districts();

// What classify_district reads of a sub-cell's macro fields: urbanization u, core, district noise
// dn, industry noise ind, the world seed and the sub-cell's key (hashString of its id), whether
// it is a village's and whether a big lake's shore is near (a port).
struct DistrictFields {
  double u = 0, core = 0, dn = 0, ind = 0, seed = 0, key = 0;
  bool village = false, port = false;
};

// The district (its id) of a sub-cell from its macro fields.
std::string classify_district(const DistrictFields& f);

}  // namespace svx::city
