#include "svx/solve/lattice.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <tuple>

#include "svx/base/parallel.hpp"

namespace svx {

namespace {

constexpr i64 kGrain = 4096;

// Open-addressing hash from packed lattice coordinates to cell index.
struct CoordHash {
  std::vector<u64> keys;
  std::vector<i32> vals;
  u64 mask = 0;
  static constexpr u64 kEmpty = ~u64(0);

  static u64 pack(const std::array<i32, 3>& p) {
    // 21 bits per axis, offset to keep coordinates non-negative.
    const u64 o = u64(1) << 20;
    return ((u64(p[0]) + o) & 0x1FFFFF) | (((u64(p[1]) + o) & 0x1FFFFF) << 21) |
           (((u64(p[2]) + o) & 0x1FFFFF) << 42);
  }
  static u64 mix(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
  }
  explicit CoordHash(size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    keys.assign(cap, kEmpty);
    vals.assign(cap, -1);
    mask = cap - 1;
  }
  void insert(const std::array<i32, 3>& p, i32 v) {
    const u64 k = pack(p);
    u64 s = mix(k) & mask;
    while (keys[s] != kEmpty && keys[s] != k) s = (s + 1) & mask;
    keys[s] = k;
    vals[s] = v;
  }
  i32 find(const std::array<i32, 3>& p) const {
    const u64 k = pack(p);
    u64 s = mix(k) & mask;
    while (keys[s] != kEmpty) {
      if (keys[s] == k) return vals[s];
      s = (s + 1) & mask;
    }
    return -1;
  }
};

}  // namespace

i32 Lattice::num_free() const {
  i32 c = 0;
  for (u8 a : anchored) c += a ? 0 : 1;
  return c;
}

i64 Lattice::num_bonds() const {
  i64 c = 0;
  for (int a = 0; a < 3; ++a)
    for (i32 j : nbr[a]) c += j >= 0 ? 1 : 0;
  return c;
}

const f64* Lattice::masked(const f64* u, i32 i, f64* out) const {
  const f64* ui = u + 6 * size_t(i);
  const u8 m = fixmask[i];
  if (m == 0) return ui;
  for (int q = 0; q < 6; ++q) out[q] = (m >> q) & 1 ? 0.0 : ui[q];
  return out;
}

void Lattice::break_bond(i32 i, int axis) {
  const i32 j = nbr[axis][i];
  if (j < 0) return;
  nbr[axis][i] = -1;
  nbrm[axis][j] = -1;
  if (!cracked[axis].empty()) cracked[axis][i] = 0;
}

void Lattice::crack_bond(i32 i, int axis) {
  if (nbr[axis][i] < 0) return;
  if (cracked[axis].empty()) {
    cracked[axis].assign(n, 0);
    cscale[axis].assign(n, Vec6{1, 1, 1, 1, 1, 1});
  }
  cracked[axis][i] = 1;
  cscale[axis][i] = {1, 1, 1, 1, 1, 1};  // closed (stick) until the next law sweep
}

bool Lattice::support(i32 i) const {
  if (dead[i]) return false;
  if (fixmask[i]) return true;
  if (!spring.empty())
    for (f64 s : spring[i])
      if (s > 0.0) return true;
  return false;
}

void Lattice::enable_damage() {
  for (int a = 0; a < 3; ++a)
    if (dmg[a].empty()) dmg[a].assign(n, 0.0f);
}

void Lattice::remove_cell(i32 i) {
  for (int a = 0; a < 3; ++a) {
    break_bond(i, a);
    const i32 m = nbrm[a][i];
    if (m >= 0) break_bond(m, a);
  }
  dead[i] = 1;
  fixmask[i] = kAllDofs;
  anchored[i] = 1;
  mass[i] = 0.0;
}

i32 Lattice::num_alive() const {
  i32 c = 0;
  for (u8 d : dead) c += d ? 0 : 1;
  return c;
}

Lattice build_lattice(std::span<const CellIn> cells, const LatticeOptions& opt) {
  Lattice L;
  L.h = opt.h;
  L.n = static_cast<i32>(cells.size());
  L.p.resize(L.n);
  L.mat.resize(L.n);
  L.anchored.resize(L.n);
  L.fixmask.resize(L.n);
  L.eff.resize(L.n);
  L.mass.resize(L.n);
  L.inertia.resize(L.n);
  L.arch.assign(L.n, -1);
  L.strength.assign(L.n, 0);
  L.dead.assign(L.n, 0);
  L.fixes = opt.fixes;
  L.law = opt.law;
  L.contact = opt.contact;
  CoordHash hash(cells.size());
  for (i32 i = 0; i < L.n; ++i) {
    const CellIn& c = cells[i];
    L.p[i] = c.p;
    L.mat[i] = c.mat;
    L.fixmask[i] = c.anchored ? kAllDofs : static_cast<u8>(c.fixmask & kAllDofs);
    L.anchored[i] = L.fixmask[i] == kAllDofs ? 1 : 0;
    bool any_spring = false;
    for (f64 s : c.spring) any_spring = any_spring || s != 0.0;
    if (any_spring) {
      if (L.spring.empty()) L.spring.assign(cells.size(), Vec6{0, 0, 0, 0, 0, 0});
      L.spring[i] = c.spring;
    }
    L.eff[i] = c.eff;
    L.strength[i] = c.strength;
    if (c.arch >= 0) {
      const CellArchetype& ca = archetype(c.arch);
      L.arch[i] = c.arch;
      L.mat[i] = ca.mat;
      L.eff[i] = ca.eff;
    }
    const f64 sx = L.eff[i][0] * L.h, sy = L.eff[i][1] * L.h, sz = L.eff[i][2] * L.h;
    const f64 m = material(L.mat[i]).rho * sx * sy * sz;
    L.mass[i] = m;
    L.inertia[i] = {m / 12.0 * (sy * sy + sz * sz), m / 12.0 * (sx * sx + sz * sz), m / 12.0 * (sx * sx + sy * sy)};
    hash.insert(c.p, i);
  }
  // raw face neighbours (+a and -a, whatever their state): one hash lookup per cell and axis
  std::array<std::vector<i32>, 3> up, down;
  for (int a = 0; a < 3; ++a) {
    up[a].assign(L.n, -1);
    down[a].assign(L.n, -1);
  }
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      std::array<i32, 3> q = L.p[i];
      q[a] += 1;
      const i32 j = hash.find(q);
      up[a][i] = j;
      if (j >= 0) down[a][j] = i;
    }
  // plate classification: bounded run lengths of same-material free cells through each cell
  if (opt.plate.enabled) {
    L.plate.assign(L.n, 0);
    const i32 tmax = opt.plate.max_thickness, wmin = opt.plate.min_width;
    auto run = [&](i32 i, int a, i32 cap) {
      i32 len = 1;
      for (int sg = -1; sg <= 1 && len < cap; sg += 2) {
        const std::vector<i32>& step = sg > 0 ? up[a] : down[a];
        i32 c = i;
        for (;;) {
          const i32 j = step[c];
          if (j < 0 || L.anchored[j] || L.mat[j] != L.mat[i]) break;
          c = j;
          if (++len >= cap) break;
        }
      }
      return len;
    };
    for (i32 i = 0; i < L.n; ++i) {
      if (cells[i].plate != kPlateAuto) {
        L.plate[i] = cells[i].plate;
        continue;
      }
      if (L.anchored[i]) continue;
      for (int n = 0; n < 3; ++n) {
        if (run(i, n, tmax + 1) > tmax) continue;
        if (run(i, (n + 1) % 3, wmin) < wmin || run(i, (n + 2) % 3, wmin) < wmin) continue;
        L.plate[i] = static_cast<u8>(1 + n);
        break;
      }
    }
  }
  using Key = std::tuple<int, int, int, int, f64, f64, f64, f64, f64, f64, int, int>;
  std::map<Key, u16> model_ids;
  for (int a = 0; a < 3; ++a) {
    L.nbr[a].assign(L.n, -1);
    L.nbrm[a].assign(L.n, -1);
    L.bid[a].assign(L.n, 0);
  }
  // (the last model looked up: neighbouring bonds nearly always share it)
  bool have_last = false;
  Key last_key{};
  u16 last_id = 0;
  for (int a = 0; a < 3; ++a) {
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = up[a][i];
      if (j < 0) continue;
      if (L.anchored[i] && L.anchored[j]) continue;
      L.nbr[a][i] = j;
      L.nbrm[a][j] = i;
      const Profile prof = (L.arch[i] >= 0 && L.arch[j] >= 0) ? resolve_profile(archetype(L.arch[i]), archetype(L.arch[j]))
                                                            : default_profile(L.mat[i], L.mat[j]);
      const u8 cls = std::min(L.strength[i], L.strength[j]);
      // an in-plane bond of a plate (both cells in the same plate orientation, not across it)
      const bool plate = !L.plate.empty() && L.plate[i] != 0 && L.plate[i] == L.plate[j] && L.plate[i] != 1 + a;
      const Key key{a, static_cast<int>(prof), static_cast<int>(L.mat[i]), static_cast<int>(L.mat[j]), L.eff[i][0],
                    L.eff[i][1], L.eff[i][2], L.eff[j][0], L.eff[j][1], L.eff[j][2], static_cast<int>(cls), plate ? 1 : 0};
      if (have_last && key == last_key) {
        L.bid[a][i] = last_id;
        continue;
      }
      auto it = model_ids.find(key);
      if (it == model_ids.end()) {
        SVX_ASSERT(L.models.size() < 65535);
        const u16 id = static_cast<u16>(L.models.size());
        LawParams law = opt.law;
        const f64 sm = strength_multiplier(cls);
        const f64 mult = sm * (plate ? opt.plate.capacity : 1.0);
        law.fragility *= mult;     // game law: capacity scale
        law.toughness *= sm * sm;  // (a design class scales the whole softening curve)
        BondModel bm = make_bond(a, L.mat[i], L.eff[i], L.mat[j], L.eff[j], L.h, prof, opt.fixes, law);
        if (plate) {
          const f64 pk = 1.0 / (1.0 - opt.plate.poisson * opt.plate.poisson);
          for (auto& v : bm.k) v *= pk;
        }
        if (!law.game && mult != 1.0) {
          // reference law: scale capacities and thresholds directly
          bm.cap.Nt *= mult;
          bm.cap.Nc *= mult;
          bm.cap.V *= mult;
          bm.cap.T *= mult;
          bm.cap.M1 *= mult;
          bm.cap.M2 *= mult;
          bm.onset.open *= mult;
          bm.onset.comp *= mult;
          bm.brk.open *= mult;
          bm.brk.comp *= mult;
          for (int q = 1; q < 6; ++q) {
            bm.onset.comp6[q] *= mult;
            bm.brk.comp6[q] *= mult;
          }
        }
        L.models.push_back(bm);
        it = model_ids.emplace(key, id).first;
      }
      L.bid[a][i] = it->second;
      have_last = true;
      last_key = key;
      last_id = it->second;
    }
  }
  return L;
}

