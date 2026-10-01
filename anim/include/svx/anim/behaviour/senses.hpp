// svx_anim — what a body senses: its balance (centre of mass, its velocity, the capture point and
// the support under the feet) and what is around it (walls and ledges within reach, the ground).
//
// Balance follows the linear inverted pendulum: a body whose centre of mass c moves at v over
// ground a height h below stops over the capture point xi = c + v / w0 (w0 = sqrt(g / h)). With xi
// inside the support polygon (the planted feet, a braced hand) it can stand; outside, it has to
// step there, or fall.
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "svx/anim/physics/collision.hpp"

namespace svx::anim {

// A convex polygon on the ground (x, y), counter-clockwise.
class SupportPolygon {
 public:
  std::vector<f64> xs, ys;
  bool empty() const { return xs.empty(); }
  void clear() {
    xs.clear();
    ys.clear();
  }
  void hull(const std::vector<f64>& pts);  // the convex hull of points (x, y pairs)
  // Signed distance from (x, y) to the polygon's boundary (negative inside); `out` gets the
  // closest point inside.
  f64 distance(f64 x, f64 y, f64* out = nullptr) const;
  void centroid(f64* out) const;
};

// A surface within reach: a wall, a ledge, a table top.
struct Surface {
  V3 point, normal;  // the point touched (world) and the surface normal (towards the body)
  f64 dist = 0.0;    // horizontal distance from the probe origin
  bool top = false;  // a horizontal top (a table, a railing, a wall's top) rather than a wall face
};

// Probes of the surroundings: walls at chest and hip height in 12 directions, refreshed now and then.
class Surroundings {
 public:
  static constexpr int kDirs = 12;
  // nearest wall per direction at chest height and hip height (none: nothing within reach)
  std::array<std::optional<Surface>, kDirs> chest, hip;
  f64 age = 99.0;  // seconds since the last probe
  // Probes from a body standing at `feet` (ground point) with height scale k: rays out to `reach`
  // metres at chest and hip height.
  void probe(const CollisionWorld& world, const V3& feet, f64 k, f64 reach = 1.0);
  // The best wall to put a hand on: in direction `dir` (horizontal, unit) or within `spread`
  // radians of it, nearest first; at chest height (else hip height).
  std::optional<Surface> wall_toward(const V3& dir, f64 spread = 1.2, f64 max_dist = 1e300) const;
  std::optional<Surface> nearest(f64 max_dist = 1e300) const;  // in any direction (chest height, else hip)
};

}  // namespace svx::anim
