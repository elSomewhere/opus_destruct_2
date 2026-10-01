// svx_city — fills every registry (districts, archetypes, styles, biomes, landforms, flavors,
// complex themes, sites) in the reference's order, once. Called by create_world; safe to call
// again (and from several threads).
#pragma once

namespace svx::city {

void register_all();

}  // namespace svx::city
