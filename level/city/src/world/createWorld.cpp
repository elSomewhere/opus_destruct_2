// svx_city — create_world (voxel_city world/createWorld.js): a World with the default subsystems
// and feature sources installed, in the reference's order; and the World's water predicates
// (createWorld's closures: World methods here). Each later subsystem adds its line where the
// reference makes it (the marked places below).
#include <cmath>

#include "core/js.hpp"
#include "nature/caves.hpp"
#include "nature/lakes.hpp"
#include "nature/landcover.hpp"
#include "nature/rivers.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city {

std::shared_ptr<World> create_world(const Value& config) {
  auto world = std::make_shared<World>(config);
  World& w = *world;
  w.land_cover = std::make_shared<LandCover>(w);
  w.rivers = std::make_shared<Rivers>(w);
  w.lakes = std::make_shared<Lakes>(w);
  // harbour towns: a terrace 3 m above the water along the port lake's shore, the town rising
  // behind it
  const World* wp = &w;
  w.terrain->port_grade = [wp](double x, double y, double h) {
    for (const Settlement* s : wp->fields->nearest_settlements(x, y)) {
      const std::shared_ptr<const Lake> L = wp->lakes->port_lake_of(*s);
      if (!L) continue;
      const double dx = x - L->x;
      const double dy = y - L->y;
      if (std::fabs(dx) > L->r0 * 1.35 + 11200 || std::fabs(dy) > L->r0 * 1.35 + 11200) continue;
      const double d = (wp->lakes->shore_k(*L, x, y, true) - 1) * L->r0;
      if (d > 11200) continue;
      // along the town's waterfront, fading out over its outskirts: a flat harbour terrace within
      // 400 m of the shore, then the town rises
      const double q = js::hypot(x - s->x, y - s->y) / s->radius;
      const double near = 1 - js::max(0.0, js::min(1.0, (q - 1.1) / 0.9));
      const double t = js::max(0.0, js::min(1.0, (d - 3200) / 8000));
      const double k = t * t * (3 - 2 * t);
      return h + (L->level / 8 + 3 - h) * (1 - k) * near * near * (3 - 2 * near);
    }
    return h;
  };
  w.caves = std::make_shared<Caves>(w);
  // (seaAt, isWet, seaHitsRect, seaShare, seaHitsSeg, openWaterAt, shoreNear, waterHitsRect: the
  // World methods below; streetLevel: network/roadLevel)
  // (world/landmarks: island landmarks - lighthouse, boathouses, fish racks, cairns - on islands)
  // (nature/forest: the forest)
  // (nature/boulders: the boulders)
  // (network/highways: the highways unless config.highways.enabled is false)
  // (underground/subway: the subway unless config.subway.enabled is false)
  // (underground/sewers: the sewers)
  // (world/sites: the site layer)
  // (sites/links: the site links)
  // (city/dressing: the dressing cache, World::dressing)
  // (buildings/interior/plan: World::building_plan; buildings/interior/voxelize:
  // World::voxelize_building)
  // (World::blocks_surface: the subway's and the sewers' openings)
  // (feature sources, in this order: caves (nature/caves: id kCaveSourceId, order kCaveSourceOrder,
  // max_lod kCaveSourceMaxLod; cave_z_range and cave_rasterize over cave_columns(tile), wrapped
  // once voxel/compose's GroundTile exists), sewers, subway
  // (with a subway), site links, sites, highways (with highways), buildings, dressing, skybridges,
  // forest, boulders, ground cover, landmarks (with landmarks))
  return world;
}

bool World::sea_at(double x, double y, double margin_m) const {
  const IslandPlan* island = fields->island.get();
  return island ? island->coast(x / 8, y / 8) < margin_m : false;
}

bool World::is_wet(double x, double y, double margin_m) const {
  const IslandPlan* island = fields->island.get();
  if (island && island->coast(x / 8, y / 8) < margin_m) return true;
  const std::optional<RiverInfo> ri = rivers->at(x, y);
  if (ri && ri->d < ri->half + margin_m) return true;
  const std::optional<LakeInfo> lk = lakes->at(x, y);
  return lk && lk->k < 1 + (margin_m * 8) / lk->lake->r0;
}

bool World::sea_hits_rect(const Rect& r, double margin_m) const {
  const IslandPlan* island = fields->island.get();
  if (!island) return false;
  const double step = 96;
  for (double y = r.y0; y <= r.y1 + step - 1; y += step)
    for (double x = r.x0; x <= r.x1 + step - 1; x += step)
      if (island->coast(js::min(x, r.x1) / 8, js::min(y, r.y1) / 8) < margin_m + 8.5) return true;
  return false;
}

double World::sea_share(const Rect& r) const {
  const IslandPlan* island = fields->island.get();
  if (!island) return 0;
  double n = 0;
  for (int j = 0; j < 5; ++j)
    for (int i = 0; i < 5; ++i)
      if (island->coast((r.x0 + ((r.x1 - r.x0) * (i + 0.5)) / 5) / 8, (r.y0 + ((r.y1 - r.y0) * (j + 0.5)) / 5) / 8) < 0) n += 1;
  return n / 25;
}

bool World::sea_hits_seg(double ax, double ay, double bx, double by, double margin_m) const {
  const IslandPlan* island = fields->island.get();
  if (!island) return false;
  const double n = js::max(2.0, std::ceil(js::hypot(bx - ax, by - ay) / 240));
  for (double k = 0; k <= n; k += 1)
    if (island->coast((ax + ((bx - ax) * k) / n) / 8, (ay + ((by - ay) * k) / n) / 8) < margin_m) return true;
  return false;
}

bool World::open_water_at(double x, double y) const {
  const IslandPlan* island = fields->island.get();
  if (island && island->coast(x / 8, y / 8) < 0) return true;
  const std::optional<LakeInfo> lk = lakes->at(x, y);
  return lk && lk->k < 1;
}

std::optional<Shore> World::shore_near(double x, double y, double max_dist) const {
  const std::optional<LakeShore> lk = lakes->shore_near(x, y, max_dist);
  if (lk) return Shore{lk->lake->level, lk->dist, lk->nx, lk->ny, lk->lake};
  const IslandPlan* island = fields->island.get();
  if (!island) return std::nullopt;
  const double c = island->coast(x / 8, y / 8);
  if (std::fabs(c) * 8 > max_dist) return std::nullopt;
  const double e = 6;
  const double gx = island->coast(x / 8 + e, y / 8) - c;
  const double gy = island->coast(x / 8, y / 8 + e) - c;
  const double g = js::or_(js::hypot(gx, gy), 1);
  return Shore{js::round(config["world"]["seaLevel"].to_number() * 8), c * 8, gx / g, gy / g, nullptr};
}

bool World::water_hits_rect(const Rect& r, double margin_m) const {
  return sea_hits_rect(r, margin_m) || rivers->hits_rect(r, margin_m) || lakes->hits_rect(r, margin_m);
}

}  // namespace svx::city
