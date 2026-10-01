// svx_city — lots: the subdivision of a block (voxel_city city/lots.js), and the lot record.
//
// Every lot is an axis-aligned inclusive voxel rect inside the block's property rect. It records
// which of its sides face streets (and of what class), which side faces an alley, and its main
// frontage. A block is subdivided by its district's lot mode (planBlockLots):
//
//   downtown    the whole block (a third of the time, or a block under 60 m), halves across its
//               long axis (moved off the middle by up to 12 %), or quarters
//   perimeter   two rows of lots back to back along the long axis (one row by an alley or on a
//   suburban    block under 40 m / 50 m deep), frontage widths drawn from the district's range,
//               the corner lots a third wider
//   industrial  wide lots across the long axis, halved where the block is over 150 m deep
//   rural       farmsteads set back 30 m behind the roads on the block's road sides: a farmhouse
//               plot with a barn plot beside it (the rest of the block stays fields and nature)
//   village     house plots strung along the block's road sides (the village's own streets close
//               by, the country roads further back)
//   others      no lots (micro: the cell plan's superblocks; none: parks, the sea)
//
// The cell plan also asks for superblocks of freestanding slabs and point towers (microLots), wharf
// rows (rowLotsOf), the whole block (wholeBlockLot), freestanding lots (freeLot) and a lot's main
// frontage (mainFrontage).
//
// Pure functions of their arguments (the block, the district, the stream drawn from).
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "buildings/frame.hpp"
#include "city/blockPoly.hpp"
#include "city/cellNetwork.hpp"
#include "city/districts.hpp"
#include "core/hash.hpp"
#include "core/obb.hpp"
#include "core/rect.hpp"

namespace svx::city {

// A lot's frontage: a side of its rect on the block's property line that faces a street (not an
// alley), and that street's class.
struct Frontage {
  char side = 'S';  // 'N', 'S', 'W', 'E'
  std::string cls;  // the street's class (never null on a frontage)
  // cellPlan's fitLots: a side trimmed back to a slanted street fronts it; `slant` is the cut's
  // road id (JS: slant undefined on every other frontage - slanted false - and a cut's id may be
  // null).
  bool slanted = false;
  std::optional<std::string> slant = std::nullopt;
};

// What the cell plan caches on a block (city/cellPlan: the block's graded surface, JS levelAt).
struct BlockSurface;

// A lot. Every field is JS's of the same name in snake_case. lots.js makes the first group (and
// the extras of some lots); the cell plan copies a lot into its record and adds the rest (JS:
// `{ ...lot, groundZ, ... }`; a site's surface lots are made there too). A field JS leaves
// undefined on a lot is false, "", NaN or empty here, as noted.
struct Lot {
  // ---- lots.js (makeLot: every lot)
  // id: `${block}/l${k}` (planBlockLots, rowLotsOf, wholeBlockLot); "" while lots.js leaves it unset
  // (microLots, freeLot: the cell plan names those - `/l${k}`, `/c${k}`, `/c`).
  std::string id;
  Rect rect{};           // inclusive voxels, inside the block's property rect
  std::string block;     // its block's id (a site's lot: the site's id)
  std::string cell;      // its block's cell (`C${i}_${j}`)
  std::string district;  // its block's district
  // The sides of the rect on the block's property line that face a street, in the order N, S, W, E.
  std::vector<Frontage> frontages;
  // The main frontage (mainFrontage), else the side away from its alley, else S; an extra's front
  // where the lot has one (farmsteads, village plots, superblock slabs, free lots).
  char front = 'S';
  char alley = 0;       // the side on an alley ('\0': null; a site's lots leave it undefined)
  bool corner = false;  // two frontages or more

