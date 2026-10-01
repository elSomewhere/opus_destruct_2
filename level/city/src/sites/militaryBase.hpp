// svx_city — the military research base (voxel_city sites/militaryBase.js): a fenced compound in
// the countryside.
//
// Surface: a perimeter fence with a gate and a guard booth facing the nearest road (joined by a
// driveway), watchtowers, a radar mast, fuel tanks, a helipad, an HQ, barracks and hangars
// (regular archetypes: full interiors; the cell plan merges them through the kind's surface hook)
// and a concrete bunker over the stairs down.
//
// Underground: one military sector of 2-3 levels 15 m apart under the compound (sites/complex),
// its first level 14 m down, entered by a stair shaft from the bunker.
//
// A site of this kind is a Site whose plan is a MilitaryBasePlan (its placement the default level
// pad); register_military_base() registers its district ("military") and its kind
// ("militaryBase") in that order (register_all calls it after the complex themes).
#pragma once

#include <vector>

#include "core/rect.hpp"
#include "sites/kit.hpp"
#include "world/sites.hpp"

namespace svx::city {

// The base's plan: the gate (plan_gate; its driveway is SitePlan::drive), the internal ring road
// (its outer rect and width), the inner rect the buildings stand in, the building lots (office,
// walkup, two warehouses), the bunker, the helipad, the apron (radar mast, fuel tanks, trucks),
// the number of underground levels; bounds: the blend rect and the driveway.
struct MilitaryBasePlan : SitePlan {
  char gate_side = 'S';
  Rect gate{};
  Rect ring{};
  double ring_w = 0;
  Rect inner{};
  std::vector<SiteLot> lots;
  Rect bunker{};
  SiteHelipad helipad{};
  Rect apron{};
  double levels = 0;
};

// sites/militaryBase.js's registrations: DISTRICTS "military", SITES "militaryBase".
void register_military_base();

}  // namespace svx::city
