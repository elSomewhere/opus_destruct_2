// structvox — run-length column voxel grid (used by importers and offline analysis).
//
// Each (x, y) column stores sorted, non-overlapping runs [z0, z1) of solid voxels.
// Run kinds: Structural (participates in mechanics) or Anchored (implicit bedrock/rock;
// Dirichlet). Air is implicit.
#pragma once

#include <array>
#include <vector>

#include "svx/base/types.hpp"
#include "svx/mech/material.hpp"

namespace svx {

enum class RunKind : u8 { Structural = 0, Anchored = 1 };

struct Run {
  i32 z0, z1;  // [z0, z1)
  RunKind kind;
  MaterialId mat;
};

struct ColumnGrid {
  i32 nx = 0, ny = 0;
  i32 zmin = 0, zmax = 0;
  std::vector<u32> col_start;  // nx*ny + 1 offsets into runs
  std::vector<Run> runs;

  i32 column(i32 x, i32 y) const { return y * nx + x; }
  const Run* begin(i32 c) const { return runs.data() + col_start[c]; }
  const Run* end(i32 c) const { return runs.data() + col_start[c + 1]; }
  i64 count(RunKind k) const;
};

// Connected components of structural voxels (6-connectivity) computed on runs.
struct RunComponents {
  struct Comp {
    i64 voxels = 0;
    i64 anchor_bonds = 0;  // bonds from structural voxels to anchored voxels
    std::array<i32, 6> bbox{};  // xmin, ymin, zmin, xmax, ymax, zmax (inclusive)
    i32 first_run = -1;    // representative run index
  };
  std::vector<i32> run_comp;  // per run: component id, or -1 for anchored runs
  std::vector<Comp> comps;
};

RunComponents run_components(const ColumnGrid& g);

}  // namespace svx
