// svx_city — nature/landcover.hpp (voxel_city nature/landcover.js).
#include "nature/landcover.hpp"

#include <cmath>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "nature/biomes.hpp"
#include "terrain/terrain.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"
#include "world/chart.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city {

namespace {

// Layered desert rock: bands of sandstone / red earth / pale sand by height.
uint16_t strata(double h_m) {
  const double band = std::floor(h_m / 5);
  const uint32_t k = js::to_uint32(band * 7919) % 5u;
  return k == 0 ? MAT::RED_EARTH : k == 1 ? MAT::SAND_DUNE : k == 2 ? MAT::CLAY : MAT::SANDSTONE;
}

}  // namespace

LandCover::LandCover(const World& w)
    : world(&w),
      wrap(wrap_of(w.config)),
      n_forest(derive_seed(w.seed, "landcover.forest")),
      n_farm(derive_seed(w.seed, "landcover.farm")),
      n_patch(derive_seed(w.seed, "landcover.patch")),
      n_pool(derive_seed(w.seed, "landcover.pool")),
      torus(w.chart->id.rfind("torus", 0) == 0),
      sea_level_(w.config["world"]["seaLevel"].to_number()),
      farmland_(w) {}

double LandCover::field_noise(const SimplexNoise& noise, double x, double y, double s_vox, double ox, double oy, int octaves) const {
  if (!torus) return octaves ? noise.fbm2(x / s_vox + ox, y / s_vox + oy, octaves) : noise.n2(x / s_vox + ox, y / s_vox + oy);
  const FieldPoint f = world->chart->to_field(x * kVoxelSize, y * kVoxelSize);
  const double s = s_vox * kVoxelSize;
  return octaves ? noise.fbm4(f.x / s + ox, f.y / s + oy, f.z / s, f.w / s, octaves) : noise.n4(f.x / s + ox, f.y / s + oy, f.z / s, f.w / s);
}

double LandCover::field_fbm_m(const SimplexNoise& noise, double x, double y, double s_m, int octaves) const {
  if (!torus) return noise.fbm2((x * kVoxelSize) / s_m, (y * kVoxelSize) / s_m, octaves);
  return field_noise(noise, x, y, s_m / kVoxelSize, 0, 0, octaves);
}

LocalClimate LandCover::climate(double x, double y, double h_m) const {
  const MacroFields& f = *world->fields;
  // local exposure / microclimate: shifts the tree line and snow line by ~±150 m so altitude
  // zones end in ragged edges instead of contour bands
  const double micro = 0.012 * field_noise(n_patch, x, y, 2400, 3.1, 0) + 0.007 * field_noise(n_patch, x, y, 520, 0, -7.7);
  const double t = f.temperature(x, y) - js::max(0.0, h_m) / kLapse + micro;
  return {t, f.moisture(x, y)};
}

double LandCover::tree_line(double t) const { return smoothstep(kTreeline, kTreeline + 0.06, t); }

const Biome* LandCover::biome_at(double x, double y, double h_m) const {
  const LocalClimate c = climate(x, y, h_m);
  const double j = 0.035 * field_noise(n_patch, x, y, 2400);
  return classify_biome(c.t + j, c.m - j);
}

double LandCover::clearing(double x, double y) const { return smoothstep(0.32, 0.58, field_fbm_m(n_patch, x + 91000, y - 37000, 260, 2)); }

double LandCover::forest_density(double x, double y, double u, const Biome* biome, std::optional<double> h_meters) const {
  const MacroFields& F = *world->fields;
  const double m = F.moisture(x, y);
  const double n = field_fbm_m(n_forest, x, y, 700, 3);
  double base = smoothstep(0.1, 0.55, n * 0.6 + (m - 0.45) * 1.2) * (1 - 0.9 * clearing(x, y));
  // mountain flanks below the tree line are wooded
  const double mtn = F.mountainness(x, y);
  if (mtn > 0.02) base = js::max(base, 0.9 * smoothstep(0.02, 0.25, mtn) * smoothstep(0.25, 0.4, m + 0.1));
  const double h_m = h_meters ? *h_meters : world->terrain->sample(x, y).h * kVoxelSize;
  const Biome& b = biome ? *biome : *biome_at(x, y, h_m);
  const double t = climate(x, y, h_m).t;
  // (farmland round the towns and farms takes the place of the woods)
  return js::min(1.0, base * js::max(b.forest, mtn > 0.1 ? 0.9 : 0)) * (1 - smoothstep(0.08, 0.2, u)) * tree_line(t) *
         (1 - 0.85 * farm_mask(x, y, u, b));
}

