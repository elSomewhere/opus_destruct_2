#include "svx/anim/damage/anatomy.hpp"
#include "svx/anim/voxel/sculpt.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace svx::anim {

namespace {

// Math.hypot as V8 computes it (the squares of the values over their largest, summed with Kahan's
// compensation): the original's distances to the last bit, so its shapes cover the same voxels.
template <int N>
f64 js_hypot(const f64 (&v)[N]) {
  f64 a[N];
  f64 max = 0.0;
  bool nan = false;
  for (int i = 0; i < N; ++i) {
    if (std::isnan(v[i])) {
      nan = true;
      a[i] = 0.0;
      continue;
    }
    a[i] = std::abs(v[i]);
    if (a[i] > max) max = a[i];
  }
  if (max == std::numeric_limits<f64>::infinity()) return max;
  if (nan) return std::numeric_limits<f64>::quiet_NaN();
  if (max == 0.0) return 0.0;
  f64 sum = 0.0, comp = 0.0;
  for (int i = 0; i < N; ++i) {
    const f64 n = a[i] / max;
    const f64 summand = n * n - comp;
    const f64 pre = sum + summand;
    comp = (pre - sum) - summand;
    sum = pre;
  }
  return std::sqrt(sum) * max;
}
inline f64 js_hypot3(f64 a, f64 b, f64 c) { return js_hypot<3>({a, b, c}); }
inline f64 js_hypot2(f64 a, f64 b) { return js_hypot<2>({a, b}); }

// Math.round: the nearest integer, halves up.
inline f64 js_round(f64 x) {
  const f64 r = std::ceil(x);
  return r - 0.5 <= x ? r : r - 1.0;
}

// A hash3 seed plus d, wrapped to 32 bits (the original's seeds are numbers, taken mod 2^32).
inline i32 seed_plus(i32 seed, i32 d) { return static_cast<i32>(static_cast<u32>(seed) + static_cast<u32>(d)); }

Shape make_box(const V3& c, const V3& h, f64 round, const Quat* q) {
  const f64 reach = js_hypot3(h.x, h.y, h.z);
  Shape out;
  if (q) {
    const Quat inv = conj(*q);
    out.sdf = [c, h, round, inv](f64 x, f64 y, f64 z) {
      const V3 p = rotate(inv, V3{x - c.x, y - c.y, z - c.z});
      const f64 qx = std::abs(p.x) - h.x + round;
      const f64 qy = std::abs(p.y) - h.y + round;
      const f64 qz = std::abs(p.z) - h.z + round;
      const f64 o = js_hypot3(std::max(qx, 0.0), std::max(qy, 0.0), std::max(qz, 0.0));
      return o + std::min(std::max(std::max(qx, qy), qz), 0.0) - round;
    };
    out.min = V3{c.x - reach, c.y - reach, c.z - reach};
    out.max = V3{c.x + reach, c.y + reach, c.z + reach};
  } else {
    out.sdf = [c, h, round](f64 x, f64 y, f64 z) {
      const f64 qx = std::abs(x - c.x) - h.x + round;
      const f64 qy = std::abs(y - c.y) - h.y + round;
      const f64 qz = std::abs(z - c.z) - h.z + round;
      const f64 o = js_hypot3(std::max(qx, 0.0), std::max(qy, 0.0), std::max(qz, 0.0));
      return o + std::min(std::max(std::max(qx, qy), qz), 0.0) - round;
    };
    out.min = V3{c.x - h.x, c.y - h.y, c.z - h.z};
    out.max = V3{c.x + h.x, c.y + h.y, c.z + h.z};
  }
  return out;
}

f64 dist_to_segment(f64 x, f64 y, f64 z, const V3& a, const V3& b) {
  const f64 bx = b.x - a.x, by = b.y - a.y, bz = b.z - a.z;
  const f64 px = x - a.x, py = y - a.y, pz = z - a.z;
  const f64 l2 = bx * bx + by * by + bz * bz;
  const f64 t = l2 > 0.0 ? std::min(1.0, std::max(0.0, (px * bx + py * by + pz * bz) / l2)) : 0.0;
  return js_hypot3(px - bx * t, py - by * t, pz - bz * t);
}

// The cells [a, b] of the extent [lo_f, hi_f] (cell units, lattice-relative) on an axis of n cells;
// false if it misses the lattice.
bool cell_range(f64 lo_f, f64 hi_f, i32 n, i32& a, i32& b) {
  if (std::isnan(lo_f) || std::isnan(hi_f)) return false;
  const f64 fa = std::max(0.0, lo_f), fb = std::min(static_cast<f64>(n - 1), hi_f);
  if (fa > fb) return false;
  a = static_cast<i32>(fa);
  b = static_cast<i32>(fb);
  return true;
}

}  // namespace

