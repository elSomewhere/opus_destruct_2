// svx_city tests — natural land cover and farmland (voxel_city nature/landcover.js,
// nature/farmland.js) against the reference (stage "landcover").
#include <doctest.h>

#include "nature/biomes.hpp"
#include "nature/farmland.hpp"
#include "nature/landcover.hpp"
#include "records.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// Worlds in the seasons the presets leave out (stages/landcover.mjs SEASON_WORLDS).
const test::ExtraWorld kSeasonWorlds[] = {
    {"spring", R"({"seed":17,"world":{"season":"spring"}})"},
    {"autumnCool", R"({"seed":23,"world":{"season":"autumn","climate":{"temperature":0.38}}})"},
    {"winterWarm", R"({"seed":29,"world":{"season":"winter","climate":{"temperature":0.68,"moisture":0.5}}})"},
};

void surf(Line& l, const Surface& s) { l << s.top << s.sub << s.pond << s.bump << s.field; }

std::vector<double> ground_of(const FieldGround& g) { return {static_cast<double>(g.top), static_cast<double>(g.sub), g.bump}; }

// a field's record and its ground in three climates (stages/landcover.mjs field())
void field(Line& l, const Farmland& FM, const Field& f, double x, double y) {
  l << f.key << f.along_x << f.a << f.b << f.edge << f.edge_key << std::string(FM.crop(f.key, 0.3)) << std::string(FM.crop(f.key, 0.5))
    << std::string(FM.crop(f.key, 0.7)) << ground_of(FM.ground(f, 0.3, x, y)) << ground_of(FM.ground(f, 0.5, x, y)) << ground_of(FM.ground(f, 0.7, x, y))
    << FM.hedge(f, 0.3) << FM.hedge(f, 0.5);
}

}  // namespace

TEST_CASE("city landcover: land cover and farmland conform to the reference (stage landcover)") {
  rec::Samples r(61);
  rec::Out out;
  out << (Line() << "const" << kLapse << kTreeline << kSnowline);
  std::vector<test::WorldCase> worlds = test::all_worlds();
  for (const test::ExtraWorld& e : kSeasonWorlds) {
    Value v;
    REQUIRE(Value::parse_json(e.json, &v));
    worlds.push_back({e.key, v});
  }
  bool first = true;
  for (const test::WorldCase& ws : worlds) {
    const World w(ws.overrides);
    const LandCover LC(w);
    const Farmland& FM = LC.farmland();
    out << (Line() << "lc" << ws.key << LC.torus << FM.season << FM.seed);
    const double sea = w.config["world"]["seaLevel"].to_number() * 8;
    const std::vector<test::Point> all = test::sample_points(w, r);
    for (size_t pi = 0; pi < all.size(); pi += 3) {
      const double x = all[pi][0], y = all[pi][1];
      const TerrainSample ts = w.terrain->sample(x, y);
      const double hM = ts.h * 0.125;
      const LocalClimate c = LC.climate(x, y, hM);
      const Biome& b = *LC.biome_at(x, y, hM);
      out << (Line() << "p" << x << y << ts.h << ts.u << c.t << c.m << b.id << LC.tree_line(c.t) << LC.clearing(x, y) << LC.forest_density(x, y, ts.u)
                     << LC.is_farmland(x, y, ts.u) << LC.farm_mask(x, y, 0, b) << LC.farm_mask(x, y, 0.03, b) << LC.farm_mask(x, y, 0.12, b)
                     << LC.farm_mask(x, y, 0.19, b));
      // surfaces: at the ground's height, then drawn ones (high and low ground, slopes, towns, farms), then a shore
      const double ra = r();
      const double rb = r();
      const double slope1 = ra * rb * 1.2;
      const Surface s1 = LC.surface(x, y, js::round(ts.h), slope1, ts.u);
      const double h2 = js::round(ts.h + (r() - 0.3) * 24000);
      const double slope2 = r() * 2.8;
      const double u2 = r() * 0.25;
      const bool farm2 = r() < 0.25;
      const Surface s2 = LC.surface(x, y, h2, slope2, u2, farm2);
      const double h3 = js::round(sea + r() * 14 - 4);
      const double slope3 = r();
      const double u3 = r() * 0.2;
      const Surface s3 = LC.surface(x, y, h3, slope3, u3);
      Line ls;
      ls << "s" << slope1;
      surf(ls, s1);
      ls << h2 << slope2 << u2 << farm2;
      surf(ls, s2);
      ls << h3 << slope3 << u3;
      surf(ls, s3);
      out << ls;
      const double u4 = r() * 0.25;
      const double pslope = r() * 0.15;
      const double pu = r() * 0.12;
      const double hu = r() * 0.25;
      const double ht = 0.2 + r() * 0.6;
      const bool hfarm = r() < 0.5;
      const double sv = 20 + r() * 2000;
      const double ox = (r() - 0.5) * 20;
      const double oy = (r() - 0.5) * 20;
      const int oct = static_cast<int>(std::floor(r() * 4));
      const double sM = 50 + r() * 2000;
      const int oct2 = 1 + static_cast<int>(std::floor(r() * 3));
      out << (Line() << "q" << LC.forest_density(x, y, u4, &b, hM) << LC.pool_depth(x, y, b, pslope, pu) << LC.hedge_at(x, y, hu, b, ht, hfarm)
                     << LC.field_noise(LC.n_patch, x, y, sv, ox, oy, oct) << LC.field_fbm_m(LC.n_forest, x, y, sM, oct2));
      Line lf;
      lf << "f";
      field(lf, FM, FM.field_at(x, y), x, y);
      out << lf;
    }
    // fields along farm block edges (the block's own edges, the walls and hedges both blocks share)
    for (int k = 0; k < 300; ++k) {
      const double bi = std::floor((r() - 0.5) * 200);
      const double x = bi * 2400 + std::floor(r() * 24) - 12;
      const double y = std::floor((r() - 0.5) * 480000);
      const bool along = r() < 0.5;
      const double px = along ? x : y;
      const double py = along ? y : x;
      Line le;
      le << "e" << px << py;
      field(le, FM, FM.field_at(px, py), px, py);
      out << le;
    }
    if (first) {
      // every biome's forest floor
      first = false;
      for (const Biome& bio : biomes().all())
        for (int k = 0; k < 40; ++k) {
          const double t = r() * 0.8;
          const double h = r();
          const double p2 = r() * 2 - 1;
          const double patch = r() * 2 - 1;
          out << (Line() << "ff" << bio.id << t << h << p2 << patch << LC.forest_floor(bio, t, h, p2, patch));
        }
    }
  }
  CHECK(rec::record("landcover", out.text()) == rec::recorded_digest("landcover"));
}
