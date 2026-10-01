// svx_city — the research complex (voxel_city sites/researchComplex.js), Black Mesa style: a
// campus on the surface over a deep multi-sector facility.
//
// Surface: a fenced campus with a gate, a ring road, an HQ and lab buildings (office archetypes:
// full interiors), a hangar, a parking garage (the cell plan merges them through the kind's
// surface hook), radomes and a dish array, fuel tanks, and portal blocks over the stairs down.
//
// Underground: four sectors in a 2 x 2 grid under the campus (lab, containment, power and
// barracks themes, shuffled), each 2-3 levels at staggered depths, joined by a tram loop at the
// bottom; the two portals lead down to the sectors under them (one each), the rest are reached by
// tram.
//
// A site of this kind is a Site whose plan is a ResearchComplexPlan (its placement the default
// level pad); register_research_complex() registers its district ("research") and its kind
// ("researchComplex") in that order (register_all calls it after the military base's).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/rect.hpp"
#include "sites/kit.hpp"
#include "world/sites.hpp"

namespace svx::city {

// The campus's plan: the gate (plan_gate; its driveway is SitePlan::drive), the ring road, the
// inner rect, the building lots (three offices, a warehouse, a garage, each with its style), the
// two portal blocks, the helipad, the radomes, the dish array's and the fuel tanks' places, the
// sectors' themes and levels; bounds: the blend rect and the driveway.
struct ResearchComplexPlan : SitePlan {
  char gate_side = 'S';
  Rect gate{};
  Rect ring{};
  double ring_w = 0;
  Rect inner{};
  std::vector<SiteLot> lots;
  std::vector<Rect> portals;
  SiteHelipad helipad{};
  std::vector<Point2> radomes;
  Point2 dishes{}, tanks{};
  std::vector<std::string> themes;
  std::array<double, 4> levels{};
};

// sites/researchComplex.js's registrations: DISTRICTS "research", SITES "researchComplex".
void register_research_complex();

}  // namespace svx::city
