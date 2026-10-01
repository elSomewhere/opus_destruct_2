// svx_city — voxel_city config/presets.js, preset for preset (registration order is the
// reference's: the angled variants last).
#include "config/presets.hpp"

#include "core/js.hpp"

namespace svx::city {

namespace {

using V = Value;
using M = Value::Member;
V obj(std::vector<M> m) { return Value::object(std::move(m)); }
V arr(std::vector<V> a) { return Value::array(std::move(a)); }

V wrap_sizes() {
  return arr({obj({{"id", "small"}, {"label", "48 km"}, {"size", 48000}}), obj({{"id", "medium"}, {"label", "96 km"}, {"size", 96000}}),
              obj({{"id", "large"}, {"label", "192 km"}, {"size", 192000}})});
}

// Terrain and climate scales follow the world size.
V wrap_config(double size) {
  const double k = js::min(1.0, size / 200000);
  return obj({
      {"world", obj({{"mode", "cities"}, {"chart", "torus"}, {"size", size}, {"latitude", true}, {"climateScale", js::round(8000 + 24000 * k)}})},
      {"terrain", obj({{"mountainBeltScale", js::round(js::max(30000.0, size * 0.9))},
                       {"mountainMassifScale", js::round(js::max(18000.0, size * 0.35))},
                       {"mountainScale", js::round(12000 + 15000 * k)},
                       {"spawnMountains", arr({js::min(10.0, size / 12000), js::min(35.0, size / 4000)})}})},
  });
}

V island_sizes() {
  return arr({
      obj({{"id", "small"}, {"label", "Small (≈ 9 km)"}, {"radius", 4500}, {"population", 9000}, {"towns", 0}, {"villages", 1}, {"hamlets", 2}, {"peak", 620}}),
      obj({{"id", "medium"}, {"label", "Medium (≈ 16 km)"}, {"radius", 8000}, {"population", 20000}, {"towns", 1}, {"villages", 2}, {"hamlets", 2}, {"peak", 950}}),
      obj({{"id", "large"}, {"label", "Large (≈ 26 km)"}, {"radius", 13000}, {"population", 32000}, {"towns", 1}, {"villages", 3}, {"hamlets", 4}, {"peak", 1250}}),
  });
}

const char* const kIslandKeys[] = {"radius", "population", "towns", "villages", "hamlets", "peak", "elongation", "roughness",
                                   "highlands", "fjords", "skerries", "cliffs", "shelf", "townRise", "cabins"};

V island_size(const V& size) {
  V out = Value::object();
  for (const char* k : kIslandKeys)
    if (!size[k].is_undefined()) out.set(k, size[k]);
  return out;
}

// Island mode keeps a small place's infrastructure: no elevated highways or subway.
V island_base() {
  return obj({{"highways", obj({{"enabled", false}})},
              {"subway", obj({{"enabled", false}})},
              {"lakes", obj({{"bigChance", 0}})},
              {"city", obj({{"ruralRoadChance", 0.18}, {"mainRoad", "collector"}})}});
}

// Small islands (4-6 km): a lattice fine enough for one small town, tarns instead of lakes.
V small_island(const V& size, const V& extra) {
  V out = island_base();
  out.set("city", V::spread(V::spread(island_base()["city"], obj({{"arterialSpacing", 500}, {"ruralRoadChance", 0.12}})),
                            extra["city"].is_nullish() ? Value::object() : extra["city"]));
  out.set("lakes", obj({{"bigChance", 0}, {"cell", 900}, {"chance", 0.5}, {"scale", 0.28}, {"townProximity", 0.55}}));
  out.set("rivers", obj({{"enabled", false}}));
  out.set("world", obj({{"mode", "island"},
                        {"villageRadius", arr({150, 380})},
                        {"island", V::spread(island_size(size), extra["island"].is_nullish() ? Value::object() : extra["island"])},
                        {"climate", extra["climate"]}}));
  return out;
}

V nordic_seasons() {
  return obj({
      {"winter", obj({{"sky", 0xa3adb5}, {"fogDensity", 1.9}, {"sun", 0.45}, {"ambient", 0.95}, {"sunElevation", 0.2}, {"desaturate", 0.35}})},
      {"spring", obj({{"sky", 0xadbecc}, {"fogDensity", 1.5}, {"sun", 0.8}, {"ambient", 1}, {"sunElevation", 0.5}, {"desaturate", 0.14}})},
      {"summer", obj({{"sky", 0xa6bdd0}, {"fogDensity", 1.25}, {"sun", 0.95}, {"ambient", 1}, {"sunElevation", 0.72}, {"desaturate", 0.06}})},
      {"autumn", obj({{"sky", 0xa2acb3}, {"fogDensity", 1.75}, {"sun", 0.62}, {"ambient", 0.95}, {"sunElevation", 0.34}, {"desaturate", 0.24}})},
  });
}

V town_islands() {
  return arr({
      obj({{"id", "skerry"}, {"label", "Skerry town (≈ 4 km)"}, {"radius", 1900}, {"elongation", 1.35}, {"roughness", 0.8}, {"population", 3200},
           {"towns", 0}, {"villages", 0}, {"hamlets", 1}, {"peak", 120}, {"highlands", 0.18}, {"fjords", 0}, {"skerries", 1}, {"cliffs", 0.3},
           {"shelf", 900}, {"townRise", 10}, {"cabins", 0.9}, {"flavor", "nordicHarbour"}}),
      obj({{"id", "fjord"}, {"label", "Fjord town (≈ 5 km)"}, {"radius", 2500}, {"elongation", 1.5}, {"roughness", 0.6}, {"population", 4600},
           {"towns", 0}, {"villages", 0}, {"hamlets", 2}, {"peak", 470}, {"highlands", 0.48}, {"fjords", 0.45}, {"skerries", 0.5}, {"cliffs", 0.5},
           {"shelf", 1100}, {"townRise", 34}, {"cabins", 0.8}, {"flavor", "nordicHarbour"}}),
      obj({{"id", "forest"}, {"label", "Forest town (≈ 5 km)"}, {"radius", 2400}, {"elongation", 1.2}, {"roughness", 0.5}, {"population", 4200},
           {"towns", 0}, {"villages", 1}, {"hamlets", 1}, {"peak", 200}, {"highlands", 0.2}, {"fjords", 0.1}, {"skerries", 0.35}, {"cliffs", 0.2},
           {"shelf", 1000}, {"townRise", 8}, {"cabins", 1}, {"flavor", "nordicBleak"}, {"moisture", 0.62}}),
  });
}

std::vector<Preset> make_presets() {
  std::vector<Preset> out;
  auto reg = [&](Preset p) { out.push_back(std::move(p)); };
  {
    Preset p;
    p.id = "cities";
    p.label = "Cities in countryside";
    p.description = "An endless plane of towns, villages, farms, mountains and biomes.";
    p.config = [](const V&) { return obj({{"world", obj({{"mode", "cities"}})}}); };
    reg(p);
  }
  {
    Preset p;
    p.id = "infiniteCity";
    p.label = "Infinite city";
    p.description = "Urban land everywhere.";
    p.config = [](const V&) { return obj({{"world", obj({{"mode", "infiniteCity"}})}}); };
    reg(p);
  }
  {
    Preset p;
    p.id = "wrapWorld";
    p.label = "Wrapping world (flat planet)";
    p.description =
        "A finite world whose edges join: walk east (or north) long enough and you are back where you started. Climate runs from an equator to a pole and back.";
    p.sizes = wrap_sizes();
    p.default_size = "medium";
    p.config = [](const V& size) { return wrap_config(size["size"].to_number()); };
    reg(p);
  }
  {
    Preset p;
    p.id = "island";
    p.label = "Island";
    p.description = "One temperate island in an endless sea: a harbour town, villages, forests, highlands, beaches and cliffs.";
    p.sizes = island_sizes();
    p.default_size = "medium";
    p.config = [](const V& size) {
      V c = island_base();
      c.set("world", obj({{"mode", "island"},
                          {"island", V::spread(island_size(size), obj({{"cliffs", 0.3}, {"flavor", "harbourTown"}}))},
                          {"climate", obj({{"temperature", 0.5}, {"temperatureVar", 0.04}, {"moisture", 0.6}, {"moistureVar", 0.1}})}}));
      return c;
    };
    reg(p);
  }
  {
    Preset p;
    p.id = "nordicIsland";
    p.label = "Nordic island";
    p.description =
        "A bleak Scandinavian / north-Russian island: old wooden town centre, post-Soviet housing estates, a small port, cabins in pine and spruce forests, fjords and fells. Winter by default.";
    p.sizes = island_sizes();
    p.default_size = "medium";
    p.default_season = "winter";
    p.config = [](const V& size) {
      V c = island_base();
      c.set("world", obj({{"mode", "island"},
                          {"island", V::spread(island_size(size), obj({{"highlands", 0.5}, {"fjords", 0.8}, {"skerries", 0.7}, {"cliffs", 0.45},
                                                                       {"elongation", 1.7}, {"flavor", "nordicBleak"}, {"cabins", 0.8}}))},
                          {"climate", obj({{"temperature", 0.345}, {"temperatureVar", 0.025}, {"moisture", 0.66}, {"moistureVar", 0.18}})}}));
      c.set("city", V::spread(island_base()["city"], obj({{"ruralRoadChance", 0.14}})));
      return c;
    };
    p.viewer = obj({{"timeOfDay", 11.5}, {"seasons", nordic_seasons()}});
    reg(p);
  }
  {
    Preset p;
    p.id = "nordicTown";
    p.label = "Nordic small-town island";
    p.description =
        "A 4-5 km island with one small northern town: an old wooden centre on cobbled lanes, a church and its cemetery, a harbour, allotments, a few bleak blocks by the works; dense spruce forest, meadows, creeks, tarns and skerries. Summer by default.";
    p.sizes = town_islands();
    p.default_size = "fjord";
    p.default_season = "summer";
    p.config = [](const V& size) {
      return small_island(size, obj({{"island", obj({{"flavor", size["flavor"]}})},
                                     {"climate", obj({{"temperature", 0.36}, {"temperatureVar", 0.02}, {"moisture", size["moisture"].num(0.66)}, {"moistureVar", 0.14}})}}));
    };
    p.viewer = obj({{"timeOfDay", 14}, {"seasons", nordic_seasons()}});
    reg(p);
  }
  {
    Preset p;
    p.id = "oldHarbourTown";
    p.label = "Old harbour town (Bergen-like)";
    p.description =
        "A 6 km fjord island with an old Hanseatic harbour town climbing the hillside: wharf warehouses, cobbled lanes, wooden houses, churches and a market square; fells all round. Autumn by default.";
    p.default_season = "autumn";
    p.config = [](const V&) {
      return small_island(obj({{"radius", 3000}, {"elongation", 1.45}, {"roughness", 0.65}, {"population", 7500}, {"towns", 0}, {"villages", 0}, {"hamlets", 2},
                               {"peak", 560}, {"highlands", 0.55}, {"fjords", 0.55}, {"skerries", 0.45}, {"cliffs", 0.55}, {"shelf", 1200}, {"townRise", 48}, {"cabins", 0.6}}),
                          obj({{"island", obj({{"flavor", "nordicHarbour"}})},
                               {"climate", obj({{"temperature", 0.38}, {"temperatureVar", 0.02}, {"moisture", 0.63}, {"moistureVar", 0.1}})}}));
    };
    V seasons = nordic_seasons();
    seasons.set("autumn", obj({{"sky", 0x9ea9b0}, {"fogDensity", 1.9}, {"sun", 0.55}, {"ambient", 0.95}, {"sunElevation", 0.38}, {"desaturate", 0.26}}));
    p.viewer = obj({{"timeOfDay", 13}, {"seasons", seasons}});
    reg(p);
  }
  {
    Preset p;
    p.id = "whiteSeaTown";
    p.label = "White Sea town (north Russian)";
    p.description =
        "A flat 5 km island of spruce forest, bogs and tarns with a Karelian wooden town, Soviet panel blocks, garages, a sawmill and a small port. Autumn by default.";
    p.default_season = "autumn";
    p.config = [](const V&) {
      return small_island(obj({{"radius", 2500}, {"elongation", 1.3}, {"roughness", 0.45}, {"population", 5200}, {"towns", 0}, {"villages", 1}, {"hamlets", 1},
                               {"peak", 90}, {"highlands", 0.1}, {"fjords", 0}, {"skerries", 0.4}, {"cliffs", 0.12}, {"shelf", 1300}, {"townRise", 6}, {"cabins", 0.7}}),
                          obj({{"island", obj({{"flavor", "nordicBleak"}})},
                               {"climate", obj({{"temperature", 0.33}, {"temperatureVar", 0.02}, {"moisture", 0.7}, {"moistureVar", 0.14}})}}));
    };
    p.viewer = obj({{"timeOfDay", 12.5}, {"seasons", nordic_seasons()}});
    reg(p);
  }
  {
    Preset p;
    p.id = "planetEquator";
    p.label = "Planet face (equator)";
    p.description = "One face of a cube-sphere planet on the equator.";
    p.config = [](const V&) {
      return obj({{"world", obj({{"mode", "cities"}, {"chart", "cube"}, {"planet", obj({{"radius", 240000}, {"face", 0}})}})}});
    };
    reg(p);
  }
  {
    Preset p;
    p.id = "planetNorth";
    p.label = "Planet face (north pole)";
    p.description = "The north-pole face of a cube-sphere planet.";
    p.config = [](const V&) {
      return obj({{"world", obj({{"mode", "cities"}, {"chart", "cube"}, {"planet", obj({{"radius", 240000}, {"face", 4}})}})}});
    };
    reg(p);
  }
  // Angled variants: the same worlds with world.angles on (and wings), presets of their own.
  auto angled = [&](const std::string& id, const std::string& description) {
    Preset base;
    for (const Preset& q : out)
      if (q.id == id) base = q;
    Preset p = base;
    p.id = "angled" + std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])))) + id.substr(1);
    p.label = base.label + ", angled";
    p.description = description;
    auto base_config = base.config;
    p.config = [base_config](const V& size) {
      V cfg = base_config(size);
      const V angles = cfg["world"]["angles"].is_nullish() ? Value::object() : cfg["world"]["angles"];
      V features = V::spread(obj({{"wings", true}}), angles["features"].is_nullish() ? Value::object() : angles["features"]);
      V a = V::spread(angles, obj({{"enabled", true}, {"features", features}}));
      V world = V::spread(cfg["world"].is_nullish() ? Value::object() : cfg["world"], obj({{"angles", a}}));
      V res = V::spread(cfg, obj({{"world", world}}));
      return res;
    };
    out.push_back(p);
  };
  angled("cities", "Cities in countryside with angled streets and buildings: diagonal boulevards, curving country roads, a building in eight turned to its street.");
  angled("infiniteCity", "The infinite city, angled: diagonal boulevards through the grid, turned buildings along them.");
  angled("nordicTown", "A small northern town island, angled: crooked old-town lanes, winding roads through a more natural forest.");
  angled("oldHarbourTown", "The old harbour town, angled: steep streets climbing the hillside on smooth ramps.");
  return out;
}

}  // namespace

