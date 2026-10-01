// svx_city — the generator's default configuration and makeConfig (voxel_city config/defaults.js).
// Every length is in metres; generators convert with vx(). The defaults are the reference's
// DEFAULT_CONFIG, as JSON (written from it: JSON.stringify keeps its key order and numbers).
#include "config/defaults.hpp"

#include "core/js.hpp"

namespace svx::city {

namespace {
const char* const kDefaultConfigJson = R"JSON({
 "seed": 1337,
 "rendering": {
  "mesher": "greedy"
 },
 "world": {
  "mode": "cities",
  "chart": "flat",
  "size": 96000,
  "latitude": true,
  "planet": {
   "radius": 240000,
   "face": 0
  },
  "faceMargin": 6000,
  "seaLevel": 0,
  "settlementCell": 9000,
  "settlementChance": 0.55,
  "cityRadius": [
   1600,
   3600
  ],
  "spawnCityRadius": 3400,
  "villageCell": 3600,
  "villageChance": 0.55,
  "villageRadius": [
   180,
   760
  ],
  "siteCell": 5200,
  "climateScale": 32000,
  "climate": null,
  "island": {
   "radius": 8000,
   "elongation": 1.5,
   "roughness": 0.5,
   "highlands": 0.45,
   "peak": 950,
   "fjords": 0.6,
   "skerries": 0.5,
   "cliffs": 0.35,
   "shelf": 2200,
   "population": 20000,
   "towns": 1,
   "villages": 2,
   "hamlets": 2,
   "cabins": 0.6,
   "flavor": null
  },
  "angles": {
   "enabled": false,
   "yawSet": "triples100",
   "pitchSet": "grades",
   "maxPartsPerChunk": 1,
   "partArea": 3600,
   "partCluster": 4,
   "maxResident": 8,
   "residentRadius": 96,
   "partsMode": "grid",
   "diagonalSpacing": 2000,
   "features": {
    "roads": true,
    "vegetation": true,
    "buildings": true,
    "ramps": true,
    "wings": false
   }
  }
 },
 "terrain": {
  "cityRelief": 4,
  "cityReliefScale": 2600,
  "lowlandBase": 45,
  "continentScale": 36000,
  "continentAmplitude": 110,
  "hillAmplitude": 38,
  "hillScale": 2800,
  "detailAmplitude": 9,
  "detailScale": 700,
  "mountainUplift": 2300,
  "mountainBase": 400,
  "mountainHeight": 3700,
  "mountainScale": 27000,
  "mountainDetail": 340,
  "mountainDetailScale": 2600,
  "valleyScale": 9000,
  "valleyDepth": 700,
  "mountainGain": 0.4,
  "mountainBeltScale": 200000,
  "mountainBelt": [
   0.8,
   0.99
  ],
  "mountainBeltWarp": 0.25,
  "mountainMassifScale": 70000,
  "mountainMassif": [
   0.48,
   0.95
  ],
  "spawnMountains": [
   10,
   35
  ],
  "plateauHeight": 260,
  "mesaStep": 32,
  "canyonScale": 9000,
  "canyonDepth": 260,
  "ravineScale": 2600,
  "ravineDepth": 36
 },
 "city": {
  "arterialSpacing": 620,
  "arterialJitter": 0.14,
  "collectorUrbanThreshold": 0.35,
  "ruralRoadWobble": 28,
  "ruralRoadChance": 0.6,
  "lotGroundStep": 1
 },
 "roads": {
  "arterial": {
   "lanes": 4,
   "laneWidth": 3.25,
   "median": 2,
   "parking": 0,
   "sidewalk": 5,
   "cornerRadius": 6
  },
  "collector": {
   "lanes": 2,
   "laneWidth": 3.25,
   "median": 0,
   "parking": 2.25,
   "sidewalk": 4,
   "cornerRadius": 5
  },
  "local": {
   "lanes": 2,
   "laneWidth": 3,
   "median": 0,
   "parking": 2,
   "sidewalk": 3,
   "cornerRadius": 4
  },
  "village": {
   "lanes": 2,
   "laneWidth": 2.9,
   "median": 0,
   "parking": 0,
   "sidewalk": 1.75,
   "cornerRadius": 4
  },
  "alley": {
   "lanes": 1,
   "laneWidth": 5,
   "median": 0,
   "parking": 0,
   "sidewalk": 0,
   "cornerRadius": 1.5
  },
  "lane": {
   "lanes": 1,
   "laneWidth": 3.5,
   "median": 0,
   "parking": 0,
   "sidewalk": 0,
   "cornerRadius": 1
  },
  "pedestrian": {
   "lanes": 0,
   "laneWidth": 0,
   "median": 0,
   "parking": 0,
   "sidewalk": 6,
   "cornerRadius": 3
  },
  "rural": {
   "lanes": 2,
   "laneWidth": 3.25,
   "median": 0,
   "parking": 0,
   "sidewalk": 0,
   "shoulder": 1.25,
   "cornerRadius": 8
  }
 },
 "highways": {
  "enabled": true,
  "nodeSpacing": 3400,
  "jitter": 0.28,
  "edgeChance": 0.7,
  "minUrbanization": 0.12,
  "deckHeight": 11,
  "lanesPerSide": 2,
  "laneWidth": 3.6,
  "shoulder": 1.2,
  "pierSpacing": 32,
  "corridorMargin": 4
 },
 "caves": {
  "enabled": true,
  "depth": 70
 },
 "lakes": {
  "enabled": true,
  "cell": 3500,
  "chance": 0.4,
  "bigChance": 0.18
 },
 "rivers": {
  "enabled": true,
  "maxHalfWidth": 18
 },
 "subway": {
  "enabled": true,
  "lineEvery": 2,
  "minUrbanization": 0.45,
  "depth": 14,
  "stationLength": 96
 },
 "buildings": {
  "residentialStory": 3,
  "officeStory": 3.75,
  "retailStory": 4.5,
  "industrialStory": 8,
  "slab": 0.25,
  "exteriorWall": 0.25,
  "interiorWall": 0.125,
  "doorWidth": 1,
  "doorHeight": 2.125
 },
 "vehicles": {
  "parked": false
 },
 "streaming": {
  "lodFactor": 5,
  "maxLod": 9,
  "cityDetailLod": 6,
  "canopyLod": 4,
  "workers": 0
 }
})JSON";
}  // namespace

