// svx_city — voxel_city core/obb.js.
#include "core/obb.hpp"

namespace svx::city {

Rect obb_bounds(const Placement& p, const Rect& r) {
  double x0 = js::kInf, y0 = js::kInf, x1 = -js::kInf, y1 = -js::kInf;
  for (const XY& c : obb_corners(p, r)) {
    if (c[0] < x0) x0 = c[0];
    if (c[1] < y0) y0 = c[1];
    if (c[0] > x1) x1 = c[0];
    if (c[1] > y1) y1 = c[1];
  }
  return {std::floor(x0), std::floor(y0), std::ceil(x1) - 1, std::ceil(y1) - 1};
}

double obb_distance(const Placement& p, const Rect& r, double x, double y) {
  const XY uv = world_point_to_local(p, x + 0.5, y + 0.5);
  const double du = js::max(r.x0 - uv[0], 0.0, uv[0] - (r.x1 + 1));
  const double dv = js::max(r.y0 - uv[1], 0.0, uv[1] - (r.y1 + 1));
  return std::sqrt(du * du + dv * dv);
}

bool obb_contains(const Placement& p, const Rect& r, double x, double y, double pad) {
  const XY uv = world_point_to_local(p, x + 0.5, y + 0.5);
  return uv[0] >= r.x0 - pad && uv[0] <= r.x1 + 1 + pad && uv[1] >= r.y0 - pad && uv[1] <= r.y1 + 1 + pad;
}

namespace {
// The u range a convex polygon (local points, in order) spans at depth v, or none.
std::optional<XY> span_at(const std::vector<XY>& poly, double v) {
  double lo = js::kInf, hi = -js::kInf;
  const size_t n = poly.size();
  for (size_t k = 0; k < n; ++k) {
    const double au = poly[k][0], av = poly[k][1];
    const double bu = poly[(k + 1) % n][0], bv = poly[(k + 1) % n][1];
    if ((av - v) * (bv - v) > 0) continue;
    if (av == bv) {
      lo = js::min(lo, au, bu);
      hi = js::max(hi, au, bu);
      continue;
    }
    const double u = au + ((bu - au) * (v - av)) / (bv - av);
    lo = js::min(lo, u);
    hi = js::max(hi, u);
  }
  if (lo <= hi) return XY{lo, hi};
  return std::nullopt;
}
}  // namespace

std::optional<Rect> fit_local_rect(const Placement& p, const std::vector<XY>& poly, const FitOpts& o) {
  std::vector<XY> loc;
  loc.reserve(poly.size());
  for (const XY& q : poly) loc.push_back(world_point_to_local(p, q[0], q[1]));
  double v_min = js::kInf, v_max = -js::kInf;
  for (const XY& q : loc) {
    v_min = js::min(v_min, q[1]);
    v_max = js::max(v_max, q[1]);
  }
  bool have = false;
  Rect best{};
  double best_area = 0;
  for (double f = std::ceil(v_min); f <= std::ceil(v_min) + o.front_slack; f += 1) {
    const auto a = span_at(loc, f);
    if (!a) continue;
    for (double b = f + js::max(1.0, o.depth_min); b <= js::min(std::floor(v_max), f + o.depth_max); b += 1) {
      const auto s = span_at(loc, b);
      if (!s) break;
      double u0 = std::ceil(js::max((*a)[0], (*s)[0]));
      double u1 = std::floor(js::min((*a)[1], (*s)[1])) - 1;
      if (u1 - u0 + 1 < o.min_width) continue;
      if (u1 - u0 + 1 > o.max_width) {
        u0 += std::floor((u1 - u0 + 1 - o.max_width) / 2);
        u1 = u0 + o.max_width - 1;
      }
      const double area = (u1 - u0 + 1) * (b - f);
      if (!have || area > best_area) {
        have = true;
        best = {u0, f, u1, b - 1};
        best_area = area;
      }
    }
  }
  if (!have) return std::nullopt;
  return best;
}

std::vector<XY> clip_half_plane(const std::vector<XY>& poly, double nx, double ny, double c) {
  std::vector<XY> out;
  const size_t n = poly.size();
  for (size_t k = 0; k < n; ++k) {
    const XY& a = poly[k];
    const XY& b = poly[(k + 1) % n];
    const double da = nx * a[0] + ny * a[1] - c;
    const double db = nx * b[0] + ny * b[1] - c;
    if (da >= 0) out.push_back(a);
    if ((da >= 0) != (db >= 0)) {
      const double t = da / (da - db);
      out.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t});
    }
  }
  return out;
}

}  // namespace svx::city