void bond_side_matrices(int a, f64 hh, f64 Bi[36], f64 Bj[36]) {
  const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
  std::memset(Bi, 0, sizeof(f64) * 36);
  std::memset(Bj, 0, sizeof(f64) * 36);
  auto set = [](f64* B, int r, int c, f64 v) { B[r * 6 + c] += v; };
  // g0 = uj[a] - ui[a]
  set(Bi, 0, a, -1.0);
  set(Bj, 0, a, 1.0);
  // g1 = uj[t1] - ui[t1] - hh (thi[t2] + thj[t2])
  set(Bi, 1, t1, -1.0);
  set(Bj, 1, t1, 1.0);
  set(Bi, 1, 3 + t2, -hh);
  set(Bj, 1, 3 + t2, -hh);
  // g2 = uj[t2] - ui[t2] + hh (thi[t1] + thj[t1])
  set(Bi, 2, t2, -1.0);
  set(Bj, 2, t2, 1.0);
  set(Bi, 2, 3 + t1, hh);
  set(Bj, 2, 3 + t1, hh);
  // g3..g5 = thj - thi along (a, t1, t2)
  set(Bi, 3, 3 + a, -1.0);
  set(Bj, 3, 3 + a, 1.0);
  set(Bi, 4, 3 + t1, -1.0);
  set(Bj, 4, 3 + t1, 1.0);
  set(Bi, 5, 3 + t2, -1.0);
  set(Bj, 5, 3 + t2, 1.0);
}

