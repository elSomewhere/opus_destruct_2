// svx_city — a building's wing (buildings/wings.js planWings' record) as its envelope keeps it
// (Envelope::wings, set by the cell plan): a mass the building adds to its main one, an oriented
// part of its own at a yaw of the table (buildings/wings.hpp plans and draws them; the interior
// planners read its canonical rect: nearWing).
//
// JS's record { kind, key, turn { yaw, yaw2?, origin, ou, ov }, U, V, f0, f1, z0, z1, bounds,
// canon, part } and a chamfer's { side, a, b }; the cell plan sets `key` and `part` when it grants
// the wing its part.
#pragma once

#include <optional>
#include <string>

#include "buildings/chamfer.hpp"
#include "buildings/frame.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"

namespace svx::city {

struct Wing {
  Rect canon{};         // its bounds in the building's canonical frame (cells)
  std::string kind{};   // "corner" (a corner bay), "wing" (to a slanted street), "bay" (a canted bay), "chamfer"
  std::string key{};    // (the cell plan: its part's key, `${env.id}/${kind}`; "": not granted)
  Turn turn{};          // its lattice: { yaw, yaw2 (0: none), origin, ou: 0, ov: 0 }
  double U = 0, V = 0;  // its box (cells): u along its outer face, v in from it
  double f0 = 0, f1 = 0;  // the floors it spans
  double z0 = 0, z1 = 0;  // its world z range (its w is the world's z)
  Box3 bounds{};          // its world box (localBoundsToWorldAABB: x, y and z)
  std::optional<Chamfer> chamfer{};  // (a chamfer: the cut of its building's corner)
  double part = 0;        // (the cell plan: its part's id; 0: not granted)
};

}  // namespace svx::city
