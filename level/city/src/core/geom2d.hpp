// svx_city — continuous 2D geometry (voxel_city core/geom2d.js): points are (x, y) in voxels.
#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/js.hpp"
#include "core/rect.hpp"

namespace svx::city {

// projectToSegment: t (0..1, clamped), raw_t, dist, side (the signed lateral offset, + left of
// a->b), along (the distance from a along it, unclamped), len, and the closest point (cx, cy).
struct SegProj {
  double t, raw_t, dist, side, along, len, cx, cy;
};
inline SegProj project_to_segment(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay;
  const double len2 = dx * dx + dy * dy;
  const double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
  const double tc = t < 0 ? 0 : t > 1 ? 1 : t;
  const double cx = ax + dx * tc, cy = ay + dy * tc;
  const double len = std::sqrt(len2);
  const double side = len > 0 ? ((px - ax) * -dy + (py - ay) * dx) / len : 0;
  return {tc, t, js::hypot(px - cx, py - cy), side, t * len, len, cx, cy};
}
inline double dist_to_segment(double px, double py, double ax, double ay, double bx, double by) {
  return project_to_segment(px, py, ax, ay, bx, by).dist;
}

// A polyline point; z NaN: none (JS: undefined).
struct PPoint {
  double x = 0, y = 0, z = js::kNaN;
  bool has_z() const { return z == z; }
};
// A uniform Catmull-Rom spline through control points, samples_per_span samples a span.
std::vector<PPoint> catmull_rom(const std::vector<PPoint>& points, int samples_per_span = 8, bool closed = false);
// Cumulative arc lengths of a polyline (x, y).
std::vector<double> polyline_lengths(const std::vector<PPoint>& points);
// The point and the unit tangent at arc distance s.
struct PolyAt {
  double x, y, tx, ty, z;  // (z NaN where an end has none)
};
PolyAt polyline_at(const std::vector<PPoint>& points, const std::vector<double>& lengths, double s);

// The integer bounds of points (x, y) padded by pad.
template <class P>
Rect points_bounds(const std::vector<P>& points, double pad = 0) {
  double x0 = js::kInf, y0 = js::kInf, x1 = -js::kInf, y1 = -js::kInf;
  for (const P& p : points) {
    if (p.x < x0) x0 = p.x;
    if (p.y < y0) y0 = p.y;
    if (p.x > x1) x1 = p.x;
    if (p.y > y1) y1 = p.y;
  }
  return {std::floor(x0 - pad), std::floor(y0 - pad), std::ceil(x1 + pad), std::ceil(y1 + pad)};
}

// A uniform grid over items with inclusive bounds (SpatialGrid). Queries return items in the order
// JS's does (cells row by row, each cell's items in insertion order, each item once); they do not
// change the grid, so any number of threads may query one at once.
template <class T>
class SpatialGrid {
 public:
  explicit SpatialGrid(double cell_size = 256) : cs_(cell_size) {}
  void insert(const T& item, const Rect& b) {
    const double cx0 = std::floor(b.x0 / cs_), cy0 = std::floor(b.y0 / cs_);
    const double cx1 = std::floor(b.x1 / cs_), cy1 = std::floor(b.y1 / cs_);
    const uint32_t e = static_cast<uint32_t>(items_.size());
    items_.push_back({item, b});
    for (double cy = cy0; cy <= cy1; cy += 1)
      for (double cx = cx0; cx <= cx1; cx += 1) cells_[key(cx, cy)].push_back(e);
  }
  // Items whose bounds overlap q.
  void query(const Rect& q, std::vector<T>& out) const {
    const double cx0 = std::floor(q.x0 / cs_), cy0 = std::floor(q.y0 / cs_);
    const double cx1 = std::floor(q.x1 / cs_), cy1 = std::floor(q.y1 / cs_);
    std::vector<uint32_t> seen;
    std::unordered_set<uint32_t> seen_set;
    for (double cy = cy0; cy <= cy1; cy += 1)
      for (double cx = cx0; cx <= cx1; cx += 1) {
        auto it = cells_.find(key(cx, cy));
        if (it == cells_.end()) continue;
        for (uint32_t e : it->second) {
          if (seen.size() < 48) {
            if (std::find(seen.begin(), seen.end(), e) != seen.end()) continue;
            seen.push_back(e);
            if (seen.size() == 48) seen_set.insert(seen.begin(), seen.end());
          } else if (!seen_set.insert(e).second) {
            continue;
          }
          const Rect& b = items_[e].b;
          if (b.x0 <= q.x1 && q.x0 <= b.x1 && b.y0 <= q.y1 && q.y0 <= b.y1) out.push_back(items_[e].item);
        }
      }
  }
  std::vector<T> query(const Rect& q) const {
    std::vector<T> out;
    query(q, out);
    return out;
  }
  void query_point(double x, double y, std::vector<T>& out) const {
    auto it = cells_.find(key(std::floor(x / cs_), std::floor(y / cs_)));
    if (it == cells_.end()) return;
    for (uint32_t e : it->second) {
      const Rect& b = items_[e].b;
      if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) out.push_back(items_[e].item);
    }
  }
  std::vector<T> query_point(double x, double y) const {
    std::vector<T> out;
    query_point(x, y, out);
    return out;
  }
  size_t size() const { return items_.size(); }
  const T& item(size_t i) const { return items_[i].item; }

 private:
  struct Entry {
    T item;
    Rect b;
  };
  // (JS: cx * 73856093 + cy * 19349663, a number: two cells of the same key share a list)
  static int64_t key(double cx, double cy) {
    return static_cast<int64_t>(cx) * 73856093 + static_cast<int64_t>(cy) * 19349663;
  }
  double cs_;
  std::vector<Entry> items_;
  std::unordered_map<int64_t, std::vector<uint32_t>> cells_;
};

}  // namespace svx::city
