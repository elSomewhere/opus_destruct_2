// svx_city — axis-aligned rectangles with INCLUSIVE bounds {x0, y0, x1, y1} on the voxel grid
// (voxel_city core/rect.js). All building and interior planning works on these.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/js.hpp"

namespace svx::city {

struct Rect {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool operator==(const Rect& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};
struct Point2 {
  double x = 0, y = 0;
};

inline Rect rect(double x0, double y0, double x1, double y1) { return {x0, y0, x1, y1}; }
inline double rw(const Rect& r) { return r.x1 - r.x0 + 1; }
inline double rh(const Rect& r) { return r.y1 - r.y0 + 1; }
inline double r_area(const Rect& r) { return (r.x1 < r.x0 || r.y1 < r.y0) ? 0 : rw(r) * rh(r); }
inline bool r_valid(const Rect& r) { return r.x1 >= r.x0 && r.y1 >= r.y0; }
inline bool r_valid(const std::optional<Rect>& r) { return r && r_valid(*r); }
inline Point2 r_center(const Rect& r) { return {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2}; }
inline double r_min_side(const Rect& r) { return js::min(rw(r), rh(r)); }
inline double r_max_side(const Rect& r) { return js::max(rw(r), rh(r)); }
inline std::string r_key(const Rect& r) { return js::cat(r.x0, ",", r.y0, ",", r.x1, ",", r.y1); }

inline bool r_contains(const Rect& r, double x, double y) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; }
inline bool r_contains_rect(const Rect& outer, const Rect& inner) {
  return inner.x0 >= outer.x0 && inner.x1 <= outer.x1 && inner.y0 >= outer.y0 && inner.y1 <= outer.y1;
}
inline bool r_overlaps(const Rect& a, const Rect& b) { return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1; }
inline std::optional<Rect> r_intersect(const Rect& a, const Rect& b) {
  const Rect r{js::max(a.x0, b.x0), js::max(a.y0, b.y0), js::min(a.x1, b.x1), js::min(a.y1, b.y1)};
  if (r_valid(r)) return r;
  return std::nullopt;
}
inline Rect r_union_bounds(const Rect& a, const Rect& b) {
  return {js::min(a.x0, b.x0), js::min(a.y0, b.y0), js::max(a.x1, b.x1), js::max(a.y1, b.y1)};
}
inline std::optional<Rect> r_bounds_of(const std::vector<Rect>& rects) {
  std::optional<Rect> out;
  for (const Rect& r : rects) out = out ? r_union_bounds(*out, r) : r;
  return out;
}
inline Rect r_inset(const Rect& r, double d) { return {r.x0 + d, r.y0 + d, r.x1 - d, r.y1 - d}; }
// N = -y, S = +y, W = -x, E = +x
inline Rect r_inset_sides(const Rect& r, double n, double s, double e, double w) { return {r.x0 + w, r.y0 + n, r.x1 - e, r.y1 - s}; }
inline Rect r_translate(const Rect& r, double dx, double dy) { return {r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy}; }

// A minus B: up to 4 rects (not overlapping).
std::vector<Rect> r_subtract(const Rect& a, const Rect& b);
std::vector<Rect> r_subtract_all(const std::vector<Rect>& rects, const std::vector<Rect>& cutters);
// Split along an axis at `at`, leaving a wall line of `gap` cells between the halves. Axis 'x' is
// a vertical cut (it splits the x range).
std::pair<Rect, Rect> r_split(const Rect& r, char axis, double at, double gap = 0);

// Sides: 'N' (-y), 'E' (+x), 'S' (+y), 'W' (-x).
constexpr char kSides[4] = {'N', 'E', 'S', 'W'};
inline int side_dx(char s) { return s == 'E' ? 1 : s == 'W' ? -1 : 0; }
inline int side_dy(char s) { return s == 'S' ? 1 : s == 'N' ? -1 : 0; }
inline char opposite(char s) { return s == 'N' ? 'S' : s == 'S' ? 'N' : s == 'E' ? 'W' : 'E'; }
inline char side_axis(char s) { return (s == 'N' || s == 'S') ? 'y' : 'x'; }
inline int side_index(char s) { return s == 'N' ? 0 : s == 'E' ? 1 : s == 'S' ? 2 : 3; }

// The strip of r `depth` cells thick along a side, inside r / just outside it.
Rect r_edge_strip(const Rect& r, char side, double depth = 1);
Rect r_outer_strip(const Rect& r, char side, double depth = 1);
inline double r_side_length(const Rect& r, char side) { return (side == 'N' || side == 'S') ? rw(r) : rh(r); }

// The wall between two rects separated by a wall line `gap` thick (0: touching): orient 'v' (a
// wall along y, x0..x1 its columns) or 'h' (along x, y0..y1 its rows), t0..t1 its extent along
// the wall, and the side of a it lies on.
struct SharedWall {
  char orient = 'v';
  double x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // (orient 'v': x0, x1; 'h': y0, y1)
  double t0 = 0, t1 = 0;
  char side_of_a = 'E';
};
std::optional<SharedWall> r_shared_wall(const Rect& a, const Rect& b, double gap = 1);

}  // namespace svx::city