const std::vector<Preset>& presets() {
  static const std::vector<Preset> p = make_presets();
  return p;
}

const Preset* find_preset(const std::string& id) {
  for (const Preset& p : presets())
    if (p.id == id) return &p;
  return nullptr;
}

const std::vector<std::string>& seasons() {
  static const std::vector<std::string> s = {"spring", "summer", "autumn", "winter"};
  return s;
}

const Value& season_atmosphere() {
  static const Value a = obj({
      {"spring", obj({{"sky", 0xb4c8da}, {"sun", 1}, {"sunElevation", 0.8}, {"fogDensity", 1}, {"desaturate", 0.05}})},
      {"summer", Value::object()},
      {"autumn", obj({{"sky", 0xadb8c0}, {"sun", 0.8}, {"sunElevation", 0.55}, {"fogDensity", 1.35}, {"desaturate", 0.15}})},
      {"winter", obj({{"sky", 0xb9c1c8}, {"sun", 0.65}, {"sunElevation", 0.4}, {"fogDensity", 1.5}, {"desaturate", 0.3}})},
  });
  return a;
}

Value preset_size(const Preset& p, const std::string& size_id) {
  if (!p.sizes.is_array()) return Value(nullptr);
  for (const Value& s : p.sizes.items())
    if (s["id"].str() == size_id && !size_id.empty()) return s;
  for (const Value& s : p.sizes.items())
    if (s["id"].str() == p.default_size && !p.default_size.empty()) return s;
  return p.sizes[size_t(0)];
}

