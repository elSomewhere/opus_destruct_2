#include "svx/bubble/composite.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "svx/base/dmath.hpp"
#include "svx/base/parallel.hpp"
#include "svx/base/work.hpp"
#include "svx/solve/block6.hpp"

namespace svx {

using namespace blk6;

namespace {

inline u64 pack3(i32 x, i32 y, i32 z) {
  constexpr i64 off = i64(1) << 20;
  return (u64(i64(x) + off) << 42) | (u64(i64(y) + off) << 21) | u64(i64(z) + off);
}


}  // namespace

Composite build_composite(const Lattice& L, std::span<const std::array<f64, 3>> centers, const CompositeOptions& opt) {
  add_work(i64(L.n) * kWorkComposite / 4);  // (deterministic work accounting)
  Composite C;
  using PClock = std::chrono::steady_clock;
  auto pt = PClock::now();
  auto phase = [&](int k) {
    const auto now = PClock::now();
    C.ms[size_t(k)] += std::chrono::duration<f64, std::milli>(now - pt).count();
    pt = now;
  };
  const i32 n = L.n;
  const f64 h = L.h;
  const int Lmax = std::max(0, opt.max_level);
  // 1. desired level of every free cell
  std::vector<i8> desired(n, -1);
  std::vector<u8> forced(n, 0);
  for (i32 c : opt.force_fine)
    if (c >= 0 && c < n) forced[c] = 1;
  for (i32 i = 0; i < n; ++i) {
    if (L.dead[i] || L.anchored[i]) continue;
    if (L.fixmask[i] || forced[i]) {
      desired[i] = 0;
      continue;
    }
    f64 d2 = INFINITY;
    for (const auto& c : centers) {
      const f64 dx = h * L.p[i][0] - c[0], dy = h * L.p[i][1] - c[1], dz = h * L.p[i][2] - c[2];
      d2 = std::min(d2, dx * dx + dy * dy + dz * dz);
    }
    const f64 d = std::sqrt(d2) / h;
    i8 lvl = 0;
    for (int l = 1; l <= Lmax; ++l)
      if (d >= opt.R0 * dm::ipow(opt.grading, l - 1)) lvl = static_cast<i8>(l);
    desired[i] = lvl;
  }
  phase(0);
  // 2. aggregates, coarsest level first
  C.node_of.assign(n, -1);
  std::vector<i32> uf(n);
  auto find = [&](i32 x) {
    while (uf[x] != x) {
      uf[x] = uf[uf[x]];
      x = uf[x];
    }
    return x;
  };
  for (int l = Lmax; l >= 1; --l) {
    struct BlockStat {
      i8 min_desired = 127;
      bool assigned = false;
    };
    std::unordered_map<u64, BlockStat> stat;
    stat.reserve((size_t(n) >> (2 * l)) + 16);
    auto key = [&](i32 i) { return pack3(L.p[i][0] >> l, L.p[i][1] >> l, L.p[i][2] >> l); };
    for (i32 i = 0; i < n; ++i) {
      if (desired[i] < 0) continue;
      BlockStat& b = stat[key(i)];
      b.min_desired = std::min(b.min_desired, desired[i]);
      b.assigned = b.assigned || C.node_of[i] >= 0;
    }
    std::vector<u8> qual(n, 0);
    for (i32 i = 0; i < n; ++i) {
      if (desired[i] < 0 || C.node_of[i] >= 0) continue;
      const BlockStat& b = stat[key(i)];
      qual[i] = b.min_desired >= l && !b.assigned;
    }
    for (i32 i = 0; i < n; ++i) uf[i] = i;
    for (int a = 0; a < 3; ++a)
      for (i32 i = 0; i < n; ++i) {
        const i32 j = L.nbr[a][i];
        if (j < 0 || !qual[i] || !qual[j] || key(i) != key(j)) continue;
        const i32 ri = find(i), rj = find(j);
        if (ri != rj) uf[std::max(ri, rj)] = std::min(ri, rj);
      }
    std::vector<i32> root_node(n, -1);
    for (i32 i = 0; i < n; ++i) {
      if (!qual[i]) continue;
      const i32 r = find(i);
      if (root_node[r] < 0) {
        root_node[r] = C.n++;
        C.level.push_back(static_cast<u8>(l));
        C.cell.push_back(-1);
      }
      C.node_of[i] = root_node[r];
    }
  }
  // 3. the rest stays fine
  for (i32 i = 0; i < n; ++i) {
    if (desired[i] < 0 || C.node_of[i] >= 0) continue;
    C.node_of[i] = C.n++;
    C.level.push_back(0);
    C.cell.push_back(i);
    ++C.n_fine;
  }
  phase(1);
  const i32 N = C.n;
  // members (CSR), mass centroids, offsets
  C.mptr.assign(N + 1, 0);
  for (i32 i = 0; i < n; ++i)
    if (C.node_of[i] >= 0) C.mptr[C.node_of[i] + 1]++;
  for (i32 k = 0; k < N; ++k) C.mptr[k + 1] += C.mptr[k];
  C.members.resize(C.mptr[N]);
  {
    std::vector<i32> fill(C.mptr.begin(), C.mptr.end() - 1);
    for (i32 i = 0; i < n; ++i)
      if (C.node_of[i] >= 0) C.members[fill[C.node_of[i]]++] = i;
  }
  C.X.assign(3 * size_t(N), 0.0);
  std::vector<f64> msum(N, 0.0);
  for (i32 i = 0; i < n; ++i) {
    const i32 k = C.node_of[i];
    if (k < 0) continue;
    const f64 m = std::max(L.mass[i], 1e-30);
    for (int d = 0; d < 3; ++d) C.X[3 * size_t(k) + d] += m * h * L.p[i][d];
    msum[k] += m;
  }
  for (i32 k = 0; k < N; ++k)
    for (int d = 0; d < 3; ++d) C.X[3 * size_t(k) + d] /= msum[k];
  C.off.assign(3 * size_t(n), 0.0);
  for (i32 i = 0; i < n; ++i) {
    const i32 k = C.node_of[i];
    if (k < 0 || C.level[k] == 0) continue;
    for (int d = 0; d < 3; ++d) C.off[3 * size_t(i) + d] = h * L.p[i][d] - C.X[3 * size_t(k) + d];
  }
  C.fix.assign(N, 0);
  for (i32 k = 0; k < N; ++k)
    if (C.level[k] == 0) C.fix[k] = L.fixmask[C.cell[k]];
  C.blk.resize(N);
  for (i32 k = 0; k < N; ++k)
    for (int d = 0; d < 3; ++d) C.blk[k][d] = static_cast<i32>(std::floor(C.X[3 * size_t(k) + d] / h + 0.5));

  phase(2);
  // 4. fibre lengths and bond scaling: per aggregate node and axis, the member count of each
  // line through its 2^l block (flat tables, indexed by the transverse local coordinates)
  std::vector<i32> fib_off(size_t(N) + 1, 0);
  for (i32 k = 0; k < N; ++k) {
    const int l = C.level[k];
    fib_off[size_t(k) + 1] = fib_off[size_t(k)] + (l > 0 && opt.scaling ? 3 << (2 * l) : 0);
  }
  std::vector<i32> fib(size_t(fib_off[size_t(N)]), 0);
  auto fib_slot = [&](i32 cell, i32 k, int a) -> i32& {
    const int l = C.level[k], side = 1 << l, t1 = (a + 1) % 3, t2 = (a + 2) % 3;
    const i32 u = L.p[cell][t1] & (side - 1), v = L.p[cell][t2] & (side - 1);
    return fib[size_t(fib_off[size_t(k)]) + size_t(a) * size_t(side * side) + size_t(u * side + v)];
  };
  if (opt.scaling)
    for (i32 i = 0; i < n; ++i) {
      const i32 k = C.node_of[i];
      if (k < 0 || C.level[k] == 0) continue;
      for (int a = 0; a < 3; ++a) ++fib_slot(i, k, a);
    }
  auto fibre_len = [&](i32 cell, int a) -> f64 {
    const i32 k = C.node_of[cell];
    if (k < 0 || C.level[k] == 0) return 1.0;
    return f64(fib_slot(cell, k, a));
  };
  for (int a = 0; a < 3; ++a) {
    C.scale[a].assign(n, 1.0f);
    if (!opt.scaling) continue;
    for (i32 i = 0; i < n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0) continue;
      C.scale[a][i] = static_cast<f32>(2.0 / (fibre_len(i, a) + fibre_len(j, a)));
    }
  }