void bond_side_matrices_framed(int a, const f64* fr, f64 Bi[36], f64 Bj[36]) {
  const int ax[3] = {a, (a + 1) % 3, (a + 2) % 3};
  const f64* R = fr;       // row-major 3x3
  const f64* ri = fr + 9;  // lever arms (world)
  const f64* rj = fr + 12;
  std::memset(Bi, 0, sizeof(f64) * 36);
  std::memset(Bj, 0, sizeof(f64) * 36);
  for (int r = 0; r < 3; ++r) {
    const f64 e[3] = {R[0 * 3 + ax[r]], R[1 * 3 + ax[r]], R[2 * 3 + ax[r]]};  // local axis in world
    // translation row: e . (du_j + dth_j x r_j - du_i - dth_i x r_i)
    const f64 ci[3] = {e[1] * ri[2] - e[2] * ri[1], e[2] * ri[0] - e[0] * ri[2], e[0] * ri[1] - e[1] * ri[0]};
    const f64 cj[3] = {e[1] * rj[2] - e[2] * rj[1], e[2] * rj[0] - e[0] * rj[2], e[0] * rj[1] - e[1] * rj[0]};
    for (int c = 0; c < 3; ++c) {
      Bi[r * 6 + c] = -e[c];
      Bj[r * 6 + c] = e[c];
      Bi[r * 6 + 3 + c] = ci[c];   // (e x r_i)
      Bj[r * 6 + 3 + c] = -cj[c];  // -(e x r_j)
      // rotation row: e . (dth_j - dth_i)
      Bi[(3 + r) * 6 + 3 + c] = -e[c];
      Bj[(3 + r) * 6 + 3 + c] = e[c];
    }
  }
}

