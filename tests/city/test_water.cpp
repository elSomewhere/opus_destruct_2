// svx_city tests — rivers, lakes, the harbour grading hook and the World's water predicates
// (voxel_city nature/rivers.js, nature/lakes.js, world/createWorld.js) against the reference
// (stage "water").
#include <doctest.h>

#include "nature/lakes.hpp"
#include "nature/rivers.hpp"
#include "records.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

constexpr double kPi = 3.141592653589793;
// the region round the spawn (voxels: 20 km)
constexpr double R0 = 160000;

// The worlds of lib/worlds.mjs the stage samples (stages/water.mjs WATER_KEYS).
const char* const kWaterKeys[] = {"cities",      "infiniteCity",  "wrapWorld:small", "wrapWorld:large", "island:large", "nordicIsland:medium", "nordicTown:fjord",
                                  "oldHarbourTown", "planetNorth", "seed7",          "torusInfinite",   "cube2",        "islandFlat",          "islandDesert",
                                  "islandTiny",  "islandBeaches", "desert",          "spawnFar",        "allMountains", "cube5Wet"};

// Worlds beyond lib/worlds.mjs (stages/water.mjs WATER_WORLDS).
const test::ExtraWorld kWaterWorlds[] = {
    {"lakesOff", R"({"seed":43,"lakes":{"enabled":false},"world":{"mode":"island","island":{"radius":6000}}})"},
    {"lakesNull", R"({"seed":41,"lakes":{"cell":null,"scale":null,"townProximity":null,"bigChance":0.5}})"},
};

void lake_of(Line& l, const std::shared_ptr<const Lake>& L) {
  if (!L) {
    l << rec::kUndef;
    return;
  }
  l << L->id << L->x << L->y << L->cx << L->cy << L->r0 << L->level << L->depth << L->big;
  if (L->port.empty())
    l << rec::kUndef;
  else
    l << L->port;
  l << L->reach;
}

void ids(Line& l, const std::vector<std::shared_ptr<const Lake>>& lakes) {
  for (const auto& L : lakes) l << L->id;
}

}  // namespace

