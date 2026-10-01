// svx_city tests — the worlds the stages of the world base sample (tools/procgen_ref/lib/worlds.mjs
// is the Node twin): every golden preset and size variant of the reference, plus two angled
// presets; and the golden sample points.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "config/presets.hpp"
#include "svx/base/types.hpp"
#include "core/value.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city::test {

struct WorldSpec {
  const char* key;
  const char* id;
  const char* size;  // "" none
};

inline const std::vector<WorldSpec>& worlds() {
  static const std::vector<WorldSpec> w = {
      {"cities", "cities", ""},
      {"infiniteCity", "infiniteCity", ""},
      {"wrapWorld:small", "wrapWorld", "small"},
      {"wrapWorld:medium", "wrapWorld", "medium"},
      {"wrapWorld:large", "wrapWorld", "large"},
      {"island:small", "island", "small"},
      {"island:medium", "island", "medium"},
      {"island:large", "island", "large"},
      {"nordicIsland:small", "nordicIsland", "small"},
      {"nordicIsland:medium", "nordicIsland", "medium"},
      {"nordicIsland:large", "nordicIsland", "large"},
      {"nordicTown:skerry", "nordicTown", "skerry"},
      {"nordicTown:fjord", "nordicTown", "fjord"},
      {"nordicTown:forest", "nordicTown", "forest"},
      {"oldHarbourTown", "oldHarbourTown", ""},
      {"whiteSeaTown", "whiteSeaTown", ""},
      {"planetEquator", "planetEquator", ""},
      {"planetNorth", "planetNorth", ""},
      {"angledCities", "angledCities", ""},
      {"angledOldHarbourTown", "angledOldHarbourTown", ""},
  };
  return w;
}

// presetConfig(id, { size }) (JS's size null: the preset's default).
inline Value world_overrides(const WorldSpec& w) { return preset_config(w.id, w.size); }

// Worlds beyond the presets, as config overrides (JSON, parsed alike by both sides): other seeds,
// a torus without latitude, other cube faces, islands without highlands or fjords, tiny and
// desert islands (the main town's fallback site), deserts, a wet planet face, mountains
// everywhere, spawn-mountain ranges no offset can meet (lib/worlds.mjs EXTRA_WORLDS).
struct ExtraWorld {
  const char* key;
  const char* json;
};
inline const std::vector<ExtraWorld>& extra_worlds() {
  static const std::vector<ExtraWorld> w = {
      {"seed7", R"({"seed":7})"},
      {"torusInfinite", R"({"seed":99,"world":{"mode":"infiniteCity","chart":"torus","size":30000,"latitude":false}})"},
      {"cube2", R"({"seed":2024,"world":{"chart":"cube","planet":{"radius":120000,"face":2}}})"},
      {"islandFlat",
       R"({"seed":5,"world":{"mode":"island","island":{"radius":3000,"highlands":0,"fjords":0,"skerries":0,"population":60000,"towns":2,"villages":3,"hamlets":3}}})"},
      {"islandDesert",
       R"({"seed":11,"world":{"mode":"island","island":{"radius":12000,"highlands":0.9,"fjords":1,"cliffs":0.9,"elongation":3,"roughness":1,"townRise":20},"climate":{"temperature":0.7,"moisture":0.2}}})"},
      {"desert", R"({"seed":3,"world":{"climate":{"temperature":0.75,"temperatureVar":0.05,"moisture":0.15}}})"},
      {"islandTiny", R"({"seed":13,"world":{"mode":"island","island":{"radius":1500,"population":500,"peak":300}}})"},
      {"islandCrowded", R"({"seed":21,"world":{"mode":"island","island":{"radius":900,"population":30000,"highlands":0.95}}})"},
      {"spawnNear", R"({"seed":8,"terrain":{"spawnMountains":[1,2]}})"},
      {"spawnFar", R"({"seed":9,"terrain":{"spawnMountains":[300,400]}})"},
      {"allMountains", R"({"seed":4,"terrain":{"mountainBelt":[0,0.01]}})"},
      {"cube5Wet", R"({"seed":-12345,"world":{"chart":"cube","planet":{"radius":240000,"face":5},"climate":{"moisture":0.9}}})"},
      {"islandBeaches", R"({"seed":31,"world":{"mode":"island","season":"winter","island":{"radius":5000,"fjords":0.3,"skerries":1,"cliffs":0}}})"},
  };
  return w;
}