  phase(3);
  // 5. assembly. Every crossing bond is a set of springs at its face centre c joining two rigid
  // nodes: translational springs along the axes act on U + Theta x rho (rho = c - the node's
  // reference point), rotational ones on Theta. A face's blocks (node pair, axis) therefore
  // follow exactly from per-spring totals and moments of rho, accumulated in O(1) per bond; the
  // 6x6 blocks are formed once per face. Bonds of partially fixed cells (masked prolongation)
  // and the reference option take the per-bond Galerkin path.
  const f64 hh = 0.5 * h;
  struct Face {
    i32 I, J;  // nodes (-1: none on that side: anchored or dead)
    int a;
    f64 k0[6] = {};     // per spring: sum k
    f64 m1[3][3] = {};  // translational springs: sum k rho
    f64 m2[3][9] = {};  // translational springs: sum k rho rho^T (rho of I, or of J when I < 0)
  };
  std::vector<Face> faces;
  struct FaceKey {
    i32 I, J;
    int a;
    bool operator==(const FaceKey& o) const { return I == o.I && J == o.J && a == o.a; }
  };
  struct FaceKeyHash {
    size_t operator()(const FaceKey& k) const {
      u64 x = u64(u32(k.I)) * 0x9E3779B97F4A7C15ull;
      x ^= u64(u32(k.J)) * 0xC2B2AE3D27D4EB4Full + (x << 6) + (x >> 2);
      x ^= u64(k.a) * 0x165667B19E3779F9ull;
      return static_cast<size_t>(x ^ (x >> 29));
    }
  };
  std::unordered_map<FaceKey, i32, FaceKeyHash> face_of;
  struct Direct {
    int a;
    i32 i;
  };
  std::vector<Direct> direct;  // bonds assembled per bond
  BsrBuilder bb(N);
  for (i32 k = 0; k < N; ++k) bb.touch(k, k);
  FaceKey last{-2, -2, -1};
  i32 last_f = -1;
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0) continue;
      const i32 I = C.node_of[i], J = C.node_of[j];
      if (I >= 0 && I == J) continue;  // internal to a rigid aggregate: zero energy
      ++C.crossing_bonds;
      if (I >= 0 && J >= 0) {
        bb.touch(I, J);
        bb.touch(J, I);
      }
      const bool fine_i = I < 0 || C.level[I] == 0, fine_j = J < 0 || C.level[J] == 0;
      if (!opt.fine_bonds && fine_i && fine_j) continue;  // evaluated by the caller
      if (!opt.moment_assembly || (I >= 0 && L.fixmask[i]) || (J >= 0 && L.fixmask[j])) {
        direct.push_back({a, i});
        continue;
      }
      const FaceKey key{I < 0 ? -1 : I, J < 0 ? -1 : J, a};
      i32 f;
      if (key == last) {
        f = last_f;
      } else {
        const auto [it, fresh] = face_of.emplace(key, static_cast<i32>(faces.size()));
        if (fresh) faces.push_back(Face{key.I, key.J, a});
        f = it->second;
        last = key;
        last_f = f;
      }
      Face& F = faces[size_t(f)];
      Vec6 k = L.bond_k(a, i);
      const f64 s = C.scale[a][i];
      // rho of the reference side (I, or J when there is no I): face centre - node point
      f64 rho[3];
      if (I >= 0) {
        for (int d = 0; d < 3; ++d) rho[d] = C.off[3 * size_t(i) + d];
        rho[a] += hh;
      } else {
        for (int d = 0; d < 3; ++d) rho[d] = C.off[3 * size_t(j) + d];
        rho[a] -= hh;
      }
      for (int q = 0; q < 6; ++q) {
        const f64 kq = k[q] * s;
        F.k0[q] += kq;
        if (q >= 3) continue;
        for (int d = 0; d < 3; ++d) {
          F.m1[q][d] += kq * rho[d];
          for (int e = 0; e < 3; ++e) F.m2[q][d * 3 + e] += kq * rho[d] * rho[e];
        }
      }
    }
  C.A = bb.finish_pattern();
  phase(4);
  // per-face blocks
  for (const Face& F : faces) {
    const int a = F.a;
    const int dir[3] = {a, (a + 1) % 3, (a + 2) % 3};
    const bool hasI = F.I >= 0, hasJ = F.J >= 0;
    f64* AII = hasI ? block_at(C.A, F.I, F.I) : nullptr;
    f64* AJJ = hasJ ? block_at(C.A, F.J, F.J) : nullptr;
    f64* AIJ = hasI && hasJ ? block_at(C.A, F.I, F.J) : nullptr;
    f64* AJI = hasI && hasJ ? block_at(C.A, F.J, F.I) : nullptr;
    f64 D[3] = {0, 0, 0};  // X_J - X_I: rho_J = rho_I - D
    if (hasI && hasJ)
      for (int d = 0; d < 3; ++d) D[d] = C.X[3 * size_t(F.J) + d] - C.X[3 * size_t(F.I) + d];
    for (int q = 0; q < 3; ++q) {  // translational springs along e_d
      const int d = dir[q], d1 = (d + 1) % 3, d2 = (d + 2) % 3;
      const f64 K0 = F.k0[q];
      const f64* m = F.m1[q];
      const f64* M = F.m2[q];
      // moments of both sides (the reference side's are m, M)
      f64 mI[3], MI[9], mJ[3], MJ[9], MIJ[9];
      if (hasI) {
        for (int x = 0; x < 3; ++x) mI[x] = m[x];
        for (int x = 0; x < 9; ++x) MI[x] = M[x];
        for (int x = 0; x < 3; ++x) mJ[x] = m[x] - K0 * D[x];
        for (int x = 0; x < 3; ++x)
          for (int y = 0; y < 3; ++y) {
            MIJ[x * 3 + y] = M[x * 3 + y] - m[x] * D[y];
            MJ[x * 3 + y] = M[x * 3 + y] - m[x] * D[y] - D[x] * m[y] + K0 * D[x] * D[y];
          }
      } else {
        for (int x = 0; x < 3; ++x) mJ[x] = m[x];
        for (int x = 0; x < 9; ++x) MJ[x] = M[x];
      }
      // w = rho x e_d: w[d] = 0, w[d1] = rho[d2], w[d2] = -rho[d1]
      auto wsum = [&](const f64* mm, f64* w) {
        w[d] = 0.0;
        w[d1] = mm[d2];
        w[d2] = -mm[d1];
      };
      auto wwsum = [&](const f64* MM, f64* W) {  // sum k w_x w_y^T from sum k rho_x rho_y^T
        for (int x = 0; x < 9; ++x) W[x] = 0.0;
        W[d1 * 3 + d1] = MM[d2 * 3 + d2];
        W[d1 * 3 + d2] = -MM[d2 * 3 + d1];
        W[d2 * 3 + d1] = -MM[d1 * 3 + d2];
        W[d2 * 3 + d2] = MM[d1 * 3 + d1];
      };
      f64 wI[3], wJ[3], WII[9], WJJ[9], WIJ[9];
      if (hasI) {
        wsum(mI, wI);
        wwsum(MI, WII);
        AII[d * 6 + d] += K0;
        for (int x = 0; x < 3; ++x) {
          AII[d * 6 + 3 + x] += wI[x];
          AII[(3 + x) * 6 + d] += wI[x];
          for (int y = 0; y < 3; ++y) AII[(3 + x) * 6 + 3 + y] += WII[x * 3 + y];
        }
      }
      if (hasJ) {
        wsum(mJ, wJ);
        wwsum(MJ, WJJ);
        AJJ[d * 6 + d] += K0;
        for (int x = 0; x < 3; ++x) {
          AJJ[d * 6 + 3 + x] += wJ[x];
          AJJ[(3 + x) * 6 + d] += wJ[x];
          for (int y = 0; y < 3; ++y) AJJ[(3 + x) * 6 + 3 + y] += WJJ[x * 3 + y];
        }
      }
      if (hasI && hasJ) {
        wwsum(MIJ, WIJ);
        AIJ[d * 6 + d] -= K0;
        AJI[d * 6 + d] -= K0;
        for (int x = 0; x < 3; ++x) {
          AIJ[d * 6 + 3 + x] -= wJ[x];
          AIJ[(3 + x) * 6 + d] -= wI[x];
          AJI[(3 + x) * 6 + d] -= wJ[x];
          AJI[d * 6 + 3 + x] -= wI[x];
          for (int y = 0; y < 3; ++y) {
            AIJ[(3 + x) * 6 + 3 + y] -= WIJ[x * 3 + y];
            AJI[(3 + y) * 6 + 3 + x] -= WIJ[x * 3 + y];
          }
        }
      }
    }
    for (int q = 3; q < 6; ++q) {  // rotational springs about e_d
      const int d = 3 + dir[q - 3];
      const f64 K0 = F.k0[q];
      if (hasI) AII[d * 6 + d] += K0;
      if (hasJ) AJJ[d * 6 + d] += K0;
      if (hasI && hasJ) {
        AIJ[d * 6 + d] -= K0;
        AJI[d * 6 + d] -= K0;
      }
    }
  }
  // per-bond Galerkin products (masked cells; the reference path)
  {
    f64 B[2][36], P[36], G[2][36];
    int ba = -1;
    for (const Direct& db : direct) {
      const int a = db.a;
      const i32 i = db.i, j = L.nbr[a][i];
      if (a != ba) {
        bond_side_matrices(a, hh, B[0], B[1]);
        ba = a;
      }
      const i32 I = C.node_of[i], J = C.node_of[j];
      Vec6 k = L.bond_k(a, i);
      const f64 s = C.scale[a][i];
      for (auto& kv : k) kv *= s;
      if (I >= 0) {
        rigid_block(&C.off[3 * size_t(i)], P);
        mask_rows(L.fixmask[i], P);
        mm6(B[0], P, G[0]);
        add_gtkg(G[0], k.data(), G[0], block_at(C.A, I, I));
      }
      if (J >= 0) {
        rigid_block(&C.off[3 * size_t(j)], P);
        mask_rows(L.fixmask[j], P);
        mm6(B[1], P, G[1]);
        add_gtkg(G[1], k.data(), G[1], block_at(C.A, J, J));
      }
      if (I >= 0 && J >= 0) {
        add_gtkg(G[0], k.data(), G[1], block_at(C.A, I, J));
        add_gtkg(G[1], k.data(), G[0], block_at(C.A, J, I));
      }
    }
  }
  f64 P[36];
  // support springs
  if (!L.spring.empty())
    for (i32 i = 0; i < n; ++i) {
      const i32 I = C.node_of[i];
      if (I < 0) continue;
      f64 Sd[36] = {};
      bool any = false;
      for (int q = 0; q < 6; ++q)
        if (!((L.fixmask[i] >> q) & 1) && L.spring[i][q] != 0.0) {
          Sd[q * 6 + q] = L.spring[i][q];
          any = true;
        }
      if (!any) continue;
      rigid_block(&C.off[3 * size_t(i)], P);
      add_ptaq(P, Sd, P, 1.0, block_at(C.A, I, I));
    }
  phase(5);
  // 6. St-Venant torsion correction of coarse faces
  if (opt.torsion) {
    struct Face {
      i64 kI, kJ;
      int a;
      i32 cnt = 0;
      f64 cen[3] = {0, 0, 0};
      f64 s_sum = 0.0, ks_sum = 0.0, gal = 0.0, G = 0.0;
    };
    std::vector<Face> faces;
    struct FaceKey {
      i64 kI, kJ;
      int a;
      bool operator==(const FaceKey& o) const { return kI == o.kI && kJ == o.kJ && a == o.a; }
    };
    struct FaceHash {
      size_t operator()(const FaceKey& k) const {
        u64 x = u64(k.kI) * 0x9E3779B97F4A7C15ull;
        x ^= u64(k.kJ) * 0xC2B2AE3D27D4EB4Full + (x << 6) + (x >> 2);
        x ^= u64(k.a) * 0x165667B19E3779F9ull;
        return static_cast<size_t>(x ^ (x >> 29));
      }
    };
    std::unordered_map<FaceKey, i32, FaceHash> face_of;
    auto node_multi = [&](i32 k) { return k >= 0 && C.mptr[k + 1] - C.mptr[k] > 1; };
    struct BF {
      i32 i;
      int a;
      i32 f;
    };
    std::vector<BF> bfs;
    for (int a = 0; a < 3; ++a)
      for (i32 i = 0; i < n; ++i) {
        const i32 j = L.nbr[a][i];
        if (j < 0) continue;
        const i32 I = C.node_of[i], J = C.node_of[j];
        if (I >= 0 && I == J) continue;
        if (!node_multi(I) && !node_multi(J)) continue;
        const i64 kI = I >= 0 ? I : -1 - i64(i);
        const i64 kJ = J >= 0 ? J : -1 - i64(j);
        const FaceKey key{kI, kJ, a};
        auto it = face_of.find(key);
        i32 f;
        if (it == face_of.end()) {
          f = static_cast<i32>(faces.size());
          faces.push_back(Face{kI, kJ, a});
          face_of.emplace(key, f);
        } else {
          f = it->second;
        }
        Face& F = faces[f];
        ++F.cnt;
        for (int d = 0; d < 3; ++d) F.cen[d] += h * (L.p[i][d] + (d == a ? 0.5 : 0.0));
        bfs.push_back({i, a, f});
      }
    for (Face& F : faces)
      for (int d = 0; d < 3; ++d) F.cen[d] /= F.cnt;
    std::vector<f64> Ip(faces.size(), 0.0);
    for (const BF& b : bfs) {
      Face& F = faces[b.f];
      const int a = b.a, t1 = (a + 1) % 3, t2 = (a + 2) % 3;
      const f64 r1 = h * (L.p[b.i][t1]) - F.cen[t1];
      const f64 r2 = h * (L.p[b.i][t2]) - F.cen[t2];
      const f64 s = C.scale[a][b.i];
      const Vec6 k = L.bond_k(a, b.i);
      F.s_sum += s;
      F.ks_sum += s * L.bond_scale(a, b.i);
      F.gal += s * (k[3] + k[1] * r2 * r2 + k[2] * r1 * r1);
      F.G += material(L.mat[b.i]).G;
      Ip[b.f] += (r1 * r1 + r2 * r2) * h * h;
    }
    for (size_t f = 0; f < faces.size(); ++f) {
      Face& F = faces[f];
      const f64 area = F.cnt * h * h;
      const f64 ip = Ip[f] + F.cnt * h * h * h * h / 6.0;
      const f64 Jsv = area * area * area * area / (4.0 * M_PI * M_PI * ip);
      const f64 Gm = F.G / F.cnt;
      const f64 target = Gm * Jsv / h * (F.ks_sum / F.cnt);
      const f64 c = std::min(target - F.gal, 0.0);
      if (c == 0.0) continue;
      const int q = 3 + F.a;
      struct Ent {
        i32 node;
        f64 g;
      };
      Ent ent[2];
      int ne = 0;
      if (F.kI >= 0) ent[ne++] = {static_cast<i32>(F.kI), -1.0};
      if (F.kJ >= 0) ent[ne++] = {static_cast<i32>(F.kJ), 1.0};
      for (int x = 0; x < ne; ++x)
        for (int y = 0; y < ne; ++y) block_at(C.A, ent[x].node, ent[y].node)[q * 6 + q] += c * ent[x].g * ent[y].g;
    }
  }
  phase(6);
  // 7. Galerkin mass: a rigid node's mass from its members' totals and moments about its point,
  // P^T diag(m, m, m, J) P = [[m I, m [o]x^T], [m [o]x, J + m (|o|^2 I - o o^T)]] summed
  // (members with masked DOFs take the per-cell product)
  C.M.assign(36 * size_t(N), 0.0);
  {
    std::vector<f64> mom(size_t(N) * 13, 0.0);  // m, m o (3), m o o^T (6: xx yy zz xy xz yz), J (3)
    for (i32 i = 0; i < n; ++i) {
      const i32 I = C.node_of[i];
      if (I < 0) continue;
      if (!opt.moment_assembly || L.fixmask[i]) {
        rigid_block(&C.off[3 * size_t(i)], P);
        mask_rows(L.fixmask[i], P);
        f64 Md[36] = {};
        Md[0] = Md[7] = Md[14] = L.mass[i];
        Md[21] = L.inertia[i][0];
        Md[28] = L.inertia[i][1];
        Md[35] = L.inertia[i][2];
        add_ptaq(P, Md, P, 1.0, &C.M[36 * size_t(I)]);
        continue;
      }
      const f64 m = L.mass[i];
      const f64* o = &C.off[3 * size_t(i)];
      f64* w = &mom[size_t(I) * 13];
      w[0] += m;
      w[1] += m * o[0];
      w[2] += m * o[1];
      w[3] += m * o[2];
      w[4] += m * o[0] * o[0];
      w[5] += m * o[1] * o[1];
      w[6] += m * o[2] * o[2];
      w[7] += m * o[0] * o[1];
      w[8] += m * o[0] * o[2];
      w[9] += m * o[1] * o[2];
      w[10] += L.inertia[i][0];
      w[11] += L.inertia[i][1];
      w[12] += L.inertia[i][2];
    }
    for (i32 I = 0; I < N; ++I) {
      const f64* w = &mom[size_t(I) * 13];
      if (w[0] == 0.0 && w[10] == 0.0 && w[11] == 0.0 && w[12] == 0.0) continue;
      f64* B = &C.M[36 * size_t(I)];
      const f64 m = w[0], sx = w[1], sy = w[2], sz = w[3];
      B[0] += m;
      B[7] += m;
      B[14] += m;
      // top right m [o]x^T = -m [o]x, bottom left m [o]x (sums of m o)
      const f64 S[9] = {0.0, -sz, sy, sz, 0.0, -sx, -sy, sx, 0.0};  // [s]x, s = sum m o
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
          B[r * 6 + 3 + c] += -S[r * 3 + c];
          B[(3 + r) * 6 + c] += S[r * 3 + c];
        }
      // bottom right: J + sum m (|o|^2 I - o o^T)
      const f64 xx = w[4], yy = w[5], zz = w[6], xy = w[7], xz = w[8], yz = w[9];
      B[21] += w[10] + yy + zz;
      B[28] += w[11] + xx + zz;
      B[35] += w[12] + xx + yy;
      B[22] += -xy;
      B[27] += -xy;
      B[23] += -xz;
      B[33] += -xz;
      B[29] += -yz;
      B[34] += -yz;
    }
  }
  // 8. Dirichlet DOFs of fine nodes: identity rows / columns
  for (i32 k = 0; k < N; ++k) {
    const u8 m = C.fix[k];
    if (!m) continue;
    for (i32 e = C.A.rowptr[k]; e < C.A.rowptr[k + 1]; ++e) {
      const i32 c = C.A.col[e];
      f64* Bkc = &C.A.val[36 * size_t(e)];
      f64* Bck = block_at(C.A, c, k);
      for (int q = 0; q < 6; ++q) {
        if (!((m >> q) & 1)) continue;
        for (int x = 0; x < 6; ++x) {
          Bkc[q * 6 + x] = 0.0;
          Bck[x * 6 + q] = 0.0;
        }
      }
    }
    f64* D = block_at(C.A, k, k);
    for (int q = 0; q < 6; ++q)
      if ((m >> q) & 1) D[q * 6 + q] = 1.0;
  }
  phase(7);
  return C;
}

