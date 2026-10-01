// svx_city — city flavors (voxel_city city/flavors.js): a settlement-level character that
// re-weights architectural styles per district and scales building heights.
//
// A flavor may also shape the street network: main_road (the class of the main streets),
// main_road_wobble (0..1, how much they bend), cobble_within (main streets inside this normalized
// distance are cobbled), collectors false (no collector grid), patterns (street pattern per
// district), adaptive (the whole town laid out organically, each part at the block size of its
// own district), pitched (the chance per district that a flat-roofed block gets a pitched roof),
// old_core (radius, as a share of the town's, of a historic core of irregular cobbled blocks: the
// "oldcore" district), church_style and church_dome. Settlements pick one by weight (the spawn
// city is always "modern"); flavors with a `when` climate predicate take precedence where it holds.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "city/districts.hpp"
#include "core/cache.hpp"
#include "core/js.hpp"
#include "world/registry.hpp"

namespace svx::city {

// What flavors read of a settlement record (world/fields.js settlements and villages,
// world/island.js towns): pass one made from the settlement, or null for none.
struct FlavorSettlement {
  std::string flavor;        // a flavor set by the world (an island's town) ("": null)
  bool origin_cell = false;  // i === 0 && j === 0: the spawn town's cell (a village's i is a string: never)
  bool island = false;
  bool village = false;
  double t = js::kNaN, m = js::kNaN;  // regional climate at the centre (NaN: undefined)
  double style = 0;                   // the settlement's style hash (uint32)
};

// Where a flavor maps a district: { d (the distance to the town centre in radii), dn (district
// noise), u, core, ind (industry noise) } - NaN where JS leaves a field undefined.
struct FlavorPlace {
  double d = js::kNaN, dn = js::kNaN, u = js::kNaN, core = js::kNaN, ind = js::kNaN;
};

// A JS object keyed by district id, in its key order (no key is integer-like).
template <class T>
using ByDistrict = std::vector<std::pair<std::string, T>>;

// (registrations initialize it by member designators: every member has a default)
struct Flavor {
  std::string id{};
  double weight = 0;
  double floor_scale = 1;
  std::function<bool(const FlavorSettlement&)> when{};  // (empty: none)
  std::optional<double> old_core{};
  bool adaptive = false;
  bool collectors = true;   // (JS: undefined unless false)
  std::string main_road{};  // ("": undefined)
  std::optional<double> main_road_wobble{}, cobble_within{};
  std::string church_style{};              // ("": undefined)
  std::vector<std::string> church_dome{};  // material names (empty: undefined)
  ByDistrict<std::string> patterns{};
  ByDistrict<double> pitched{};
  // (a value may be undefined: a village flavor's downtown)
  ByDistrict<std::optional<WeightedIds>> styles{};
  ByDistrict<WeightedIds> archetypes{};
  ByDistrict<std::array<double, 2>> floors{};
  ByDistrict<WeightedIds> block_use{};
  // districts: a map of district ids, or a function of the district picked from the macro fields
  // and the place (district_fn set)
  ByDistrict<std::string> districts{};
  std::function<std::string(const std::string& id, const FlavorPlace& f)> district_fn{};
  // (villageFlavor's cache: this flavor's village variant, made on first use)
  Lazy<Flavor> village{};
};

// FLAVORS: read-only once register_all() has run; _mut for registration only.
const Registry<Flavor>& flavor_registry();
Registry<Flavor>& flavor_registry_mut();

// city/flavors.js's registrations (register_all calls it, in the reference's order).
void register_flavors();

// The flavor of a settlement (null: none - "modern"); a village's is its region's look at
// village height (the flavor's "<id>Village" variant).
const Flavor& flavor_of(const FlavorSettlement* settlement);
// A flavor's village variant (villageFlavor).
const Flavor& village_flavor(const Flavor& f);

// The district as seen through a flavor: styles, archetype weights, floor range and block programs
// per district (unregistered style / archetype ids skipped, so a flavor may name optional kits).
// (JS returns the district itself when the flavor changes nothing: here an equal copy.)
District flavored_district(const District& district, const Flavor& flavor);

// The district id as re-mapped by a settlement's flavor (Soviet towns build microdistricts); ctx
// (null: none) lets a flavor map by place.
std::string flavored_district_id(const std::string& district_id, const FlavorSettlement* settlement, const FlavorPlace* ctx = nullptr);

}  // namespace svx::city
