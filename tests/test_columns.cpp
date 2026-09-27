// Run-length column grid connectivity.
#include <vector>

#include "doctest.h"
#include "svx/world/columns.hpp"

using namespace svx;

TEST_CASE("run components: two walls on bedrock and a floating block") {
  // 5x1 columns. Column 0: bedrock [0,2) + wall [2,6). Column 1: bedrock only.
  // Column 2: bedrock + wall [2,4). Column 3: floating block [7,9). Column 4: empty.
  ColumnGrid g;
  g.nx = 5;
  g.ny = 1;
  g.zmin = 0;
  g.zmax = 10;
  auto add = [&](i32 z0, i32 z1, RunKind k) { g.runs.push_back({z0, z1, k, MaterialId::Concrete}); };
  g.col_start.push_back(0);
  add(0, 2, RunKind::Anchored);
  add(2, 6, RunKind::Structural);
  g.col_start.push_back(static_cast<u32>(g.runs.size()));
  add(0, 2, RunKind::Anchored);
  g.col_start.push_back(static_cast<u32>(g.runs.size()));
  add(0, 2, RunKind::Anchored);
  add(2, 4, RunKind::Structural);
  g.col_start.push_back(static_cast<u32>(g.runs.size()));
  add(7, 9, RunKind::Structural);
  g.col_start.push_back(static_cast<u32>(g.runs.size()));
  g.col_start.push_back(static_cast<u32>(g.runs.size()));
  const RunComponents rc = run_components(g);
  REQUIRE(rc.comps.size() == 3);
  CHECK(rc.comps[0].voxels == 4);
  CHECK(rc.comps[0].anchor_bonds == 1);
  CHECK(rc.comps[1].voxels == 2);
  CHECK(rc.comps[1].anchor_bonds >= 1);
  CHECK(rc.comps[2].voxels == 2);
  CHECK(rc.comps[2].anchor_bonds == 0);
  const std::vector<CellIn> cells = extract_component_cells(g, rc, 0);
  int anchored = 0;
  for (const auto& c : cells) anchored += c.anchored;
  CHECK(cells.size() == 5);  // 4 wall voxels + 1 bedrock voxel below
  CHECK(anchored == 1);
}
