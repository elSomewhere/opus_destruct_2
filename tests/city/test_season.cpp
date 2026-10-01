// svx_city tests — seasons (voxel_city world/season.js) against the reference (stage "season").
#include <doctest.h>

#include "records.hpp"
#include "voxel/materials.hpp"
#include "world/season.hpp"

using namespace svx::city;
using rec::Line;

namespace {

Value obj(std::vector<Value::Member> m) { return Value::object(std::move(m)); }

Value season_config(const char* id, const Value& climate) {
  Value w = Value::object();
  if (id) w.set("season", id);
  w.set("climate", climate);
  return obj({{"world", w}});
}

}  // namespace

TEST_CASE("city season: seasons conform to the reference (stage season)") {
  const std::vector<Value> climates = {
      Value(nullptr),
      obj({{"temperature", 0.5}, {"temperatureVar", 0.04}, {"moisture", 0.6}, {"moistureVar", 0.1}}),
      obj({{"temperature", 0.345}, {"temperatureVar", 0.025}, {"moisture", 0.66}, {"moistureVar", 0.18}}),
      obj({{"temperature", 0.36}, {"temperatureVar", 0.02}}),
      obj({{"temperature", 0.3}, {"snowCover", 0.5}}),
      obj({{"snowCover", 0}}),
      obj({{"snowCover", nullptr}}),
      obj({{"snowCover", 1.4}, {"freeze", 0.6}}),
      obj({{"temperature", 0.2}, {"temperatureVar", 0.1}, {"snowCover", 0.2}, {"freeze", nullptr}}),
  };
  const std::vector<const char*> ids = {"spring", "summer", "autumn", "winter", nullptr, "monsoon"};
  const std::vector<const char*> kinds = {"pine",  "spruce", "dwarfpine", "juniper", "jungle", "palm",  "cactus", "acacia", "shrubDry", "log",
                                          "stump", "snag",   "birch",     "oak",     "maple",  "autumn", "street", "blossom", "rowan",   "willow",
                                          "poplar", "shrub", "fern",      "berry",   "hazel",  "aspen", "alder",  "larch",  "unknownKind"};
  const std::vector<const char*> ground = {"GRASS", "GRASS_DARK", "GRASS_LAWN", "GRASS_DRY", "MARSH_GRASS", "FOREST_FLOOR", "TUNDRA",
                                           "MOSS",  "SAVANNA_GRASS", "STONE",  "SAND",      "SNOW",        "LEAF_LITTER",  "GRASS_SPRING"};
  auto mat_or_null = [](Line& l, int m) {
    if (m)
      l << m;
    else
      l << rec::kUndef;
  };
  rec::Samples r(11);
  rec::Out out;
  for (const Value& cl : climates)
    for (const char* id : ids) {
      const Season s(season_config(id, cl));
      Line head;
      head << "season" << (id ? id : "-") << s.id;
      if (s.fixed_snow)
        head << *s.fixed_snow;
      else
        head << rec::kUndef;
      head << s.cold << s.freeze_t << s.any << s.has_table();
      out << head;
      std::vector<double> ts;
      for (int k = -4; k <= 24; ++k) ts.push_back(k / 20.0);
      Line ls, lsn, lr;
      ls << "strength";
      lsn << "snow";
      lr << "roof";
      for (double t : ts) {
        ls << s.strength(t);
        lsn << s.snow(t);
        lr << s.roof_snow(t);
      }
      out << ls << lsn << lr;
      for (const char* kind : kinds)
        for (int q = 0; q < 4; ++q) {
          const double seed = std::floor(r() * 4294967296.0);
          const double t = r() * 1.2 - 0.1;
          const std::optional<TreeLook> L = s.tree_look(kind, seed, t);
          Line l;
          l << "tl" << kind << seed << t;
          if (L) {
            if (L->leaves)
              l << std::vector<double>{double((*L->leaves)[0]), double((*L->leaves)[1]), double((*L->leaves)[2])};
            else
              l << rec::kUndef;
            l << L->mix << L->bare << L->snow << L->holes;
            mat_or_null(l, L->accent);
          } else {
            l << rec::kUndef;
          }
          out << l;
        }
    }
  for (const char* id : ids) {
    const Season s(season_config(id, Value(nullptr)));
    for (const char* name : ground)
      for (int k = -2; k <= 14; ++k) {
        const double t = k / 20.0 + 0.3;
        const int m = material_id(name);
        Line l;
        l << "g" << name << t;
        for (int q = 0; q <= 10; ++q) l << s.ground(m, t, q / 10.0);
        const double n = r();
        l << s.ground(m, t, n);
        out << l;
      }
    for (const char* name : ground)
      for (int k = 0; k < 120; ++k) {
        const double t = r() * 1.2 - 0.1;
        const double cl = r();
        const double h = r() * 0.5;
        const double sp = r();
        Line l;
        l << "fl" << name << t << cl << h << sp;
        mat_or_null(l, s.flower(material_id(name), t, cl, h, sp));
        out << l;
      }
    for (int k = 0; k < 300; ++k) {
      const bool conifer = r() < 0.3;
      const double tl = r() * 1.2 - 0.1;
      const double kk = r();
      out << (Line() << "cn" << conifer << tl << kk << s.canopy(conifer, tl, kk));
    }
  }
  static constexpr double kCells[5] = {240, 48, 7, 1920, 0.5};
  for (int k = 0; k < 600; ++k) {
    const double seed = std::floor((r() - 0.5) * 8589934592.0);
    const double x = (r() - 0.5) * 2e6;
    const double y = (r() - 0.5) * 2e6;
    const double cell = kCells[k % 5];
    const double salt = std::floor(r() * 1000);
    out << (Line() << "pn" << seed << x << y << cell << salt << patch_noise(seed, x, y, cell, salt)
                   << patch_noise(seed, js::round(x), js::round(y), cell, salt));
  }
  CHECK(rec::record("season", out.text()) == rec::recorded_digest("season"));
}
