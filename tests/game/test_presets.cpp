// Presets (docs/PRESETS.md): every preset the build carries parses and names what exists; the
// legacy ones load what svx_load_procedural loads, bit for bit.
#include <cstring>
#include <set>
#include <string>

#include "doctest.h"
#include "svx/game/api/svx_api.h"
#include "svx/procgen/presets.hpp"

using namespace svx;

TEST_CASE("presets: every embedded preset parses, its id is its path, ids are unique") {
  const auto& files = embedded_presets();
  REQUIRE(files.size() >= 20);
  std::set<std::string> ids;
  for (const EmbeddedPreset& f : files) {
    Preset p;
    std::string err;
    INFO(f.path);
    REQUIRE_MESSAGE(parse_preset(f.json, &p, &err), err);
    std::string path = f.path;
    CHECK(path.substr(0, path.size() - 5) == p.id);  // (".json")
    CHECK(ids.insert(p.id).second);
    CHECK_FALSE(p.label.empty());
    CHECK((p.group == "city" || p.group == "legacy"));
  }
  CHECK(presets().size() == files.size());
  REQUIRE(find_preset("city/angledInfiniteCity") != nullptr);
  REQUIRE(find_preset("city/infiniteCity") != nullptr);
  CHECK(find_preset("legacy/drive")->stream.load_radius == 112.0);
  CHECK(find_preset("city/angledInfiniteCity")->tunables.size() == 1);
  CHECK(find_preset(default_preset_id()) != nullptr);
}

TEST_CASE("presets: malformed presets are refused with a reason") {
  Preset p;
  std::string err;
  CHECK_FALSE(parse_preset("[1, 2]", &p, &err));
  CHECK_FALSE(parse_preset(R"({"id": "x/y", "generator": "nope"})", &p, &err));
  CHECK(err.find("generator") != std::string::npos);
  CHECK_FALSE(parse_preset(R"({"id": "x/y", "generator": "level"})", &p, &err));
  CHECK_FALSE(parse_preset(R"({"id": "x/y", "generator": "drive", "streaming": {"load_radius": "far"}})", &p, &err));
  CHECK(err.find("load_radius") != std::string::npos);
  CHECK_FALSE(parse_preset(R"({"id": "x/y", "generator": "drive", "tunables": {"pretouch_radius": true}})", &p, &err));
  CHECK(parse_preset(R"({"id": "x/y", "generator": "drive", "tunables": {"pretouch_radius": 32}, "spawn": {"pos": [1, 2, 3]}})", &p, &err));
  CHECK(p.has_spawn);
  CHECK(p.spawn_pos.z == 3.0);
}

TEST_CASE("presets: a legacy preset loads the world svx_load_procedural loads") {
  for (const char* kind : {"rooms", "tower", "drive"}) {
    INFO(kind);
    svx_engine* a = svx_create(0.125);
    svx_engine* b = svx_create(0.125);
    REQUIRE(svx_load_procedural(a, kind, 7) == 0);
    REQUIRE(svx_load_preset(b, (std::string("legacy/") + kind).c_str(), 7) == 0);
    double ia[13], ib[13];
    svx_world_info(a, ia);
    svx_world_info(b, ib);
    for (int q = 0; q < 13; ++q) CHECK(ia[q] == ib[q]);
    for (int t = 0; t < 10; ++t) {
      svx_tick(a);
      svx_tick(b);
    }
    svx_world_info(a, ia);
    svx_world_info(b, ib);
    CHECK(ia[6] == ib[6]);
    CHECK(std::string(svx_preset_atmosphere(b)) == "{}");
    svx_destroy(a);
    svx_destroy(b);
  }
}

TEST_CASE("presets: the listing names every preset; a city preset says it is not in this build yet") {
  svx_engine* e = svx_create(0.125);
  const std::string list = svx_presets(e);
  CHECK(list.find("\"id\":\"city/angledInfiniteCity\"") != std::string::npos);
  CHECK(list.find("\"id\":\"legacy/machines\"") != std::string::npos);
  CHECK(svx_load_preset(e, "nope/none", 0) != 0);
  CHECK(std::string(svx_last_error(e)).find("unknown preset") != std::string::npos);
  CHECK(svx_load_preset(e, "city/infiniteCity", 0) != 0);
  CHECK(std::strlen(svx_last_error(e)) > 0);
  svx_destroy(e);
}
