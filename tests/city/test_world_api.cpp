// svx_city tests — the public entry (svx/city/world.hpp): a world of every voxel_city preset, as
// the export makes it.
#include <doctest.h>

#include "svx/city/world.hpp"
#include "world/World.hpp"

using namespace svx::city;

TEST_CASE("city world api: every preset makes a world; unknown presets and malformed overrides do not") {
  const std::vector<std::string> ids = preset_ids();
  CHECK(ids.size() >= 14);
  for (const std::string& id : ids) {
    WorldSpec s;
    s.preset = id;
    std::string err;
    const std::shared_ptr<World> w = make_world(s, &err);
    CHECK_MESSAGE(w != nullptr, id, ": ", err);
    if (w) CHECK(w->config["world"]["angles"]["partsMode"].str() == "separate");
  }
  WorldSpec bad;
  bad.preset = "nope";
  std::string err;
  CHECK(make_world(bad, &err) == nullptr);
  CHECK(err.find("nope") != std::string::npos);
  WorldSpec junk;
  junk.overrides_json = "{ not json";
  CHECK(make_world(junk, &err) == nullptr);
  WorldSpec arr;
  arr.overrides_json = "[1, 2]";
  CHECK(make_world(arr, &err) == nullptr);
  WorldSpec ok;
  ok.overrides_json = R"({"seed": 99, "lakes": {"enabled": false}})";
  const std::shared_ptr<World> w = make_world(ok, &err);
  REQUIRE(w != nullptr);
  CHECK(w->config["seed"].to_number() == 99);
  CHECK(!w->config["lakes"]["enabled"].truthy());  // (the overrides merged over the preset)
}
