// svx_city — road views (voxel_city network/roadView.js): the polyline roads of a neighbourhood
// of cells flattened into straight segments with junction annotations. Junctions are found on the
// merged set, so roads owned by different cells still know about each other (crosswalks, stop
// lines, marking suppression and road levels need that).
//
// World::road_view(i, j) (defined here) is the view of the roads of cell (i, j) and its eight
// neighbours, cached by cell. A view is immutable once made, except for what the road levels
// cache on its segments and junctions (Lazy fields: network/roadLevel.cpp); its segments point at
// each other and into the view, so a view is made in place (make_shared) and never moves, and a
// pointer to a segment is good while the view is held.
//
// Identity: JS compares roads by object identity. Within a view each road is one object (a view
// gathers nine distinct cells), and the port compares pointers there. Across views (a road level
// asking another cell's view about a road) the reference's objects are the same only while its
// cell-network cache keeps them; the port compares ids there (same_road), which is what JS gives
// when nothing is evicted, whatever the cache sizes (docs/CITY.md §6).
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/cache.hpp"
#include "core/geom2d.hpp"
#include "core/rect.hpp"
#include "network/road.hpp"

namespace svx::city {

struct RoadSeg;

// A junction annotation (roadView.js junction()): segment s (the one whose jn holds it) meets
// segment c (other) at s's arc s and c's arc t_other.
struct RoadJunction {
  double s = 0;                  // arc along the owner segment, within its length
  double hc = 0, hr = 0;         // the crossing road's half carriageway and right-of-way
  const std::string* cls = nullptr;  // the crossing road's class
  double rank = 0;               // ... and its rank (roadClasses.js CLASS_RANK)
  bool c_ends = false;           // the crossing road ends here (a T into the owner)
  bool s_ends = false;           // the owner road ends here
  bool cw = false;               // crosswalks
  bool signal = false;           // a signalized junction
  const RoadSeg* other = nullptr;  // the crossing segment (road levels meet there)
  double t_other = 0;            // where it is crossed

  // (network/roadLevel.js, cached on the annotation: the junction's level and whether the owner
  // road dominates it; the blend back to the owner road's profile)
  struct Level {
    double z = 0;
    bool dom = false;
  };
  Lazy<Level> level;
  Lazy<double> blend;
};

// A junction along a road, as its segments share them (RoadView: road arc `at`).
struct RoadJunctionRef {
  const RoadJunction* j = nullptr;
  const RoadSeg* seg = nullptr;  // the segment whose jn holds it
  double at = 0;
};
// Every junction along one road of a view, in the order its segments found them; sorted by arc
// once the angled world's road levels ask (roadLevel.js `rj._sorted`).
struct RoadJunctions {
  std::vector<RoadJunctionRef> list;
  Lazy<std::vector<RoadJunctionRef>> sorted;
};

// A straight piece of a road (roadView.js buildSegments).
struct RoadSeg {
  RoadPtr road;
  double idx = 0;  // the index of its first point in road.pts
  double ax = 0, ay = 0, bx = 0, by = 0;
  double az = js::kNaN, bz = js::kNaN;  // (NaN: the points have none)
  double len = 0, dx = 0, dy = 0;       // length and unit direction
  double s0 = 0;                        // arc along the road where it starts
  double hc = 0, hr = 0, sidewalk = 0, parking = 0, median = 0, lanes = 0, lane = 0, shoulder = 0;
  const std::string* cls = nullptr;  // the road's class
  double rank = 0;
  bool first = false, last = false;  // the road's first / last piece
  std::vector<RoadJunction> jn;      // junctions, by arc
  Rect bbox;                         // its reach: right-of-way, corner radius and 2 voxels
  const RoadJunctions* rj = nullptr;  // every junction along its road (null: only its own, jn)

  // (network/roadLevel.js ownSeg: where the road's own segment of this index lies in its owner
  // cell's view - the angled world - cached as its index there, -1 none: views are remade alike)
  Lazy<int> own;
};

// Does a and b denote the same road (JS: the same object)? Within a view, pointers; across views,
// ids (see above).
inline bool same_road(const Road& a, const Road& b) { return &a == &b || a.id == b.id; }

// buildSegments(roads): the straight pieces of the roads, in order (degenerate pieces left out).
std::vector<RoadSeg> build_segments(const RoadList& roads);

class RoadView {
 public:
  explicit RoadView(RoadList roads);
  RoadView(const RoadView&) = delete;
  RoadView& operator=(const RoadView&) = delete;

  // the roads it was made of, in order
  RoadList roads;
  std::vector<RoadSeg> segs;
  // the longest reach of a segment: right-of-way, corner radius and 2 voxels (NaN where a road
  // has no corner radius: JS's Math.max)
  double max_reach = 0;

  // Segments whose bounds overlap rect (each once, in the grid's order).
  void near(const Rect& rect, std::vector<const RoadSeg*>& out) const;
  std::vector<const RoadSeg*> near(const Rect& rect) const;

 private:
  SpatialGrid<const RoadSeg*> grid_{128};
  std::vector<RoadJunctions> by_road_;
};

// annotateJunctions(segs, grid): every segment's junctions with the segments the grid finds round
// it (its jn, sorted by arc). The grid holds pointers into segs.
void annotate_junctions(std::vector<RoadSeg>& segs, const SpatialGrid<const RoadSeg*>& grid);

}  // namespace svx::city
