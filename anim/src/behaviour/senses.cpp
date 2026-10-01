#include "svx/anim/behaviour/senses.hpp"

#include <cmath>

namespace svx::anim {

void SupportPolygon::hull(const std::vector<f64>& pts) {
  clear();
  const size_t n = pts.size() / 2;
  if (n == 0) return;
  // gift wrapping (a handful of points)
  size_t start = 0;
  for (size_t i = 1; i < n; ++i)
    if (pts[2 * i] < pts[2 * start] || (pts[2 * i] == pts[2 * start] && pts[2 * i + 1] < pts[2 * start + 1])) start = i;
  size_t p = start;
  for (size_t guard = 0; guard <= n; ++guard) {
    xs.push_back(pts[2 * p]);
    ys.push_back(pts[2 * p + 1]);
    size_t q = (p + 1) % n;
    for (size_t r = 0; r < n; ++r) {
      const f64 cr = (pts[2 * q] - pts[2 * p]) * (pts[2 * r + 1] - pts[2 * p + 1]) - (pts[2 * q + 1] - pts[2 * p + 1]) * (pts[2 * r] - pts[2 * p]);
      if (cr < 0.0) q = r;
    }
    p = q;
    if (p == start) break;
  }
}

f64 SupportPolygon::distance(f64 x, f64 y, f64* out) const {
  const size_t n = xs.size();
  if (n == 0) {
    if (out) {
      out[0] = x;
      out[1] = y;
    }
    return 1e300;
  }
  if (n == 1) {
    if (out) {
      out[0] = xs[0];
      out[1] = ys[0];
    }
    return hypot2(x - xs[0], y - ys[0]);
  }
  bool inside = n >= 3;
  f64 best = 1e300, bx = x, by = y;
  for (size_t i = 0; i < n; ++i) {
    const f64 ax = xs[i], ay = ys[i];
    const f64 cx = xs[(i + 1) % n], cy = ys[(i + 1) % n];
    const f64 ex = cx - ax, ey = cy - ay;
    // (counter-clockwise: inside is to the left of every edge)
    if (ex * (y - ay) - ey * (x - ax) < 0.0) inside = false;
    const f64 l2 = ex * ex + ey * ey;
    const f64 t = l2 > 0.0 ? clamp(((x - ax) * ex + (y - ay) * ey) / l2, 0.0, 1.0) : 0.0;
    const f64 px = ax + ex * t, py = ay + ey * t;
    const f64 d = hypot2(x - px, y - py);
    if (d < best) {
      best = d;
      bx = px;
      by = py;
    }
  }
  if (out) {
    out[0] = inside ? x : bx;
    out[1] = inside ? y : by;
  }
  return inside ? -best : best;
}

void SupportPolygon::centroid(f64* out) const {
  const size_t n = xs.size();
  f64 x = 0.0, y = 0.0;
  for (size_t i = 0; i < n; ++i) {
    x += xs[i];
    y += ys[i];
  }
  out[0] = n > 0 ? x / static_cast<f64>(n) : 0.0;
  out[1] = n > 0 ? y / static_cast<f64>(n) : 0.0;
}

void Surroundings::probe(const CollisionWorld& world, const V3& feet, f64 k, f64 reach) {
  age = 0.0;
  SphereContact contact;
  for (int level = 0; level < 2; ++level) {
    auto& list = level == 0 ? chest : hip;
    const f64 h = level == 0 ? 1.3 : 0.95;
    const V3 o{feet.x, feet.y, feet.z + h * k};
    for (int i = 0; i < kDirs; ++i) {
      const f64 a = (static_cast<f64>(i) / kDirs) * kPi * 2.0;
      const V3 d{cos(a), sin(a), 0.0};
      const f64 t = world.raycast(o, d, reach * k);
      if (t < 0.0) {
        list[size_t(i)].reset();
        continue;
      }
      const V3 p{o.x + d.x * t, o.y + d.y * t, o.z};
      // the normal: push a small sphere just in front of the hit out of the wall
      V3 n{-d.x, -d.y, 0.0};
      const V3 c{p.x - d.x * 0.02, p.y - d.y * 0.02, p.z};
      if (world.sphere(c, 0.05, &contact)) {
        const V3& m = contact.normal;
        const f64 l = hypot2(m.x, m.y);
        if (l > 0.5) n = V3{m.x / l, m.y / l, 0.0};
      }
      list[size_t(i)] = Surface{p, n, t, false};
    }
  }
}

std::optional<Surface> Surroundings::wall_toward(const V3& dir, f64 spread, f64 max_dist) const {
  std::optional<Surface> best;
  f64 score = 1e300;
  const f64 heading = atan2(dir.y, dir.x);
  for (int level = 0; level < 2; ++level) {
    const auto& list = level == 0 ? chest : hip;
    for (int i = 0; i < kDirs; ++i) {
      const std::optional<Surface>& s = list[size_t(i)];
      if (!s || s->dist > max_dist) continue;
      const f64 a = (static_cast<f64>(i) / kDirs) * kPi * 2.0;
      f64 da = std::fmod(std::abs(a - heading), kPi * 2.0);
      if (da > kPi) da = kPi * 2.0 - da;
      if (da > spread) continue;
      const f64 sc = s->dist + da * 0.35 + (level == 1 ? 0.15 : 0.0);
      if (sc < score) {
        score = sc;
        best = s;
      }
    }
    if (best) break;
  }
  return best;
}

std::optional<Surface> Surroundings::nearest(f64 max_dist) const {
  std::optional<Surface> best;
  for (int level = 0; level < 2; ++level) {
    const auto& list = level == 0 ? chest : hip;
    for (const std::optional<Surface>& s : list)
      if (s && s->dist <= max_dist && (!best || s->dist < best->dist)) best = s;
    if (best) return best;
  }
  return best;
}

}  // namespace svx::anim
