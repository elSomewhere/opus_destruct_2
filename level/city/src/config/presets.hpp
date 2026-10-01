// svx_city — world presets (voxel_city config/presets.js): named starting points, config overrides
// plus a viewer mood. presetConfig(id, size, seed, season) resolves one into the overrides
// make_config takes.
#pragma once

#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "core/value.hpp"

namespace svx::city {

struct Preset {
  std::string id, label, description;
  Value sizes;                // array of size variants ({ id, label, ... }) or undefined
  std::string default_size;   // ("" none)
  std::string default_season; // ("" summer)
  std::function<Value(const Value& size)> config;
  Value viewer;               // { timeOfDay, atmosphere, seasons } or undefined
};

// The presets in registration order.
const std::vector<Preset>& presets();
const Preset* find_preset(const std::string& id);
// The seasons ("spring", "summer", "autumn", "winter") and their default viewer moods.
const std::vector<std::string>& seasons();
const Value& season_atmosphere();

// A preset's size variant by id (or its default, else its first); undefined without sizes.
Value preset_size(const Preset& p, const std::string& size_id);
// A preset's season: the given one if valid, else its default, else summer.
std::string preset_season(const Preset& p, const std::string& season);
// The config overrides of a preset (+ size, seed - NaN: none - and season).
Value preset_config(const std::string& id, const std::string& size = "", double seed = std::numeric_limits<double>::quiet_NaN(), const std::string& season = "");
// The viewer mood of a preset in a season: { timeOfDay, atmosphere }.
Value preset_viewer(const std::string& id, const std::string& season = "");

}  // namespace svx::city