// ---- primitives

Shape sphere(const V3& c, f64 r) {
  return {[c, r](f64 x, f64 y, f64 z) { return js_hypot3(x - c.x, y - c.y, z - c.z) - r; }, V3{c.x - r, c.y - r, c.z - r},
          V3{c.x + r, c.y + r, c.z + r}};
}

Shape ellipsoid(const V3& c, const V3& r) {
  return {[c, r](f64 x, f64 y, f64 z) {
            const f64 px = (x - c.x) / r.x, py = (y - c.y) / r.y, pz = (z - c.z) / r.z;
            const f64 k0 = js_hypot3(px, py, pz);
            const f64 k1 = js_hypot3(px / r.x, py / r.y, pz / r.z);
            return k1 > 0.0 ? (k0 * (k0 - 1.0)) / k1 : -std::min(std::min(r.x, r.y), r.z);
          },
          V3{c.x - r.x, c.y - r.y, c.z - r.z}, V3{c.x + r.x, c.y + r.y, c.z + r.z}};
}

Shape capsule(const V3& a, const V3& b, f64 ra, f64 rb) {
  const V3 ba{b.x - a.x, b.y - a.y, b.z - a.z};
  const f64 l2 = ba.x * ba.x + ba.y * ba.y + ba.z * ba.z;
  const f64 r = std::max(ra, rb);
  return {[a, ba, l2, ra, rb](f64 x, f64 y, f64 z) {
            const f64 px = x - a.x, py = y - a.y, pz = z - a.z;
            const f64 t = l2 > 0.0 ? std::min(1.0, std::max(0.0, (px * ba.x + py * ba.y + pz * ba.z) / l2)) : 0.0;
            const f64 dx = px - ba.x * t, dy = py - ba.y * t, dz = pz - ba.z * t;
            return std::sqrt(dx * dx + dy * dy + dz * dz) - (ra + (rb - ra) * t);
          },
          V3{std::min(a.x, b.x) - r, std::min(a.y, b.y) - r, std::min(a.z, b.z) - r},
          V3{std::max(a.x, b.x) + r, std::max(a.y, b.y) + r, std::max(a.z, b.z) + r}};
}

Shape box(const V3& c, const V3& h, f64 round) { return make_box(c, h, round, nullptr); }
Shape box(const V3& c, const V3& h, f64 round, const Quat& q) { return make_box(c, h, round, &q); }

Shape cylinder(const V3& a, const V3& b, f64 r) {
  const V3 ba{b.x - a.x, b.y - a.y, b.z - a.z};
  const f64 l = js_hypot3(ba.x, ba.y, ba.z);
  const V3 n{ba.x / l, ba.y / l, ba.z / l};
  return {[a, n, l, r](f64 x, f64 y, f64 z) {
            const f64 px = x - a.x, py = y - a.y, pz = z - a.z;
            const f64 t = px * n.x + py * n.y + pz * n.z;
            const f64 rx = px - n.x * t, ry = py - n.y * t, rz = pz - n.z * t;
            const f64 dr = std::sqrt(rx * rx + ry * ry + rz * rz) - r;
            const f64 da = std::abs(t - l / 2.0) - l / 2.0;
            return std::min(std::max(dr, da), 0.0) + js_hypot2(std::max(dr, 0.0), std::max(da, 0.0));
          },
          V3{std::min(a.x, b.x) - r, std::min(a.y, b.y) - r, std::min(a.z, b.z) - r},
          V3{std::max(a.x, b.x) + r, std::max(a.y, b.y) + r, std::max(a.z, b.z) + r}};
}

