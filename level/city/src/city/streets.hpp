// svx_city — local street patterns (voxel_city city/streets.js). A pattern receives a sub-cell
// (a rectangle between road CENTRE LINES, with the cross section of the road on each side) and
// emits local roads plus the resulting blocks. Blocks keep, per side, the half right-of-way of
// the road they front, so the property line can be derived exactly.
//
//   emit.road(cls, ax, ay, bx, by[, paving]) -> the road's side {cls, hr, id}
//   emit.block(rect, sides[, poly])
//   emit.local(rect) -> the district of a part of the sub-cell (adaptive patterns; may be absent)
//
// Patterns: grid (rows and columns of blocks, jogging streets, merged pairs), subdivide (cut
// again and again near the target block size), organic (old-town cuts at irregular places, lanes
// between small blocks; in the angled world on convex polygons, now and then tilted by a small
// exact angle) and none (the sub-cell is one block).
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "city/blockPoly.hpp"
#include "city/districts.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"

namespace svx::city {

// A sub-cell as the patterns see it (city/cellNetwork's sub-cell record: they read its rect and
// sides only).
struct StreetSub {
  Rect rect{};
  BlockSides sides{};
};

// Where a pattern emits its roads and blocks (cellNetwork's `emit`).
struct StreetEmit {
  // emit.road(cls, ax, ay, bx, by, pav): a road from (ax, ay) to (bx, by) and its side. pav null:
  // JS's default (the sub-cell's paving); else the paving given ("": null).
  std::function<RoadSide(const std::string& cls, double ax, double ay, double bx, double by, const std::string* pav)> road;
  // emit.block(rect, sides, poly): a block, with its polygon in the angled world (null: a rect).
  std::function<void(const Rect& r, const BlockSides& s, const Poly* poly)> block;
  // emit.local(rect): the district of a part of the sub-cell, laid out at its own block size
  // (adaptive towns); empty: none.
  std::function<const District*(const Rect& r)> local;
};

// The patterns' options (JS opts: cellNetwork passes { angled: true, tilt } in the angled world,
// else undefined): organic cuts on polygons, `tilt` the chance a cut tilts.
struct StreetOpts {
  bool angled = false;
  double tilt = 0.35;
};

using StreetPattern = void (*)(const StreetSub& sub, const District& district, Rng& rng, StreetEmit& emit, const StreetOpts* opts);

void grid_streets(const StreetSub& sub, const District& district, Rng& rng, StreetEmit& emit, const StreetOpts* opts = nullptr);
void subdivide_streets(const StreetSub& sub, const District& district, Rng& rng, StreetEmit& emit, const StreetOpts* opts = nullptr);
void organic_streets(const StreetSub& sub, const District& district, Rng& rng, StreetEmit& emit, const StreetOpts* opts = nullptr);
void no_streets(const StreetSub& sub, const District& district, Rng& rng, StreetEmit& emit, const StreetOpts* opts = nullptr);

// STREET_PATTERNS[name] (null: JS undefined).
StreetPattern street_pattern(std::string_view name);

// PATTERN_FAMILY[pattern]: "fine" (grid, organic), "coarse" (subdivide), "none"; "" undefined. A
// block may change district within its family only.
std::string_view pattern_family(std::string_view pattern);

}  // namespace svx::city
