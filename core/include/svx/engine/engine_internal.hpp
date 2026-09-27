// structvox v2 — engine internals shared by engine*.cpp (not part of the public API).
#pragma once

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "svx/engine/engine.hpp"

namespace svx {

namespace engine_detail {

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
inline IVec3 local_of(int i) { return {i / (kChunk * kChunk), (i / kChunk) % kChunk, i % kChunk}; }
inline IVec3 voxel_of(const V3& p, f64 h) {
  return {static_cast<i32>(std::floor(p.x / h + 0.5)), static_cast<i32>(std::floor(p.y / h + 0.5)),
          static_cast<i32>(std::floor(p.z / h + 0.5))};
}

// Design strength classes: 1, 1.5, 2, 3, 4, 6, 8, ... x 1024 (class 20).
constexpr int kStrengthClasses = 21;
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
// fragment on a support along one axis. The bond runs along the line between the two centres
// (a support: along the face normal); its section is the faces projected onto the plane normal
// to that line (rigid-body-spring network).
inline void frame_of(const V3& n, V3* t1, V3* t2) {
  const f64 ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
  const V3 e = (ax <= ay && ax <= az) ? V3{1, 0, 0} : (ay <= az ? V3{0, 1, 0} : V3{0, 0, 1});
  *t1 = normalized(cross(n, e));
  *t2 = cross(n, *t1);
}

struct SecAcc {
  i32 a = -1, b = -1;  // b < 0: support
  u8 axis = 0;         // supports: face axis
  i8 sign = 1;         // supports: side of a
  MaterialId mb = MaterialId::Rock;
  f64 strength_b = 1.0;
  std::vector<IVec3> faces;  // lower voxel of each face (the face p -> p + e_axis)
  std::vector<u8> fax;       // its axis
  std::vector<i8> fsg;       // +1: a is on the lower side of the face (its normal from a to b is +e_axis)
  void add(const IVec3& lower, int ax, int sign_from_a = 1) {
    faces.push_back(lower);
    fax.push_back(static_cast<u8>(ax));
    fsg.push_back(static_cast<i8>(sign_from_a));
  }
  SBond finish(f64 h, const V3& ca, const V3* cb, MaterialId ma, f64 strength_a) const {
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

// Pair bonds: one per unordered node pair; supports: one per (node, axis, side).
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

}  // namespace engine_detail

// A body's bond graph (the fracture layer's data attached to a rigid body).
struct BodyGraph {
  StressProblem P;
  std::vector<i32> node_frag;  // node -> body fragment
  std::vector<i32> frag_node;  // fragment -> node (-1: none)
  std::vector<i32> face_start;
  std::vector<IVec3> face_p;
  std::vector<u8> face_axis;
  std::vector<f64> u;
};

struct Engine::Structure {
  i64 id = 0;
  StressProblem P;
  std::vector<FragKey> refs;       // node -> fragment
  std::vector<u64> ident;          // node -> fragment identity
  std::vector<IVec3> vox0;         // node -> a voxel of it (re-seeding)
  std::vector<f64> weight;         // node -> N
  std::unordered_map<u64, std::vector<i32>> nodemap;  // chunk -> node per fragment (-1)
  std::vector<i32> face_start;     // bond -> faces
  std::vector<IVec3> face_p;
  std::vector<u8> face_axis;
  std::vector<u64> bid;            // bond identity
  std::vector<f32> phi;            // bond utilization at the last judge (debug view)
  std::vector<f64> u;              // 6 per node
  std::vector<f64> ext, ext_solved, acc, peak;  // external loads (6 per node)
  std::vector<f64> pending;        // an impact load case waiting for the running solve
  bool pending_impact = false, reload = false;
  std::vector<f64> peak_mag;
  bool solving = true, stale = false, shock = true, dead = false;
  std::vector<u64> changed;        // chunks re-fragmented since the structure was made (stale)
  i32 gone = 0;                    // retired nodes (detached / carved away)
  i32 run_iters = 0;               // PCG iterations of the solve in progress
  i32 rounds = 0, idle = 0;
  bool truncated = false;
  i32 node(const FragKey& f) const {
    const auto it = nodemap.find(f.chunk);
    if (it == nodemap.end() || f.idx < 0 || f.idx >= static_cast<i32>(it->second.size())) return -1;
    return it->second[size_t(f.idx)];
  }
};

}  // namespace svx
