// svx_city — nature/biomes.hpp (voxel_city nature/biomes.js).
#include "nature/biomes.hpp"

#include "core/js.hpp"
#include "core/math.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

using G = std::array<uint16_t, 2>;
using C = BiomeGroundCtx;

Biome make(const char* id, const char* label, const char* color, double t, double m, double forest, double meadow, bool farmland,
           std::vector<std::pair<std::string, double>> trees, uint16_t floor, std::function<G(const C&)> ground) {
  Biome b;
  b.id = id;
  b.label = label;
  b.color = color;
  b.climate = {t, m};
  b.forest = forest;
  b.meadow = meadow;
  b.farmland = farmland;
  b.trees = std::move(trees);
  b.floor = floor;
  b.ground = std::move(ground);
  return b;
}

}  // namespace

Registry<Biome>& biomes_mut() {
  static Registry<Biome> r("biome");
  return r;
}
const Registry<Biome>& biomes() { return biomes_mut(); }

void register_biomes() {
  Registry<Biome>& R = biomes_mut();
  R.add(make("temperate", "Temperate forest", "#4f8a3e", 0.5, 0.62, 1.0, 0.035, true,
             {{"oak", 4}, {"maple", 3}, {"birch", 2}, {"pine", 0.6}, {"spruce", 0.8}, {"rowan", 0.4}, {"aspen", 0.45}, {"alder", 0.35}},
             MAT::FOREST_FLOOR, [](const C& c) {
               return G{c.h < smoothstep(0.38, 0.62, c.patch + 0.25 * c.p2)   ? MAT::GRASS_DRY
                        : c.h < smoothstep(-0.45, -0.7, c.patch - 0.3 * c.p2) ? MAT::GRASS_DARK
                                                                               : MAT::GRASS,
                        MAT::DIRT};
             }));
  R.add(make("grassland", "Grassland", "#8fb35a", 0.52, 0.4, 0.45, 0.02, true,
             {{"oak", 5}, {"maple", 1}, {"birch", 1}, {"poplar", 0.4}, {"aspen", 0.4}, {"shrub", 3}}, MAT::FOREST_FLOOR, [](const C& c) {
               return G{c.h < smoothstep(0.2, 0.45, c.patch + 0.25 * c.p2) ? MAT::GRASS_DRY
                        : c.h < smoothstep(-0.5, -0.75, c.patch)            ? MAT::GRASS_DARK
                                                                             : MAT::GRASS,
                        MAT::DIRT};
             }));
  // dark spruce stands and pine on the drier ground, birches, aspens and rowans at the edges,
  // larches on the dry and rocky ground, alders along the water
  R.add(make("boreal", "Boreal forest", "#3d6a4a", 0.3, 0.58, 1.3, 0.05, false,
             {{"spruce", 5}, {"pine", 4}, {"birch", 2.5}, {"rowan", 0.4}, {"aspen", 0.45}, {"larch", 0.5}, {"alder", 0.25}}, MAT::MOSS,
             [](const C& c) {
               return G{c.h < smoothstep(0.3, 0.55, c.patch + 0.3 * c.p2) ? MAT::MOSS
                        : c.h < smoothstep(-0.4, -0.65, c.patch)           ? MAT::GRASS
                                                                            : MAT::GRASS_DRY,
                        MAT::DIRT};
             }));
  R.add(make("tundra", "Tundra", "#a9ae98", 0.14, 0.45, 0.15, 0.012, false, {{"dwarfpine", 4}, {"shrub", 2}}, MAT::MOSS, [](const C& c) {
    return c.patch > 0.35 ? G{MAT::SNOW, MAT::TUNDRA} : c.patch < -0.3 ? G{MAT::GRAVEL, MAT::STONE} : G{MAT::TUNDRA, MAT::DIRT};
  }));
  {
    Biome d = make("desert", "Desert", "#dcc58c", 0.78, 0.18, 0, 0.02, false, {{"cactus", 5}, {"shrubDry", 4}}, MAT::SAND, [](const C& c) {
      return c.patch > 0.62 ? G{MAT::SANDSTONE, MAT::SANDSTONE} : c.patch > 0.05 ? G{MAT::SAND_DUNE, MAT::SAND} : G{MAT::SAND, MAT::SAND};
    });
    d.dunes = 1;
    R.add(std::move(d));
  }
  R.add(make("savanna", "Savanna", "#c2b060", 0.74, 0.38, 0.12, 0.018, false, {{"acacia", 5}, {"shrubDry", 3}}, MAT::RED_EARTH, [](const C& c) {
    return c.patch > 0.5 ? G{MAT::RED_EARTH, MAT::RED_EARTH} : G{MAT::SAVANNA_GRASS, MAT::RED_EARTH};
  }));
  R.add(make("tropical", "Rainforest", "#2f6a36", 0.8, 0.78, 1.5, 0.08, false, {{"jungle", 6}, {"palm", 2}, {"shrub", 2}}, MAT::JUNGLE_FLOOR,
             [](const C& c) { return c.patch > 0.3 ? G{MAT::JUNGLE_FLOOR, MAT::DIRT} : G{MAT::GRASS, MAT::DIRT}; }));
  {
    Biome w = make("wetland", "Wetland", "#5f8a6a", 0.48, 0.86, 0.35, 0.02, false,
                   {{"willow", 3}, {"birch", 3}, {"oak", 1}, {"alder", 3}, {"shrub", 3}}, MAT::MARSH_GRASS, [](const C& c) {
                     return G{c.h < smoothstep(0.1, 0.35, c.patch + 0.3 * c.p2) ? MAT::MARSH_GRASS
                              : c.h < smoothstep(-0.4, -0.65, c.patch)           ? MAT::GRASS_DARK
                                                                                  : MAT::GRASS,
                              MAT::MUD};
                   });
    w.pools = 0.28;
    R.add(std::move(w));
  }
  // Northern bog (myr): sphagnum and sedge, black pools, stunted pines.
  {
    Biome b = make("bog", "Bog", "#7d8a5e", 0.33, 0.84, 0.22, 0.02, false, {{"dwarfpine", 5}, {"pine", 2}, {"birch", 0.8}, {"shrub", 2}},
                   MAT::MOSS, [](const C& c) {
                     return c.patch > 0.25 ? G{MAT::MARSH_GRASS, MAT::MUD} : c.patch < -0.35 ? G{MAT::TUNDRA, MAT::MUD} : G{MAT::MOSS, MAT::MUD};
                   });
    b.pools = 0.3;
    R.add(std::move(b));
  }
}

const Biome* classify_biome(double t, double m) {
  const Biome* best = nullptr;
  double bd = js::kInf;
  for (const Biome& b : biomes().all()) {
    const double dt = t - b.climate[0];
    const double dm = m - b.climate[1];
    const double d = dt * dt * 1.3 + dm * dm;
    if (d < bd) {
      bd = d;
      best = &b;
    }
  }
  return best;
}

double desertness(double t, double m) {
  const std::array<double, 2>& d = biomes().get("desert").climate;
  const double dist = js::hypot((t - d[0]) * 1.14, m - d[1]);
  return js::max(0.0, js::min(1.0, 1 - dist / 0.27));
}

}  // namespace svx::city
