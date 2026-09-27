// structvox game — a procedural streamed city.
#pragma once

#include <memory>

#include "svx/game/source.hpp"

namespace svx {

// A grid of blocks (streets between them) with column / slab buildings of seeded height and
// layout, on an anchored ground slab. extent_m: side of the square world.
std::unique_ptr<GameSource> make_city_source(u64 seed, f64 extent_m = 1000.0, f64 h = 0.125);

}  // namespace svx