namespace {

// Corotated tangent y = K_t u (gather form, as apply_stiffness).
void apply_stiffness_framed(const Lattice& L, const f64* u, f64* y) {
  parallel_for(L.n, kGrain, [&](i64 b, i64 e) {
    static const f64 kZero[6] = {0, 0, 0, 0, 0, 0};
    for (i64 ii = b; ii < e; ++ii) {
      const i32 c = static_cast<i32>(ii);
      f64* yc = y + 6 * size_t(c);
      for (int q = 0; q < 6; ++q) yc[q] = 0.0;
      if (L.anchored[c]) continue;
      f64 cbuf[6], obuf[6];
      const f64* uc = L.masked(u, c, cbuf);
      for (int a = 0; a < 3; ++a) {
        const int ax[3] = {a, (a + 1) % 3, (a + 2) % 3};
        for (int side = 0; side < 2; ++side) {
          const i32 lo = side == 0 ? c : L.nbrm[a][c];
          if (lo < 0) continue;
          const i32 hi = side == 0 ? L.nbr[a][c] : c;
          if (hi < 0) continue;
          const i32 other = side == 0 ? hi : lo;
          const f64* uo = L.anchored[other] ? kZero : L.masked(u, other, obuf);
          const f64* ui = side == 0 ? uc : uo;
          const f64* uj = side == 0 ? uo : uc;
          const f64* fr = &L.frame[a][Lattice::kFrameSize * size_t(lo)];
          const f64* R = fr;
          const f64* ri = fr + 9;
          const f64* rj = fr + 12;
          // w_t = du_j + dth_j x r_j - du_i - dth_i x r_i ; w_r = dth_j - dth_i
          const f64 wt[3] = {uj[0] + (uj[4] * rj[2] - uj[5] * rj[1]) - ui[0] - (ui[4] * ri[2] - ui[5] * ri[1]),
                             uj[1] + (uj[5] * rj[0] - uj[3] * rj[2]) - ui[1] - (ui[5] * ri[0] - ui[3] * ri[2]),
                             uj[2] + (uj[3] * rj[1] - uj[4] * rj[0]) - ui[2] - (ui[3] * ri[1] - ui[4] * ri[0])};
          const f64 wr[3] = {uj[3] - ui[3], uj[4] - ui[4], uj[5] - ui[5]};
          const Vec6 kk = L.bond_k(a, lo);
          f64 st[3], sr[3];  // local stresses per local axis index 0..2 (a, t1, t2)
          for (int r = 0; r < 3; ++r) {
            const int x = ax[r];
            const f64 gt = R[0 * 3 + x] * wt[0] + R[1 * 3 + x] * wt[1] + R[2 * 3 + x] * wt[2];
            const f64 gr = R[0 * 3 + x] * wr[0] + R[1 * 3 + x] * wr[1] + R[2 * 3 + x] * wr[2];
            st[r] = kk[r] * gt;
            sr[r] = kk[3 + r] * gr;
          }
          f64 F[3], M[3];
          for (int q = 0; q < 3; ++q) {
            F[q] = R[q * 3 + ax[0]] * st[0] + R[q * 3 + ax[1]] * st[1] + R[q * 3 + ax[2]] * st[2];
            M[q] = R[q * 3 + ax[0]] * sr[0] + R[q * 3 + ax[1]] * sr[1] + R[q * 3 + ax[2]] * sr[2];
          }
          const f64* r = side == 0 ? ri : rj;
          const f64 rxF[3] = {r[1] * F[2] - r[2] * F[1], r[2] * F[0] - r[0] * F[2], r[0] * F[1] - r[1] * F[0]};
          const f64 sg = side == 0 ? -1.0 : 1.0;
          for (int q = 0; q < 3; ++q) {
            yc[q] += sg * F[q];
            yc[3 + q] += sg * (M[q] + rxF[q]);
          }
        }
      }
      if (!L.spring.empty())
        for (int q = 0; q < 6; ++q) yc[q] += L.spring[c][q] * uc[q];
      if (L.fixmask[c])
        for (int q = 0; q < 6; ++q)
          if ((L.fixmask[c] >> q) & 1) yc[q] = 0.0;
    }
  });
}

}  // namespace

