// svx_city tests — the sewers (voxel_city underground/sewers.js) and their feature source against
// the reference (stage "sewers"): the plans of the cell network stage's cells, the queries, the
// World's blocks_surface and the rasterizer over chunks filled with ground of the stage's own.
#include <doctest.h>

#include "city/cellNetwork.hpp"
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

// The worlds of lib/worlds.mjs cityWorlds the stage samples (stages/sewers.mjs SEWER_KEYS).
const char* const kSewerKeys[] = {"cities",       "infiniteCity", "wrapWorld:small", "island:medium", "island:large",       "nordicIsland:small",
                                  "oldHarbourTown", "planetNorth",  "torusInfinite",   "islandFlat",    "allMountains",       "cube5Wet",
                                  "angledCities",   "angledInfiniteCity", "angledNordicTown:forest"};

bool sampled(const std::string& key) {
  for (const char* k : kSewerKeys)
    if (key == k) return true;
  return false;
}

}  // namespace

TEST_CASE("city sewers: the sewers and their feature source conform to the reference (stage sewers)") {
  rec::Samples r(89);
  rec::Out out;
  for (const test::WorldCase& ws : test::city_worlds()) {
    if (!sampled(ws.key)) continue;
    const std::unique_ptr<World> wu = test::underground_world(ws.overrides);
    const World& w = *wu;
    const Sewers& S = *w.sewers;
    const ArterialGrid& A = *w.arterials;
    const StreetLevel street_level = test::terrain_street_level(w);
    out << (Line() << "sewers" << ws.key << (w.subway != nullptr));
    // the cells and their plans
    const std::vector<test::Cell> cells = test::cells_of(w, r);
    out << (Line() << "cells" << cells.size());
    std::vector<std::shared_ptr<const SewerPlan>> plans;
    bool listed = false;
    for (const test::Cell& c : cells) {
      const double i = c[0];
      const double j = c[1];
      const std::shared_ptr<const CellNet> net = w.cell_net(i, j);
      std::string el;
      for (const auto& rd : net->roads) el += S.eligible(*rd, i, j) ? '1' : '0';
      out << (Line() << "c" << i << j << el);
      std::shared_ptr<const SewerPlan> p = S.cell_plan(i, j);
      // every box of the world's first hall stair
      const bool full = !listed && !p->boxes.empty();
      if (full) listed = true;
      test::plan_lines(out, *p, full);
      plans.push_back(std::move(p));
    }
    std::vector<const SewerNode*> nodes;
    std::vector<const SewerRun*> runs;
    for (const auto& p : plans)
      for (const SewerNode& n : p->nodes) nodes.push_back(&n);
    for (const auto& p : plans)
      for (const SewerRun& rn : p->runs) runs.push_back(&rn);
    auto cell_at = [&]() -> const test::Cell& { return cells[static_cast<size_t>(std::floor(r() * static_cast<double>(cells.size())))]; };
    // plans near rects (a point, a chunk, a tile, a cell) round the cells, and their z range
    const double sizes[4] = {0, 33, 270, 4000};
    for (int k = 0; k < 24; ++k) {
      const test::Cell& c = cell_at();
      const Rect R = A.cell_rect(c[0], c[1]);
      const double x0 = js::round(R.x0 + r() * (R.x1 - R.x0));
      const double y0 = js::round(R.y0 + r() * (R.y1 - R.y0));
      const double size = sizes[k % 4];
      const Rect rect{x0, y0, x0 + size, y0 + size};
      Line l;
      l << "near" << x0 << y0 << size;
      for (const auto& p : S.near(rect)) l << js::cat(test::fbb(p->bb), "/", static_cast<double>(p->runs.size()), "/", static_cast<double>(p->nodes.size()));
      out << l;
      double z0 = 0, z1 = 0;
      const bool zr = sewer_z_range(w, rect, k % 3, &z0, &z1);
      out << (Line() << "zr" << test::fzr(zr, z0, z1));
    }
    // blocksSurface, the sewers' and the World's: round every opening and the first open manholes
    // (each bound of its test), and anywhere in the cells
    auto probe = [&](double x, double y) { return std::string(S.blocks_surface(x, y) ? "1" : "0") + (w.blocks_surface(x, y) ? "1" : "0"); };
    for (const auto& p : plans)
      for (const SewerOpening& o : p->openings) {
        std::string b;
        for (const double x : {o.x0 - 1, o.x0, js::round((o.x0 + o.x1) / 2), o.x1, o.x1 + 1})
          for (const double y : {o.y0 - 1, o.y0, js::round((o.y0 + o.y1) / 2), o.y1, o.y1 + 1}) b += probe(x, y);
        out << (Line() << "bo" << o.x0 << o.y0 << b);
      }
    int manholes = 0;
    for (const SewerNode* n : nodes) {
      if (!n->open || manholes >= 12) continue;
      manholes += 1;
      const double sx = n->x + n->qx * 12;
      const double sy = n->y + n->qy * 12;
      std::string b;
      for (const double dx : {-20.0, -19.0, 0.0, 19.0, 20.0})
        for (const double dy : {-20.0, -19.0, 0.0, 19.0, 20.0}) b += probe(sx + dx, sy + dy);
      out << (Line() << "bm" << n->key() << b);
    }
    {
      std::string b;
      for (int k = 0; k < 120; ++k) {
        const test::Cell& c = cell_at();
        const Rect R = A.cell_rect(c[0], c[1]);
        const double x = js::round(R.x0 + r() * (R.x1 - R.x0));
        const double y = js::round(R.y0 + r() * (R.y1 - R.y0));
        b += probe(x, y);
      }
      out << (Line() << "br" << b);
    }
    // mapData, nearestHall
    for (int k = 0; k < 2; ++k) {
      const test::Cell& c = cell_at();
      const Rect R = A.cell_rect(c[0], c[1]);
      const SewerMap md = S.map_data({R.x0 - 400, R.y0 - 300, R.x1 + 200, R.y1 + 500});
      out << (Line() << "map" << md.lines.size() << md.halls.size());
      for (const auto& ml : md.lines) out << (Line() << "ml" << std::vector<double>{ml[0][0], ml[0][1], ml[1][0], ml[1][1]});
      for (const SewerMap::Hall& h : md.halls) out << (Line() << "mh" << h.x << h.y << h.top.x << h.top.y << h.top.z);
    }
    for (int k = 0; k < 6; ++k) {
      const test::Cell& c = cell_at();
      const Rect R = A.cell_rect(c[0], c[1]);
      const double x = js::round(R.x0 + r() * (R.x1 - R.x0));
      const double y = js::round(R.y0 + r() * (R.y1 - R.y0));
      const std::shared_ptr<const SewerNode> h = k == 0 ? S.nearest_hall(x, y) : S.nearest_hall(x, y, 3000);
      Line l;
      l << "hall" << x << y;
      if (h)
        l << js::cat("[", h->key(), ",", h->stair_top->x, ",", h->stair_top->y, ",", h->stair_top->z, "]");
      else
        l << rec::kUndef;
      out << l;
    }
    // hitsSubway round the stations near the spawn's cells
    if (w.subway) {
      const CellIJ c0 = A.cell_at(0, 0);
      const Rect R = A.cell_rect(c0.i, c0.j);
      const auto st = w.subway->stations_near({R.x0 - 6000, R.y0 - 6000, R.x1 + 6000, R.y1 + 6000});
      std::string h;
      for (size_t q = 0; q < st.size() && q < 6; ++q)
        for (int k = 0; k < 20; ++k) {
          const double x = js::round(st[q]->x + (r() - 0.5) * 900);
          const double y = js::round(st[q]->y + (r() - 0.5) * 900);
          const double z = js::round(st[q]->zp + (r() - 0.6) * 200);
          const double e = std::floor(r() * 40);
          h += S.hits_subway({x - e, y - e, z - e, x + e, y + e, z + e}) ? '1' : '0';
        }
      out << (Line() << "hs" << st.size() << h);
    }
    // column tiles' z range and chunks round halls, chambers, shafts and runs
    auto raster = [&](ChunkBuffer& ch, const SewerColumns* tile) { sewer_rasterize(w, ch, tile); };
    std::vector<const SewerNode*> picks;
    auto pick = [&](auto pred, size_t most) {
      size_t n = 0;
      for (const SewerNode* nd : nodes)
        if (n < most && pred(*nd)) {
          picks.push_back(nd);
          n += 1;
        }
    };
    pick([](const SewerNode& n) { return n.hall; }, 2);
    pick([](const SewerNode& n) { return n.open; }, 3);
    pick([](const SewerNode& n) { return n.shaft && !n.open; }, 3);
    pick([](const SewerNode& n) { return !n.shaft && !n.hall; }, 2);
    int q = 0;
    for (const SewerNode* n : picks) {
      for (const int lod : {0, 1, 2}) {
        const double span = 32 << lod;
        double z0 = 0, z1 = 0;
        const bool zr = sewer_z_range(w, test::tile_rect(lod, std::floor(n->x / span), std::floor(n->y / span)), lod, &z0, &z1);
        out << (Line() << "tz" << n->key() << lod << test::fzr(zr, z0, z1));
        const double sx = n->x + n->qx * 12;
        const double sy = n->y + n->qy * 12;
        out << test::chunk_line(w, street_level, lod, n->x + 3, n->y - 5, n->z, raster, q++ % 5 == 4);
        out << test::chunk_line(w, street_level, lod, sx, sy, n->zr, raster, q++ % 5 == 4);
        if (n->hall) out << test::chunk_line(w, street_level, lod, n->x - 40, n->y + 30, n->z - 20, raster, q++ % 5 == 4);
      }
    }
    for (size_t k = 0; k < runs.size() && k < 4; ++k) {
      const SewerRun& rn = *runs[k];
      const double l = js::round((rn.l0 + rn.l1) / 2);
      const double x = rn.axis == 0 ? rn.fixed + 6 : l;
      const double y = rn.axis == 0 ? l : rn.fixed - 8;
      for (const int lod : {0, 1, 2}) out << test::chunk_line(w, street_level, lod, x, y, js::round((rn.z0 + rn.z1) / 2) + 4, raster, q++ % 5 == 4);
    }
    // ... above and below the first pick (the plans kept out by their bounds), and in the middle of
    // the first cell without sewers
    if (!picks.empty()) {
      const SewerNode& n = *picks[0];
      out << test::chunk_line(w, street_level, 0, n.x, n.y, n.zr + 120, raster);
      out << test::chunk_line(w, street_level, 0, n.x, n.y, n.z - 200, raster);
    }
    const test::Cell* bare = nullptr;
    for (const test::Cell& c : cells)
      if (!S.cell_plan(c[0], c[1])->bb) {
        bare = &c;
        break;
      }
    if (bare) {
      const Rect R = A.cell_rect((*bare)[0], (*bare)[1]);
      const double x = js::round((R.x0 + R.x1) / 2);
      const double y = js::round((R.y0 + R.y1) / 2);
      out << test::chunk_line(w, street_level, 0, x, y, js::round(street_level(x, y)), raster);
    }
    Line count;
    count << "count" << plans.size() << nodes.size() << runs.size() << picks.size();
    if (bare)
      count << std::vector<double>{(*bare)[0], (*bare)[1]};
    else
      count << rec::kUndef;
    out << count;
  }
  CHECK(rec::record("sewers", out.text()) == rec::recorded_digest("sewers"));
}
