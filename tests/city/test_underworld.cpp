// svx_city tests — the subway and the sewers of created worlds (create_world: the street level the
// road levels', the cell networks with the lakes, highways and harbour grading) against the
// reference (stage "underworld"), where the stages subway and sewers check their logic on a World
// of World.js with the terrain's height for a street level.
#include <doctest.h>

#include "city_records.hpp"
#include "network/arterials.hpp"
#include "records.hpp"
#include "underground/sewers.hpp"
#include "underground/subway.hpp"
#include "underground_records.hpp"
#include "world/World.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// The worlds of lib/worlds.mjs cityWorlds the stage samples (stages/underworld.mjs UNDERWORLD_KEYS).
const char* const kUnderworldKeys[] = {"cities", "infiniteCity", "island:medium", "oldHarbourTown", "angledCities"};

bool sampled(const std::string& key) {
  for (const char* k : kUnderworldKeys)
    if (key == k) return true;
  return false;
}

}  // namespace

TEST_CASE("city underground: the subway and the sewers of created worlds conform to the reference (stage underworld)") {
  rec::Samples r(61);
  rec::Out out;
  for (const test::WorldCase& ws : test::city_worlds()) {
    if (!sampled(ws.key)) continue;
    const std::shared_ptr<World> wp = create_world(ws.overrides);
    const World& w = *wp;
    const Sewers& S = *w.sewers;
    const Subway* sw = w.subway.get();
    const StreetLevel street_level = [&w](double x, double y) { return w.street_level(x, y); };
    out << (Line() << "world" << ws.key << (sw != nullptr));
    // the first cells and their plans
    std::vector<test::Cell> cells = test::cells_of(w, r);
    if (cells.size() > 12) cells.resize(12);
    std::vector<std::shared_ptr<const SewerPlan>> plans;
    for (const test::Cell& c : cells) {
      out << (Line() << "c" << c[0] << c[1]);
      std::shared_ptr<const SewerPlan> p = S.cell_plan(c[0], c[1]);
      test::plan_lines(out, *p, false);
      plans.push_back(std::move(p));
    }
    // the stations round the spawn
    if (sw) {
      const CellIJ c0 = w.arterials->cell_at(0, 0);
      for (int axis = 0; axis < 2; ++axis)
        for (double di = -2; di <= 2; di += 1)
          for (double dj = -2; dj <= 2; dj += 1) {
            const double i = (axis == 0 ? c0.i : c0.j) + di;
            const double j = (axis == 0 ? c0.j : c0.i) + dj;
            if (!sw->line_exists(axis, i)) continue;
            if (const std::shared_ptr<const Station> s = sw->station(axis, i, j)) test::station_lines(out, *s, false);
          }
    }
    // the World's blocksSurface round openings and the first open manholes
    std::vector<const SewerNode*> nodes;
    for (const auto& p : plans)
      for (const SewerNode& n : p->nodes) nodes.push_back(&n);
    std::string b;
    for (const auto& p : plans)
      for (const SewerOpening& o : p->openings) {
        const double pts[4][2] = {{o.x0 - 1, o.y0}, {o.x0, o.y0}, {js::round((o.x0 + o.x1) / 2), o.y1}, {o.x1 + 1, o.y1}};
        for (const auto& q : pts) b += w.blocks_surface(q[0], q[1]) ? '1' : '0';
      }
    int open = 0;
    for (const SewerNode* n : nodes) {
      if (!n->open || open >= 8) continue;
      open += 1;
      const double offs[4][2] = {{0, 0}, {19, -19}, {20, 0}, {-20, 19}};
      for (const auto& d : offs) b += w.blocks_surface(n->x + n->qx * 12 + d[0], n->y + n->qy * 12 + d[1]) ? '1' : '0';
    }
    out << (Line() << "bs" << b);
    // the feature sources round a few nodes
    auto raster = [&](ChunkBuffer& ch, const SewerColumns* tile) {
      sewer_rasterize(w, ch, tile);
      if (sw) subway_rasterize(w, ch);
    };
    std::vector<const SewerNode*> picks;
    auto pick = [&](auto pred, size_t most) {
      size_t k = 0;
      for (const SewerNode* n : nodes)
        if (k < most && pred(*n)) {
          picks.push_back(n);
          k += 1;
        }
    };
    pick([](const SewerNode& n) { return n.hall; }, 1);
    pick([](const SewerNode& n) { return n.open; }, 2);
    pick([](const SewerNode& n) { return !n.shaft; }, 1);
    for (const SewerNode* n : picks)
      for (const int lod : {0, 1}) {
        const double span = 32 << lod;
        const Rect rect = test::tile_rect(lod, std::floor(n->x / span), std::floor(n->y / span));
        double a0 = 0, a1 = 0, b0 = 0, b1 = 0;
        const bool za = sewer_z_range(w, rect, lod, &a0, &a1);
        Line tz;
        tz << "tz" << n->key() << lod << test::fzr(za, a0, a1);
        if (sw) {
          const bool zb = subway_z_range(w, rect, lod, &b0, &b1);
          tz << test::fzr(zb, b0, b1);
        } else {
          tz << rec::kUndef;
        }
        out << tz;
        out << test::chunk_line(w, street_level, lod, n->x + 3, n->y - 5, n->z, raster);
        out << test::chunk_line(w, street_level, lod, n->x + n->qx * 12, n->y + n->qy * 12, n->zr, raster);
      }
    out << (Line() << "count" << plans.size() << nodes.size() << picks.size());
  }
  CHECK(rec::record("underworld", out.text()) == rec::recorded_digest("underworld"));
}