  // ---- extras of some lots (JS: undefined on the others)
  // whole: true on the whole block taken as one lot (a downtown block, wholeBlockLot); the cell
  // plan's fitLots sets it to the lot's rect as planned before it was trimmed (whole_rect). JS
  // reads `lot.whole ?? lot.rect`: whole_rect when set, else the boolean true when whole (whose
  // fields read undefined), else rect.
  bool whole = false;
  std::optional<Rect> whole_rect = std::nullopt;
  bool farmstead = false;  // a farmstead's plot (rural)
  std::string farm_role;   // "house" | "barn" (farmsteads)
  std::string farm;        // `f${n}`: the house and the barn of one farm share it
  bool village = false;    // a village house plot
  bool micro = false;      // a superblock's slab or tower (microLots)
  std::string arch;        // microLots: "panelSlab" | "panelTower"
  bool cabin = false;      // the cell plan's cabins in the woods (freeLot extra)
  bool chapel = false;     // a cemetery's chapel (freeLot extra)
  std::string civic;       // a civic building's lot: its id (freeLot extra)

  // ---- the cell plan's lot record (city/cellPlan.js lotRec; undefined until it makes one)
  double ground_z = js::kNaN;  // the level its building stands on (voxels)
  double u = js::kNaN, core = js::kNaN;  // its block's urban sample
  std::string building;        // its envelope's id ("": null - no building)
  std::shared_ptr<const BlockSurface> level_at;  // its block's graded surface (null: none)
  bool under_highway = false;  // under a highway's corridor (no building: parking under the deck)
  bool churchyard = false;     // a church's churchyard (the whole block)
  bool wharf = false;          // a wharf row's lot facing the harbour
  bool underground = false;    // a site's lot on a cavern floor (the surface above stays)
  std::string site;            // a site's lot: the site's id
  // The angled world's turned lots (cellPlan's turnedLot): the turn (with its U x V), the sidewalk
  // point in front of its middle (JS turn.fp), the block's cuts, the lot as planned cut to them
  // (its bounds are `rect`) and the slanted edge it fronts (`${block}/c${cut}`, "": none).
  std::optional<Turn> turn = std::nullopt;
  Point2 turn_fp{};
  std::vector<BlockCut> cuts;
  std::vector<XY> poly;
  std::string edge;
};

// mainFrontage(rect, frontages): the side of the highest-class street, the longer side preferred
// ('\0': none).
char main_frontage(const Rect& rect, const std::vector<Frontage>& frontages);

// planBlockLots(block, district, rng): the lots of a block by its district's lot mode, named
// `${block.id}/l${k}`. (The block's `prop` is the rect filled: the cell plan passes a block cleared
// of its streets, or its bounding rect - JS `{ ...block, prop }`.)
std::vector<Lot> plan_block_lots(const Block& block, const District& district, Rng& rng);

// rowLotsOf(block, rng, width): row lots of a frontage width range (m) on any block (the wharf's
// warehouses), two rows on a block 40 m deep; named `${block.id}/l${k}`.
std::vector<Lot> row_lots_of(const Block& block, Rng& rng, const std::array<double, 2>& width);

// microLots(block, rng): a superblock of freestanding slabs (microdistricts, projects): rows of
// long panel slabs parallel to the block's long side with courtyards between them, now and then a
// point tower; neighbouring rows' entrances face each other. Each lot is the building's footprint;
// the rest of the block is the shared courtyard (an open space of the cell plan). Unnamed.
std::vector<Lot> micro_lots(const Block& block, Rng& rng);

// freeLot(block, rect, front, rng, extra): a freestanding lot anywhere inside a block (a cabin in
// the woods, a cemetery's chapel, a civic building) facing `front`, no street frontage of its own
// required. Unnamed; the caller sets its extras (JS: extra's keys, spread before `front`).
Lot free_lot(const Block& block, const Rect& rect, char front);

// wholeBlockLot(block, rng): the whole block as one lot (schools and other civic buildings),
// named `${block.id}/l0`.
Lot whole_block_lot(const Block& block);

}  // namespace svx::city
