// svx_city — oriented rects (voxel_city core/obb.js, ANGLED_WORLD_PLAN.md S3): a local rect of a
// placement with only a yaw, seen from the world. The local rect {x0, y0, x1, y1} (inclusive
// cells, x = u, y = v) covers [x0, x1 + 1) x [y0, y1 + 1) of local space, which the placement's
// exact rotation puts at origin + M (u, v) / D. Distances and fits are plain floating point.
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/placement.hpp"
#include "core/rect.hpp"

namespace svx::city {

using XY = std::array<double, 2>;

inline XY local_point_to_world(const Placement& p, double u, double v) {
  return {p.origin.x + (p.m[0] * u + p.m[1] * v) / p.d, p.origin.y + (p.m[3] * u + p.m[4] * v) / p.d};
}
inline XY world_point_to_local(const Placement& p, double x, double y) {
  const double dx = x - p.origin.x, dy = y - p.origin.y;
  return {(p.m[0] * dx + p.m[3] * dy) / p.d, (p.m[1] * dx + p.m[4] * dy) / p.d};
}
// World corners of a local rect, in order round it.
inline std::array<XY, 4> obb_corners(const Placement& p, const Rect& r) {
  return {local_point_to_world(p, r.x0, r.y0), local_point_to_world(p, r.x1 + 1, r.y0), local_point_to_world(p, r.x1 + 1, r.y1 + 1),
          local_point_to_world(p, r.x0, r.y1 + 1)};
}
// Integer world bounds (inclusive voxels) of a local rect: every voxel it touches.
Rect obb_bounds(const Placement& p, const Rect& r);
// Distance (voxels) from the centre of world voxel (x, y) to a local rect, 0 inside.
double obb_distance(const Placement& p, const Rect& r, double x, double y);
// Whether the centre of world voxel (x, y) is inside a local rect grown by pad voxels.
bool obb_contains(const Placement& p, const Rect& r, double x, double y, double pad = 0);

struct FitOpts {
  double depth_min = 1, depth_max = js::kInf;
  double min_width = 1, max_width = js::kInf;
  double front_slack = 0;
};
// The largest local rect (integer cells) of a placement inside a convex world polygon, its front
// as near the polygon's front as it can be (fitLocalRect). Null if nothing fits.
std::optional<Rect> fit_local_rect(const Placement& p, const std::vector<XY>& poly, const FitOpts& o);
// A convex polygon clipped to the half-plane nx x + ny y >= c (Sutherland-Hodgman).
std::vector<XY> clip_half_plane(const std::vector<XY>& poly, double nx, double ny, double c);

}  // namespace svx::city