void apply_stiffness(const Lattice& L, const f64* u, f64* y) {
  if (L.framed()) {
    apply_stiffness_framed(L, u, y);
    return;
  }
  // Gather form: every cell sums the contributions of its (up to) six bonds, so rows are
  // written by exactly one task (no races, thread-count independent results).
  const f64 hh = 0.5 * L.h;
  parallel_for(L.n, kGrain, [&](i64 b, i64 e) {
    static const f64 kZero[6] = {0, 0, 0, 0, 0, 0};
    for (i64 ii = b; ii < e; ++ii) {
      const i32 i = static_cast<i32>(ii);
      f64* yi = y + 6 * size_t(i);
      for (int q = 0; q < 6; ++q) yi[q] = 0.0;
      if (L.anchored[i]) continue;
      f64 ubuf[6], nbuf[6];
      const f64* ui = L.masked(u, i, ubuf);
      for (int a = 0; a < 3; ++a) {
        const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
        // bond (i -> j = +a neighbour): i is the lower side
        const i32 j = L.nbr[a][i];
        if (j >= 0) {
          const f64* uj = L.anchored[j] ? kZero : L.masked(u, j, nbuf);
          const Vec6 k = L.bond_k(a, i);
          const f64 f0 = k[0] * (uj[a] - ui[a]);
          const f64 f1 = k[1] * (uj[t1] - ui[t1] - hh * (ui[3 + t2] + uj[3 + t2]));
          const f64 f2 = k[2] * (uj[t2] - ui[t2] + hh * (ui[3 + t1] + uj[3 + t1]));
          const f64 f3 = k[3] * (uj[3 + a] - ui[3 + a]);
          const f64 f4 = k[4] * (uj[3 + t1] - ui[3 + t1]);
          const f64 f5 = k[5] * (uj[3 + t2] - ui[3 + t2]);
          yi[a] -= f0;
          yi[t1] -= f1;
          yi[t2] -= f2;
          yi[3 + a] -= f3;
          yi[3 + t1] += -f4 + hh * f2;
          yi[3 + t2] += -f5 - hh * f1;
        }
        // bond (m -> i, m = -a neighbour): i is the upper side
        const i32 m = L.nbrm[a][i];
        if (m >= 0) {
          const f64* um = L.anchored[m] ? kZero : L.masked(u, m, nbuf);
          const Vec6 k = L.bond_k(a, m);
          const f64 f0 = k[0] * (ui[a] - um[a]);
          const f64 f1 = k[1] * (ui[t1] - um[t1] - hh * (um[3 + t2] + ui[3 + t2]));
          const f64 f2 = k[2] * (ui[t2] - um[t2] + hh * (um[3 + t1] + ui[3 + t1]));
          const f64 f3 = k[3] * (ui[3 + a] - um[3 + a]);
          const f64 f4 = k[4] * (ui[3 + t1] - um[3 + t1]);
          const f64 f5 = k[5] * (ui[3 + t2] - um[3 + t2]);
          yi[a] += f0;
          yi[t1] += f1;
          yi[t2] += f2;
          yi[3 + a] += f3;
          yi[3 + t1] += f4 + hh * f2;
          yi[3 + t2] += f5 - hh * f1;
        }
      }
      if (!L.spring.empty())
        for (int q = 0; q < 6; ++q) yi[q] += L.spring[i][q] * ui[q];
      if (L.fixmask[i])
        for (int q = 0; q < 6; ++q)
          if ((L.fixmask[i] >> q) & 1) yi[q] = 0.0;
    }
  });
}

