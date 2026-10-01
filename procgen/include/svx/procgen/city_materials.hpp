// structvox procgen — the city generator's materials in the engine (docs/CITY.md, §9 of
// docs/PROCGEN_MERGE_PLAN.md): its own physics classes registered, their fire facets, and the
// appearance table of its looks.
#pragma once

#include <memory>

#include "svx/env/fire.hpp"
#include "svx/game/appearance.hpp"

namespace svx {

// Registers the city's own physics classes in the default material table at their ids (21: roofing,
// partition, soft, ice, snow, foliage - foliage decorative and passable: plants), once. Idempotent.
void register_city_materials();

// Their fire facets: roofing burns slowly (battens and coverings, one class today), partitions
// (plasterboard) do not burn but weaken, soft furnishings and goods burn easily, plants fast; ice
// and snow do not (melting is not modelled).
void set_city_fire_materials(FireSystem& fire);

// The city's looks as an appearance table: (class, 1 + look) for every look of every class (the
// city's "look" layer holds 1 + the look's index within its voxel's class; 0: none).
std::shared_ptr<const AppearanceTable> city_appearances();

}  // namespace svx