// Every world of the world base's stages: the presets (worlds()), then the extra worlds
// (lib/worlds.mjs allWorlds).
struct WorldCase {
  std::string key;
  Value overrides;
};
inline std::vector<WorldCase> all_worlds() {
  std::vector<WorldCase> out;
  for (const WorldSpec& w : worlds()) out.push_back({w.key, world_overrides(w)});
  for (const ExtraWorld& e : extra_worlds()) {
    Value v;
    if (!Value::parse_json(e.json, &v)) SVX_FAIL("worlds: an extra world's JSON does not parse");
    out.push_back({e.key, v});
  }
  return out;
}

// The golden sample points (metres).
constexpr std::array<std::array<double, 2>, 5> kGoldenPoints = {{{0, 0}, {180, -140}, {1400, 900}, {-3200, 2600}, {7000, -5200}}};

using Point = std::array<double, 2>;

// Sample points (voxels) of a World, drawn from r: the golden points, 1200 within 40 km of the
// spawn, 300 within 400 km (other climates: deserts, canyons), 8 round every town (within 20 km)
// and village (within 10 km) of the spawn, 800 over an island's bounds (its coasts), and 50 off
// the voxel grid (lib/worlds.mjs samplePoints).
inline std::vector<Point> sample_points(const World& w, rec::Samples& r) {
  constexpr double kPi = 3.141592653589793;
  std::vector<Point> pts;
  for (const auto& p : kGoldenPoints) pts.push_back({js::round(p[0] * 8), js::round(p[1] * 8)});
  for (int k = 0; k < 1200; ++k) {
    const double x = js::round((r() - 0.5) * 640000);
    const double y = js::round((r() - 0.5) * 640000);
    pts.push_back({x, y});
  }
  for (int k = 0; k < 300; ++k) {
    const double x = js::round((r() - 0.5) * 6400000);
    const double y = js::round((r() - 0.5) * 6400000);
    pts.push_back({x, y});
  }
  const MacroFields& F = *w.fields;
  std::vector<const Settlement*> near = F.settlements_in({-160000, -160000, 160000, 160000});
  for (const Settlement* v : F.villages_in({-80000, -80000, 80000, 80000})) near.push_back(v);
  for (const Settlement* s : near)
    for (int k = 0; k < 8; ++k) {
      const double a = r() * 2 * kPi;
      const double d = r() * 3 * s->radius;
      pts.push_back({js::round(s->x + js::cos(a) * d), js::round(s->y + js::sin(a) * d)});
    }
  if (F.island) {
    const Rect b = F.island->bounds();
    for (int k = 0; k < 800; ++k) {
      const double x = js::round((b.x0 + r() * (b.x1 - b.x0)) * 8);
      const double y = js::round((b.y0 + r() * (b.y1 - b.y0)) * 8);
      pts.push_back({x, y});
    }
  }
  for (int k = 0; k < 50; ++k) {
    const double x = (r() - 0.5) * 100000;
    const double y = (r() - 0.5) * 100000;
    pts.push_back({x, y});
  }
  return pts;
}

// A settlement record's fields (lib/worlds.mjs settlementFields: "-" where JS leaves one undefined).
inline void settlement_fields(rec::Line& l, const Settlement& s) {
  l << s.id;
  if (s.village)
    l << s.i_js();
  else
    l << s.i;
  l << s.j << s.village << s.hamlet << s.x << s.y << s.radius << s.importance << s.style << s.peak;
  if (s.island)
    l << rec::kUndef << rec::kUndef;
  else
    l << s.cx << s.cy;
  l << s.t << s.m;
  if (s.flavor.empty())
    l << rec::kUndef;
  else
    l << s.flavor;
  l << s.island;
}

}  // namespace svx::city::test
