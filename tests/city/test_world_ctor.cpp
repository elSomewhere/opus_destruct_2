// svx_city tests — the World's constructor and its feature sources (voxel_city world/World.js).
#include <doctest.h>

#include "network/arterials.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/caches.hpp"
#include "world/chart.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"
#include "worlds.hpp"

using namespace svx::city;

TEST_CASE("city world: the constructor makes the config, chart, fields, terrain and arterial grid") {
  const World w(Value::object({{"seed", 42}, {"world", Value::object({{"chart", "torus"}, {"size", 50000}})}}));
  CHECK(w.seed == 42);
  CHECK(w.config["world"]["size"].to_number() == 49920);  // (makeConfig: a multiple of 240 m)
  CHECK(w.chart->id == "torus:49920");
  CHECK(w.fields->wrap.on);
  CHECK(w.fields->island == nullptr);
  CHECK(w.terrain->forms().size() == 14);
  CHECK(w.terrain->forms().front().lf->id == "continent");
  CHECK(w.terrain->forms().back().lf->id == "roughness");
  CHECK(w.arterials->n > 0);
  CHECK(w.caches().cell_nets.size() == 0);
  const World isl(test::world_overrides(test::worlds()[11]));  // nordicTown:skerry
  REQUIRE(isl.fields->island != nullptr);
  CHECK(isl.fields->settlement(0, 0)->id == "S0_0");
}

TEST_CASE("city world: feature sources stay sorted by order, ties in the order they were added") {
  World w(Value::object());
  auto src = [](const char* id, double order) {
    auto s = std::make_shared<FeatureSource>();
    s->id = id;
    s->order = order;
    return std::shared_ptr<const FeatureSource>(s);
  };
  w.add_feature_source(src("a", 5));
  w.add_feature_source(src("b", 1));
  w.add_feature_source(src("c", 5));
  w.add_feature_source(src("d", 3));
  w.add_feature_source(src("e", 1));
  w.add_feature_source(src("f", 6.5));
  w.add_feature_source(src("g", 5));
  std::string order;
  for (const auto& s : w.feature_sources) order += s->id;
  CHECK(order == "bedacgf");
}
