#include "svx/sim/corot.hpp"

#include <algorithm>
#include <cmath>

#include "svx/base/dmath.hpp"
#include "svx/base/parallel.hpp"
#include "svx/base/work.hpp"

namespace svx {

namespace {

constexpr i64 kGrain = 2048;

inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

inline Quat normalize(Quat q) {
  const f64 n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  for (auto& v : q) v /= n;
  return q;
}

// Midpoint of the shortest arc between two unit quaternions (= slerp at t = 1/2).
inline Quat quat_mid(const Quat& a, Quat b) {
  const f64 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0.0)
    for (auto& v : b) v = -v;
  return normalize({a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]});
}

struct BondKin {
  Vec6 g;        // svx-order jump
  Quat qm;       // bond frame rotation (mid)
  Vec3 ri, rj;   // current lever arms a_i - x_i, a_j - x_j (world)
};

inline const f64* cell_u(const Lattice& L, const f64* u, i32 c, f64* buf) {
  static const f64 kZero[6] = {0, 0, 0, 0, 0, 0};
  if (L.anchored[c]) return L.u_fixed.empty() ? kZero : &L.u_fixed[6 * size_t(c)];
  return L.masked(u, c, buf);
}

// R(q) v - v without forming R v (no cancellation of |v|-sized terms for small rotations).
inline Vec3 rotate_delta(const Quat& q, const Vec3& v) {
  const Vec3 qv{q[0], q[1], q[2]};
  const Vec3 t = cross(qv, v);
  const Vec3 t2{2 * t[0], 2 * t[1], 2 * t[2]};
  const Vec3 c = cross(qv, t2);
  return {q[3] * t2[0] + c[0], q[3] * t2[1] + c[1], q[3] * t2[2] + c[2]};
}

BondKin kinematics(const Lattice& L, int a, i32 i, i32 j, const f64* u) {
  const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
  f64 bi[6], bj[6];
  const f64* ui = cell_u(L, u, i, bi);
  const f64* uj = cell_u(L, u, j, bj);
  const Quat qi = quat_from_rotvec(ui + 3);
  const Quat qj = quat_from_rotvec(uj + 3);
  BondKin k;
  k.qm = quat_mid(qi, qj);
  Vec3 rho{0, 0, 0};
  rho[a] = 0.5 * L.h;
  const Vec3 mrho{-rho[0], -rho[1], -rho[2]};
  const Vec3 dri = rotate_delta(qi, rho);   // R_i rho - rho
  const Vec3 drj = rotate_delta(qj, mrho);  // R_j (-rho) + rho
  for (int q = 0; q < 3; ++q) {
    k.ri[q] = rho[q] + dri[q];
    k.rj[q] = mrho[q] + drj[q];
  }
  // a_j - a_i = (x0_j - x0_i) + (u_j - u_i) + R_j(-rho) - R_i rho with x0_j - x0_i = h e_a = 2 rho:
  // the rest terms cancel exactly, leaving (u_j - u_i) + drj - dri.
  const Vec3 d{uj[0] - ui[0] + drj[0] - dri[0], uj[1] - ui[1] + drj[1] - dri[1], uj[2] - ui[2] + drj[2] - dri[2]};
  const Quat qmc = quat_conj(k.qm);
  const Vec3 tl = quat_rotate(qmc, d);
  // relative rotation, expressed in the bond frame
  f64 wb[3];
  rotvec_from_quat(quat_mul(quat_conj(qi), qj), wb);
  const Vec3 ww = quat_rotate(qi, {wb[0], wb[1], wb[2]});
  const Vec3 wl = quat_rotate(qmc, ww);
  k.g = {tl[a], tl[t1], tl[t2], wl[a], wl[t1], wl[t2]};
  return k;
}

}  // namespace

Quat quat_from_rotvec(const f64* th) {
  const f64 ang = std::sqrt(th[0] * th[0] + th[1] * th[1] + th[2] * th[2]);
  if (ang < 1e-12) return normalize({0.5 * th[0], 0.5 * th[1], 0.5 * th[2], 1.0});
  const f64 s = dm::sin(0.5 * ang) / ang;
  return {th[0] * s, th[1] * s, th[2] * s, dm::cos(0.5 * ang)};
}

void rotvec_from_quat(const Quat& q0, f64* th) {
  Quat q = q0;
  if (q[3] < 0.0)
    for (auto& v : q) v = -v;
  const f64 sv = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
  if (sv < 1e-12) {
    th[0] = 2.0 * q[0];
    th[1] = 2.0 * q[1];
    th[2] = 2.0 * q[2];
    return;
  }
  const f64 ang = 2.0 * dm::atan2(sv, q[3]);
  const f64 s = ang / sv;
  th[0] = q[0] * s;
  th[1] = q[1] * s;
  th[2] = q[2] * s;
}