void gravity_load(const Lattice& L, f64 g, f64* f) {
  std::fill(f, f + 6 * size_t(L.n), 0.0);
  for (i32 i = 0; i < L.n; ++i)
    if (!((L.fixmask[i] >> 2) & 1)) f[6 * size_t(i) + 2] = -L.mass[i] * g;
}

void stiffness_diag_blocks(const Lattice& L, f64* D) {
  const f64 hh = 0.5 * L.h;
  f64 B[3][2][36];
  for (int a = 0; a < 3; ++a) bond_side_matrices(a, hh, B[a][0], B[a][1]);
  parallel_for(L.n, kGrain, [&](i64 b, i64 e) {
    for (i64 ii = b; ii < e; ++ii) {
      const i32 i = static_cast<i32>(ii);
      f64* Dc = D + 36 * size_t(i);
      std::fill(Dc, Dc + 36, 0.0);
      if (L.anchored[i]) {
        for (int r = 0; r < 6; ++r) Dc[r * 6 + r] = 1.0;
        continue;
      }
      for (int a = 0; a < 3; ++a) {
        for (int side = 0; side < 2; ++side) {
          // side 0: i is the lower cell of its +a bond; side 1: upper cell of the -a bond
          const i32 other = side == 0 ? L.nbr[a][i] : L.nbrm[a][i];
          if (other < 0) continue;
          const i32 lo = side == 0 ? i : other;
          const Vec6 kv = L.bond_k(a, lo);
          const f64* k = kv.data();
          f64 Bf[2][36];
          const f64* Bs = B[a][side];
          if (L.framed()) {
            bond_side_matrices_framed(a, &L.frame[a][Lattice::kFrameSize * size_t(lo)], Bf[0], Bf[1]);
            Bs = Bf[side];
          }
          for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 6; ++c) {
              f64 acc = 0.0;
              for (int q = 0; q < 6; ++q) acc += Bs[q * 6 + r] * k[q] * Bs[q * 6 + c];
              Dc[r * 6 + c] += acc;
            }
        }
      }
      if (!L.spring.empty())
        for (int q = 0; q < 6; ++q) Dc[q * 6 + q] += L.spring[i][q];
      for (int q = 0; q < 6; ++q)
        if ((L.fixmask[i] >> q) & 1) {
          for (int c = 0; c < 6; ++c) Dc[q * 6 + c] = Dc[c * 6 + q] = 0.0;
          Dc[q * 6 + q] = 1.0;
        }
    }
  });
}

