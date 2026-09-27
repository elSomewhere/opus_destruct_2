// Doom WAD reader + voxelizer on Freedoom (skipped if the WADs are not installed).
#include <cstdio>
#include <string>

#include "doctest.h"
#include "svx/doom/slenderness.hpp"
#include "svx/doom/voxelize.hpp"
#include "svx/engine/engine.hpp"
#include "svx/doom/wad.hpp"
#include "svx/world/columns.hpp"

using namespace svx;

TEST_CASE("freedoom: read and voxelize a map") {
  const std::string path = std::string(SVX_SOURCE_DIR) + "/data/freedoom/freedoom2.wad";
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    MESSAGE("freedoom2.wad not found; run scripts/fetch_freedoom.sh");
    return;
  }
  std::fclose(f);
  doom::Wad wad;
  std::string err;
  REQUIRE(wad.load(path, &err));
  const auto names = wad.map_names();
  CHECK(names.size() == 32);
  doom::Map map;
  REQUIRE(wad.read_map("MAP01", &map, &err));
  CHECK(map.sectors.size() > 10);
  ColumnGrid g;
  doom::VoxelizeStats st;
  REQUIRE(doom::voxelize(map, doom::VoxelizeOptions{}, &g, &st, &err));
  CHECK(st.structural_voxels > 100000);
  CHECK(st.sector_columns > 1000);
  const RunComponents rc = run_components(g);
  CHECK(rc.comps.size() >= 1);
  MESSAGE("MAP01: grid " << st.nx << "x" << st.ny << "x" << st.nz << ", structural " << st.structural_voxels
                         << ", components " << rc.comps.size());
}

TEST_CASE("freedoom: the walls' buckling margin caps S_p (plan B3, B10)") {
  const std::string path = std::string(SVX_SOURCE_DIR) + "/data/freedoom/freedoom2.wad";
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    MESSAGE("freedoom2.wad not found; run scripts/fetch_freedoom.sh");
    return;
  }
  std::fclose(f);
  doom::Wad wad;
  std::string err;
  REQUIRE(wad.load(path, &err));
  for (const char* name : {"MAP01", "MAP29"}) {
    doom::Map map;
    REQUIRE(wad.read_map(name, &map, &err));
    ColumnGrid g;
    doom::VoxelizeStats st;
    REQUIRE(doom::voxelize(map, doom::VoxelizeOptions{}, &g, &st, &err));
    const doom::SlendernessReport r = doom::wall_slenderness(g, material(MaterialId::Concrete), 0.125);
    MESSAGE(std::string(name) << ": " << r.free_members << " free members, S_p cap " << r.max_compliance_p99 << " (p99)");
    CHECK(r.free_members > 1000);
    // Phase 0 (docs/phase0/PHASE0_REPORT.md section 6): MAP01 49.7, MAP29 7.5 (tall outdoor walls)
    if (std::string(name) == "MAP01") CHECK(r.max_compliance_p99 == doctest::Approx(49.7).epsilon(0.02));
    else CHECK(r.max_compliance_p99 == doctest::Approx(7.5).epsilon(0.02));
  }
}

TEST_CASE("engine: a compliance cap clamps S_p; the request survives it; load clears it") {
  Engine eng;
  EngineParams p;
  p.compliance = 9.0;
  eng.set_params(p);
  eng.set_compliance_cap(7.5);
  CHECK(eng.params().compliance == 7.5);
  p.compliance = 5.0;
  eng.set_params(p);
  CHECK(eng.params().compliance == 5.0);  // under the cap
  p.compliance = 12.0;
  eng.set_params(p);
  CHECK(eng.params().compliance == 7.5);
  eng.load(VoxelGrid{}, {0, 0, 0}, {1, 0, 0});  // a new world: no cap
  CHECK(eng.compliance_cap() == 0.0);
  CHECK(eng.params().compliance == 12.0);
}