// ---- combinators

Shape clip(const Shape& s, const V3& p, const V3& n) {
  return {[f = s.sdf, p, n](f64 x, f64 y, f64 z) { return std::max(f(x, y, z), -((x - p.x) * n.x + (y - p.y) * n.y + (z - p.z) * n.z)); },
          s.min, s.max};
}

Shape intersect(const Shape& a, const Shape& b) {
  return {[fa = a.sdf, fb = b.sdf](f64 x, f64 y, f64 z) { return std::max(fa(x, y, z), fb(x, y, z)); },
          V3{std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), std::max(a.min.z, b.min.z)},
          V3{std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), std::min(a.max.z, b.max.z)}};
}

Shape subtract(const Shape& a, const Shape& b) {
  return {[fa = a.sdf, fb = b.sdf](f64 x, f64 y, f64 z) { return std::max(fa(x, y, z), -fb(x, y, z)); }, a.min, a.max};
}

Shape smooth_union(std::vector<Shape> shapes, f64 k) {
  const f64 inf = std::numeric_limits<f64>::infinity();
  V3 lo{inf, inf, inf}, hi{-inf, -inf, -inf};
  std::vector<Sdf> fs;
  fs.reserve(shapes.size());
  for (Shape& s : shapes) {
    lo = V3{std::min(lo.x, s.min.x), std::min(lo.y, s.min.y), std::min(lo.z, s.min.z)};
    hi = V3{std::max(hi.x, s.max.x), std::max(hi.y, s.max.y), std::max(hi.z, s.max.z)};
    fs.push_back(std::move(s.sdf));
  }
  return {[fs = std::move(fs), k](f64 x, f64 y, f64 z) {
            if (fs.empty()) return std::numeric_limits<f64>::infinity();
            f64 d = fs[0](x, y, z);
            for (size_t i = 1; i < fs.size(); ++i) {
              const f64 b = fs[i](x, y, z);
              const f64 h = std::max(k - std::abs(d - b), 0.0) / k;
              d = std::min(d, b) - (h * h * k) / 4.0;
            }
            return d;
          },
          V3{lo.x - k, lo.y - k, lo.z - k}, V3{hi.x + k, hi.y + k, hi.z + k}};
}

Shape shell(const Shape& s, f64 t) {
  return {[f = s.sdf, t](f64 x, f64 y, f64 z) { return std::abs(f(x, y, z) + t / 2.0) - t / 2.0; }, s.min, s.max};
}

Shape inflate(const Shape& s, f64 d) {
  return {[f = s.sdf, d](f64 x, f64 y, f64 z) { return f(x, y, z) - d; }, V3{s.min.x - d, s.min.y - d, s.min.z - d},
          V3{s.max.x + d, s.max.y + d, s.max.z + d}};
}

Shape mirror_x(const Shape& s) {
  return {[f = s.sdf](f64 x, f64 y, f64 z) { return f(-x, y, z); }, V3{-s.max.x, s.min.y, s.min.z}, V3{-s.min.x, s.max.y, s.max.z}};
}

// ---- sculptor

