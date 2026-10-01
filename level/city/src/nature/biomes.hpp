// svx_city — biomes (voxel_city nature/biomes.js): the natural counterpart of urban districts. A
// biome is chosen per column from climate (temperature cooled by altitude, moisture) as the
// nearest climate centroid, with a little noise so borders interleave. Each says how its ground
// looks, how dense and which vegetation grows, whether fields are farmed and whether shallow pools
// form. Elevation overlays (beach, rock slopes, snow line) are LandCover's, on top.
//
//   climate   [temperature, moisture] centroid (both 0..1)
//   forest    multiplier on the forest-patch noise (0 = never forest)
//   meadow    background tree chance outside forest patches
//   farmland  whether farm parcels appear on flat open land
//   trees     weighted tree kinds (nature/trees)
//   ground(c) [top, sub] for open (non-forest) ground; c = { patch (~11 m noise), p2 (~5 m noise),
//             h (0..1 hash of the column), x, y }
//   floor     ground under forest canopy
//   pools     chance-ish threshold for shallow ponds (wetlands; 0: none)
//
// Adding a biome = registering an entry (register_biomes, in the reference's order).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "world/registry.hpp"

namespace svx::city {

// biome.ground's argument.
struct BiomeGroundCtx {
  double patch = 0, p2 = 0, h = 0, x = 0, y = 0;
};

struct Biome {
  std::string id, label, color;
  std::array<double, 2> climate{};
  double forest = 0, meadow = 0;
  bool farmland = false;
  std::vector<std::pair<std::string, double>> trees;
  uint16_t floor = 0;
  std::function<std::array<uint16_t, 2>(const BiomeGroundCtx&)> ground;
  double pools = 0;  // (undefined in most biomes: 0, as falsy)
  double dunes = 0;  // (the desert's 1)
};

// BIOMES: the registry (world/registry.js), in registration order.
const Registry<Biome>& biomes();
// The registry for registering (register_all, and code adding biomes before generation).
Registry<Biome>& biomes_mut();
// Registers the reference's biomes (nature/biomes.js), in its order.
void register_biomes();

// classifyBiome(t, m): the nearest climate centroid (t the local, altitude-cooled temperature, m
// moisture; callers jitter both slightly so borders interleave). Null only when no distance is a
// number (NaN inputs).
const Biome* classify_biome(double t, double m);
// desertness(t, m): how desert-like a climate is (0..1): drives dunes in the terrain.
double desertness(double t, double m);

}  // namespace svx::city
