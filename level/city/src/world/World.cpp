// svx_city — world/World.hpp: the World's constructor (voxel_city world/World.js), its feature
// sources. (The other World methods are defined by the modules that implement them.)
#include "world/World.hpp"

#include "config/defaults.hpp"
#include "core/js.hpp"
#include "network/arterials.hpp"
#include "terrain/terrain.hpp"
#include "world/caches.hpp"
#include "world/chart.hpp"
#include "world/fields.hpp"
#include "world/register_all.hpp"

namespace svx::city {

World::World(const Value& config_overrides) {
  // (JS registers at module evaluation, before any world exists)
  register_all();
  config = make_config(config_overrides);
  seed = config["seed"].to_number();
  chart = std::make_shared<Chart>(make_chart(config["world"]));
  fields = std::make_shared<MacroFields>(config, chart);
  terrain = std::make_shared<Terrain>(config, chart, fields);
  arterials = std::make_shared<ArterialGrid>(config);
  caches_ = std::make_unique<Caches>();
}

World::~World() = default;

void World::add_feature_source(std::shared_ptr<const FeatureSource> src) {
  feature_sources.push_back(std::move(src));
  js::sort(feature_sources, [](const std::shared_ptr<const FeatureSource>& a, const std::shared_ptr<const FeatureSource>& b) {
    return a->order - b->order;
  });
}

}  // namespace svx::city