std::string preset_season(const Preset& p, const std::string& season) {
  for (const std::string& s : seasons())
    if (!season.empty() && s == season) return season;
  return p.default_season.empty() ? "summer" : p.default_season;
}

Value preset_config(const std::string& id, const std::string& size, double seed, const std::string& season) {
  const Preset* p = find_preset(id);
  if (!p) SVX_FAIL("city: no such preset");
  Value cfg = p->config(preset_size(*p, size));
  if (seed == seed) cfg.set("seed", seed);
  cfg.set("world", Value::spread(cfg["world"].is_nullish() ? Value::object() : cfg["world"], obj({{"season", preset_season(*p, season)}})));
  return cfg;
}

Value preset_viewer(const std::string& id, const std::string& season) {
  const Preset* p = find_preset(id);
  if (!p) SVX_FAIL("city: no such preset");
  const std::string s = preset_season(*p, season);
  const Value& v = p->viewer;
  Value atmosphere = Value::spread(Value::spread(season_atmosphere()[s], v["atmosphere"].is_nullish() ? Value::object() : v["atmosphere"]),
                                   v["seasons"][s].is_nullish() ? Value::object() : v["seasons"][s]);
  return obj({{"timeOfDay", v["timeOfDay"].num(13)}, {"atmosphere", atmosphere}});
}

}  // namespace svx::city