Sculptor::Sculptor(SkeletonPtr sk, f64 s_, const V3& min, const V3& max, i32 seed) : skeleton(std::move(sk)), s(s_), seed_(seed) {
  for (int a = 0; a < 3; ++a) {
    lo[size_t(a)] = static_cast<i32>(std::floor(min[a] / s));
    dims[size_t(a)] = std::max(0, static_cast<i32>(std::ceil(max[a] / s)) - lo[size_t(a)]);
  }
  const size_t n = size_t(dims[0]) * size_t(dims[1]) * size_t(dims[2]);
  cells.assign(n, 0);
  bone.assign(n, 0);
  shade.assign(n, 0);
  organic.assign(n, 0);
}

template <class Fn>
void Sculptor::each(const Shape& shape, bool solid_only, Fn&& fn) const {
  const i32 nx = dims[0], ny = dims[1], nz = dims[2];
  i32 i0, i1, j0, j1, k0, k1;
  if (!cell_range(std::floor(shape.min.x / s) - lo[0], std::ceil(shape.max.x / s) - lo[0], nx, i0, i1)) return;
  if (!cell_range(std::floor(shape.min.y / s) - lo[1], std::ceil(shape.max.y / s) - lo[1], ny, j0, j1)) return;
  if (!cell_range(std::floor(shape.min.z / s) - lo[2], std::ceil(shape.max.z / s) - lo[2], nz, k0, k1)) return;
  const Sdf& sdf = shape.sdf;
  for (i32 k = k0; k <= k1; ++k) {
    const f64 z = (k + lo[2] + 0.5) * s;
    for (i32 j = j0; j <= j1; ++j) {
      const f64 y = (j + lo[1] + 0.5) * s;
      for (i32 i = i0; i <= i1; ++i) {
        const i32 idx = i + nx * (j + ny * k);
        if (solid_only && cells[size_t(idx)] == 0) continue;
        const f64 x = (i + lo[0] + 0.5) * s;
        if (sdf(x, y, z) <= 0.0) fn(idx, i + lo[0], j + lo[1], k + lo[2]);
      }
    }
  }
}

u8 Sculptor::shade_of(i32 i, i32 j, i32 k, f64 sh, f64 jitter) const {
  const f64 n = (hash3(i, j, k, seed_) + hash3(i >> 1, j >> 1, k >> 1, seed_plus(seed_, 1))) - 1.0;
  const f64 v = js_round(128.0 * sh * (1.0 + jitter * n));
  if (std::isnan(v)) return 0;  // (a typed array stores NaN as 0)
  return static_cast<u8>(std::max(1.0, std::min(255.0, v)));
}

Sculptor& Sculptor::add(const Shape& sh, i32 b, u8 slot, const AddOptions& opts) {
  const u8 owner = static_cast<u8>(b), org = opts.organic ? 1 : 0, c = static_cast<u8>(slot + 1);
  each(sh, false, [&](i32 idx, i32 i, i32 j, i32 k) {
    cells[size_t(idx)] = c;
    bone[size_t(idx)] = owner;
    organic[size_t(idx)] = org;
    shade[size_t(idx)] = shade_of(i, j, k, opts.shade, opts.jitter);
  });
  return *this;
}

Sculptor& Sculptor::paint(const Shape& sh, u8 slot, const PaintOptions& opts) {
  const u8 to = static_cast<u8>(slot + 1);
  each(sh, true, [&](i32 idx, i32 i, i32 j, i32 k) {
    const u8 c = cells[size_t(idx)];
    if (c == 0) return;
    if (!opts.only.empty() && std::find(opts.only.begin(), opts.only.end(), static_cast<u8>(c - 1)) == opts.only.end()) return;
    if (!opts.bones.empty() && std::find(opts.bones.begin(), opts.bones.end(), static_cast<i32>(bone[size_t(idx)])) == opts.bones.end()) return;
    cells[size_t(idx)] = to;
    shade[size_t(idx)] = shade_of(i, j, k, opts.shade, opts.jitter);
  });
  return *this;
}

