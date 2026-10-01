// svx_city — houses and row houses (voxel_city buildings/interior/houses.js): three columns across
// the width,
//
//   [ stair column | hall | rooms column ]   (mirrored for half the houses)
//
// The hall runs the full depth from the front door, so every room on every floor opens onto it;
// the open U-stair sits mid-depth in its column with small rooms (closet, WC, laundry, bath) in
// front of and behind it.
#pragma once

#include <array>

#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "core/hash.hpp"

namespace svx::city {

constexpr double kHouseLane = 7;     // HOUSE_LANE: a house stair's flights
constexpr double kHouseLanding = 8;  // HOUSE_LANDING

// The column layout of a house U cells wide (canonical u ranges, inclusive); `mirror` swaps left
// and right. entrance_u: the front door's middle (the hall's).
struct HouseColumns {
  std::array<double, 2> stair_col{}, hall{}, rooms{};
  double entrance_u = 0;
};
HouseColumns house_columns(double U, bool mirror);

// planHouse({env, rng, pb, frontDoorMargin}): every floor of a house (basements too) into pb; a
// house too narrow for its rooms column is one room without a stair. front_door_margin: the front
// door's corner margin in the 10-cell hall (an 8-cell door fits with 1; 2 leaves no room for it).
void plan_house(const Envelope& env, Rng& rng, PlanBuilder& pb, double front_door_margin = 1);

}  // namespace svx::city