const Value& default_config() {
  static const Value v = [] {
    Value out;
    std::string err;
    if (!Value::parse_json(kDefaultConfigJson, &out, &err)) SVX_FAIL("city: default config does not parse");
    return out;
  }();
  return v;
}

Value island_terrain(const Value& config) {
  const Value& cfg = config["world"]["island"];
  const double peak = cfg["peak"].num(950);
  const double k = peak / 950;
  const double s = js::min(1.6, js::max(0.55, cfg["radius"].to_number() / 8000));
  // a small island (a few km) gets finer relief: hills and knolls at its own scale
  const bool small = cfg["radius"].to_number() < 4000;
  return Value::object({
      {"lowlandBase", small ? 18 : 30},
      {"continentAmplitude", 0},
      {"hillAmplitude", small ? 16 : 22},
      {"hillScale", small ? 900 : 1800},
      {"detailAmplitude", small ? 5 : 6},
      {"detailScale", small ? 260 : 700},
      {"cityRelief", small ? 6 : 4},
      {"cityReliefScale", small ? 700 : 2600},
      {"mountainBase", 70 * k},
      {"mountainUplift", 260 * k},
      {"mountainHeight", 720 * k},
      {"mountainScale", 5200 * s},
      {"mountainDetail", 110 * k},
      {"mountainDetailScale", 900},
      {"valleyScale", 3000 * s},
      {"valleyDepth", 230 * k},
      {"plateauHeight", 0},
      {"canyonDepth", 0},
      {"ravineDepth", 22},
      // glaciated island country: knolls, hollows, boulders and bare rock
      {"rugged", 0.15},
  });
}

namespace {

// Defaults that depend on the world mode, between DEFAULT_CONFIG and the overrides: an island's
// terrain scaled to its size and peak.
Value mode_defaults(const Value& overrides) {
  if (overrides["world"]["mode"].str() != "island" || !overrides["world"]["mode"].is_string()) return Value::object();
  Value island = Value::spread(default_config()["world"]["island"], overrides["world"]["island"].is_nullish() ? Value::object() : overrides["world"]["island"]);
  Value cfg = Value::object({{"world", Value::object({{"island", island}})}});
  return Value::object({{"terrain", island_terrain(cfg)}});
}

// A wrapping world: its size a multiple of 240 m, every coarse lattice a whole number of cells
// round it (arterials an even number). Idempotent.
void wrap_lattices(Value& cfg) {
  Value& w = cfg.at_mut("world");
  if (w["chart"].str() != "torus") return;
  const double S = js::max(9600.0, js::round(w["size"].to_number() / 240) * 240);
  w.set("size", S);
  auto fit = [&](double spacing, bool even) {
    const double n = even ? js::max(2.0, 2 * js::round(S / spacing / 2)) : js::max(1.0, js::round(S / spacing));
    return S / n;
  };
  cfg.at_mut("city").set("arterialSpacing", fit(cfg["city"]["arterialSpacing"].to_number(), true));
  Value& w2 = cfg.at_mut("world");
  w2.set("settlementCell", fit(w2["settlementCell"].to_number(), false));
  w2.set("villageCell", fit(w2["villageCell"].to_number(), false));
  w2.set("siteCell", fit(w2["siteCell"].to_number(), false));
  cfg.at_mut("highways").set("nodeSpacing", fit(cfg["highways"]["nodeSpacing"].to_number(), false));
  cfg.at_mut("lakes").set("cell", fit(cfg["lakes"]["cell"].to_number(), false));
}

}  // namespace

Value make_config(const Value& overrides) {
  Value cfg = default_config();
  cfg.deep_merge(mode_defaults(overrides));
  cfg.deep_merge(overrides);
  wrap_lattices(cfg);
  return cfg;
}

}  // namespace svx::city
