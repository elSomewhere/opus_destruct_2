// svx_city tests — the worlds the stages of the world base sample (tools/procgen_ref/lib/worlds.mjs
// is the Node twin): every golden preset and size variant of the reference, plus two angled
// presets; and the golden sample points.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "config/presets.hpp"
#include "core/value.hpp"

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

// The golden sample points (metres).
constexpr std::array<std::array<double, 2>, 5> kGoldenPoints = {{{0, 0}, {180, -140}, {1400, 900}, {-3200, 2600}, {7000, -5200}}};

}  // namespace svx::city::test