void composite_prolong(const Lattice& L, const Composite& C, const f64* xc, f64* u) {
  parallel_for(L.n, 2048, [&](i64 b, i64 e) {
    for (i64 cc = b; cc < e; ++cc) {
      const i32 c = static_cast<i32>(cc);
      f64* uc = u + 6 * size_t(c);
      const i32 k = C.node_of[c];
      if (k < 0) {
        for (int q = 0; q < 6; ++q) uc[q] = 0.0;
        continue;
      }
      const f64* U = xc + 6 * size_t(k);
      const f64* o = &C.off[3 * size_t(c)];
      uc[0] = U[0] + U[4] * o[2] - U[5] * o[1];
      uc[1] = U[1] + U[5] * o[0] - U[3] * o[2];
      uc[2] = U[2] + U[3] * o[1] - U[4] * o[0];
      uc[3] = U[3];
      uc[4] = U[4];
      uc[5] = U[5];
      const u8 m = L.fixmask[c];
      if (m)
        for (int q = 0; q < 6; ++q)
          if ((m >> q) & 1) uc[q] = 0.0;
    }
  });
}

void composite_restrict(const Lattice& L, const Composite& C, const f64* f, f64* fc) {
  std::fill(fc, fc + 6 * size_t(C.n), 0.0);
  for (i32 k = 0; k < C.n; ++k) {
    f64* R = fc + 6 * size_t(k);
    for (i32 e = C.mptr[k]; e < C.mptr[k + 1]; ++e) {
      const i32 c = C.members[e];
      const f64* fcell = f + 6 * size_t(c);
      const f64* o = &C.off[3 * size_t(c)];
      R[0] += fcell[0];
      R[1] += fcell[1];
      R[2] += fcell[2];
      R[3] += fcell[3] + o[1] * fcell[2] - o[2] * fcell[1];
      R[4] += fcell[4] + o[2] * fcell[0] - o[0] * fcell[2];
      R[5] += fcell[5] + o[0] * fcell[1] - o[1] * fcell[0];
    }
    const u8 m = C.fix[k];
    if (m)
      for (int q = 0; q < 6; ++q)
        if ((m >> q) & 1) R[q] = 0.0;
  }
  (void)L;
}