double LandCover::farm_mask(double x, double y, double u, const Biome& biome) const {
  if (u >= 0.2) return 0;
  const double near = smoothstep(0.002, 0.04, u) * (1 - smoothstep(0.14, 0.2, u));
  if (biome.farmland) return smoothstep(0.22, 0.36, field_fbm_m(n_farm, x, y, 1500, 2) + 0.22 * near);
  if (biome.id != "boreal" && biome.id != "temperate" && biome.id != "grassland") return 0;
  return smoothstep(0.35, 0.5, field_fbm_m(n_farm, x, y, 900, 2) * 0.5 + 0.7 * near);
}

bool LandCover::is_farmland(double x, double y, double u, const Biome* biome) const {
  const Biome& b = biome ? *biome : *biome_at(x, y, world->terrain->sample(x, y).h * kVoxelSize);
  return farm_mask(x, y, u, b) > 0.5;
}

double LandCover::hedge_at(double x, double y, double u, const Biome& biome, double t, bool farm) const {
  if (!farm && farm_mask(x, y, u, biome) <= 0.5) return 0;
  return farmland_.hedge(farmland_.field_at(x, y), t);
}

double LandCover::pool_depth(double x, double y, const Biome& biome, double slope, double u) const {
  if (!js::truthy(biome.pools) || slope > 0.12 || u > 0.1) return 0;
  const double n = field_noise(n_pool, x, y, 520, 0, 0, 2);
  const double e = n - (1 - biome.pools * 2.2);
  return e > 0 ? js::min(4.0, 1 + std::floor(e * 18)) : 0;
}

