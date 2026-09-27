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


