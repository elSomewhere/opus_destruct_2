// svx_city — a building's chamfer (voxel_city buildings/chamfer.js, ANGLED_WORLD_PLAN.md S5).
//
// The front corner of a building at a street corner (buildings/wings.js) cut off along a line of
// the 20-21-29 triple: `c` {side: 'L' | 'R' (the front's left or right end), a (the leg along the
// front), b (the leg along the side street)}, the legs 20 and 21 times k in either order: the cut
// faces the corner at 43.6 or 46.4 degrees to both streets (no table yaw is 45), its facade 29 k
// cells long. Every floor from the ground floor up loses the cells whose centres lie in front of
// the line through (a, 0) and (0, b) (mirrored at the right end, through (U - a, 0) and (U, b)); a
// slab part on the line carries the facade (wings.js). Pure integers: (2u + 1) b + (2v + 1) a
// against 2 a b, never a tie.
#pragma once

#include "core/rect.hpp"

namespace svx::city {

struct Chamfer {
  char side = 'L';  // 'L' | 'R'
  double a = 0, b = 0;
};

// Is canonical cell (u, v) of a building U cells wide in front of its chamfer `c` (cut off)?
inline bool chamfer_cut(const Chamfer& c, double U, double u, double v) {
  const double x = c.side == 'L' ? u : U - 1 - u;
  return c.b * (2 * x + 1) + c.a * (2 * v + 1) < 2 * c.a * c.b;
}

// Does a canonical rect (cells, any extent, outside the footprint too) reach in front of the
// chamfer within its corner's quadrant (u within the front leg, v within the side leg: the cut,
// and the sidewalk before it)?
bool chamfer_hits(const Chamfer& c, double U, const Rect& r);

}  // namespace svx::city
