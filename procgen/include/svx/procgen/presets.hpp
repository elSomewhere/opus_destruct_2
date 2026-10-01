// structvox procgen — presets (docs/PRESETS.md): a world to load, as data. A preset
// (data/presets/<group>/<name>.json, embedded in every build) names its generator and the
// generator's parameters, the streaming and far-tier configuration, world tunables, environment
// settings, population and the front end's atmosphere and spawn.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "svx/base/types.hpp"
#include "svx/base/vec.hpp"
#include "svx/game/game.hpp"
#include "svx/game/source.hpp"
#include "svx/world/source.hpp"

namespace svx {

struct Preset {
  std::string id;           // "<group>/<name>": "city/angledInfiniteCity", "legacy/drive"
  std::string label, group, description;
  // The generator: "drive" (the endless drive city), "city1km" (the 1 km streamed city), "level"
  // (a bounded procedural level: `level` names it), "city" (the city generator, docs/CITY.md:
  // params_json holds its parameters - the voxel_city preset, size, season, config overrides).
  std::string generator, level;
  std::string params_json = "{}";
  bool experimental = false;
  u64 seed = 1;  // (when the caller gives none)
  bool has_stream = false;
  StreamConfig stream;
  bool has_far = false;
  FarConfig far;
  std::vector<std::pair<std::string, f64>> tunables;  // world tunables by name (Game::set_tunable)
  std::vector<std::pair<std::string, f64>> env;       // environment parameters by name (Game::set_env)
  bool has_traffic = false, has_pedestrians = false;
  TrafficConfig traffic;
  PedestrianConfig pedestrians;
  std::string atmosphere_json;  // the front end's sky, fog, sun, season, time of day (JSON; empty: its defaults)
  bool has_spawn = false;
  V3 spawn_pos, spawn_dir{1, 0, 0};
};

// Parses a preset (false: malformed, or a field of the wrong kind - *error says which).
bool parse_preset(const std::string& json, Preset* out, std::string* error);

// The presets every build carries (data/presets, embedded at build time), by file name.
struct EmbeddedPreset {
  const char* path;  // relative to data/presets: "city/angledInfiniteCity.json"
  const char* json;
};
const std::vector<EmbeddedPreset>& embedded_presets();

// The embedded presets, parsed, in id order (a malformed one is left out: tests check them all).
const std::vector<Preset>& presets();
const Preset* find_preset(const std::string& id);

// The id of the default world (city/angledInfiniteCity once the city generator is in the build;
// until then legacy/drive).
const char* default_preset_id();

// Loads a preset into a game: its world (seed 0: the preset's own), then its tunables,
// environment and population. False (and *error) when the preset cannot be loaded here: a
// generator this build does not have, an unknown tunable or environment parameter.
bool load_preset(Game& game, const Preset& p, u64 seed, f64 h, std::string* error);

}  // namespace svx
