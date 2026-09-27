// structvox — physics windows over the world grid and world-level connectivity.
//
// A Region is a Lattice of every solid voxel within a sphere around an event, plus the rock
// (anchored) voxels bonded to it from outside. Structural voxels whose bonds leave the window
// are pinned (they act as supports): for rock-embedded levels (Doom shells are within ~1 m of
// anchored rock everywhere) a window of a few metres reproduces full-structure solves to a
// fraction of a percent (Phase 0 content study), and structures smaller than the window are
// taken whole (no rim). Broken bonds and committed damage come from the grid overlays.
//
// Detachment is always decided on the world grid (a piece may reach beyond any window): the
// same lockstep multi-source search as topo/connectivity, over grid bonds, with anchored
// voxels as supports.
#pragma once

#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "svx/base/coord_map.hpp"
#include "svx/solve/lattice.hpp"
#include "svx/topo/connectivity.hpp"
#include "svx/world/coarse.hpp"
#include "svx/world/grid.hpp"

namespace svx {

struct Region {
  Lattice L;
  std::vector<IVec3> vox;                 // cell -> voxel
  CoordMap index;                         // voxel -> cell
  std::vector<u8> rim;                    // cells pinned because the window cut their bonds
  IVec3 center{0, 0, 0};
  i32 radius = 0;
  i32 rim_count = 0;
  i32 cell(const IVec3& p) const { return index.find(p); }
};

Region extract_region(const VoxelGrid& g, const IVec3& center, i32 radius, const LatticeOptions& lo);
// Box window [lo, hi) (same rim / rock rules).
Region extract_box(const VoxelGrid& g, const IVec3& lo, const IVec3& hi, const LatticeOptions& opt);
// An arbitrary set of voxels (same rim / rock rules; e.g. whole structures, see structures_of).
Region extract_set(const VoxelGrid& g, std::span<const IVec3> set, const LatticeOptions& opt);

// The structures touched by `seeds`: every non-anchored voxel connected to a non-anchored seed
// through intact bonds (anchored voxels are supports, not traversed), in search order. Stops
// after max_cells voxels, or at the edge of a partial grid (a snapshot) (*truncated set).
// `hydrate` (optional) is called for a chunk the walk needs that `g` (a snapshot) does not hold;
// true: it now does (e.g. regenerated from a streaming source), and the walk goes on.
std::vector<IVec3> structures_of(const VoxelGrid& g, std::span<const IVec3> seeds, i64 max_cells, bool* truncated,
                                 const std::function<bool(const IVec3& cc)>& hydrate = {});

// Detached islands on the world grid (voxels), seeded at voxels next to a change.
// Streamed worlds pass `coarse`, the chunks the grid does not hold: the search continues on
// their summaries (world/coarse.hpp), so a structure anchored beyond the resident chunks stays
// supported. An island that reaches into them is returned with its resident voxels only, and
// its non-resident chunks are added to *nonresident (sorted): the caller makes those chunks
// resident and searches again for the whole island.
std::vector<std::vector<IVec3>> detached_islands_grid(const VoxelGrid& g, std::span<const IVec3> seeds,
                                                      bool supports_removed, ConnStats* stats = nullptr,
                                                      CoarseWorld* coarse = nullptr,
                                                      std::vector<u64>* nonresident = nullptr);

}  // namespace svx
