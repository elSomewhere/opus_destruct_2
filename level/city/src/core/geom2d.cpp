// svx_city — voxel_city core/geom2d.js.
#include "core/geom2d.hpp"

namespace svx::city {

std::vector<PPoint> catmull_rom(const std::vector<PPoint>& points, int samples_per_span, bool closed) {
  std::vector<PPoint> out;
  const int n = static_cast<int>(points.size());
  if (n < 2) return points;
  auto get = [&](int i) -> const PPoint& {
    if (closed) return points[static_cast<size_t>(((i % n) + n) % n)];
    return points[static_cast<size_t>(std::max(0, std::min(n - 1, i)))];
  };
  const int spans = closed ? n : n - 1;
  for (int i = 0; i < spans; ++i) {
    const PPoint &p0 = get(i - 1), &p1 = get(i), &p2 = get(i + 1), &p3 = get(i + 2);
    for (int s = 0; s < samples_per_span; ++s) {
      const double t = static_cast<double>(s) / samples_per_span;
      const double t2 = t * t, t3 = t2 * t;
      auto f = [&](double a, double b, double c, double d) {
        return 0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
      };
      PPoint pt;
      pt.x = f(p0.x, p1.x, p2.x, p3.x);
      pt.y = f(p0.y, p1.y, p2.y, p3.y);
      if (p1.has_z()) {
        const double z0 = p0.has_z() ? p0.z : p1.z;
        const double z2 = p2.has_z() ? p2.z : p1.z;
        const double z3 = p3.has_z() ? p3.z : (p2.has_z() ? p2.z : p1.z);
        pt.z = f(z0, p1.z, z2, z3);
      }
      out.push_back(pt);
    }
  }
  if (!closed) out.push_back(points[static_cast<size_t>(n - 1)]);
  return out;
}

std::vector<double> polyline_lengths(const std::vector<PPoint>& points) {
  std::vector<double> acc{0.0};
  for (size_t i = 1; i < points.size(); ++i)
    acc.push_back(acc[i - 1] + js::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y));
  return acc;
}

PolyAt polyline_at(const std::vector<PPoint>& points, const std::vector<double>& lengths, double s) {
  const double total = lengths.back();
  const double sc = s < 0 ? 0 : s > total ? total : s;
  int lo = 0, hi = static_cast<int>(lengths.size()) - 1;
  while (hi - lo > 1) {
    const int mid = (lo + hi) >> 1;
    if (lengths[static_cast<size_t>(mid)] <= sc)
      lo = mid;
    else
      hi = mid;
  }
  const PPoint &a = points[static_cast<size_t>(lo)], &b = points[static_cast<size_t>(hi)];
  const double seg = js::or_(lengths[static_cast<size_t>(hi)] - lengths[static_cast<size_t>(lo)], 1.0);
  const double t = (sc - lengths[static_cast<size_t>(lo)]) / seg;
  PolyAt out;
  out.x = a.x + (b.x - a.x) * t;
  out.y = a.y + (b.y - a.y) * t;
  out.tx = (b.x - a.x) / seg;
  out.ty = (b.y - a.y) / seg;
  out.z = (a.has_z() && b.has_z()) ? a.z + (b.z - a.z) * t : js::kNaN;
  return out;
}

}  // namespace svx::city