Surface LandCover::surface(double x, double y, double h_vox, double slope, double u, bool farm) const {
  const double h_m = h_vox * kVoxelSize;
  const double t = climate(x, y, h_m).t;
  const Biome& biome = *biome_at(x, y, h_m);
  const double patch = field_noise(n_patch, x, y, 800);
  const bool cold = t < 0.22;
  Surface out;
  auto two = [&](uint16_t top, uint16_t sub) {
    out.top = top;
    out.sub = sub;
    return out;
  };
  if (h_m < sea_level_ + 1.2) {
    // northern shores are shingle with sand in the coves
    if (cold || (world->fields->island && t < 0.42 && patch > -0.2)) return two(MAT::GRAVEL, MAT::STONE);
    return two(MAT::SAND, MAT::SAND);
  }
  const bool desert = biome.id == "desert" || biome.id == "savanna";
  // cliffs: bare rock; dry country shows layered sandstone strata; outcrops break up steep ground
  // (a noise lowers the threshold in bands)
  const double crag = 0.25 * field_noise(n_patch, x, y, 60);
  // vegetation clings to steep ground below the tree line; above it rock takes over sooner
  const double cliff = t < kTreeline ? 1.05 : 1.45 + 0.8 * js::min(1.0, (t - kTreeline) * 4);
  if (slope > cliff + crag) return desert ? two(strata(h_m), MAT::SANDSTONE) : two(MAT::ROCK, MAT::STONE);
  // glaciers and permanent snow above the snow line (steep faces stay rock); wind-scoured crust in
  // streaks, rock ribs through the steeper snow
  if (t < kSnowline + 0.025 * patch && slope < 1.0) {
    // rock outcrops break through where the snow lies steeper
    const double o = field_noise(n_patch, x, y, 150, 11, 0) + 0.45 * field_noise(n_patch, x, y, 38, 0, -4);
    if (slope > 0.5 && o > 1.25 - slope) return two(MAT::ROCK_DARK, MAT::ROCK);
    const double wind = (torus ? field_noise(n_patch, x, y, 36, -7, 3) : n_patch.n2(x / 60 - 7, y / 22 + 3)) + 0.4 * field_noise(n_patch, x, y, 9);
    return two(wind > 0.45 ? MAT::SNOW_WIND : MAT::SNOW, MAT::ROCK);
  }
  if (desert && slope > 0.7) return two(strata(h_m), MAT::SANDSTONE);
  // scree aprons under the cliffs: rubble of the rock above
  if (desert && slope > 0.38) {
    const uint32_t hr = hash32(wrap.vi(x), wrap.vi(y), 0x9a7) & 7u;
    return two(hr < 3 ? static_cast<uint16_t>(MAT::GRAVEL) : hr < 5 ? static_cast<uint16_t>(MAT::RED_EARTH) : strata(h_m), MAT::SANDSTONE);
  }
  if (slope > 0.8 + crag && t < kTreeline + 0.02) return two(MAT::GRAVEL, MAT::ROCK);
  // alpine zone above the tree line: meadows, scree and snow patches
  if (t < kTreeline) {
    const double p = field_noise(n_patch, x, y, 140);
    if (p > 0.45 && t < kTreeline - 0.03) return two(MAT::SNOW, MAT::GRAVEL);
    if (p < -0.3 || slope > 0.45) return two(MAT::GRAVEL, MAT::ROCK);
    return two(p > 0.1 ? MAT::TUNDRA : MAT::GRASS_DRY, MAT::DIRT);
  }
  const double pool = pool_depth(x, y, biome, slope, u);
  if (js::truthy(pool)) {
    out.pond = pool;
    return two(MAT::MUD, MAT::MUD);
  }
  const double fd = farm ? 0 : forest_density(x, y, u, &biome, h_m);
  // per column: a hash and a fine patch noise, so materials mix in dithered drifts instead of flat
  // blobs with hard outlines
  const double h = hash32(wrap.vi(x), wrap.vi(y), 0x9a5) / 4294967296.0;
  const double p2 = field_noise(n_patch, x, y, 40, 3.3, 7.7);
  if (fd > 0.35) return two(forest_floor(biome, t, h, p2, patch), MAT::DIRT);
  if (slope < 0.3 && (farm || farm_mask(x, y, u, biome) > 0.5)) {
    // fields: crops by climate and season, grass margins, hedges and field walls
    const FieldGround g = farmland_.ground(farmland_.field_at(x, y), t, x, y);
    out.bump = g.bump;
    out.field = true;
    return two(g.top, g.sub);
  }
  // meadows in the glades of wooded country: lush grass (flowers in summer)
  if (biome.forest >= 0.8 && clearing(x, y) > 0.35)
    return two(h < smoothstep(0.35, 0.6, patch + 0.3 * p2) ? MAT::GRASS_DRY : h < smoothstep(-0.4, -0.65, patch) ? MAT::GRASS_DARK : MAT::GRASS,
               MAT::DIRT);
  BiomeGroundCtx c;
  c.patch = field_noise(n_patch, x, y, 90);
  c.x = x;
  c.y = y;
  c.h = h;
  c.p2 = p2;
  const std::array<uint16_t, 2> g = biome.ground(c);
  return two(g[0], g[1]);
}

uint16_t LandCover::forest_floor(const Biome& biome, double t, double h, double p2, double patch) const {
  const uint16_t f = biome.floor;
  if (f == MAT::MOSS || t < 0.42) {
    // boreal: moss carpets, needle litter under the spruce, grey lichen on the dry
    if (h < smoothstep(0.05, 0.4, p2) * 0.8) return MAT::NEEDLE_LITTER;
    if (h > 0.94 && patch > 0.2) return MAT::LICHEN;
    return f == MAT::MOSS ? static_cast<uint16_t>(MAT::MOSS) : f;
  }
  if (f == MAT::FOREST_FLOOR) {
    if (h < smoothstep(0.1, 0.45, p2) * 0.6) return MAT::MOSS;
    if (h > 0.9) return MAT::LEAF_LITTER;
    return MAT::FOREST_FLOOR;
  }
  return h > 0.85 ? static_cast<uint16_t>(MAT::FOREST_FLOOR) : f;
}

}  // namespace svx::city
