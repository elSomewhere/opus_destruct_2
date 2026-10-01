// svx_city — blocks as convex polygons (voxel_city city/blockPoly.js, ANGLED_WORLD_PLAN.md S1).
//
// An angled street (a diagonal boulevard, a tilted old-town cut) splits a block along its centre
// line, so blocks of the angled world are convex polygons between road centre lines. Each edge
// carries the side of the road it follows ({cls, hr, id}, or no road). The rest of the engine
// keeps its rect blocks: a polygon block is recorded as its bounding rect with the sides that lie
// on it, plus one property half-plane per slanted edge (`cuts`), and lots planned in the rect are
// trimmed to those.
//
// Polygons are vertices {x, y, side} in order; the edge from vertex i to vertex i + 1 follows the
// side of vertex i.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/rect.hpp"

namespace svx::city {

// The road an edge follows: its class (null: no road), half right-of-way (voxels) and id (null:
// none). Default-constructed: NO_SIDE {cls: null, hr: 0, id: null}.
struct RoadSide {
  std::optional<std::string> cls = std::nullopt;
  double hr = 0;
  std::optional<std::string> id = std::nullopt;
};

struct PolyPt {
  double x = 0, y = 0;
  RoadSide side{};
};
using Poly = std::vector<PolyPt>;

// A rect block's sides (rectPoly's `sides`, polyBlock's `s`); an absent side is NO_SIDE.
struct BlockSides {
  RoadSide N{}, E{}, S{}, W{};
};

// A rect block {x0, y0, x1, y1} with sides {N, E, S, W} as a polygon.
Poly rect_poly(const Rect& r, const BlockSides& sides);
double poly_area(const Poly& poly);
Point2 poly_centroid(const Poly& poly);
// The bounds of the vertices (not rounded).
Rect poly_bounds(const Poly& poly);

// Splits a convex polygon by the line through a with direction (dx, dy): [left, right] (either
// may be null: less than 3 vertices, or an area of 1 or less), the new edge on both following
// `side`. Left: dx (y - ay) - dy (x - ax) < 0.
std::pair<std::optional<Poly>, std::optional<Poly>> split_poly(const Poly& poly, const Point2& a, double dx, double dy, const RoadSide& side);
// Does the line through a with direction (dx, dy) cross the polygon's interior?
bool line_crosses(const Poly& poly, const Point2& a, double dx, double dy);

// A property half-plane of a slanted edge: a point is on the block's side when
// nx x + ny y >= c (the road's right-of-way off); the road's class, half width and id.
struct BlockCut {
  double nx = 0, ny = 0, c = 0;
  std::optional<std::string> cls = std::nullopt;
  double hr = 0;
  std::optional<std::string> id = std::nullopt;
};
// A polygon block's record: `r` its bounding rect (whole voxels), `s` the sides of the polygon's
// edges that lie on that rect's edges (the others have no road), `cuts` the property half-planes
// of the slanted edges, and `poly`.
struct PolyBlock {
  Rect r{};
  BlockSides s{};
  std::vector<BlockCut> cuts{};
  Poly poly{};
};
PolyBlock poly_block(const Poly& poly);

// Is (x, y) on the block's side of all its cuts?
bool inside_cuts(const std::vector<BlockCut>& cuts, double x, double y);

// A rect (inclusive voxels) trimmed to lie on the block's side of every cut (voxel centres
// tested): the side facing each cut moves in, the way that keeps the most area; null when nothing
// is left. `trimmed` lists the rect sides that moved ('N', 'E', 'S', 'W'), with the cut's road.
struct TrimmedSide {
  char side = 'N';
  std::optional<std::string> cls = std::nullopt, id = std::nullopt;
};
struct TrimResult {
  Rect rect{};
  std::vector<TrimmedSide> trimmed{};
};
std::optional<TrimResult> trim_to_cuts(const Rect& rect, const std::vector<BlockCut>& cuts);

}  // namespace svx::city