void bond_forces(const Lattice& L, int a, const f64* u, f64* F) {
  const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
  const f64 hh = 0.5 * L.h;
  parallel_for(L.n, kGrain, [&](i64 b, i64 e) {
    static const f64 kZero[6] = {0, 0, 0, 0, 0, 0};
    for (i64 ii = b; ii < e; ++ii) {
      const i32 i = static_cast<i32>(ii);
      f64* Fi = F + 6 * size_t(i);
      const i32 j = L.nbr[a][i];
      if (j < 0) {
        for (int q = 0; q < 6; ++q) Fi[q] = 0.0;
        continue;
      }
      f64 bi[6], bj[6];
      const f64* ui = L.anchored[i] ? kZero : L.masked(u, i, bi);
      const f64* uj = L.anchored[j] ? kZero : L.masked(u, j, bj);
      const Vec6 kv = L.bond_k(a, i);
      const f64* k = kv.data();
      Fi[0] = k[0] * (uj[a] - ui[a]);
      Fi[1] = k[1] * (uj[t1] - ui[t1] - hh * (ui[3 + t2] + uj[3 + t2]));
      Fi[2] = k[2] * (uj[t2] - ui[t2] + hh * (ui[3 + t1] + uj[3 + t1]));
      Fi[3] = k[3] * (uj[3 + a] - ui[3 + a]);
      Fi[4] = k[4] * (uj[3 + t1] - ui[3 + t1]);
      Fi[5] = k[5] * (uj[3 + t2] - ui[3 + t2]);
    }
  });
}

Vec6 bond_force(const Lattice& L, int a, i32 i, const f64* u) {
  static const f64 kZero[6] = {0, 0, 0, 0, 0, 0};
  const i32 j = L.nbr[a][i];
  Vec6 F{};
  if (j < 0) return F;
  const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
  const f64 hh = 0.5 * L.h;
  f64 bi[6], bj[6];
  const f64* ui = L.anchored[i] ? kZero : L.masked(u, i, bi);
  const f64* uj = L.anchored[j] ? kZero : L.masked(u, j, bj);
  const Vec6 kv = L.bond_k(a, i);
  const f64* k = kv.data();
  F[0] = k[0] * (uj[a] - ui[a]);
  F[1] = k[1] * (uj[t1] - ui[t1] - hh * (ui[3 + t2] + uj[3 + t2]));
  F[2] = k[2] * (uj[t2] - ui[t2] + hh * (ui[3 + t1] + uj[3 + t1]));
  F[3] = k[3] * (uj[3 + a] - ui[3 + a]);
  F[4] = k[4] * (uj[3 + t1] - ui[3 + t1]);
  F[5] = k[5] * (uj[3 + t2] - ui[3 + t2]);
  return F;
}

void released_bond_residual(const Lattice& L, const std::vector<u8>& removed, const f64* u0, f64* r) {
  std::fill(r, r + 6 * size_t(L.n), 0.0);
  const f64 hh = 0.5 * L.h;
  for (int a = 0; a < 3; ++a) {
    const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || removed[i] == removed[j]) continue;
      const Vec6 F = bond_force(L, a, i, u0);
      // Contribution of this bond to (K u0): i-side gets -F (+ lever terms), j-side +F.
      if (!removed[i] && !L.anchored[i]) {
        f64* ri = r + 6 * size_t(i);
        ri[a] -= F[0];
        ri[t1] -= F[1];
        ri[t2] -= F[2];
        ri[3 + a] -= F[3];
        ri[3 + t1] += -F[4] + hh * F[2];
        ri[3 + t2] += -F[5] - hh * F[1];
      }
      if (!removed[j] && !L.anchored[j]) {
        f64* rj = r + 6 * size_t(j);
        rj[a] += F[0];
        rj[t1] += F[1];
        rj[t2] += F[2];
        rj[3 + a] += F[3];
        rj[3 + t1] += F[4] + hh * F[2];
        rj[3 + t2] += F[5] - hh * F[1];
      }
    }
  }
  for (i32 i = 0; i < L.n; ++i)
    if (L.fixmask[i])
      for (int q = 0; q < 6; ++q)
        if ((L.fixmask[i] >> q) & 1) r[6 * size_t(i) + q] = 0.0;
}

f64 strength_multiplier(u8 cls) {
  static const f64 kMul[kStrengthClasses] = {1.0,  1.5,  2.0,   3.0,   4.0,   6.0,   8.0,   12.0,  16.0,  24.0, 32.0,
                                             48.0, 64.0, 96.0,  128.0, 192.0, 256.0, 384.0, 512.0, 768.0, 1024.0};
  return kMul[std::min<int>(cls, kStrengthClasses - 1)];
}

u8 strength_class_for(f64 m) {
  for (u8 c = 0; c + 1 < kStrengthClasses; ++c)
    if (strength_multiplier(c) >= m) return c;
  return static_cast<u8>(kStrengthClasses - 1);
}

