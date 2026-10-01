// svx_city — natural land cover for non-urban ground (voxel_city nature/landcover.js).
//
// The biome (nature/biomes) decides ground, vegetation, farmland and ponds; elevation overlays
// (beaches, bare rock on steep slopes, scree, the snow line, which drops in cold climates) are
// applied on top. forest_density also drives the trees (nature/forest). The fields of open
// country are the Farmland's (nature/farmland).
//
// Pure functions of position; immutable after construction (the reference makes its Farmland on
// first use: here with the LandCover, which changes nothing), so any thread may use one.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "core/noise.hpp"
#include "nature/farmland.hpp"
#include "world/wrap.hpp"

namespace svx::city {

class World;
struct Biome;

// LAPSE: metres of altitude per unit of the 0..1 temperature scale.
constexpr double kLapse = 10000;
// TREELINE, SNOWLINE: the local temperature of the tree line and of the snow line (a temperate
// climate, t = 0.5 at sea level, has its tree line near 2,200 m and permanent snow above
// ~3,200 m; warm regions carry both far higher).
constexpr double kTreeline = 0.28;
constexpr double kSnowline = 0.18;

// LandCover.climate's record: the local temperature (cooled by altitude) and the moisture.
struct LocalClimate {
  double t = 0, m = 0;
};

// LandCover.surface's [top, sub, waterDepth, bump, field]: the surface and subsurface materials of
// a natural column, a pond depth (voxels; 0 dry), a bump (a field wall's height, voxels) and
// whether it is a cultivated field. (JS leaves the last three undefined in most branches: 0 and
// false here, as its callers read them.)
struct Surface {
  uint16_t top = 0, sub = 0;
  double pond = 0;
  double bump = 0;
  bool field = false;
};

class LandCover {
 public:
  explicit LandCover(const World& world);
  LandCover(const LandCover&) = delete;
  LandCover& operator=(const LandCover&) = delete;

  const World* world;
  Wrap wrap;
  SimplexNoise n_forest, n_farm, n_patch, n_pool;
  bool torus = false;  // the chart wraps (a torus): noise is sampled on the chart

  // Planar noise at a voxel point with a scale in voxels, sampled on the chart where the world
  // wraps (a torus), so forest patches, fields and biome borders repeat with the world; plain 2D
  // noise elsewhere. octaves 0: one octave of noise, else fbm.
  double field_noise(const SimplexNoise& noise, double x, double y, double s_vox, double ox = 0, double oy = 0, int octaves = 0) const;
  // fbm at a voxel point with a scale in metres (see field_noise).
  double field_fbm_m(const SimplexNoise& noise, double x, double y, double s_m, int octaves) const;
  // Local climate: temperature cooled by altitude (h_m metres; about 6.5 °C per km on a 0..1
  // scale spanning ~55 °C), moisture.
  LocalClimate climate(double x, double y, double h_m) const;
  // 0 above the tree line, 1 well below it (for vegetation density).
  double tree_line(double t) const;
  // The biome at a column (h_m: its height in metres). Null only for NaN climates.
  const Biome* biome_at(double x, double y, double h_m) const;
  // 0..1 open meadow in forest country (glades, old hay meadows, pasture round the farms).
  double clearing(double x, double y) const;
  // Forest density 0..1 at a column of urbanization u (biome and height made when not given: JS's
  // null).
  double forest_density(double x, double y, double u, const Biome* biome = nullptr, std::optional<double> h_meters = std::nullopt) const;
  // The fields of open country.
  const Farmland& farmland() const { return farmland_; }
  // How much (x, y) is farmland (0..1): wide stretches of fields in mild farming country,
  // thickest round the towns and villages; in the north only small fields close round them.
  double farm_mask(double x, double y, double u, const Biome& biome) const;
  bool is_farmland(double x, double y, double u, const Biome* biome = nullptr) const;
  // Hedgerow strength at (x, y) (0..1: shrubs and trees along some field boundaries), t the local
  // climate.
  double hedge_at(double x, double y, double u, const Biome& biome, double t, bool farm = false) const;
  // Pond depth (voxels) of a wetland column, 0 = dry.
  double pool_depth(double x, double y, const Biome& biome, double slope, double u) const;
  // Surface and subsurface materials of a natural column of height h_vox (voxels) and slope.
  Surface surface(double x, double y, double h_vox, double slope, double u, bool farm = false) const;
  // Forest floor: the biome's floor dithered with moss, leaf litter or brown needles (under the
  // conifers of cool woods) and lichen.
  uint16_t forest_floor(const Biome& biome, double t, double h, double p2, double patch) const;

 private:
  double sea_level_ = 0;  // config.world.seaLevel (m)
  Farmland farmland_;
};

}  // namespace svx::city