void composite_apply(const Composite& C, const f64* x, f64* y, f64 mass_shift) {
  C.A.apply(x, y);
  if (mass_shift == 0.0) return;
  parallel_for(C.n, 2048, [&](i64 b, i64 e) {
    for (i64 r = b; r < e; ++r) {
      f64 t[6];
      mv6(&C.M[36 * size_t(r)], x + 6 * size_t(r), t);
      const u8 m = C.fix[r];
      for (int q = 0; q < 6; ++q)
        if (!((m >> q) & 1)) y[6 * size_t(r) + q] += mass_shift * t[q];
    }
  });
}

void add_sublattice_bonds(const Composite& C, const SubLattice& S, Bsr6& A) {
  const Lattice& F = S.F;
  const f64 hh = 0.5 * F.h;
  f64 B[2][36], Bf[2][36];
  auto node = [&](i32 k) -> i32 {
    if (F.anchored[k] || F.dead[k]) return -1;
    return C.node_of[S.to_world[k]];
  };
  for (int a = 0; a < 3; ++a) {
    bond_side_matrices(a, hh, B[0], B[1]);
    // the side matrices' nonzeros per row (at most two): G^T diag(k) G from them directly
    struct Nz {
      int n = 0, col[2] = {0, 0};
      f64 v[2] = {0, 0};
    };
    Nz nz[2][6];
    for (int s = 0; s < 2; ++s)
      for (int q = 0; q < 6; ++q)
        for (int c = 0; c < 6; ++c)
          if (B[s][q * 6 + c] != 0.0) {
            Nz& z = nz[s][q];
            z.col[z.n] = c;
            z.v[z.n] = B[s][q * 6 + c];
            ++z.n;
          }
    for (i32 i = 0; i < F.n; ++i) {
      const i32 j = F.nbr[a][i];
      if (j < 0) continue;
      const i32 I = node(i), J = node(j);
      if (I < 0 && J < 0) continue;
      if (!F.framed() && !F.fixmask[i] && !F.fixmask[j]) {
        // unframed, unmasked: out += k_q (row q of G1)^T (row q of G2), rows of <= 2 nonzeros
        const Vec6 k = F.bond_k(a, i);
        auto add = [&](const Nz* g1, const Nz* g2, f64* out) {
          for (int q = 0; q < 6; ++q) {
            const Nz& x = g1[q];
            const Nz& y = g2[q];
            for (int p = 0; p < x.n; ++p)
              for (int r = 0; r < y.n; ++r) out[x.col[p] * 6 + y.col[r]] += x.v[p] * k[q] * y.v[r];
          }
        };
        if (I >= 0) add(nz[0], nz[0], block_at(A, I, I));
        if (J >= 0) add(nz[1], nz[1], block_at(A, J, J));
        if (I >= 0 && J >= 0) {
          add(nz[0], nz[1], block_at(A, I, J));
          add(nz[1], nz[0], block_at(A, J, I));
        }
        continue;
      }
      const f64* Bi = B[0];
      const f64* Bj = B[1];
      if (F.framed()) {
        bond_side_matrices_framed(a, &F.frame[a][Lattice::kFrameSize * size_t(i)], Bf[0], Bf[1]);
        Bi = Bf[0];
        Bj = Bf[1];
      }
      f64 Gi[36], Gj[36];
      std::memcpy(Gi, Bi, sizeof(Gi));
      std::memcpy(Gj, Bj, sizeof(Gj));
      // masked DOFs (partial supports) carry no stiffness
      for (int q = 0; q < 6; ++q) {
        if ((F.fixmask[i] >> q) & 1)
          for (int r = 0; r < 6; ++r) Gi[r * 6 + q] = 0.0;
        if ((F.fixmask[j] >> q) & 1)
          for (int r = 0; r < 6; ++r) Gj[r * 6 + q] = 0.0;
      }
      const Vec6 k = F.bond_k(a, i);
      if (I >= 0) add_gtkg(Gi, k.data(), Gi, block_at(A, I, I));
      if (J >= 0) add_gtkg(Gj, k.data(), Gj, block_at(A, J, J));
      if (I >= 0 && J >= 0) {
        add_gtkg(Gi, k.data(), Gj, block_at(A, I, J));
        add_gtkg(Gj, k.data(), Gi, block_at(A, J, I));
      }
    }
  }
  // keep the Dirichlet identity of partially fixed nodes clean
  for (i32 k = 0; k < C.n; ++k) {
    const u8 m = C.fix[k];
    if (!m) continue;
    f64* D = block_at(A, k, k);
    for (int q = 0; q < 6; ++q)
      if ((m >> q) & 1) D[q * 6 + q] = 1.0;
  }
}

void build_composite_mg(const Composite& C, Multigrid& mg, const MGOptions& opt) {
  MGOptions o = opt;
  o.greedy = true;
  mg.build_assembled(C.A, C.M, C.X, C.blk, C.fix, o);
}

}  // namespace svx
