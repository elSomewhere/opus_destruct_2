#include "svx/sim/blast.hpp"

#include <algorithm>
#include <cmath>

#include "svx/base/dmath.hpp"
#include "svx/sim/statics.hpp"

namespace svx {

std::array<f64, 3> cell_position(const Lattice& L, const f64* u, i32 i) {
  std::array<f64, 3> x{L.h * L.p[i][0], L.h * L.p[i][1], L.h * L.p[i][2]};
  if (u && !L.anchored[i])
    for (int q = 0; q < 3; ++q) x[q] += u[6 * size_t(i) + q];
  return x;
}

namespace {

inline f64 dist3(const std::array<f64, 3>& a, const std::array<f64, 3>& b) {
  const f64 dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

f64 damage_radius(const Lattice& L, const BlastParams& bp) {
  return std::max(bp.radius + bp.damage_radius_pad * L.h, bp.radius * bp.damage_radius_scale);
}

}  // namespace

BlastResult apply_blast(Lattice& L, const f64* u, const BlastParams& bp) {
  BlastResult res;
  const f64 r = bp.radius;
  const f64 dR = damage_radius(L, bp);
  res.damage_radius = dR;
  if (!bp.prefracture) return res;
  L.enable_damage();
  std::vector<u8> seed(L.n, 0);
  auto mark_neighbors = [&](i32 c) {
    for (int a = 0; a < 3; ++a) {
      if (L.nbr[a][c] >= 0) seed[L.nbr[a][c]] = 1;
      if (L.nbrm[a][c] >= 0) seed[L.nbrm[a][c]] = 1;
    }
  };
  // Positions are taken before any mutation (the prototype reads xCommit).
  if (bp.remove_core) {
    for (i32 i = 0; i < L.n; ++i) {
      if (L.dead[i]) continue;
      if (dist3(cell_position(L, u, i), bp.center) <= r) res.removed.push_back(i);
    }
    for (i32 c : res.removed) {
      if (L.support(c)) res.supports_removed = true;
      mark_neighbors(c);
      L.remove_cell(c);
    }
  }
  const f64 frac = r + bp.fracture_padding * L.h;
  for (i32 i = 0; i < L.n; ++i) {
    if (L.dead[i]) continue;
    const auto xi = cell_position(L, u, i);
    for (int a = 0; a < 3; ++a) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || L.dead[j]) continue;
      const auto xj = cell_position(L, u, j);
      const std::array<f64, 3> mid{0.5 * (xi[0] + xj[0]), 0.5 * (xi[1] + xj[1]), 0.5 * (xi[2] + xj[2])};
      const f64 d = dist3(mid, bp.center);
      if (d <= frac) {
        L.break_bond(i, a);
        res.fractured.push_back(bond_key(L, a, i));
        seed[i] = seed[j] = 1;
        continue;
      }
      if (d > dR) continue;
      const f64 t = std::max(0.0, 1.0 - (d - r) / std::max(dR - r, 1e-9));
      const f64 nd = bp.damage_peak * t;
      if (nd > f64(L.dmg[a][i]) + 1e-12) {
        L.dmg[a][i] = static_cast<f32>(nd);
        res.damaged.push_back(bond_key(L, a, i));
        seed[i] = seed[j] = 1;
      }
    }
  }
  for (i32 i = 0; i < L.n; ++i)
    if (seed[i] && !L.dead[i]) res.seeds.push_back(i);
  return res;
}

void add_blast_impulse(const Lattice& L, const f64* u, const BlastParams& bp, f64* f) {
  const f64 dR = damage_radius(L, bp);
  const f64 rimp = std::max(bp.radius, dR);
  const f64 broad = rimp * bp.impulse_radius_scale + L.h;
  const f64 strength = bp.impulse_strength >= 0.0 ? bp.impulse_strength : std::max(1.0, bp.radius / L.h);
  for (i32 i = 0; i < L.n; ++i) {
    if (L.dead[i] || L.anchored[i]) continue;
    const auto x = cell_position(L, u, i);
    const std::array<f64, 3> dv{x[0] - bp.center[0], x[1] - bp.center[1], x[2] - bp.center[2]};
    const f64 d = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
    if (d > broad) continue;
    std::array<f64, 3> dir{0, 0, 1};
    if (d > 1e-9) dir = {dv[0] / d, dv[1] / d, dv[2] / d};
    const f64 t = std::max(0.0, 1.0 - d / std::max(broad, 1e-9));
    const f64 load = bp.impulse_scale * strength * L.mass[i] * dm::pow(t, bp.falloff_power);
    f64* fi = f + 6 * size_t(i);
    for (int q = 0; q < 3; ++q) fi[q] += dir[q] * load;
    if (bp.angular_strength > 0.0) {
      // spin axis = normalize(dir x z), fallback +y
      std::array<f64, 3> ax{dir[1], -dir[0], 0.0};
      const f64 an = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1]);
      if (an > 1e-12) {
        ax[0] /= an;
        ax[1] /= an;
      } else {
        ax = {0, 1, 0};
      }
      const f64 tq = load * L.h * bp.angular_strength;
      for (int q = 0; q < 3; ++q) fi[3 + q] += ax[q] * tq;
    }
    const u8 m = L.fixmask[i];
    if (m)
      for (int q = 0; q < 6; ++q)
        if ((m >> q) & 1) fi[q] = 0.0;
  }
}

}  // namespace svx