Sculptor& Sculptor::paint_fn(const PaintFn& fn, f64 sh, f64 jitter) {
  const i32 nx = dims[0], ny = dims[1], nz = dims[2];
  for (i32 k = 0; k < nz; ++k)
    for (i32 j = 0; j < ny; ++j)
      for (i32 i = 0; i < nx; ++i) {
        const size_t idx = size_t(i + nx * (j + ny * k));
        const u8 c = cells[idx];
        if (c == 0) continue;
        const i32 gi = i + lo[0], gj = j + lo[1], gk = k + lo[2];
        const i32 r = fn((gi + 0.5) * s, (gj + 0.5) * s, (gk + 0.5) * s, static_cast<u8>(c - 1), gi, gj, gk);
        if (r < 0 || r == c - 1) continue;
        cells[idx] = static_cast<u8>(r + 1);
        shade[idx] = shade_of(gi, gj, gk, sh, jitter);
      }
  return *this;
}

Sculptor& Sculptor::carve(const Shape& sh) {
  each(sh, true, [&](i32 idx, i32, i32, i32) { cells[size_t(idx)] = 0; });
  return *this;
}

Sculptor& Sculptor::joint(const V3& c, f64 r, std::vector<i32> bones) {
  joints_.push_back(Joint{c, r, std::move(bones)});
  return *this;
}

