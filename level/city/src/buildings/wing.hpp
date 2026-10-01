// svx_city — a building's wing (buildings/wings.js planWings' record) as its envelope keeps it
// (Envelope::wings, set by the cell plan). The port of wings.js completes the record; the interior
// planners read its canonical rect (nearWing).
#pragma once

#include "core/rect.hpp"

namespace svx::city {

struct Wing {
  Rect canon{};  // its rect in the building's canonical frame
  // (the rest of planWings' record comes with the port of buildings/wings.js)
};

}  // namespace svx::city