SubLattice extract_sublattice(const Lattice& L, std::span<const i32> cells) {
  SubLattice S;
  std::unordered_map<i32, i32> idx;
  idx.reserve(cells.size() * 2 + 16);
  auto add = [&](i32 c) {
    const auto it = idx.find(c);
    if (it != idx.end()) return it->second;
    const i32 k = static_cast<i32>(S.to_world.size());
    idx.emplace(c, k);
    S.to_world.push_back(c);
    return k;
  };
  for (i32 c : cells)
    if (!L.dead[c]) add(c);
  const size_t n_core = S.to_world.size();
  for (size_t k = 0; k < n_core; ++k) {
    const i32 c = S.to_world[k];
    for (int a = 0; a < 3; ++a) {
      const i32 p = L.nbr[a][c], m = L.nbrm[a][c];
      if (p >= 0 && L.anchored[p]) add(p);
      if (m >= 0 && L.anchored[m]) add(m);
    }
  }
  Lattice& F = S.F;
  const i32 n = static_cast<i32>(S.to_world.size());
  F.h = L.h;
  F.n = n;
  F.kscale = L.kscale;
  F.fixes = L.fixes;
  F.law = L.law;
  F.contact = L.contact;
  F.models = L.models;
  F.p.resize(n);
  F.mat.resize(n);
  F.anchored.resize(n);
  F.fixmask.resize(n);
  F.eff.resize(n);
  F.mass.resize(n);
  F.inertia.resize(n);
  F.arch.resize(n);
  F.strength.resize(n);
  F.dead.assign(n, 0);
  if (!L.spring.empty()) F.spring.resize(n);
  for (i32 k = 0; k < n; ++k) {
    const i32 c = S.to_world[k];
    F.p[k] = L.p[c];
    F.mat[k] = L.mat[c];
    F.anchored[k] = L.anchored[c];
    F.fixmask[k] = L.fixmask[c];
    F.eff[k] = L.eff[c];
    F.mass[k] = L.mass[c];
    F.inertia[k] = L.inertia[c];
    F.arch[k] = L.arch[c];
    F.strength[k] = L.strength.empty() ? 0 : L.strength[c];
    if (!L.plate.empty()) {
      if (F.plate.empty()) F.plate.assign(n, 0);
      F.plate[k] = L.plate[c];
    }
    if (!L.spring.empty()) F.spring[k] = L.spring[c];
  }
  if (!L.u_fixed.empty()) {
    F.u_fixed.assign(6 * size_t(n), 0.0);
    for (i32 k = 0; k < n; ++k)
      for (int q = 0; q < 6; ++q) F.u_fixed[6 * size_t(k) + q] = L.u_fixed[6 * size_t(S.to_world[k]) + q];
  }
  const bool dm = !L.dmg[0].empty();
  for (int a = 0; a < 3; ++a) {
    F.nbr[a].assign(n, -1);
    F.nbrm[a].assign(n, -1);
    F.bid[a].assign(n, 0);
    if (dm) F.dmg[a].assign(n, 0.0f);
    if (!L.ext_id[a].empty()) F.ext_id[a].assign(n, -1);
  }
  for (i32 k = 0; k < n; ++k) {
    const i32 c = S.to_world[k];
    for (int a = 0; a < 3; ++a) {
      const i32 j = L.nbr[a][c];
      if (j < 0) continue;
      const auto it = idx.find(j);
      if (it == idx.end()) continue;
      const i32 kj = it->second;
      if (F.anchored[k] && F.anchored[kj]) continue;
      F.nbr[a][k] = kj;
      F.nbrm[a][kj] = k;
      F.bid[a][k] = L.bid[a][c];
      if (dm) F.dmg[a][k] = L.dmg[a][c];
      if (L.is_cracked(a, c)) {
        if (F.cracked[a].empty()) {
          F.cracked[a].assign(n, 0);
          F.cscale[a].assign(n, Vec6{1, 1, 1, 1, 1, 1});
        }
        F.cracked[a][k] = 1;
        F.cscale[a][k] = L.cscale[a][c];
      }
      if (!L.ext_id[a].empty()) F.ext_id[a][k] = L.ext_id[a][c];
    }
  }
  return S;
}

}  // namespace svx
