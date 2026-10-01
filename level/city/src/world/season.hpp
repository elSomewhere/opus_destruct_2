// svx_city — seasons (voxel_city world/season.js): `config.world.season` is "spring", "summer"
// (the default), "autumn" or "winter", a lens over the climate. Everything it changes depends on
// the LOCAL temperature t (sea-level temperature cooled by altitude, 0..1, nature/landcover), so
// one world can be autumn-gold in its valleys, first-snow white on its fells and still green in its
// warm south; warm lands (t > ~0.64) hardly change.
//
//   snow(t)          seasonal snow cover of open ground (0..1)
//   roof_snow(t)     the same for roofs
//   freeze_t         still water (lakes, ponds, rivers) freezes below it
//   tree_look(...)   foliage of one tree: leaf palette, bare branches, snow on the crown
//   ground(m, t, n)  seasonal ground material
//   flower(...)      wild flowers on meadows
//   canopy(...)      the crown blanket of distant forests
//
// A climate that sets `snowCover` / `freeze` explicitly keeps that fixed snow and ice; the season
// still colours foliage and grass. (config/presets.cpp holds SEASONS and SEASON_ATMOSPHERE.)
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/value.hpp"

namespace svx::city {

// A seasonal leaf palette: [shadow, body, sunlit] materials.
using LeafPalette = std::array<uint16_t, 3>;

// treeLook's record: leaves (null: the tree's own summer palette), mix (the share of the crown's
// clusters in that palette), bare (branches only), snow (0..1 on the crown), holes (extra gaps in
// the crown shell), accent (berries: autumn only; 0 for JS's null and undefined).
struct TreeLook {
  const LeafPalette* leaves = nullptr;
  double mix = 0;
  bool bare = false;
  double snow = 0;
  double holes = 0;
  uint16_t accent = 0;
  bool has_accent = false;  // (JS: the record has the key - autumn - even when it is null)
};

class Season {
 public:
  explicit Season(const Value& config);

  std::string id;  // spring, summer, autumn, winter
  // the climate's explicit (legacy) snow cover (none: undefined; JS's null reads as 0)
  std::optional<double> fixed_snow;
  bool cold = false;
  double freeze_t = 0;
  // does the season change anything on the ground at all (summer: flowers only)
  bool any = false;
  // (the seasonal ground table: none in summer)
  bool has_table() const { return table_ >= 0; }

  // 0..1: how strongly deciduous vegetation shows the season at local temperature t.
  double strength(double t) const;
  // Seasonal snow cover (0..1) of open ground at local temperature t.
  double snow(double t) const;
  // Snow cover of roofs (an explicit climate cover lies on every roof).
  double roof_snow(double t) const;
  // Foliage of one tree of `kind` (hash `seed`) at local temperature t; nothing for the plain
  // summer look (JS: null).
  std::optional<TreeLook> tree_look(std::string_view kind, double seed, double t) const;
  // Seasonal ground material replacing summer ground m at local temperature t; n (0..1) a smooth
  // patch value so the change is patchy near the warm edge.
  int ground(int m, double t, double n) const;
  // Wild flowers on open natural ground (top material m), or 0 (JS: null). cl (0..1) a smooth
  // cluster value (flowers grow in drifts), h a hash per column, sp a hash per cluster (its species).
  int flower(int m, double t, double cl, double h, double sp) const;
  // Distant canopy (the crown blanket at coarse LODs): a conifer crown, or a broadleaf one in its
  // seasonal colours at local temperature tl; k a per-crown hash.
  int canopy(bool conifer, double tl, double k) const;

 private:
  int table_ = -1;  // 0 spring, 1 autumn, 2 winter
};

// seasonOf(config): the Season of a config (the reference caches it per config object; it is a
// few numbers, made when asked: hoist it out of loops).
inline Season season_of(const Value& config) { return Season(config); }

// Smooth value noise (0..1) from hashes on a square lattice of `cell` voxels: cheap patchiness
// for seasonal ground, flower drifts and the like. (x, y) should be canonical coordinates in a
// wrapping world.
double patch_noise(double seed, double x, double y, double cell, double salt);

}  // namespace svx::city
