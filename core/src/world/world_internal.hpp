// structvox v2 — engine internals shared by engine*.cpp (not part of the public API).
#pragma once

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "svx/world/world.hpp"

namespace svx {

namespace world_detail {

inline u64 mix64(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
inline f64 unit01(u64 h) { return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0); }
inline u64 frag_ident(u64 chunk, i32 first) { return mix64(chunk * 0x9E3779B97F4A7C15ull ^ static_cast<u64>(first)); }
inline u64 bond_ident(u64 a, u64 b, int axis, int sign) {
  return mix64(a * 0xD6E8FEB86659FD93ull ^ mix64(b + static_cast<u64>(axis * 2 + (sign > 0 ? 1 : 0))));
}
inline bool finite3(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline IVec3 local_of(int i) { return {i / (kChunk * kChunk), (i / kChunk) % kChunk, i % kChunk}; }
inline IVec3 voxel_of(const V3& p, f64 h) {
  return {static_cast<i32>(std::floor(p.x / h + 0.5)), static_cast<i32>(std::floor(p.y / h + 0.5)),
          static_cast<i32>(std::floor(p.z / h + 0.5))};
}

// Design strength classes: 1, 1.5, 2, 3, 4, 6, 8, ... x 1024 (class 20).
constexpr int kStrengthClasses = 21;
constexpr i32 kExisting = 1 << 30;  // (patch accumulators: endpoints >= this are existing nodes)
inline f64 class_mult(u8 c) {
  if (c == 0) return 1.0;
  const int k = std::min<int>(c, kStrengthClasses - 1);
  f64 m = static_cast<f64>(1u << (k / 2));
  if (k % 2) m *= 1.5;
  return m;
}
inline u8 class_for(f64 m) {
  for (int c = 0; c < kStrengthClasses; ++c)
    if (class_mult(static_cast<u8>(c)) >= m) return static_cast<u8>(c);
  return kStrengthClasses - 1;
}

// Accumulator of one bond: the voxel faces two fragments share (any axis), or the faces of one
// fragment on a support along one axis; or the junction samples between fragments of two grids
// (a fragment and a support of another grid). The bond runs along the line between the two
// centres (a support: along the face normal); its section is the faces projected onto the plane
// normal to that line (rigid-body-spring network).
inline void frame_of(const V3& n, V3* t1, V3* t2) {
  const f64 ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
  const V3 e = (ax <= ay && ax <= az) ? V3{1, 0, 0} : (ay <= az ? V3{0, 1, 0} : V3{0, 0, 1});
  *t1 = normalized(cross(n, e));
  *t2 = cross(n, *t1);
}

// A junction sample (a bond between two grids, docs/GRIDS.md): sample `sub` of face `face`
// (axis * 2 + 1 for the +axis face) of voxel v of lattice vg lands in voxel o of lattice og (a
// world's grids, or a body's shapes). kind 1: in the unknown world (a world grid chunk that is not
// resident: it holds what reaches into it).
struct JSample {
  IVec3 v{0, 0, 0}, o{0, 0, 0};
  u16 vg = 0, og = 0;
  u8 face = 0, sub = 0, kind = 0;
};
constexpr u8 kJunctionUnknown = 1;
// A sample's share of its face's (h / S)^2. An interface is measured once, by the faces of the
// newer of the two grids (the one added later, which owns the space both have voxels in): where
// the grids meet, it has its faces at or inside the older one's solid, flush or cast in, whatever
// the angle between them. So every sample counts in full.
inline f32 junction_weight(const JSample&) { return 1.0f; }

inline V3 face_normal(int face) {
  V3 n;
  n[face >> 1] = (face & 1) ? 1.0 : -1.0;
  return n;
}
// Where a junction sample of a face is, in its lattice (metres), pushed out of the face by push
// (m): S x S samples at the centres of an even subdivision of the face.
inline V3 junction_point(const IVec3& v, int face, int sub, i32 S, f64 h, f64 push) {
  const int a = face >> 1, a1 = (a + 1) % 3, a2 = (a + 2) % 3;
  const f64 sg = (face & 1) ? 1.0 : -1.0;
  const int i = sub / S, j = sub % S;
  V3 s{h * v[0], h * v[1], h * v[2]};
  s[a] += sg * (0.5 * h + push);
  s[a1] += ((i + 0.5) / static_cast<f64>(S) - 0.5) * h;
  s[a2] += ((j + 0.5) / static_cast<f64>(S) - 0.5) * h;
  return s;
}

struct SecAcc {
  i32 a = -1, b = -1;  // b < 0: support
  u32 axis = 0;        // supports: face axis (a junction support: 3 + 6 x the lattice it reaches + its side)
  i8 sign = 1;         // supports: side of a
  MaterialId mb = MaterialId::Rock;
  f64 strength_b = 1.0;
  u16 grid = 0;              // the lattice of its faces
  std::vector<IVec3> faces;  // lower voxel of each face (the face p -> p + e_axis)
  std::vector<u8> fax;       // its axis
  std::vector<i8> fsg;       // +1: a is on the lower side of the face (its normal from a to b is +e_axis)
  // junction samples: jsg +1 when the sample's face normal points from a towards b (or out of a,
  // to a support), jw its weight (the samples of both sides count half each)
  std::vector<JSample> js;
  std::vector<i8> jsg;
  std::vector<f32> jw;
  void add(const IVec3& lower, int ax, int sign_from_a = 1) {
    faces.push_back(lower);
    fax.push_back(static_cast<u8>(ax));
    fsg.push_back(static_cast<i8>(sign_from_a));
  }
  void add_sample(const JSample& s, int sign_from_a, f32 w) {
    js.push_back(s);
    jsg.push_back(static_cast<i8>(sign_from_a));
    jw.push_back(w);
  }
  // hx(lattice) -> its voxel size; xf(lattice) -> const LatticeXf&: the lattices in the frame the
  // bond is made in (a body's frame: the static world's is the world); S: junction samples per
  // face edge. Faces of an unplaced lattice (the world grid's, a body's first shape) are measured
  // exactly as they always were.
  template <class Hx, class Xf>
  SBond finish(Hx&& hx, Xf&& xf, i32 S, const V3& ca, const V3* cb, MaterialId ma, f64 strength_a) const {
    if (js.empty() && xf(grid).identity) return finish_lattice(hx(grid), ca, cb, ma, strength_a);
    return finish_general(hx, xf, S, ca, cb, ma, strength_a);
  }
  template <class Hx, class Xf>
  SBond finish_general(Hx&& hx, Xf&& xf, i32 S, const V3& ca, const V3* cb, MaterialId ma, f64 strength_a) const;
  SBond finish_lattice(f64 h, const V3& ca, const V3* cb, MaterialId ma, f64 strength_a) const {
    SBond B;
    B.a = a;
    B.b = b;
    B.ma = ma;
    B.mb = mb;
    const i32 nf = static_cast<i32>(faces.size());
    B.faces = nf;
    const f64 A1 = h * h;
    // the interface normal: the mean face normal from a to b (a support: its face axis). The
    // section lies in the plane normal to it; the centres only set the spring lengths.
    V3 n;
    if (cb) {
      for (size_t k = 0; k < faces.size(); ++k) n[fax[k]] += fsg[k];
      if (norm2(n) < 1e-12) n = *cb - ca;
      if (norm2(n) < 1e-12) n = V3{0, 0, 1};
      n = normalized(n);
    } else {
      n[axis] = sign;
    }
    V3 t1, t2;
    frame_of(n, &t1, &t2);
    // projected weights and centroid
    f64 W = 0.0;
    V3 pc;
    std::vector<V3> xs(faces.size());
    std::vector<f64> ws(faces.size());
    for (size_t k = 0; k < faces.size(); ++k) {
      V3 x{h * faces[k][0], h * faces[k][1], h * faces[k][2]};
      x[fax[k]] += 0.5 * h;  // the face centre
      xs[k] = x;
      ws[k] = A1 * std::max(0.05, std::abs(n[fax[k]]));
      W += ws[k];
      pc += x * ws[k];
    }
    pc *= 1.0 / W;
    // the section's principal axes (an L- or T-shaped interface has a product of inertia in an
    // arbitrary frame: ignoring it would make its weak axis stiffer and stronger than it is).
    // The eigenvector of [[s11, s12], [s12, s22]] with sqrt only (the same on every platform).
    {
      f64 s11 = 0.0, s22 = 0.0, s12 = 0.0;
      for (size_t k = 0; k < faces.size(); ++k) {
        const V3 d = xs[k] - pc;
        const f64 y1 = dot(d, t1), y2 = dot(d, t2);
        s11 += ws[k] * y1 * y1;
        s22 += ws[k] * y2 * y2;
        s12 += ws[k] * y1 * y2;
      }
      if (std::abs(s12) > 1e-9 * (s11 + s22)) {
        const f64 half = 0.5 * (s11 - s22);
        const f64 l1 = 0.5 * (s11 + s22) + std::sqrt(half * half + s12 * s12);
        // (two forms of the eigenvector of l1: the better conditioned one)
        f64 e1 = s12, e2 = l1 - s11;
        if (std::abs(l1 - s22) > std::abs(e2)) {
          e1 = l1 - s22;
          e2 = s12;
        }
        const f64 en = std::sqrt(e1 * e1 + e2 * e2);
        if (en > 0.0) {
          e1 /= en;
          e2 /= en;
          const V3 u1 = t1 * e1 + t2 * e2;
          const V3 u2 = t2 * e1 - t1 * e2;  // (the same handedness: n x u1 = u2)
          t1 = u1;
          t2 = u2;
        }
      }
    }
    B.n = n;
    B.t1 = t1;
    B.t2 = t2;
    B.p = pc;
    B.area = std::max(W, 0.25 * nf * A1);
    f64 s1 = 0.0, s2 = 0.0, m1 = 0.0, m2 = 0.0;
    for (size_t k = 0; k < faces.size(); ++k) {
      const V3 d = xs[k] - pc;
      const f64 y1 = dot(d, t1), y2 = dot(d, t2);
      s1 += ws[k] * y1 * y1;
      s2 += ws[k] * y2 * y2;
      m1 = std::max(m1, std::abs(y1));
      m2 = std::max(m2, std::abs(y2));
    }
    B.s1 = s1 + B.area * A1 / 12.0;
    B.s2 = s2 + B.area * A1 / 12.0;
    B.c1 = m1 + 0.5 * h;
    B.c2 = m2 + 0.5 * h;
    B.rmax = std::sqrt(B.c1 * B.c1 + B.c2 * B.c2);
    if (cb) {
      const V3 d = *cb - ca;
      const f64 L = std::max({std::abs(dot(d, n)), 0.5 * norm(d), 0.5 * h});
      B.la = std::clamp(dot(pc - ca, n), 0.1 * L, 0.9 * L);
      B.lb = L - B.la;
    } else {
      B.la = std::max(0.25 * h, dot(pc - ca, n));
      B.lb = 0.5 * h;
    }
    B.strength = std::min(strength_a, strength_b);
    return B;
  }
};

// The general case: faces of a lattice placed in the frame, and junction samples. Each face or
// sample is a small square (side, area) at a point with a normal; the section is theirs
// projected onto the plane normal to the bond (their mean normal).
template <class Hx, class Xf>
SBond SecAcc::finish_general(Hx&& hx, Xf&& xf, i32 S, const V3& ca, const V3* cb, MaterialId ma, f64 strength_a) const {
  SBond B;
  B.a = a;
  B.b = b;
  B.ma = ma;
  B.mb = mb;
  const size_t nf = faces.size(), nj = js.size(), ne = nf + nj;
  B.faces = static_cast<i32>(ne);
  std::vector<V3> xs(ne), nk(ne);
  std::vector<f64> ar(ne), sd(ne);
  const f64 h = hx(grid);  // (the voxel size of its faces' lattice; each sample: its own lattice's)
  const f64 A1 = h * h;
  if (nf) {
    const LatticeXf& X = xf(grid);
    for (size_t k = 0; k < nf; ++k) {
      V3 x{h * faces[k][0], h * faces[k][1], h * faces[k][2]};
      x[fax[k]] += 0.5 * h;
      xs[k] = X.to(x);
      V3 n;
      n[fax[k]] = fsg[k];
      nk[k] = X.dir_to(n);
      ar[k] = A1;
      sd[k] = h;
    }
  }
  for (size_t q = 0; q < nj; ++q) {
    const JSample& s = js[q];
    const LatticeXf& X = xf(s.vg);
    const f64 hs = hx(s.vg), sj = hs / static_cast<f64>(std::max(1, S));
    const size_t k = nf + q;
    xs[k] = X.to(junction_point(s.v, s.face, s.sub, S, hs, 0.0));
    nk[k] = X.dir_to(face_normal(s.face)) * static_cast<f64>(jsg[q]);
    ar[k] = sj * sj * static_cast<f64>(jw[q]);
    sd[k] = sj;
  }
  f64 atot = 0.0;
  for (size_t k = 0; k < ne; ++k) atot += ar[k];
  // the interface normal: the area-weighted mean face normal from a to b (a support of a lattice:
  // its face axis; a junction support: the mean normal out of a)
  V3 n;
  if (cb || nj) {
    for (size_t k = 0; k < ne; ++k) n += nk[k] * ar[k];
    const f64 tiny = 1e-12 * atot * atot;
    if (norm2(n) <= tiny && cb) n = *cb - ca;
    if (norm2(n) <= tiny) n = V3{0, 0, 1};
    n = normalized(n);
  } else {
    V3 e;
    e[axis] = sign;
    n = xf(grid).dir_to(e);
  }
  V3 t1, t2;
  frame_of(n, &t1, &t2);
  f64 W = 0.0;
  V3 pc;
  std::vector<f64> ws(ne);
  for (size_t k = 0; k < ne; ++k) {
    ws[k] = ar[k] * std::max(0.05, std::abs(dot(n, nk[k])));
    W += ws[k];
    pc += xs[k] * ws[k];
  }
  pc *= 1.0 / W;
  {
    f64 s11 = 0.0, s22 = 0.0, s12 = 0.0;
    for (size_t k = 0; k < ne; ++k) {
      const V3 d = xs[k] - pc;
      const f64 y1 = dot(d, t1), y2 = dot(d, t2);
      s11 += ws[k] * y1 * y1;
      s22 += ws[k] * y2 * y2;
      s12 += ws[k] * y1 * y2;
    }
    if (std::abs(s12) > 1e-9 * (s11 + s22)) {
      const f64 half = 0.5 * (s11 - s22);
      const f64 l1 = 0.5 * (s11 + s22) + std::sqrt(half * half + s12 * s12);
      f64 e1 = s12, e2 = l1 - s11;
      if (std::abs(l1 - s22) > std::abs(e2)) {
        e1 = l1 - s22;
        e2 = s12;
      }
      const f64 en = std::sqrt(e1 * e1 + e2 * e2);
      if (en > 0.0) {
        e1 /= en;
        e2 /= en;
        const V3 u1 = t1 * e1 + t2 * e2;
        const V3 u2 = t2 * e1 - t1 * e2;
        t1 = u1;
        t2 = u2;
      }
    }
  }
  B.n = n;
  B.t1 = t1;
  B.t2 = t2;
  B.p = pc;
  B.area = std::max(W, 0.25 * atot);
  f64 s1 = 0.0, s2 = 0.0, m1 = 0.0, m2 = 0.0, own = 0.0;
  for (size_t k = 0; k < ne; ++k) {
    const V3 d = xs[k] - pc;
    const f64 y1 = dot(d, t1), y2 = dot(d, t2);
    s1 += ws[k] * y1 * y1;
    s2 += ws[k] * y2 * y2;
    m1 = std::max(m1, std::abs(y1) + 0.5 * sd[k]);
    m2 = std::max(m2, std::abs(y2) + 0.5 * sd[k]);
    own += ws[k] * sd[k] * sd[k];
  }
  own /= W;  // (the mean square side of the section's squares)
  B.s1 = s1 + B.area * own / 12.0;
  B.s2 = s2 + B.area * own / 12.0;
  B.c1 = m1;
  B.c2 = m2;
  B.rmax = std::sqrt(B.c1 * B.c1 + B.c2 * B.c2);
  if (cb) {
    const V3 d = *cb - ca;
    const f64 L = std::max({std::abs(dot(d, n)), 0.5 * norm(d), 0.5 * h});
    B.la = std::clamp(dot(pc - ca, n), 0.1 * L, 0.9 * L);
    B.lb = L - B.la;
  } else {
    B.la = std::max(0.25 * h, dot(pc - ca, n));
    B.lb = 0.5 * h;
  }
  B.strength = std::min(strength_a, strength_b);
  return B;
}

// A voxel as the section of a bond sees it: its value and its damage (the core's condition
// layer: 0 intact .. 255 no strength left).
struct VoxelAt {
  Vox v = kAir;
  u8 damage = 0;
};

// Section strengths of a bond from its faces (lower voxel faces[k], axis axes[k]): each face as
// strong as the weaker material of the two voxels it joins, times the condition of the more
// damaged one; the section's strengths are the faces' mean. at(p) -> VoxelAt.
template <class At>
void section_strengths(const MaterialTable& mats, const IVec3* faces, const u8* axes, size_t n, At&& at, SBond& B) {
  f64 ft = 0, fb = 0, fc = 0, coh = 0, mu = 0;
  for (size_t k = 0; k < n; ++k) {
    IVec3 q = faces[k];
    const VoxelAt a = at(q);
    q[axes[k]] += 1;
    const VoxelAt b = at(q);
    // (a side that is air - an unloaded neighbour, a support face - is as the other side)
    const Material& A = mats[vox_solid(a.v) ? vox_mat(a.v) : vox_mat(b.v)];
    const Material& M = mats[vox_solid(b.v) ? vox_mat(b.v) : vox_mat(a.v)];
    // (full damage leaves a trace of strength: the section fails under any load, never 0 / 0)
    const f64 cond = std::max(1e-6, 1.0 - static_cast<f64>(std::max(a.damage, b.damage)) / 255.0);
    ft += cond * std::min(A.ft, M.ft);
    fb += cond * std::min(A.fb, M.fb);
    fc += cond * std::min(A.fc, M.fc);
    coh += cond * std::min(A.cohesion, M.cohesion);
    mu += std::min(A.friction, M.friction);
  }
  if (n == 0) return;
  const f64 w = 1.0 / static_cast<f64>(n);
  B.sectioned = true;
  B.ft = static_cast<f32>(ft * w);
  B.fb = static_cast<f32>(fb * w);
  B.fc = static_cast<f32>(fc * w);
  B.coh = static_cast<f32>(coh * w);
  B.mu = static_cast<f32>(mu * w);
}

// The same for a bond with junction samples (and faces of one lattice, `grid`): each sample as
// strong as the weaker of the two voxels it joins (a sample into the unknown world: its own
// voxel's), times the condition of the more damaged. at(lattice, p) -> VoxelAt.
template <class At>
void section_strengths_general(const MaterialTable& mats, u16 grid, const IVec3* faces, const u8* axes, size_t nfaces, const JSample* js,
                               size_t nj, At&& at, SBond& B, i32 S) {
  f64 ft = 0, fb = 0, fc = 0, coh = 0, mu = 0, wsum = 0;
  auto one = [&](const VoxelAt& a, const VoxelAt& b, f64 w) {
    const Material& A = mats[vox_solid(a.v) ? vox_mat(a.v) : vox_mat(b.v)];
    const Material& M = mats[vox_solid(b.v) ? vox_mat(b.v) : vox_mat(a.v)];
    const f64 cond = std::max(1e-6, 1.0 - static_cast<f64>(std::max(a.damage, b.damage)) / 255.0);
    ft += w * cond * std::min(A.ft, M.ft);
    fb += w * cond * std::min(A.fb, M.fb);
    fc += w * cond * std::min(A.fc, M.fc);
    coh += w * cond * std::min(A.cohesion, M.cohesion);
    mu += w * std::min(A.friction, M.friction);
    wsum += w;
  };
  // (by area: a face of the lattice, S x S samples of a face)
  const f64 sw = 1.0 / static_cast<f64>(std::max(1, S * S));
  for (size_t k = 0; k < nfaces; ++k) {
    IVec3 q = faces[k];
    const VoxelAt a = at(grid, q);
    q[axes[k]] += 1;
    one(a, at(grid, q), 1.0);
  }
  for (size_t k = 0; k < nj; ++k) {
    const VoxelAt a = at(js[k].vg, js[k].v);
    one(a, js[k].kind == kJunctionUnknown ? a : at(js[k].og, js[k].o), sw * junction_weight(js[k]));
  }
  if (nfaces + nj == 0 || !(wsum > 0.0)) return;
  const f64 w = 1.0 / wsum;
  B.sectioned = true;
  B.ft = static_cast<f32>(ft * w);
  B.fb = static_cast<f32>(fb * w);
  B.fc = static_cast<f32>(fc * w);
  B.coh = static_cast<f32>(coh * w);
  B.mu = static_cast<f32>(mu * w);
}

// Pair bonds: one per unordered node pair; supports: one per (node, axis, side).
// An accumulator's key: a node pair makes one bond (its faces of any axis and its junction
// samples of any side, as one interface: measured against the world grid's own bonds, per-side
// junction bonds were weaker than a member cast in whole); a support, one per axis and side.
inline u64 acc_key(i32 a, i32 b, int axis, int sign) {
  if (b >= 0) return (static_cast<u64>(std::min(a, b)) << 32) | (static_cast<u64>(std::max(a, b)) << 1) | 1u;
  return (static_cast<u64>(a) << 32) | (static_cast<u64>(axis * 2 + (sign > 0 ? 1 : 0)) << 1);
}

inline BondLoad lerp_load(const BondLoad& prev, const BondLoad& now, f64 k) {
  BondLoad r;
  r.N = prev.N + k * (now.N - prev.N);
  r.V1 = prev.V1 + k * (now.V1 - prev.V1);
  r.V2 = prev.V2 + k * (now.V2 - prev.V2);
  r.T = prev.T + k * (now.T - prev.T);
  r.M1 = prev.M1 + k * (now.M1 - prev.M1);
  r.M2 = prev.M2 + k * (now.M2 - prev.M2);
  return r;
}

// Groups items into clusters: items with the same cell key (and group, if given) that are linked
// (directly or through each other). out[i] = cluster of item i, numbered in order of first
// appearance.
inline i32 cluster_items(const std::vector<u64>& cell, const std::vector<std::pair<i32, i32>>& links, std::vector<i32>* out,
                         const std::vector<u16>* group = nullptr) {
  const i32 n = static_cast<i32>(cell.size());
  std::vector<i32> p(static_cast<size_t>(n));
  for (i32 i = 0; i < n; ++i) p[size_t(i)] = i;
  auto find = [&](i32 x) {
    while (p[size_t(x)] != x) {
      p[size_t(x)] = p[size_t(p[size_t(x)])];
      x = p[size_t(x)];
    }
    return x;
  };
  for (const auto& [a, b] : links) {
    if (cell[size_t(a)] != cell[size_t(b)]) continue;
    if (group && (*group)[size_t(a)] != (*group)[size_t(b)]) continue;
    const i32 ra = find(a), rb = find(b);
    if (ra != rb) p[size_t(std::max(ra, rb))] = std::min(ra, rb);
  }
  out->assign(static_cast<size_t>(n), -1);
  std::vector<i32> id(static_cast<size_t>(n), -1);
  i32 k = 0;
  for (i32 i = 0; i < n; ++i) {
    const i32 r = find(i);
    if (id[size_t(r)] < 0) id[size_t(r)] = k++;
    (*out)[size_t(i)] = id[size_t(r)];
  }
  return k;
}

// Node-level bond accumulators from finer ones: endpoints mapped (a support stays < 0), the
// faces and junction samples of one node pair (a support: one node, axis and side) combined,
// pairs inside one node dropped. Face and sample signs stay relative to the merged bond's a.
template <class Map>
std::vector<SecAcc> merge_accs(const std::vector<SecAcc>& fine, Map&& node_of) {
  std::vector<SecAcc> out;
  std::unordered_map<u64, i32> index;
  for (const SecAcc& F : fine) {
    const i32 a = node_of(F.a);
    const i32 b = F.b >= 0 ? node_of(F.b) : -1;
    if (b >= 0 && a == b) continue;
    const u64 k = acc_key(a, b, static_cast<int>(F.axis), F.sign);
    auto it = index.find(k);
    SecAcc* M;
    if (it == index.end()) {
      index.emplace(k, static_cast<i32>(out.size()));
      out.emplace_back();
      M = &out.back();
      M->a = b >= 0 ? std::min(a, b) : a;
      M->b = b >= 0 ? std::max(a, b) : b;
      M->axis = F.axis;
      M->sign = F.sign;
      M->mb = F.mb;
      M->grid = F.grid;
    } else {
      M = &out[size_t(it->second)];
    }
    const bool flip = b >= 0 && a != M->a;
    for (size_t q = 0; q < F.faces.size(); ++q) M->add(F.faces[q], F.fax[q], flip ? -F.fsg[q] : F.fsg[q]);
    for (size_t q = 0; q < F.js.size(); ++q) M->add_sample(F.js[q], flip ? -F.jsg[q] : F.jsg[q], F.jw[q]);
  }
  return out;
}

// Connected components over the intact bonds between nodes (supports ignored). Component 0 holds
// every node reachable from a node with seed[i] != 0; the rest are numbered from 1 in node order.
// Returns the number of components (at least 1).
inline i32 graph_components(i32 n, const std::vector<SBond>& bonds, const std::vector<u8>& seed, std::vector<i32>* comp) {
  std::vector<i32> deg(static_cast<size_t>(n) + 1, 0);
  for (const SBond& B : bonds) {
    if (B.broken || B.b < 0) continue;
    ++deg[size_t(B.a) + 1];
    ++deg[size_t(B.b) + 1];
  }
  for (i32 i = 0; i < n; ++i) deg[size_t(i) + 1] += deg[size_t(i)];
  std::vector<i32> adj(static_cast<size_t>(deg[static_cast<size_t>(n)]));
  {
    std::vector<i32> fill(deg.begin(), deg.end() - 1);
    for (const SBond& B : bonds) {
      if (B.broken || B.b < 0) continue;
      adj[size_t(fill[size_t(B.a)]++)] = B.b;
      adj[size_t(fill[size_t(B.b)]++)] = B.a;
    }
  }
  comp->assign(size_t(n), -1);
  std::vector<i32> stack;
  auto flood = [&](i32 c) {
    while (!stack.empty()) {
      const i32 k = stack.back();
      stack.pop_back();
      for (i32 e = deg[size_t(k)]; e < deg[size_t(k) + 1]; ++e) {
        const i32 j = adj[size_t(e)];
        if ((*comp)[size_t(j)] < 0) {
          (*comp)[size_t(j)] = c;
          stack.push_back(j);
        }
      }
    }
  };
  for (i32 i = 0; i < n; ++i)
    if (seed[size_t(i)] && (*comp)[size_t(i)] < 0) {
      (*comp)[size_t(i)] = 0;
      stack.push_back(i);
    }
  flood(0);
  i32 nc = 1;
  for (i32 i = 0; i < n; ++i) {
    if ((*comp)[size_t(i)] >= 0) continue;
    const i32 c = nc++;
    (*comp)[size_t(i)] = c;
    stack.push_back(i);
    flood(c);
  }
  return nc;
}

// The solid voxel nearest lattice point L (metres, voxel size h): the one it is in, else the
// nearest of its 26 neighbours.
template <class Solid>
inline bool nearest_solid(const V3& L, f64 h, Solid&& solid, IVec3* out, f64* dist2) {
  const IVec3 c = voxel_of(L, h);
  if (solid(c)) {
    *out = c;
    *dist2 = 0.0;
    return true;
  }
  bool any = false;
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      for (int dz = -1; dz <= 1; ++dz) {
        if (dx == 0 && dy == 0 && dz == 0) continue;
        const IVec3 q{c[0] + dx, c[1] + dy, c[2] + dz};
        if (!solid(q)) continue;
        const f64 e = norm2(V3{h * q[0], h * q[1], h * q[2]} - L);
        if (!any || e < *dist2) {
          *out = q;
          *dist2 = e;
          any = true;
        }
      }
  return any;
}

inline bool shape_solid(const BodyShape& S, const IVec3& p) {
  const i32 i = S.index(p);
  return i >= 0 && vox_solid(S.vox[size_t(i)]);
}

}  // namespace world_detail

// A body's bond graph (the fracture layer's data attached to a rigid body).
struct BodyGraph {
  StressProblem P;
  std::vector<i32> frag_node;  // body fragment -> node (-1: none); nodes are clusters of fragments
  std::vector<V3> node_com;    // body frame
  std::vector<f64> node_mass;
  std::vector<M3> node_inertia;  // about node_com
  i32 components = 1;            // of the intact bonds (a piece must be one: else it is split)
  // bond b: its faces face_start[b] .. face_start[b + 1] in shape face_shape[b]; its junction
  // samples (between shapes: vg, og are shape indices) jstart[b] .. jstart[b + 1]
  std::vector<i32> face_start;
  std::vector<IVec3> face_p;
  std::vector<u8> face_axis;
  std::vector<u16> face_shape;
  std::vector<i32> jstart;
  std::vector<world_detail::JSample> jref;
  std::vector<f64> u;
};

struct World::Structure {
  i64 id = 0;
  StressProblem P;
  // Nodes are clusters of fragments (single fragments for small structures): node i holds
  // frags[fstart[i] .. fstart[i + 1]) (a retired node's entries are cleared, idx -1). A node's
  // fragments are in one grid; a structure's nodes may be in several (bonded by junctions).
  std::vector<i32> fstart{0};
  std::vector<FragKey> frags;
  std::vector<u64> ident;          // node -> identity (of its first fragment)
  std::vector<MaterialId> nmat;    // node -> material (its heaviest fragment's)
  std::vector<f64> nstrength;      // node -> design strength multiplier (its weakest fragment's)
  i32 cell = 0;                    // cluster cell (voxels; 0: one node per fragment)
  std::vector<GVox> vox0;          // node -> a voxel of it (re-seeding)
  std::vector<f64> weight;         // node -> N
  std::unordered_map<GKey, std::vector<i32>, GKeyHash> nodemap;  // (grid, chunk) -> node per fragment (-1)
  // bond -> faces: bond b has [face_start[b], face_start[b + 1]) in grid bgrid[b], and junction
  // samples [jstart[b], jstart[b + 1])
  std::vector<i32> face_start{0};
  std::vector<IVec3> face_p;
  std::vector<u8> face_axis;
  std::vector<u16> bgrid;
  std::vector<i32> jstart{0};
  std::vector<world_detail::JSample> jref;
  std::vector<u64> bid;            // bond identity
  std::vector<f32> phi;            // bond utilization at the last judge (debug view)
  std::vector<f64> u;              // 6 per node
  std::vector<f64> ext, ext_solved, acc, peak;  // external loads (6 per node)
  std::vector<f64> pending;        // an impact load case waiting for the running solve
  bool pending_impact = false, reload = false;
  bool transient = false;          // ext_solved is an impact load case (not the steady state)
  std::vector<f64> peak_mag;
  bool solving = true, stale = false, shock = true, dead = false;
  bool rejudge = false;            // strengths changed (damage): judged again at the current solution
  std::vector<GKey> changed;       // chunks re-fragmented since the structure was made (stale)
  i32 gone = 0;                    // retired nodes (detached / carved away)
  i32 run_iters = 0;               // PCG iterations of the solve in progress
  i32 restarts = 0;                // solves restarted in a row without converging
  bool multigrid = false;          // (a small structure's block-Jacobi solve did not converge)
  i32 rounds = 0, idle = 0;        // (rounds: of the break cascade in progress)
  bool truncated = false;
  // The last blast's momentum on its fragments (and where it acts): parts its load breaks off
  // take it with them, for as long as its cascade lasts (at most a second).
  struct BlastHit {
    FragKey f;
    V3 J, at;
  };
  std::vector<BlastHit> blast;
  i64 blast_tick = 0;
  i32 node(const FragKey& f) const {
    const auto it = nodemap.find(GKey{f.grid, f.chunk});
    if (it == nodemap.end() || f.idx < 0 || f.idx >= static_cast<i32>(it->second.size())) return -1;
    return it->second[size_t(f.idx)];
  }
};

// A joint's anchors (docs/MOTION.md §2): what each end holds on to. (Its solver state is
// rigid_.joints, in the same order.)
struct World::JointRec {
  JointId id = 0;
  struct End {
    JointAnchor::Kind kind = JointAnchor::Kind::World;  // (a Piece anchor is held as a Grid one: its voxel)
    GridId grid = 0;                   // (Grid) the grid of its voxel
    IVec3 voxel{0, 0, 0};              // (Grid) that voxel
    // the anchor, the axis and the reference direction: Grid: in the grid's lattice (m); World:
    // in the world
    V3 point, axis{0, 0, 1}, ref{1, 0, 0};
    i64 piece = 0;                     // (Grid) the piece its voxel went with (0: it is in its grid)
    i32 shape = -1;                    // ... its shape of that grid
  } a, b;
};

// A wheel's mount (docs/VEHICLES.md): the voxel it hangs from (a JointRec::End held as a Grid
// anchor: axis = its suspension's axis down, ref = its axle, in that lattice), and the host's data.
// (Its solver state is rigid_.wheels, in the same order.)
struct World::WheelRec {
  WheelId id = 0;
  JointRec::End mount;
  u32 group = 0, tag = 0;
  // where it was last (its centre, orientation, motion): what comes off becomes a piece there
  bool placed = false;
  V3 centre, vel, ang;
  Quat rot;
};

// A saved session's pieces and joints, read and checked before they are applied
// (world_session.cpp).
struct World::SessionDelta {
  i64 steps = 0, next_id = 1;
  JointId next_joint = 1;
  std::vector<std::unique_ptr<Body>> pieces;
  std::vector<std::pair<JointRec, Joint>> joints;
  WheelId next_wheel = 1;
  std::vector<std::pair<WheelRec, Wheel>> wheels;
  struct Dead {
    i64 piece = 0;
    struct Load {
      GridId grid = 0;
      IVec3 voxel{0, 0, 0};
      V3 p, F;
    };
    std::vector<Load> loads;
  };
  std::vector<Dead> dead;
  // (a streamed world's groups archived out of range: their records as archived)
  struct Archived {
    u64 key = 0;
    std::vector<u64> chunks;
    std::vector<JointId> joints;
    std::vector<u8> record;
  };
  std::vector<Archived> archived;
};

// A grid of the world (docs/GRIDS.md): its frame, voxels (oriented grids; the world grid's are
// World::grid_) and what the world derives from them.
struct World::GridState {
  GridId id = 0;
  bool base = true;                  // (oriented grids) part of the level: only its changes are saved
  i32 priority = 0;                  // overlaps: the higher keeps its voxels (then the higher id)
  LatticeXf xf;                      // lattice -> world (the world grid: the identity)
  VoxelGrid g;                       // (oriented grids)
  std::unordered_map<u64, FragChunk> frags;         // chunk -> its fragments (cache)
  std::unordered_map<u64, std::vector<i64>> owner;  // chunk -> structure id per fragment (0 none)
  std::unordered_set<u64> undesigned;               // chunks generated and not designed yet
  V3 lo, hi;                         // (oriented grids) world box of its chunks
  V3 llo, lhi;                       // (oriented grids) ... in its lattice (metres)
  bool any = false;                  // (oriented grids) it has chunks
  std::unordered_map<u64, std::vector<u16>> near;   // chunk -> other grids a junction sample may reach (cache)
  u64 near_epoch = 0;
  u64 home = ~0ull;                  // (streamed) the world chunk it came with
  bool moved = false;                // placed anew since the level made it (saved in deltas)
  u32 placement = 0;                 // times placed anew (its fragments' identities: warm starts, reference loads)
};

inline VoxelGrid& World::vg(u16 g) { return g == 0 ? grid_ : grids_[g]->g; }
inline const VoxelGrid& World::vg(u16 g) const { return g == 0 ? grid_ : grids_[g]->g; }
inline World::GridState& World::gs(u16 g) { return *grids_[g]; }
inline const World::GridState& World::gs(u16 g) const { return *grids_[g]; }

// Junction samples found during one extraction (world_grids.cpp).
struct World::JunctionScratch {
  std::unordered_map<GKey, std::vector<world_detail::JSample>, GKeyHash> fwd;           // a chunk's faces' samples
  std::unordered_map<GKey, std::vector<std::vector<world_detail::JSample>>, GKeyHash> rev;  // samples landing in a chunk, by fragment
};

template <class Fn>
void World::each_junction(JunctionScratch& js, const FragKey& f, Fn&& fn) {
  if (oriented_ == 0) return;
  FragChunk* fc = frag_chunk_if(f);
  if (!fc || f.idx < 0) return;
  const std::vector<world_detail::JSample>& fw = junction_fwd(js, f.grid, f.chunk);
  for (const world_detail::JSample& s : fw)
    if (fc->at(chunk_index(s.v)) == f.idx) fn(s, true);
  if (const std::vector<world_detail::JSample>* rv = junction_rev(js, f))
    for (const world_detail::JSample& s : *rv) fn(s, false);
}

}  // namespace svx
