// svx_city — street prop prefabs (voxel_city city/propPrefabs.js).
//
// Local frame: a runs along the street, b points from the prop towards the carriageway, z is the
// height above the ground surface voxel (0 = the first voxel above the ground). A prop's
// build(rng, o) returns PrefabBoxes (buildings/interior/prefabs.hpp) from its options (PropOpts,
// city/industry.hpp). PROPS holds the street props, then the industrial ones of industry.hpp
// (Object.assign(PROPS, INDUSTRY_PROPS)).
#pragma once

#include <string_view>
#include <vector>

#include "city/industry.hpp"

namespace svx::city {

// PROPS, in the reference's key order (the street props, then INDUSTRY_PROPS).
const std::vector<PropDef>& props();
// PROPS[kind], or null.
const PropDef* prop(std::string_view kind);

}  // namespace svx::city
