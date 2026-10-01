// svx_city — voxel_city city/districts.js.
#include "city/districts.hpp"

#include "core/hash.hpp"

namespace svx::city {

const Registry<District>& district_registry() { return district_registry_mut(); }

Registry<District>& district_registry_mut() {
  static Registry<District> r("district");
  return r;
}

namespace {
// (the streets of districts without any: one block as big as the cell)
DistrictStreets no_streets(const char* local_class) {
  return {.pattern = "none", .block = {{{9999, 9999}, {9999, 9999}}}, .pedestrian_chance = 0, .merge_chance = 0, .local_class = local_class};
}
}  // namespace

void register_districts() {
  Registry<District>& R = district_registry_mut();
  R.add({.id = "downtown",
         .label = "Downtown",
         .color = "#c96b5a",
         .streets = {.pattern = "grid", .block = {{{82, 108}, {60, 80}}}, .pedestrian_chance = 0.08, .merge_chance = 0.12, .local_class = "local"},
         .block_use = {{"lots", 0.84}, {"plaza", 0.1}, {"park", 0.06}},
         .lots = {.mode = "downtown", .width = {26, 60}, .alley_chance = 0.25},
         .archetypes = {{"tower", 5}, {"office", 3}, {"midrise", 2}, {"garage", 0.5}},
         .floors = {8, 42},
         .styles = {{"glass", 4}, {"concrete", 2}, {"deco", 2}, {"futurist", 1}}});
  R.add({.id = "midtown",
         .label = "Midtown",
         .color = "#d98f5c",
         .streets = {.pattern = "grid", .block = {{{86, 118}, {62, 84}}}, .pedestrian_chance = 0.06, .merge_chance = 0.1, .local_class = "local"},
         .block_use = {{"lots", 0.86}, {"plaza", 0.06}, {"park", 0.08}},
         .lots = {.mode = "perimeter", .width = {18, 38}, .alley_chance = 0.35},
         .archetypes = {{"midrise", 4}, {"office", 3}, {"tower", 1}, {"walkup", 1}, {"garage", 0.6}},
         .floors = {5, 16},
         .styles = {{"concrete", 3}, {"brick", 2}, {"glass", 2}, {"deco", 1}, {"plaster", 1}}});
  R.add({.id = "mixed",
         .label = "Mixed use",
         .color = "#e0b35e",
         .streets = {.pattern = "grid", .block = {{{92, 130}, {58, 80}}}, .pedestrian_chance = 0.05, .merge_chance = 0.15, .local_class = "local"},
         .block_use = {{"lots", 0.88}, {"plaza", 0.03}, {"park", 0.05}, {"sports", 0.02}, {"school", 0.03}},
         .lots = {.mode = "perimeter", .width = {12, 26}, .alley_chance = 0.5},
         .archetypes = {{"walkup", 5}, {"midrise", 2}, {"rowhouse", 1}},
         .floors = {3, 7},
         .styles = {{"brick", 4}, {"plaster", 3}, {"concrete", 1}}});
  R.add({.id = "residential",
         .label = "Residential",
         .color = "#9cc27a",
         .streets = {.pattern = "subdivide", .block = {{{110, 170}, {56, 76}}}, .pedestrian_chance = 0.02, .merge_chance = 0, .local_class = "local"},
         .block_use = {{"lots", 0.87}, {"park", 0.07}, {"sports", 0.03}, {"school", 0.04}},
         .lots = {.mode = "perimeter", .width = {7, 22}, .alley_chance = 0.45},
         .archetypes = {{"rowhouse", 4}, {"walkup", 4}, {"midrise", 1}},
         .floors = {2, 5},
         .styles = {{"brick", 4}, {"plaster", 3}, {"siding", 1}}});
  R.add({.id = "suburban",
         .label = "Suburban",
         .color = "#b7d98c",
         .streets = {.pattern = "subdivide", .block = {{{150, 230}, {72, 96}}}, .pedestrian_chance = 0, .merge_chance = 0, .local_class = "local"},
         .block_use = {{"lots", 0.9}, {"park", 0.05}, {"sports", 0.02}, {"school", 0.03}},
         .lots = {.mode = "suburban", .width = {16, 24}, .alley_chance = 0},
         .archetypes = {{"house", 8}, {"rowhouse", 1}},
         .floors = {1, 2},
         .styles = {{"siding", 4}, {"suburbanBrick", 2}, {"plaster", 1}}});
  R.add({.id = "industrial",
         .label = "Industrial",
         .color = "#9a9aa8",
         .streets = {.pattern = "subdivide", .block = {{{170, 280}, {110, 190}}}, .pedestrian_chance = 0, .merge_chance = 0, .local_class = "local"},
         .block_use = {{"lots", 0.94}, {"parking", 0.06}},
         .lots = {.mode = "industrial", .width = {40, 110}, .alley_chance = 0},
         .archetypes = {{"warehouse", 6}, {"factory", 3}},
         .floors = {1, 2},
         .styles = {{"industrial", 5}, {"concrete", 1}}});
  // Heavy industry: the far side of a town's industrial quarter (big plants, tank farms, container yards).
  R.add({.id = "heavyIndustry",
         .label = "Heavy industry",
         .color = "#7d7a86",
         .streets = {.pattern = "subdivide", .block = {{{240, 360}, {150, 240}}}, .pedestrian_chance = 0, .merge_chance = 0.1, .local_class = "local"},
         .block_use = {{"lots", 0.5}, {"tankFarm", 0.22}, {"containerYard", 0.22}, {"parking", 0.06}},
         .lots = {.mode = "industrial", .width = {60, 140}, .alley_chance = 0},
         .archetypes = {{"factory", 6}, {"warehouse", 3}},
         .floors = {1, 2},
         .styles = {{"industrial", 5}, {"concrete", 2}}});
  // Microdistrict (Soviet mikrorayon): superblocks of freestanding panel slabs and point towers in
  // shared green courtyards (playgrounds, garage rows), with a school now and then.
  R.add({.id = "microdistrict",
         .label = "Microdistrict",
         .color = "#b9a88c",
         .streets = {.pattern = "subdivide", .block = {{{240, 360}, {170, 260}}}, .pedestrian_chance = 0, .merge_chance = 0.2, .local_class = "local"},
         .block_use = {{"micro", 0.86}, {"school", 0.08}, {"park", 0.06}},
         .lots = {.mode = "micro", .width = {0, 0}, .alley_chance = 0},
         .archetypes = {{"panelSlab", 5}, {"panelTower", 2}},
         .floors = {5, 16},
         .styles = {{"panel", 1}}});
  // Housing projects on the edge of the industrial quarter: brick slabs and towers, bleak lawns, parking.
  R.add({.id = "projects",
         .label = "Housing projects",
         .color = "#9b8577",
         .streets = {.pattern = "subdivide", .block = {{{200, 300}, {150, 220}}}, .pedestrian_chance = 0, .merge_chance = 0.15, .local_class = "local"},
         .block_use = {{"micro", 0.85}, {"parking", 0.1}, {"sports", 0.05}},
         .lots = {.mode = "micro", .width = {0, 0}, .alley_chance = 0},
         .archetypes = {{"panelSlab", 4}, {"panelTower", 3}},
         .floors = {6, 14},
         .styles = {{"projects", 3}, {"panel", 1}}});
  // Harbour front of a town on a big lake: container terminals, warehouses, tank farms on quays.
  R.add({.id = "port",
         .label = "Port",
         .color = "#6f8796",
         .port = true,
         .streets = {.pattern = "subdivide", .block = {{{200, 320}, {140, 220}}}, .pedestrian_chance = 0, .merge_chance = 0.1, .local_class = "local"},
         .block_use = {{"containerYard", 0.42}, {"lots", 0.43}, {"tankFarm", 0.15}},
         .lots = {.mode = "industrial", .width = {50, 110}, .alley_chance = 0},
         .archetypes = {{"warehouse", 5}, {"factory", 1}},
         .floors = {1, 2},
         .styles = {{"industrial", 4}, {"concrete", 1}}});
  // Small harbour of an island town: a stretch of quays with a crane or two and a moored freighter,
  // fish sheds and warehouses, a car park by the ferry.
  R.add({.id = "harbour",
         .label = "Harbour",
         .color = "#6f8796",
         .port = true,
         .streets = {.pattern = "subdivide", .block = {{{150, 230}, {96, 150}}}, .pedestrian_chance = 0, .merge_chance = 0.1, .local_class = "local"},
         .block_use = {{"lots", 0.62}, {"containerYard", 0.12}, {"parking", 0.1}, {"quay", 0.16}},
         .lots = {.mode = "industrial", .width = {30, 70}, .alley_chance = 0},
         .archetypes = {{"warehouse", 5}, {"factory", 1}},
         .floors = {1, 2},
         .styles = {{"industrial", 3}, {"nordicWood", 2}, {"concrete", 1}}});
  // Old town of a small northern harbour town: narrow streets (village lanes, a few pedestrian
  // ones), small blocks of two- to four-storey wooden and rendered town houses with steep roofs,
  // shops on the ground floor, a square or two. (irregular blocks on cobbled streets, narrow lanes
  // between them)
  R.add({.id = "oldtown",
         .label = "Old town",
         .color = "#c98a5a",
         .streets = {.pattern = "organic",
                     .block = {{{46, 84}, {32, 56}}},
                     .pedestrian_chance = 0.14,
                     .merge_chance = 0,
                     .local_class = "village",
                     .lane_class = "lane",
                     .lane_chance = 0.3,
                     .paving = "cobble"},
         .block_use = {{"lots", 0.9}, {"square", 0.025}, {"park", 0.025}, {"garden", 0.04}, {"church", 0.01}},
         .lots = {.mode = "perimeter", .width = {8, 15}, .alley_chance = 0.1},
         .archetypes = {{"townhouse", 6}, {"walkup", 1.2}, {"rowhouse", 0.8}},
         .floors = {2, 4},
         .styles = {{"nordicWood", 5}, {"nordicPlaster", 3}, {"plaster", 1}}});
  // Historic core of a European city (Altstadt, vieille ville): irregular small blocks on cobbled
  // streets and lanes, a square or two, rendered and brick houses of three to six floors with
  // pitched roofs, shops below.
  R.add({.id = "oldcore",
         .label = "Historic core",
         .color = "#b9774f",
         .streets = {.pattern = "organic",
                     .block = {{{48, 90}, {32, 58}}},
                     .pedestrian_chance = 0.2,
                     .merge_chance = 0,
                     .local_class = "village",
                     .lane_class = "lane",
                     .lane_chance = 0.25,
                     .paving = "cobble"},
         .block_use = {{"lots", 0.88}, {"square", 0.05}, {"garden", 0.03}, {"park", 0.02}, {"church", 0.02}},
         .lots = {.mode = "perimeter", .width = {7, 16}, .alley_chance = 0.05},
         .archetypes = {{"walkup", 5}, {"rowhouse", 2.5}, {"midrise", 1}},
         .floors = {3, 6},
         .styles = {{"plaster", 4}, {"brick", 3}, {"deco", 1}},
         .pitched = 0.85});
  // Villages and hamlets: houses lining the country roads, fields behind.
  R.add({.id = "village",
         .label = "Village",
         .color = "#c4b37a",
         .streets = no_streets("local"),
         .block_use = {{"lots", 1}},
         .lots = {.mode = "village", .width = {14, 26}, .alley_chance = 0},
         .archetypes = {{"house", 7}, {"rowhouse", 2}, {"walkup", 1}},
         .floors = {1, 3},
         .styles = {{"plaster", 3}, {"brick", 2}, {"siding", 2}, {"suburbanBrick", 1}}});
  // Island mode: arterial sub-cells that are mostly sea (no streets, no blocks).
  R.add({.id = "sea",
         .label = "Sea",
         .color = "#34607f",
         .streets = no_streets("local"),
         .block_use = {},
         .lots = {.mode = "none", .width = {0, 0}, .alley_chance = 0},
         .archetypes = {},
         .floors = {0, 0},
         .styles = {}});
  R.add({.id = "park",
         .label = "Park",
         .color = "#4f9a4a",
         .streets = no_streets("local"),
         .block_use = {{"park", 1}},
         .lots = {.mode = "none", .width = {0, 0}, .alley_chance = 0},
         .archetypes = {},
         .floors = {0, 0},
         .styles = {}});
  // (mostly natural land - biome ground, forests, fields - with scattered farmsteads)
  R.add({.id = "rural",
         .label = "Countryside",
         .color = "#6f8f4f",
         .streets = no_streets("rural"),
         .block_use = {{"lots", 1}},
         .lots = {.mode = "rural", .width = {0, 0}, .alley_chance = 0},
         .archetypes = {{"house", 1}},
         .floors = {1, 2},
         .styles = {{"siding", 2}, {"suburbanBrick", 1}}});
}

std::string classify_district(const DistrictFields& f) {
  const double u = f.u, core = f.core, dn = f.dn, ind = f.ind;
  const double r = hash_float(f.seed, f.key, 4711);
  if (f.village) return u > 0.06 ? "village" : "rural";
  // a town's harbour front along a big lake shore
  if (f.port && u > 0.12 && core < 0.4) return "port";
  if (u < 0.14) return "rural";
  if (core > 0.62) return r < 0.05 ? "park" : "downtown";
  if (core > 0.36) return r < 0.07 ? "park" : "midtown";
  if (u < 0.42) {
    if (ind > 0.62) return "heavyIndustry";
    if (ind > 0.32) return "industrial";
    // bleak housing projects on the fringe of the industrial quarter
    if (ind > 0.2 && u > 0.24 && r < 0.8) return "projects";
    return r < 0.06 ? "park" : "suburban";
  }
  if (ind > 0.62 && core < 0.2) return "heavyIndustry";
  if (ind > 0.26 && core < 0.25) return "industrial";
  if (ind > 0.16 && core < 0.3 && r < 0.7) return "projects";
  if (r < 0.07) return "park";
  return dn > 0.05 ? "mixed" : "residential";
}

}  // namespace svx::city
