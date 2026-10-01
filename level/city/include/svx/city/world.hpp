// svx_city — the generator's public entry (docs/CITY.md): a world made from one of voxel_city's
// presets as its export makes it (svx/source.js createSvxSource: the angled world's parts apart
// from the world grid), for a host - the adapter in svx_procgen - to stream, query and draw. The
// World is opaque here; the other public headers take it.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace svx::city {

class World;

struct WorldSpec {
  std::string preset = "infiniteCity";  // a voxel_city preset id (config/presets.js)
  std::string size;                     // its size variant ("": its default)
  std::string season;                   // ("": the preset's own)
  double seed = 1337;
  std::string overrides_json;           // config overrides over the preset's: a JSON object ("": none)
};

// The world of a spec; null (and *error) for an unknown preset or malformed overrides. Generation
// may run on several threads at once against it.
std::shared_ptr<World> make_world(const WorldSpec& spec, std::string* error = nullptr);

// The voxel_city preset ids, in registration order.
std::vector<std::string> preset_ids();

}  // namespace svx::city
