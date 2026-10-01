// svx_city — voxel_city buildings/chamfer.js.
#include "buildings/chamfer.hpp"

namespace svx::city {

bool chamfer_hits(const Chamfer& c, double U, const Rect& r) {
  // (the rect's cell nearest the corner decides: in front of the line is monotone towards it)
  const double x = c.side == 'L' ? r.x0 : U - 1 - r.x1;
  if (x >= c.a || r.y0 >= c.b) return false;
  return c.b * (2 * x + 1) + c.a * (2 * r.y0 + 1) < 2 * c.a * c.b;
}

}  // namespace svx::city
