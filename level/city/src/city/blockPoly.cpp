// svx_city — voxel_city city/blockPoly.js.
#include "city/blockPoly.hpp"

namespace svx::city {

Poly rect_poly(const Rect& r, const BlockSides& sides) {
  return {{r.x0, r.y0, sides.N}, {r.x1, r.y0, sides.E}, {r.x1, r.y1, sides.S}, {r.x0, r.y1, sides.W}};
}

double poly_area(const Poly& poly) {
  double a = 0;
  const size_t n = poly.size();
  for (size_t k = 0; k < n; ++k) {
    const PolyPt& p = poly[k];
    const PolyPt& q = poly[(k + 1) % n];
    a += p.x * q.y - q.x * p.y;
  }
  return std::fabs(a) / 2;
}

Point2 poly_centroid(const Poly& poly) {
  double x = 0;
  double y = 0;
  for (const PolyPt& p : poly) {
    x += p.x;
    y += p.y;
  }
  const double n = static_cast<double>(poly.size());
  return {x / n, y / n};
}

Rect poly_bounds(const Poly& poly) {
  double x0 = js::kInf;
  double y0 = js::kInf;
  double x1 = -js::kInf;
  double y1 = -js::kInf;
  for (const PolyPt& p : poly) {
    x0 = js::min(x0, p.x);
    y0 = js::min(y0, p.y);
    x1 = js::max(x1, p.x);
    y1 = js::max(y1, p.y);
  }
  return {x0, y0, x1, y1};
}

std::pair<std::optional<Poly>, std::optional<Poly>> split_poly(const Poly& poly, const Point2& a, double dx, double dy, const RoadSide& side) {
  auto f = [&](const PolyPt& p) { return dx * (p.y - a.y) - dy * (p.x - a.x); };
  Poly left;
  Poly right;
  const size_t n = poly.size();
  for (size_t k = 0; k < n; ++k) {
    const PolyPt& p = poly[k];
    const PolyPt& q = poly[(k + 1) % n];
    const double fp = f(p);
    const double fq = f(q);
    if (fp <= 0) left.push_back(p);
    if (fp >= 0) right.push_back(p);
    if ((fp < 0 && fq > 0) || (fp > 0 && fq < 0)) {
      const double t = fp / (fp - fq);
      // (an axis-aligned cut crosses exactly on its own line: no stray 231.99999999999994)
      const double x = dx == 0 ? a.x : p.x + (q.x - p.x) * t;
      const double y = dy == 0 ? a.y : p.y + (q.y - p.y) * t;
      // the crossing continues p's edge on p's side, then runs along the cut
      if (fp < 0) {
        left.push_back({x, y, side});
        right.push_back({x, y, p.side});
      } else {
        right.push_back({x, y, side});
        left.push_back({x, y, p.side});
      }
    }
  }
  // two vertices on the line in a row: the edge between them is the cut
  const double eps = 1e-7 * (std::fabs(dx) + std::fabs(dy));
  auto close = [&](Poly& poly2) -> std::optional<Poly> {
    if (poly2.size() < 3) return std::nullopt;
    for (size_t k = 0; k < poly2.size(); ++k) {
      PolyPt& p = poly2[k];
      const PolyPt& q = poly2[(k + 1) % poly2.size()];
      if (std::fabs(f(p)) <= eps && std::fabs(f(q)) <= eps) p.side = side;
    }
    if (poly_area(poly2) > 1) return std::move(poly2);
    return std::nullopt;
  };
  std::optional<Poly> l = close(left);
  std::optional<Poly> r = close(right);
  return {std::move(l), std::move(r)};
}

bool line_crosses(const Poly& poly, const Point2& a, double dx, double dy) {
  bool neg = false;
  bool pos = false;
  for (const PolyPt& p : poly) {
    const double v = dx * (p.y - a.y) - dy * (p.x - a.x);
    if (v < -1e-9) neg = true;
    if (v > 1e-9) pos = true;
  }
  return neg && pos;
}

PolyBlock poly_block(const Poly& poly) {
  const Rect b = poly_bounds(poly);
  PolyBlock out;
  out.r = {std::floor(b.x0), std::floor(b.y0), std::ceil(b.x1), std::ceil(b.y1)};
  const Point2 c = poly_centroid(poly);
  const size_t n = poly.size();
  for (size_t k = 0; k < n; ++k) {
    const PolyPt& p = poly[k];
    const PolyPt& q = poly[(k + 1) % n];
    if (p.y == q.y && p.y == b.y0)
      out.s.N = p.side;
    else if (p.y == q.y && p.y == b.y1)
      out.s.S = p.side;
    else if (p.x == q.x && p.x == b.x0)
      out.s.W = p.side;
    else if (p.x == q.x && p.x == b.x1)
      out.s.E = p.side;
    else if (p.x != q.x && p.y != q.y) {
      // slanted: the inward unit normal, offset by the road's half right-of-way
      const double len = js::hypot(q.x - p.x, q.y - p.y);
      double nx = -(q.y - p.y) / len;
      double ny = (q.x - p.x) / len;
      if (nx * (c.x - p.x) + ny * (c.y - p.y) < 0) {
        nx = -nx;
        ny = -ny;
      }
      BlockCut cut;
      cut.nx = nx;
      cut.ny = ny;
      cut.c = nx * p.x + ny * p.y + p.side.hr;
      cut.cls = p.side.cls;
      cut.hr = p.side.hr;
      cut.id = p.side.id;
      out.cuts.push_back(std::move(cut));
    }
  }
  out.poly = poly;
  return out;
}

bool inside_cuts(const std::vector<BlockCut>& cuts, double x, double y) {
  for (const BlockCut& k : cuts)
    if (k.nx * x + k.ny * y < k.c) return false;
  return true;
}

std::optional<TrimResult> trim_to_cuts(const Rect& rect, const std::vector<BlockCut>& cuts) {
  Rect r = rect;
  std::vector<TrimmedSide> trimmed;
  for (const BlockCut& k : cuts) {
    auto ok = [&](double x, double y) { return k.nx * (x + 0.5) + k.ny * (y + 0.5) >= k.c; };
    if (ok(r.x0, r.y0) && ok(r.x1, r.y0) && ok(r.x0, r.y1) && ok(r.x1, r.y1)) continue;
    if (!ok(r.x0, r.y0) && !ok(r.x1, r.y0) && !ok(r.x0, r.y1) && !ok(r.x1, r.y1)) return std::nullopt;
    struct Opt {
      Rect q;
      char side;
    };
    std::vector<Opt> opts;
    // (a voxel column x is in when nx (x + 1/2) + ny (y + 1/2) >= c at the rect's worst y)
    if (k.nx != 0) {
      const double worst_y = k.ny > 0 ? r.y0 : r.y1;
      const double bound = (k.c - k.ny * (worst_y + 0.5)) / k.nx - 0.5;
      if (k.nx > 0)
        opts.push_back({{js::max(r.x0, std::ceil(bound)), r.y0, r.x1, r.y1}, 'W'});
      else
        opts.push_back({{r.x0, r.y0, js::min(r.x1, std::floor(bound)), r.y1}, 'E'});
    }
    if (k.ny != 0) {
      const double worst_x = k.nx > 0 ? r.x0 : r.x1;
      const double bound = (k.c - k.nx * (worst_x + 0.5)) / k.ny - 0.5;
      if (k.ny > 0)
        opts.push_back({{r.x0, js::max(r.y0, std::ceil(bound)), r.x1, r.y1}, 'N'});
      else
        opts.push_back({{r.x0, r.y0, r.x1, js::min(r.y1, std::floor(bound))}, 'S'});
    }
    auto area = [](const Rect& q) { return q.x1 >= q.x0 && q.y1 >= q.y0 ? (q.x1 - q.x0 + 1) * (q.y1 - q.y0 + 1) : 0.0; };
    // (opts.reduce: the first of the largest)
    const Opt* best = &opts[0];
    for (size_t i = 1; i < opts.size(); ++i)
      if (area(opts[i].q) > area(best->q)) best = &opts[i];
    if (area(best->q) == 0) return std::nullopt;
    trimmed.push_back({best->side, k.cls, k.id});
    r = best->q;
  }
  return TrimResult{r, std::move(trimmed)};
}

}  // namespace svx::city
