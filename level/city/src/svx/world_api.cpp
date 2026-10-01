// svx_city — the generator's public entry (svx/city/world.hpp).
#include "svx/city/world.hpp"

#include "config/defaults.hpp"
#include "config/presets.hpp"
#include "core/value.hpp"
#include "world/World.hpp"

namespace svx::city {

std::shared_ptr<World> make_world(const WorldSpec& spec, std::string* error) {
  if (!find_preset(spec.preset)) {
    if (error) *error = "unknown voxel_city preset " + spec.preset;
    return nullptr;
  }
  Value cfg = preset_config(spec.preset, spec.size, spec.seed, spec.season);
  if (!spec.overrides_json.empty()) {
    Value o;
    std::string why;
    if (!Value::parse_json(spec.overrides_json, &o, &why) || !o.is_object()) {
      if (error) *error = "city config overrides: " + (why.empty() ? std::string("not a JSON object") : why);
      return nullptr;
    }
    cfg.deep_merge(o);
  }
  // (svx/source.js: makeConfig, then the angled world's parts as grids of their own)
  Value full = make_config(cfg);
  full.deep_merge(Value::object({{"world", Value::object({{"angles", Value::object({{"partsMode", Value("separate")}})}})}}));
  return create_world(full);
}

std::vector<std::string> preset_ids() {
  std::vector<std::string> out;
  for (const Preset& p : presets()) out.push_back(p.id);
  return out;
}

}  // namespace svx::city
