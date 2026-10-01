// svx_city — voxel_city core/rect.js.
#include "core/rect.hpp"

namespace svx::city {

std::vector<Rect> r_subtract(const Rect& a, const Rect& b) {
  const auto i = r_intersect(a, b);
  if (!i) return {a};
  std::vector<Rect> out;
  if (a.y0 < i->y0) out.push_back({a.x0, a.y0, a.x1, i->y0 - 1});
  if (i->y1 < a.y1) out.push_back({a.x0, i->y1 + 1, a.x1, a.y1});
  if (a.x0 < i->x0) out.push_back({a.x0, i->y0, i->x0 - 1, i->y1});
  if (i->x1 < a.x1) out.push_back({i->x1 + 1, i->y0, a.x1, i->y1});
  return out;
}

std::vector<Rect> r_subtract_all(const std::vector<Rect>& rects, const std::vector<Rect>& cutters) {
  std::vector<Rect> current = rects;
  for (const Rect& c : cutters) {
    std::vector<Rect> next;
    for (const Rect& r : current) {
      auto parts = r_subtract(r, c);
      next.insert(next.end(), parts.begin(), parts.end());
    }
    current = std::move(next);
  }
  return current;
}

std::pair<Rect, Rect> r_split(const Rect& r, char axis, double at, double gap) {
  if (axis == 'x') return {{r.x0, r.y0, at - 1, r.y1}, {at + gap, r.y0, r.x1, r.y1}};
  return {{r.x0, r.y0, r.x1, at - 1}, {r.x0, at + gap, r.x1, r.y1}};
}

Rect r_edge_strip(const Rect& r, char side, double depth) {
  switch (side) {
    case 'N': return {r.x0, r.y0, r.x1, r.y0 + depth - 1};
    case 'S': return {r.x0, r.y1 - depth + 1, r.x1, r.y1};
    case 'W': return {r.x0, r.y0, r.x0 + depth - 1, r.y1};
    default: return {r.x1 - depth + 1, r.y0, r.x1, r.y1};
  }
}

Rect r_outer_strip(const Rect& r, char side, double depth) {
  switch (side) {
    case 'N': return {r.x0, r.y0 - depth, r.x1, r.y0 - 1};
    case 'S': return {r.x0, r.y1 + 1, r.x1, r.y1 + depth};
    case 'W': return {r.x0 - depth, r.y0, r.x0 - 1, r.y1};
    default: return {r.x1 + 1, r.y0, r.x1 + depth, r.y1};
  }
}

std::optional<SharedWall> r_shared_wall(const Rect& a, const Rect& b, double gap) {
  if (b.x0 - a.x1 - 1 == gap) {
    const double y0 = js::max(a.y0, b.y0), y1 = js::min(a.y1, b.y1);
    if (y0 <= y1) {
      SharedWall w;
      w.orient = 'v', w.x0 = a.x1 + 1, w.x1 = b.x0 - 1, w.t0 = y0, w.t1 = y1, w.side_of_a = 'E';
      return w;
    }
  }
  if (a.x0 - b.x1 - 1 == gap) {
    const double y0 = js::max(a.y0, b.y0), y1 = js::min(a.y1, b.y1);
    if (y0 <= y1) {
      SharedWall w;
      w.orient = 'v', w.x0 = b.x1 + 1, w.x1 = a.x0 - 1, w.t0 = y0, w.t1 = y1, w.side_of_a = 'W';
      return w;
    }
  }
  if (b.y0 - a.y1 - 1 == gap) {
    const double x0 = js::max(a.x0, b.x0), x1 = js::min(a.x1, b.x1);
    if (x0 <= x1) {
      SharedWall w;
      w.orient = 'h', w.y0 = a.y1 + 1, w.y1 = b.y0 - 1, w.t0 = x0, w.t1 = x1, w.side_of_a = 'S';
      return w;
    }
  }
  if (a.y0 - b.y1 - 1 == gap) {
    const double x0 = js::max(a.x0, b.x0), x1 = js::min(a.x1, b.x1);
    if (x0 <= x1) {
      SharedWall w;
      w.orient = 'h', w.y0 = b.y1 + 1, w.y1 = a.y0 - 1, w.t0 = x0, w.t1 = x1, w.side_of_a = 'N';
      return w;
    }
  }
  return std::nullopt;
}

}  // namespace svx::city
