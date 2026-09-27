#include "svx/engine/debris.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace svx {

namespace {

using V3 = std::array<f64, 3>;
using Q = std::array<f64, 4>;

inline V3 vadd(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline V3 vmul(const V3& a, f64 s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline f64 dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 cross(const V3& a, const V3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline V3 rot(const Q& q, const V3& v) {
  const V3 u{q[0], q[1], q[2]};
  const V3 t = vmul(cross(u, v), 2.0);
  return vadd(vadd(v, vmul(t, q[3])), cross(u, t));
}
inline V3 rot_inv(const Q& q, const V3& v) { return rot({-q[0], -q[1], -q[2], q[3]}, v); }
inline V3 mat(const std::array<f64, 9>& M, const V3& v) {
  return {M[0] * v[0] + M[1] * v[1] + M[2] * v[2], M[3] * v[0] + M[4] * v[1] + M[5] * v[2],
          M[6] * v[0] + M[7] * v[1] + M[8] * v[2]};
}
// World inverse inertia applied to a world vector: R I^-1 R^T v.
inline V3 inv_inertia_world(const DebrisBody& b, const V3& v) { return rot(b.q, mat(b.inv_inertia, rot_inv(b.q, v))); }

bool inv3(const std::array<f64, 9>& A, std::array<f64, 9>& R) {
  const f64 det = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
  if (!(std::abs(det) > 1e-300)) return false;
  const f64 id = 1.0 / det;
  R = {(A[4] * A[8] - A[5] * A[7]) * id, (A[2] * A[7] - A[1] * A[8]) * id, (A[1] * A[5] - A[2] * A[4]) * id,
       (A[5] * A[6] - A[3] * A[8]) * id, (A[0] * A[8] - A[2] * A[6]) * id, (A[2] * A[3] - A[0] * A[5]) * id,
       (A[3] * A[7] - A[4] * A[6]) * id, (A[1] * A[6] - A[0] * A[7]) * id, (A[0] * A[4] - A[1] * A[3]) * id};
  return true;
}

struct Contact {
  V3 r, p;           // offset from the centre, world point
  int axis, sign;    // contact normal +-e_axis (into air)
  f64 depth;
  f64 kn, k1, k2;    // effective masses (normal, tangents)
  f64 bounce = 0.0;  // restitution target normal velocity
  f64 ln = 0.0, l1 = 0.0, l2 = 0.0;  // accumulated impulses
};

inline IVec3 voxel_at(const V3& p, f64 h) {
  return {static_cast<i32>(std::floor(p[0] / h + 0.5)), static_cast<i32>(std::floor(p[1] / h + 0.5)),
          static_cast<i32>(std::floor(p[2] / h + 0.5))};
}

}  // namespace

bool DebrisSystem::add(i64 id, const std::vector<IVec3>& voxels, const std::vector<f64>& masses, f64 h, const V3& v,
                       const V3& w, const DebrisParams& p) {
  if (voxels.empty() || masses.size() != voxels.size()) return false;
  DebrisBody b;
  b.id = id;
  V3 c{0, 0, 0};
  for (size_t k = 0; k < voxels.size(); ++k) {
    b.mass += masses[k];
    for (int q = 0; q < 3; ++q) c[q] += masses[k] * h * voxels[k][q];
  }
  if (!(b.mass > 0.0)) return false;
  c = vmul(c, 1.0 / b.mass);
  // inertia about the centre of mass: point masses plus each voxel's own cube inertia
  std::array<f64, 9> I{};
  for (size_t k = 0; k < voxels.size(); ++k) {
    const V3 r{h * voxels[k][0] - c[0], h * voxels[k][1] - c[1], h * voxels[k][2] - c[2]};
    const f64 m = masses[k];
    const f64 own = m * h * h / 6.0;
    const f64 rr = dot(r, r);
    I[0] += m * (rr - r[0] * r[0]) + own;
    I[4] += m * (rr - r[1] * r[1]) + own;
    I[8] += m * (rr - r[2] * r[2]) + own;
    I[1] -= m * r[0] * r[1];
    I[2] -= m * r[0] * r[2];
    I[5] -= m * r[1] * r[2];
  }
  I[3] = I[1];
  I[6] = I[2];
  I[7] = I[5];
  if (!inv3(I, b.inv_inertia)) return false;
  // samples: the corners (slightly inset) of voxels with a face towards a non-member
  std::unordered_set<u64> members;
  members.reserve(voxels.size() * 2);
  for (const IVec3& vx : voxels) members.insert(key3(vx[0], vx[1], vx[2]));
  std::vector<std::array<f32, 3>> pts;
  for (const IVec3& vx : voxels) {
    bool surface = false;
    for (int a = 0; a < 3 && !surface; ++a)
      for (int s = -1; s <= 1; s += 2) {
        IVec3 q = vx;
        q[a] += s;
        if (!members.count(key3(q[0], q[1], q[2]))) {
          surface = true;
          break;
        }
      }
    if (!surface) continue;
    for (int cz = -1; cz <= 1; cz += 2)
      for (int cy = -1; cy <= 1; cy += 2)
        for (int cx = -1; cx <= 1; cx += 2)
          pts.push_back({static_cast<f32>(h * (vx[0] + 0.45 * cx) - c[0]), static_cast<f32>(h * (vx[1] + 0.45 * cy) - c[1]),
                         static_cast<f32>(h * (vx[2] + 0.45 * cz) - c[2])});
  }
  std::sort(pts.begin(), pts.end());
  pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
  if (static_cast<int>(pts.size()) > p.max_points) {
    // deterministic subsample: the extreme point along each axis direction, then an even spread
    std::vector<std::array<f32, 3>> keep;
    for (int a = 0; a < 3; ++a) {
      auto lo = std::min_element(pts.begin(), pts.end(), [a](const auto& x, const auto& y) { return x[a] < y[a]; });
      auto hi = std::max_element(pts.begin(), pts.end(), [a](const auto& x, const auto& y) { return x[a] < y[a]; });
      keep.push_back(*lo);
      keep.push_back(*hi);
    }
    const size_t rest = static_cast<size_t>(std::max(0, p.max_points - 6));
    for (size_t k = 0; k < rest; ++k) keep.push_back(pts[k * pts.size() / rest]);
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    pts.swap(keep);
  }
  b.pts = std::move(pts);
  b.x0 = c;
  b.x = c;
  b.v = v;
  b.w = w;
  b.L = mat(I, w);
  b.fade_start = p.max_age;
  bodies_.push_back(std::move(b));
  return true;
}

void DebrisSystem::limit(int n, const DebrisParams& p) {
  int live = 0;
  for (auto it = bodies_.rbegin(); it != bodies_.rend(); ++it) {
    if (it->age >= it->fade_start) continue;
    if (++live > n) it->fade_start = std::min(it->fade_start, it->age);
  }
  // a collapse can shed hundreds of pieces at once: keep the cost bounded
  const size_t cap = static_cast<size_t>(std::max(1, p.hard_cap));
  if (bodies_.size() > cap) {
    const size_t drop = bodies_.size() - cap;
    for (size_t k = 0; k < drop; ++k) finished_.push_back(bodies_[k].id);
    bodies_.erase(bodies_.begin(), bodies_.begin() + static_cast<long>(drop));
  }
}

void DebrisSystem::step(f64 dt, const VoxelGrid& g, const DebrisParams& p, std::vector<DebrisImpact>* impacts) {
  const f64 h = g.h;
  const int ns = std::max(1, p.substeps);
  const f64 sdt = dt / ns;
  const f64 floor_z = h * g.lo[2] - p.kill_depth;
  std::vector<Contact> contacts;
  for (DebrisBody& b : bodies_) {
    b.age += dt;
    if (b.age >= b.fade_start) {
      b.fade = std::max(0.0, 1.0 - (b.age - b.fade_start) / p.fade_time);
      if (b.fade <= 0.0) {
        finished_.push_back(b.id);
        continue;
      }
    }
    if (b.x[2] < floor_z) {
      finished_.push_back(b.id);
      b.fade = 0.0;
      continue;
    }
    if (b.asleep) continue;
    f64 max_speed = 0.0;
    bool touched = false;
    for (int s = 0; s < ns; ++s) {
      b.v[2] -= 9.81 * sdt;
      b.x = vadd(b.x, vmul(b.v, sdt));
      b.w = inv_inertia_world(b, b.L);
      // q += dt/2 (w, 0) q, renormalized (no transcendental functions)
      const Q& q = b.q;
      const Q dq{b.w[0] * q[3] + b.w[1] * q[2] - b.w[2] * q[1], -b.w[0] * q[2] + b.w[1] * q[3] + b.w[2] * q[0],
                 b.w[0] * q[1] - b.w[1] * q[0] + b.w[2] * q[3], -b.w[0] * q[0] - b.w[1] * q[1] - b.w[2] * q[2]};
      Q nq{q[0] + 0.5 * sdt * dq[0], q[1] + 0.5 * sdt * dq[1], q[2] + 0.5 * sdt * dq[2], q[3] + 0.5 * sdt * dq[3]};
      const f64 nn = std::sqrt(nq[0] * nq[0] + nq[1] * nq[1] + nq[2] * nq[2] + nq[3] * nq[3]);
      for (auto& x : nq) x /= nn;
      b.q = nq;
      b.w = inv_inertia_world(b, b.L);
      // world inverse inertia R I^-1 R^T for this substep
      std::array<f64, 9> Iw{};
      for (int c = 0; c < 3; ++c) {
        V3 e{0, 0, 0};
        e[c] = 1.0;
        const V3 col = inv_inertia_world(b, e);
        for (int r = 0; r < 3; ++r) Iw[3 * r + c] = col[r];
      }
      // contact detection: samples inside solid voxels, with the face of least penetration
      // that leads to air (buried samples push up)
      contacts.clear();
      for (const auto& pb : b.pts) {
        const V3 r = rot(b.q, {pb[0], pb[1], pb[2]});
        const V3 pw = vadd(b.x, r);
        const IVec3 vx = voxel_at(pw, h);
        if (!vox_solid(g.get(vx))) continue;
        Contact c;
        c.r = r;
        c.p = pw;
        c.axis = 2;
        c.sign = 1;
        c.depth = -1.0;
        for (int a = 0; a < 3; ++a)
          for (int sg = -1; sg <= 1; sg += 2) {
            IVec3 nb = vx;
            nb[a] += sg;
            if (vox_solid(g.get(nb))) continue;
            const f64 face = h * (vx[a] + 0.5 * sg);
            const f64 d = sg > 0 ? face - pw[a] : pw[a] - face;
            if (d >= 0.0 && (c.depth < 0.0 || d < c.depth)) {
              c.depth = d;
              c.axis = a;
              c.sign = sg;
            }
          }
        if (c.depth < 0.0) c.depth = 0.5 * h;
        contacts.push_back(c);
      }
      if (!contacts.empty()) {
        touched = true;
        // projected Gauss-Seidel on accumulated impulses (normal >= 0, box friction)
        auto apply = [&](const V3& r, const V3& J) {
          b.v = vadd(b.v, vmul(J, 1.0 / b.mass));
          const V3 rj = cross(r, J);
          b.L = vadd(b.L, rj);
          b.w = vadd(b.w, mat(Iw, rj));
        };
        auto eff = [&](const V3& r, const V3& d) {
          const V3 rd = cross(r, d);
          return 1.0 / (1.0 / b.mass + dot(rd, mat(Iw, rd)));
        };
        f64 vin = 0.0;
        for (Contact& c : contacts) {
          V3 n{0, 0, 0}, t1{0, 0, 0}, t2{0, 0, 0};
          n[c.axis] = c.sign;
          t1[(c.axis + 1) % 3] = 1.0;
          t2[(c.axis + 2) % 3] = 1.0;
          c.kn = eff(c.r, n);
          c.k1 = eff(c.r, t1);
          c.k2 = eff(c.r, t2);
          const f64 vn = dot(vadd(b.v, cross(b.w, c.r)), n);
          vin = std::max(vin, -vn);
          c.bounce = vn < -1.0 ? -p.restitution * vn : 0.0;
        }
        for (int it = 0; it < p.iterations; ++it)
          for (Contact& c : contacts) {
            V3 n{0, 0, 0}, t1{0, 0, 0}, t2{0, 0, 0};
            n[c.axis] = c.sign;
            t1[(c.axis + 1) % 3] = 1.0;
            t2[(c.axis + 2) % 3] = 1.0;
            V3 vc = vadd(b.v, cross(b.w, c.r));
            const f64 ln = std::max(0.0, c.ln + c.kn * (c.bounce - dot(vc, n)));
            apply(c.r, vmul(n, ln - c.ln));
            c.ln = ln;
            vc = vadd(b.v, cross(b.w, c.r));
            const f64 lim = p.friction * c.ln;
            const f64 l1 = std::clamp(c.l1 - c.k1 * dot(vc, t1), -lim, lim);
            const f64 l2 = std::clamp(c.l2 - c.k2 * dot(vc, t2), -lim, lim);
            apply(c.r, vadd(vmul(t1, l1 - c.l1), vmul(t2, l2 - c.l2)));
            c.l1 = l1;
            c.l2 = l2;
          }
        // landing report: total impulse and its mean application point
        V3 Jsum{0, 0, 0}, Psum{0, 0, 0};
        f64 jw = 0.0;
        f64 dmax[6] = {0, 0, 0, 0, 0, 0};
        for (const Contact& c : contacts) {
          V3 J{0, 0, 0};
          J[c.axis] = c.sign * c.ln;
          J[(c.axis + 1) % 3] += c.l1;
          J[(c.axis + 2) % 3] += c.l2;
          Jsum = vadd(Jsum, J);
          Psum = vadd(Psum, vmul(c.p, c.ln));
          jw += c.ln;
          f64& d = dmax[2 * c.axis + (c.sign > 0 ? 1 : 0)];
          d = std::max(d, c.depth);
        }
        // position correction: the deepest penetration per face direction
        for (int a = 0; a < 3; ++a) {
          const f64 up = std::min(dmax[2 * a + 1], 0.5 * h), down = std::min(dmax[2 * a], 0.5 * h);
          b.x[a] += 0.8 * (up - down);
        }
        if (jw > 0.0) {
          if (!b.landed && vin > p.impact_speed && impacts) {
            DebrisImpact im;
            im.body = b.id;
            im.pos = vmul(Psum, 1.0 / jw);
            im.impulse = vmul(Jsum, -1.0);
            im.mass = b.mass;
            im.speed = vin;
            impacts->push_back(im);
          }
          b.landed = true;
        }
      }
      max_speed = std::max(max_speed, std::sqrt(dot(b.v, b.v)) + 0.5 * std::sqrt(dot(b.w, b.w)));
    }
    if (touched) {
      b.air_time = 0.0;
    } else {
      b.air_time += dt;
      if (b.air_time > p.relaunch_air_time) b.landed = false;
    }
    b.slow_ticks = (touched && max_speed < p.sleep_speed) ? b.slow_ticks + 1 : 0;
    if (b.slow_ticks >= p.sleep_ticks) {
      b.asleep = true;
      b.v = {0, 0, 0};
      b.w = {0, 0, 0};
      b.L = {0, 0, 0};
      b.fade_start = std::min(b.fade_start, b.age + p.fade_after);
    }
  }
}

std::vector<i64> DebrisSystem::take_finished() {
  std::vector<i64> out;
  out.swap(finished_);
  if (!out.empty()) {
    std::unordered_set<i64> done(out.begin(), out.end());
    bodies_.erase(std::remove_if(bodies_.begin(), bodies_.end(), [&](const DebrisBody& b) { return done.count(b.id) > 0; }),
                  bodies_.end());
  }
  return out;
}

}  // namespace svx