ModelPtr Sculptor::finish(const FinishOptions& opts) {
  const i32 nx = dims[0], ny = dims[1], nz = dims[2];
  const i32 n = nx * ny * nz;
  // depth of organic cells: the 6-connected distance to the nearest empty cell (BFS)
  const i32 flesh_depth = opts.flesh_depth;
  std::vector<u8> depth(size_t(n), 255);
  std::vector<i32> queue(static_cast<size_t>(n));
  i32 qh = 0, qt = 0;
  for (i32 k = 0; k < nz; ++k)
    for (i32 j = 0; j < ny; ++j)
      for (i32 i = 0; i < nx; ++i) {
        const i32 idx = i + nx * (j + ny * k);
        if (cells[size_t(idx)] == 0) continue;
        const bool border = i == 0 || j == 0 || k == 0 || i == nx - 1 || j == ny - 1 || k == nz - 1;
        if (border || cells[size_t(idx - 1)] == 0 || cells[size_t(idx + 1)] == 0 || cells[size_t(idx - nx)] == 0 || cells[size_t(idx + nx)] == 0 ||
            cells[size_t(idx - nx * ny)] == 0 || cells[size_t(idx + nx * ny)] == 0) {
          depth[size_t(idx)] = 0;
          queue[size_t(qt++)] = idx;
        }
      }
  while (qh < qt) {
    const i32 idx = queue[size_t(qh++)];
    const i32 d = depth[size_t(idx)];
    if (d >= flesh_depth) continue;
    const i32 i = idx % nx, j = (idx / nx) % ny, k = idx / (nx * ny);
    const i32 nb[6] = {i > 0 ? idx - 1 : -1,       i < nx - 1 ? idx + 1 : -1,       j > 0 ? idx - nx : -1,
                       j < ny - 1 ? idx + nx : -1, k > 0 ? idx - nx * ny : -1, k < nz - 1 ? idx + nx * ny : -1};
    for (const i32 m : nb) {
      if (m < 0 || cells[size_t(m)] == 0 || depth[size_t(m)] <= d + 1 || qt >= n) continue;
      depth[size_t(m)] = static_cast<u8>(d + 1);
      queue[size_t(qt++)] = m;
    }
  }
  const Skeleton& sk = *skeleton;
  const f64 bone_r = opts.bone_radius;
  // joints keep their surface colour inside: bending a limb shows the joint's interior
  auto in_joint = [&](f64 x, f64 y, f64 z) {
    for (const Joint& jt : joints_)
      if (js_hypot3(x - jt.c.x, y - jt.c.y, z - jt.c.z) < jt.r + 1.5 * s) return true;
    return false;
  };
  for (i32 k = 0; k < nz; ++k)
    for (i32 j = 0; j < ny; ++j)
      for (i32 i = 0; i < nx; ++i) {
        const size_t idx = size_t(i + nx * (j + ny * k));
        if (cells[idx] == 0 || !organic[idx] || depth[idx] < flesh_depth) continue;
        const i32 b = bone[idx];
        if (b >= sk.count) continue;
        const f64 x = (i + lo[0] + 0.5) * s, y = (j + lo[1] + 0.5) * s, z = (k + lo[2] + 0.5) * s;
        if (in_joint(x, y, z)) continue;
        const f64 d = dist_to_segment(x, y, z, sk.rest_head[size_t(b)], sk.rest_tail[size_t(b)]);
        const bool is_bone = d < bone_r && sk.parents[size_t(b)] > 0;
        cells[idx] = static_cast<u8>((is_bone ? Slot::Bone : Slot::Flesh) + 1);
        shade[idx] = shade_of(i, j, k, is_bone ? 1.0 : 0.9 + 0.2 * hash3(i >> 1, j >> 1, k >> 1, 7), 0.08);
      }

  // cells per bone (owned, plus joint-ball copies)
  std::vector<std::vector<i32>> lists(size_t(sk.count));
  for (i32 idx = 0; idx < n; ++idx)
    if (cells[size_t(idx)] != 0 && bone[size_t(idx)] < sk.count) lists[bone[size_t(idx)]].push_back(idx);
  std::vector<u8> have(size_t(n), 0);
  std::vector<i32> inside;
  for (const Joint& jt : joints_) {
    inside.clear();
    each(sphere(jt.c, jt.r), true, [&](i32 idx, i32, i32, i32) { inside.push_back(idx); });
    for (const i32 b : jt.bones) {
      if (b < 0 || b >= sk.count) continue;
      std::vector<i32>& list = lists[size_t(b)];
      for (const i32 idx : list) have[size_t(idx)] = 1;
      for (const i32 idx : inside)
        if (!have[size_t(idx)]) list.push_back(idx);
      for (const i32 idx : list) have[size_t(idx)] = 0;
    }
  }
  // one part per bone with cells, on the box of its cells (shrink_part of the whole lattice)
  std::vector<VoxelPart> parts;
  for (i32 b = 0; b < sk.count; ++b) {
    const std::vector<i32>& list = lists[size_t(b)];
    if (list.empty()) continue;
    i32 x0 = nx, y0 = ny, z0 = nz, x1 = -1, y1 = -1, z1 = -1;
    for (const i32 idx : list) {
      const i32 x = idx % nx, y = (idx / nx) % ny, z = idx / (nx * ny);
      x0 = std::min(x0, x);
      y0 = std::min(y0, y);
      z0 = std::min(z0, z);
      x1 = std::max(x1, x);
      y1 = std::max(y1, y);
      z1 = std::max(z1, z);
    }
    VoxelPart part;
    part.bone = b;
    part.origin = {lo[0] + x0, lo[1] + y0, lo[2] + z0};
    part.dims = {x1 - x0 + 1, y1 - y0 + 1, z1 - z0 + 1};
    const size_t m = size_t(part.dims[0]) * size_t(part.dims[1]) * size_t(part.dims[2]);
    part.cells.assign(m, 0);
    part.shade.assign(m, 0);
    for (const i32 idx : list) {
      const i32 x = idx % nx - x0, y = (idx / nx) % ny - y0, z = idx / (nx * ny) - z0;
      const size_t dst = size_t(part.index(x, y, z));
      part.cells[dst] = cells[size_t(idx)];
      part.shade[dst] = shade[size_t(idx)];
    }
    part.count = part.initial_count = static_cast<i32>(list.size());
    part.version = 0;
    parts.push_back(std::move(part));
  }
  auto model = std::make_shared<VoxelModel>(skeleton, s, std::move(parts), opts.name);
  if (sk.count == 23) fill_interior(*model);
  for (auto& part : model->parts) part.version = 0;
  return model;
}

}  // namespace svx::anim