Quat quat_mul(const Quat& a, const Quat& b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Quat quat_conj(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }

Vec3 quat_rotate(const Quat& q, const Vec3& v) {
  // v' = v + 2 w (q x v) + 2 q x (q x v)
  const Vec3 qv{q[0], q[1], q[2]};
  const Vec3 t = cross(qv, v);
  const Vec3 t2{2 * t[0], 2 * t[1], 2 * t[2]};
  const Vec3 c = cross(qv, t2);
  return {v[0] + q[3] * t2[0] + c[0], v[1] + q[3] * t2[1] + c[1], v[2] + q[3] * t2[2] + c[2]};
}

Vec6 bond_jump(const Lattice& L, int a, i32 i, const f64* u, bool corot) {
  const i32 j = L.nbr[a][i];
  if (j < 0) return Vec6{0, 0, 0, 0, 0, 0};
  if (!corot) {
    const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
    const f64 hh = 0.5 * L.h;
    f64 bi[6], bj[6];
    const f64* ui = cell_u(L, u, i, bi);
    const f64* uj = cell_u(L, u, j, bj);
    return {uj[a] - ui[a],
            uj[t1] - ui[t1] - hh * (ui[3 + t2] + uj[3 + t2]),
            uj[t2] - ui[t2] + hh * (ui[3 + t1] + uj[3 + t1]),
            uj[3 + a] - ui[3 + a],
            uj[3 + t1] - ui[3 + t1],
            uj[3 + t2] - ui[3 + t2]};
  }
  return kinematics(L, a, i, j, u).g;
}

namespace {

// One cell's internal force: its bonds gathered in a fixed order (axes, lower side first), so
// any subset of cells reproduces internal_forces bit for bit.
void cell_force(const Lattice& L, const f64* u, bool corot, i32 c, f64* fc) {
  for (int q = 0; q < 6; ++q) fc[q] = 0.0;
  if (L.anchored[c]) return;
  if (!corot) {
    f64 fi[6], fj[6];
    for (int a = 0; a < 3; ++a) {
      if (L.nbr[a][c] >= 0) {
        bond_internal_forces(L, a, c, u, false, fi, fj);
        for (int q = 0; q < 6; ++q) fc[q] += fi[q];
      }
      const i32 m = L.nbrm[a][c];
      if (m >= 0) {
        bond_internal_forces(L, a, m, u, false, fi, fj);
        for (int q = 0; q < 6; ++q) fc[q] += fj[q];
      }
    }
  } else {
    for (int a = 0; a < 3; ++a) {
      const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
      for (int side = 0; side < 2; ++side) {
        const i32 lo = side == 0 ? c : L.nbrm[a][c];
        if (lo < 0) continue;
        const i32 hi = L.nbr[a][lo];
        if (hi < 0) continue;
        const BondKin k = kinematics(L, a, lo, hi, u);
        const Vec6 kk = L.bond_k(a, lo);
        Vec3 sl{0, 0, 0}, ml{0, 0, 0};
        sl[a] = kk[0] * k.g[0];
        sl[t1] = kk[1] * k.g[1];
        sl[t2] = kk[2] * k.g[2];
        ml[a] = kk[3] * k.g[3];
        ml[t1] = kk[4] * k.g[4];
        ml[t2] = kk[5] * k.g[5];
        const Vec3 f = quat_rotate(k.qm, sl);
        const Vec3 m = quat_rotate(k.qm, ml);
        if (side == 0) {  // c is the lower cell i: gets -f at its attachment, couple -m
          const Vec3 mf = cross(k.ri, f);
          for (int q = 0; q < 3; ++q) {
            fc[q] -= f[q];
            fc[3 + q] += -m[q] - mf[q];
          }
        } else {  // c is the upper cell j: +f at its attachment, couple +m
          const Vec3 mf = cross(k.rj, f);
          for (int q = 0; q < 3; ++q) {
            fc[q] += f[q];
            fc[3 + q] += m[q] + mf[q];
          }
        }
      }
    }
  }
  if (!L.spring.empty()) {
    f64 buf[6];
    const f64* uc = L.masked(u, c, buf);
    for (int q = 0; q < 6; ++q) fc[q] += L.spring[c][q] * uc[q];
  }
  if (L.fixmask[c])
    for (int q = 0; q < 6; ++q)
      if ((L.fixmask[c] >> q) & 1) fc[q] = 0.0;
}

}  // namespace

void internal_forces(const Lattice& L, const f64* u, bool corot, f64* f_int) {
  add_work(i64(L.n) * kWorkForces);  // (deterministic work accounting: counted as it starts)
  if (!corot && L.u_fixed.empty() && !L.framed()) {
    apply_stiffness(L, u, f_int);
    return;
  }
  parallel_for(L.n, kGrain, [&](i64 b, i64 e) {
    for (i64 cc = b; cc < e; ++cc) cell_force(L, u, corot, static_cast<i32>(cc), f_int + 6 * size_t(cc));
  });
}

void internal_forces_cells(const Lattice& L, const f64* u, bool corot, std::span<const i32> cells, f64* f_int) {
  for (i32 c : cells) cell_force(L, u, corot, c, f_int + 6 * size_t(c));
}

void bond_internal_forces(const Lattice& L, int a, i32 i, const f64* u, bool corot, f64 fi[6], f64 fj[6]) {
  for (int q = 0; q < 6; ++q) fi[q] = fj[q] = 0.0;
  const i32 j = L.nbr[a][i];
  if (j < 0) return;
  const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
  const Vec6 kk = L.bond_k(a, i);
  if (!corot) {
    const Vec6 g = bond_jump(L, a, i, u, false);
    Vec6 F;
    for (int q = 0; q < 6; ++q) F[q] = kk[q] * g[q];
    const f64 hh = 0.5 * L.h;
    fi[a] = -F[0];
    fi[t1] = -F[1];
    fi[t2] = -F[2];
    fi[3 + a] = -F[3];
    fi[3 + t1] = -F[4] + hh * F[2];
    fi[3 + t2] = -F[5] - hh * F[1];
    fj[a] = F[0];
    fj[t1] = F[1];
    fj[t2] = F[2];
    fj[3 + a] = F[3];
    fj[3 + t1] = F[4] + hh * F[2];
    fj[3 + t2] = F[5] - hh * F[1];
    return;
  }
  const BondKin k = kinematics(L, a, i, j, u);
  Vec3 sl{0, 0, 0}, ml{0, 0, 0};
  sl[a] = kk[0] * k.g[0];
  sl[t1] = kk[1] * k.g[1];
  sl[t2] = kk[2] * k.g[2];
  ml[a] = kk[3] * k.g[3];
  ml[t1] = kk[4] * k.g[4];
  ml[t2] = kk[5] * k.g[5];
  const Vec3 f = quat_rotate(k.qm, sl);
  const Vec3 m = quat_rotate(k.qm, ml);
  const Vec3 mfi = cross(k.ri, f);
  const Vec3 mfj = cross(k.rj, f);
  for (int q = 0; q < 3; ++q) {
    fi[q] = -f[q];
    fi[3 + q] = -m[q] - mfi[q];
    fj[q] = f[q];
    fj[3 + q] = m[q] + mfj[q];
  }
}

void apply_increment(const Lattice& L, f64* u, const f64* du, f64 scale, bool corot) {
  parallel_for(L.n, kGrain, [&](i64 b, i64 e) {
    for (i64 cc = b; cc < e; ++cc) {
      const size_t o = 6 * size_t(cc);
      if (L.anchored[cc]) continue;
      for (int q = 0; q < 3; ++q) u[o + q] += scale * du[o + q];
      if (!corot) {
        for (int q = 3; q < 6; ++q) u[o + q] += scale * du[o + q];
      } else {
        const f64 dth[3] = {scale * du[o + 3], scale * du[o + 4], scale * du[o + 5]};
        const Quat q = quat_mul(quat_from_rotvec(dth), quat_from_rotvec(u + o + 3));
        rotvec_from_quat(q, u + o + 3);
      }
      const u8 m = L.fixmask[cc];
      if (m)
        for (int q = 0; q < 6; ++q)
          if ((m >> q) & 1) u[o + q] = 0.0;
    }
  });
}

f64 max_rotation(const Lattice& L, const f64* u) {
  f64 m = 0.0;
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i]) continue;
    const f64* t = u + 6 * size_t(i) + 3;
    m = std::max(m, std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]));
  }
  return m;
}

