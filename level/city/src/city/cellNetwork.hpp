// svx_city — stage 1 of an arterial cell (voxel_city city/cellNetwork.js): its road network,
// sub-cells (districts) and blocks. It depends only on global line functions and macro fields,
// so any cell can be produced independently of its neighbours (World::cell_net, cached by cell).
//
// Ownership: a cell owns the arterial segments on its WEST and NORTH edges; the E / S edges'
// cross sections are made again (the same pure function, edge_info) for the blocks' insets.
//
// The network of a cell, in the order it is made: the W and N arterial edges a road class gives
// (rural roads wobble, a village's main street bends, some towns' main streets too), the pieces of
// the diagonal boulevards crossing it in a town (the angled world), the collectors that split a
// town cell into up to four sub-cells (a plain cross, a jog, a T or a single collector; a village
// centred in the cell gets its main street through the centre instead), then each sub-cell: its
// district (from the macro fields and the settlement's flavor; a rural sub-cell the town reaches
// into is laid out as the town's outskirts), its street pattern (city/streets), the diagonals
// splitting its blocks, the blocks kept (the town's edge frays; each block takes the district of
// its own place), the streets that front a kept block (trimmed to the stretch they serve) and
// service alleys splitting long urban blocks. Last, an old town's collectors are cobbled where it
// lies on both sides of them.
//
// Thread-safe and pure: a function of (world, i, j) only, the same in any call order and on any
// thread (docs/CITY.md §6).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "city/blockPoly.hpp"
#include "core/cache.hpp"
#include "core/rect.hpp"
#include "network/road.hpp"
#include "network/roadClasses.hpp"

namespace svx::city {

class World;
struct Flavor;
struct Settlement;

// edgeInfo(world, axis, line, span): the deterministic description of one arterial-grid edge -
// line `line` of axis `axis` (0: the vertical line x = fixed, 1: the horizontal y = fixed) between
// the cross lines `span` and `span + 1` (s0 .. s1).
struct EdgeInfo {
  double axis = 0, line = 0, span = 0;
  double fixed = 0, s0 = 0, s1 = 0;
  // The class of its road: arterial (or a flavor's / config.city.mainRoad), collector, village,
  // rural, ... "": null (no road).
  std::string cls;
  // roadSpecs(config)[cls]: null with no road (or a class the config has no spec of: undefined).
  std::optional<RoadSpec> spec;
  // The road's half right-of-way (0 without a road) and how far it bends off its line (the
  // blocks beside it keep that much further back).
  double hr = 0, wob = 0;
  // "cobble" (an old town's main street) or "": null.
  std::string paving;
  // The centre line: the two ends, or a wobbling country road's points.
  std::vector<RoadPt> pts;
  // The urbanization deciding its class: the larger of the middle's and the ends' mean.
  double u = 0;
};
EdgeInfo edge_info(const World& world, int axis, double line, double span);

// The four edges of a cell.
struct CellEdges {
  EdgeInfo W, E, N, S;
};

// A sub-cell: a rectangle between road centre lines (the arterial edges and the collectors) and
// its district.
struct SubCell {
  std::string id;  // `${cell}/s${n}`
  Rect rect{};     // centre lines (x1, y1: the far sides' lines)
  // The road on each side: an arterial edge's {cls (null: none), hr (its bend included), id null},
  // or a collector's {cls, hr, id}.
  BlockSides sides{};
  std::string district;  // ("sea": an island's sub-cell that is mostly sea)
  double u = 0, core = 0;  // the urban sample it was laid out by
  std::string settlement;  // that sample's settlement (its id), "": null
  // A sub-cell of open country on a town's edge, laid out like the town's outskirts there (its
  // blocks kept only where the town reaches). JS: true, or undefined.
  bool fringe = false;
};

// What a block's surface is (city/cellPlan: made on first use, the graded level of its ground).
struct BlockSurface;

// A block: between road centre lines, with the road on each side.
struct Block {
  std::string id;        // `${cell}/b${n}`
  std::string cell;      // the cell's id
  std::string sub;       // its sub-cell's id
  std::string district;  // its own district (the sub-cell's, or of its own place in the town)
  Rect rect{};           // centre lines (a polygon block: its bounding rect, whole voxels)
  // The road on each side (a polygon block: the sides of its edges lying on its rect's edges; the
  // others no road).
  BlockSides sides{};
  // The property rect (inclusive voxels): rect inset by each side's half right-of-way.
  Rect prop{};
  double u = 0, core = 0;  // the sub-cell's urban sample
  // The angled world: a block cut by a slanted street is its centre-line polygon, with the
  // property half-planes of its slanted edges (cuts: non-empty exactly when poly is set; JS
  // leaves both undefined on a rect block).
  std::optional<Poly> poly;
  std::vector<BlockCut> cuts;
  // (city/cellPlan caches the block's graded surface on it: JS block.levelAt)
  Lazy<std::shared_ptr<const BlockSurface>> level_at;
};

// The cell network of arterial cell (i, j).
struct CellNet {
  std::string id;  // `C${canon(i)}_${canon(j)}` (a wrapping world's laps of a cell share it)
  double i = 0, j = 0;
  Rect rect{};  // the centre-line rect: x0 .. x1 the arterial lines i and i + 1, y0 .. y1 j, j + 1
  CellEdges edges;
  // The roads it builds (its W and N edges, diagonals, collectors, local streets, alleys), in the
  // order they were made.
  std::vector<std::shared_ptr<const Road>> roads;
  std::vector<Block> blocks;
  std::vector<SubCell> subcells;
};

// planCellNetwork(world, i, j). (World::cell_net caches it.)
std::shared_ptr<const CellNet> plan_cell_network(const World& world, double i, double j);

// flavorOf(settlement) of a settlement record (null: none - "modern").
const Flavor& flavor_of_settlement(const Settlement* s);

}  // namespace svx::city
