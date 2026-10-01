// svx_city tests — World::is_wet while the port has no waters (createWorld.js isWet: rivers, lakes,
// the island's sea are later stages): the answers a test serves through World::wet_source (the
// reference's, recorded by a stage: road_inputs.hpp). When world/createWorld.cpp defines is_wet
// (asking wet_source first, as World.hpp says), this definition goes.
#include "svx/base/types.hpp"
#include "world/World.hpp"

namespace svx::city {

bool World::is_wet(double x, double y, double margin_m) const {
  if (!wet_source) SVX_FAIL("World::is_wet: no waters (a test serves them: World::wet_source)");
  return wet_source(x, y, margin_m);
}

}  // namespace svx::city