TEST_CASE("city water: rivers, lakes, harbour grading and the water predicates conform to the reference (stage water)") {
  rec::Samples r(53);
  rec::Out out;
  std::vector<test::WorldCase> worlds;
  for (const test::WorldCase& ws : test::all_worlds())
    for (const char* key : kWaterKeys)
      if (ws.key == key) worlds.push_back(ws);
  for (const test::ExtraWorld& e : kWaterWorlds) {
    Value v;
    REQUIRE(Value::parse_json(e.json, &v));
    worlds.push_back({e.key, v});
  }
  for (const test::WorldCase& ws : worlds) {
    const std::shared_ptr<World> wp = create_world(ws.overrides);
    const World& w = *wp;
    const Rivers& RV = *w.rivers;
    const Lakes& LK = *w.lakes;
    const Terrain& T = *w.terrain;
    const MacroFields& F = *w.fields;
    out << (Line() << "water" << ws.key << RV.enabled << RV.max_half_width << LK.enabled << LK.cell << LK.n);
    // the lattice round the spawn, and other laps round a wrapping world
    for (double b = -5; b <= 5; b += 1)
      for (double a = -5; a <= 5; a += 1) {
        Line l;
        l << "lake" << a << b;
        lake_of(l, LK.lake(a, b));
        out << l;
      }
    if (js::truthy(LK.n))
      for (int k = 0; k < 30; ++k) {
        const double a = std::floor((r() - 0.5) * 6 * LK.n);
        const double b = std::floor((r() - 0.5) * 6 * LK.n);
        Line l;
        l << "lap" << a << b;
        lake_of(l, LK.lake(a, b));
        out << l;
      }
    // every town's port lake, in a fixed order; the lakes near the region
    const Rect region{-R0, -R0, R0, R0};
    const std::vector<const Settlement*> towns = F.settlements_in(region);
    for (const Settlement* s : towns) {
      const std::shared_ptr<const Lake> L = LK.port_lake_of(*s);
      Line l;
      l << "port" << s->id;
      if (L)
        l << L->id;
      else
        l << rec::kUndef;
      out << l;
    }
    const std::vector<std::shared_ptr<const Lake>> lakes = LK.near(region);
    {
      Line l;
      l << "near";
      ids(l, lakes);
      out << l;
    }
    for (int k = 0; k < 40; ++k) {
      const double x0 = js::round((r() - 0.5) * 2 * R0);
      const double y0 = js::round((r() - 0.5) * 2 * R0);
      const double x1 = x0 + std::floor(r() * 60000);
      const double y1 = y0 + std::floor(r() * 60000);
      Line l;
      l << "nr";
      ids(l, LK.near({x0, y0, x1, y1}));
      out << l;
    }
    // the points: at random, round the lakes, round the harbour towns, along rivers, over an island
    std::vector<test::Point> pts;
    for (int k = 0; k < 300; ++k) {
      const double x = js::round((r() - 0.5) * 2 * R0);
      const double y = js::round((r() - 0.5) * 2 * R0);
      pts.push_back({x, y});
    }
    for (const std::shared_ptr<const Lake>& L : lakes) {
      for (int k = 0; k < 8; ++k) {
        const double a = r() * 2 * kPi;
        const double d = (0.1 + r() * 1.9) * L->r0;
        pts.push_back({js::round(L->x + js::cos(a) * d), js::round(L->y + js::sin(a) * d)});
      }
      for (int k = 0; k < 4; ++k) {
        const double x = js::round(L->x + (r() - 0.5) * 3 * L->r0);
        const double y = js::round(L->y + (r() - 0.5) * 3 * L->r0);
        out << (Line() << "sk" << L->id << x << y << LK.shore_k(*L, x, y) << LK.shore_k(*L, x, y, true));
      }
    }
    for (const Settlement* s : towns) {
      const std::shared_ptr<const Lake> L = LK.port_lake_of(*s);
      if (!L) continue;
      for (int k = 0; k < 30; ++k) {
        const double t = r() * 1.3;
        const double j = (r() - 0.5) * 1.6 * s->radius;
        const double dx = L->x - s->x;
        const double dy = L->y - s->y;
        const double d = js::or_(js::hypot(dx, dy), 1);
        pts.push_back({js::round(s->x + dx * t - (dy / d) * j), js::round(s->y + dy * t + (dx / d) * j)});
      }
    }
    for (int k = 0; k < 24; ++k) {
      double x = js::round((r() - 0.5) * 2 * R0);
      const double y = js::round((r() - 0.5) * 2 * R0);
      int found = 0;
      for (int s = 0; s < 400 && found < 12; ++s, x += 40)
        if (RV.channel_gap(x, y) < 40) {
          pts.push_back({x, y});
          found += 1;
        }
    }
    if (F.island) {
      const Rect bb = F.island->bounds();
      for (int k = 0; k < 300; ++k) {
        const double x = js::round((bb.x0 + r() * (bb.x1 - bb.x0)) * 8);
        const double y = js::round((bb.y0 + r() * (bb.y1 - bb.y0)) * 8);
        pts.push_back({x, y});
      }
    }
    for (size_t i = 0; i < pts.size(); ++i) {
      const double x = pts[i][0], y = pts[i][1];
      const TerrainSample ts = T.sample(x, y);
      const double h = js::round(ts.h);
      Line l;
      l << "pt" << x << y << ts.h << ts.natural;
      if (const std::optional<RiverInfo> ri = RV.at(x, y))
        l << ri->d << ri->half << ri->water << ri->bed << ri->urban << ri->bank << RV.ground_at(*ri, h) << RV.ground_at(*ri, ri->water - 30)
          << RV.ground_at(*ri, ri->water + 40);
      else
        l << rec::kUndef;
      l << RV.channel_gap(x, y);
      if (const std::optional<LakeInfo> lk = LK.at(x, y))
        l << lk->level << lk->bed << lk->k << lk->lake->id << LK.ground_at(*lk, h) << LK.ground_at(*lk, lk->level - 30) << LK.ground_at(*lk, lk->level + 40);
      else
        l << rec::kUndef;
      if (i % 10 == 0)
        l << RV.water_level(x, y);
      else
        l << rec::kUndef;
      out << l;
      Line lw;
      lw << "w" << w.sea_at(x, y) << w.sea_at(x, y, 30) << w.is_wet(x, y) << w.is_wet(x, y, 0) << w.is_wet(x, y, 15) << w.open_water_at(x, y);
      if (const std::optional<Shore> sh = w.shore_near(x, y, 4000)) {
        lw << sh->level << sh->dist << sh->nx << sh->ny;
        if (sh->lake)
          lw << sh->lake->id;
        else
          lw << rec::kUndef;
      } else {
        lw << rec::kUndef;
      }
      if (const std::optional<LakeShore> ls = LK.shore_near(x, y, 2400))
        lw << ls->lake->id << ls->dist << ls->nx << ls->ny;
      else
        lw << rec::kUndef;
      out << lw;
    }
    // rects and segments round the points
    const double n = static_cast<double>(pts.size());
    for (int k = 0; k < 160; ++k) {
      const test::Point& p = pts[static_cast<size_t>(std::floor(r() * n))];
      const double px = p[0], py = p[1];
      const double x0 = js::round(px - r() * 600);
      const double y0 = js::round(py - r() * 600);
      const double x1 = x0 + js::round(r() * 800);
      const double y1 = y0 + js::round(r() * 800);
      const Rect rect{x0, y0, x1, y1};
      const double m = std::floor(r() * 12);
      out << (Line() << "rect" << rect.x0 << rect.y0 << rect.x1 << rect.y1 << m << w.sea_hits_rect(rect, m) << w.sea_hits_rect(rect) << w.sea_share(rect)
                     << RV.hits_rect(rect, m) << RV.hits_rect(rect) << LK.hits_rect(rect, m) << LK.hits_rect(rect) << w.water_hits_rect(rect, m)
                     << w.water_hits_rect(rect));
      const test::Point& q = pts[static_cast<size_t>(std::floor(r() * n))];
      out << (Line() << "seg" << px << py << q[0] << q[1] << w.sea_hits_seg(px, py, q[0], q[1], m) << w.sea_hits_seg(px, py, q[0], q[1]));
    }
  }
  CHECK(rec::record("water", out.text()) == rec::recorded_digest("water"));
}
