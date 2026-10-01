// svx_city — world/World.hpp: the World methods voxel_city's world/createWorld.js installs. So far
// the island's sea tests (the cell network asks them); the rest (create_world, the water and
// street-level queries, the dressing, building plans) comes with the stages they need.
#include "core/js.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city {

// Island mode: is (x, y) at sea (or within margin_m of the shore)?
bool World::sea_at(double x, double y, double margin_m) const {
  const IslandPlan* island = fields->island.get();
  return island ? island->coast(x / 8, y / 8) < margin_m : false;
}

// Island mode: does a rect (voxels) reach within margin_m of the sea? Sampled every ~12 m.
bool World::sea_hits_rect(const Rect& r, double margin_m) const {
  const IslandPlan* island = fields->island.get();
  if (!island) return false;
  const double step = 96;
  for (double y = r.y0; y <= r.y1 + step - 1; y += step)
    for (double x = r.x0; x <= r.x1 + step - 1; x += step)
      if (island->coast(js::min(x, r.x1) / 8, js::min(y, r.y1) / 8) < margin_m + 8.5) return true;
  return false;
}

// Share (0..1) of a rect (voxels) that lies in the sea, from a 5 x 5 sample.
double World::sea_share(const Rect& r) const {
  const IslandPlan* island = fields->island.get();
  if (!island) return 0;
  double n = 0;
  for (int j = 0; j < 5; ++j)
    for (int i = 0; i < 5; ++i)
      if (island->coast((r.x0 + ((r.x1 - r.x0) * (i + 0.5)) / 5) / 8, (r.y0 + ((r.y1 - r.y0) * (j + 0.5)) / 5) / 8) < 0) n += 1;
  return n / 25;
}

// Island mode: does the segment a-b (voxels) cross the sea (within margin_m of it)?
bool World::sea_hits_seg(double ax, double ay, double bx, double by, double margin_m) const {
  const IslandPlan* island = fields->island.get();
  if (!island) return false;
  const double n = js::max(2.0, std::ceil(js::hypot(bx - ax, by - ay) / 240));
  for (double k = 0; k <= n; k += 1)
    if (island->coast((ax + ((bx - ax) * k) / n) / 8, (ay + ((by - ay) * k) / n) / 8) < margin_m) return true;
  return false;
}

}  // namespace svx::city