void compute_frames(Lattice& L, const f64* u) {
  constexpr int FS = Lattice::kFrameSize;
  for (int a = 0; a < 3; ++a) L.frame[a].assign(FS * size_t(L.n), 0.0);
  parallel_for(L.n, kGrain, [&](i64 b, i64 e) {
    for (i64 cc = b; cc < e; ++cc) {
      const i32 i = static_cast<i32>(cc);
      for (int a = 0; a < 3; ++a) {
        const i32 j = L.nbr[a][i];
        if (j < 0) continue;
        const BondKin k = kinematics(L, a, i, j, u);
        const f64 x = k.qm[0], y = k.qm[1], z = k.qm[2], w = k.qm[3];
        f64* f = &L.frame[a][FS * size_t(i)];
        f[0] = 1 - 2 * (y * y + z * z);
        f[1] = 2 * (x * y - z * w);
        f[2] = 2 * (x * z + y * w);
        f[3] = 2 * (x * y + z * w);
        f[4] = 1 - 2 * (x * x + z * z);
        f[5] = 2 * (y * z - x * w);
        f[6] = 2 * (x * z - y * w);
        f[7] = 2 * (y * z + x * w);
        f[8] = 1 - 2 * (x * x + y * y);
        for (int q = 0; q < 3; ++q) {
          f[9 + q] = k.ri[q];
          f[12 + q] = k.rj[q];
        }
      }
    }
  });
}

void clear_frames(Lattice& L) {
  for (int a = 0; a < 3; ++a) std::vector<f64>().swap(L.frame[a]);
}

bool update_frames(Lattice& L, const f64* u, f64 threshold) {
  if (max_rotation(L, u) > threshold) {
    compute_frames(L, u);
    return true;
  }
  if (L.framed()) clear_frames(L);
  return false;
}

}  // namespace svx
