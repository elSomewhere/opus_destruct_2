#include "svx/anim/physics/collision.hpp"

#include <algorithm>

namespace svx::anim {

std::optional<f64> VoxelCollision::ground_height(f64 x, f64 y, f64 z_top, f64 z_bottom) const {
  if (!std::isfinite(x + y + z_top + z_bottom) || z_top - z_bottom > 64.0) return std::nullopt;
  const i32 i = idx(x), j = idx(y), k0 = idx(z_top), k1 = idx(z_bottom);
  bool above = solid(i, j, k0 + 1);
  for (i32 k = k0; k >= k1; --k) {
    const bool s = solid(i, j, k);
    if (s && !above) return (k + 0.5) * h;
    above = s;
  }
  return std::nullopt;
}

bool VoxelCollision::sphere(const V3& c, f64 r, SphereContact* out) const {
  // (a runaway query must not scan the world)
  if (!(r < 4.0 * 1024.0 * h) || !std::isfinite(c.x + c.y + c.z) || r > 2.0) return false;
  const i32 i0 = idx(c.x - r), i1 = idx(c.x + r), j0 = idx(c.y - r), j1 = idx(c.y + r), k0 = idx(c.z - r), k1 = idx(c.z + r);
  f64 px = 0.0, py = 0.0, pz = 0.0;
  bool hit = false;
  f64 deepest = 0.0;
  V3 dn{0, 0, 1};
  for (i32 k = k0; k <= k1; ++k)
    for (i32 j = j0; j <= j1; ++j)
      for (i32 i = i0; i <= i1; ++i) {
        if (!solid(i, j, k)) continue;
        const f64 bx0 = (i - 0.5) * h, by0 = (j - 0.5) * h, bz0 = (k - 0.5) * h;
        const f64 qx = std::max(bx0, std::min(c.x, bx0 + h)), qy = std::max(by0, std::min(c.y, by0 + h)), qz = std::max(bz0, std::min(c.z, bz0 + h));
        V3 d{c.x - qx, c.y - qy, c.z - qz};
        const f64 dist = norm(d);
        f64 pen;
        if (dist > 1e-9) {
          if (dist >= r) continue;
          pen = r - dist;
          d = d * (1.0 / dist);
        } else {
          // (the centre inside the voxel: out through the nearest face whose neighbour is open)
          const f64 faces[6][4] = {{c.x - bx0, -1, 0, 0}, {bx0 + h - c.x, 1, 0, 0}, {c.y - by0, 0, -1, 0},
                                   {by0 + h - c.y, 0, 1, 0}, {c.z - bz0, 0, 0, -1}, {bz0 + h - c.z, 0, 0, 1}};
          f64 best = 1e300;
          d = V3{0, 0, 1};
          for (const auto& f : faces) {
            const bool open = !solid(i + static_cast<i32>(f[1]), j + static_cast<i32>(f[2]), k + static_cast<i32>(f[3]));
            const f64 cost = f[0] + (open ? 0.0 : h * 4.0);
            if (cost < best) {
              best = cost;
              d = V3{f[1], f[2], f[3]};
            }
          }
          // (buried - no open face at hand: out through the top, the way it came in)
          if (best >= h * 4.0) {
            for (i32 up = 1; up <= 12; ++up) {
              if (solid(i, j, k + up)) continue;
              best = (k + up - 0.5) * h - c.z;
              d = V3{0, 0, 1};
              break;
            }
          }
          pen = best + r;
        }
        hit = true;
        // (the largest push per axis direction: overlapping voxels share faces)
        const f64 ax = d.x * pen, ay = d.y * pen, az = d.z * pen;
        if (std::abs(ax) > std::abs(px)) px = ax;
        if (std::abs(ay) > std::abs(py)) py = ay;
        if (std::abs(az) > std::abs(pz)) pz = az;
        if (pen > deepest) {
          deepest = pen;
          dn = d;
        }
      }
  if (!hit) return false;
  // (along the dominant normal first: pushing out on every axis at once would throw a sphere on a
  // floor sideways at the voxels' edges)
  const f64 along = px * dn.x + py * dn.y + pz * dn.z;
  const f64 m = std::max(along, deepest);
  out->push = dn * m;
  out->normal = dn;
  return true;
}

f64 VoxelCollision::raycast(const V3& o, const V3& d, f64 max_dist) const {
  i32 i = idx(o.x), j = idx(o.y), k = idx(o.z);
  if (solid(i, j, k)) return 0.0;
  const i32 si = d.x > 0 ? 1 : -1, sj = d.y > 0 ? 1 : -1, sk = d.z > 0 ? 1 : -1;
  auto next = [&](f64 p, i32 v, i32 s) { return (v + s * 0.5) * h - p; };
  f64 tx = d.x != 0.0 ? next(o.x, i, si) / d.x : 1e300;
  f64 ty = d.y != 0.0 ? next(o.y, j, sj) / d.y : 1e300;
  f64 tz = d.z != 0.0 ? next(o.z, k, sk) / d.z : 1e300;
  const f64 dx = d.x != 0.0 ? h / std::abs(d.x) : 1e300, dy = d.y != 0.0 ? h / std::abs(d.y) : 1e300, dz = d.z != 0.0 ? h / std::abs(d.z) : 1e300;
  for (int n = 0; n < 4096; ++n) {
    f64 t;
    if (tx < ty && tx < tz) {
      t = tx;
      i += si;
      tx += dx;
    } else if (ty < tz) {
      t = ty;
      j += sj;
      ty += dy;
    } else {
      t = tz;
      k += sk;
      tz += dz;
    }
    if (t > max_dist) return -1.0;
    if (solid(i, j, k)) return t;
  }
  return -1.0;
}

}  // namespace svx::anim
