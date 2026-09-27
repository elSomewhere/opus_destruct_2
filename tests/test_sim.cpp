// Phase 1 gates for the simulation layer: law calibration, corotational kinematics, static
// closure, implicit dynamics, blasts and connectivity (plan §C Phase 1).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/mech/law.hpp"
#include "svx/sim/blast.hpp"
#include "svx/sim/corot.hpp"
#include "svx/sim/dynamics.hpp"
#include "svx/sim/statics.hpp"
#include "svx/topo/connectivity.hpp"

using namespace svx;

namespace {

void add_box(std::vector<CellIn>& cells, int x0, int x1, int y0, int y1, int z0, int z1, MaterialId mat,
             bool anchored = false) {
  for (int x = x0; x < x1; ++x)
    for (int y = y0; y < y1; ++y)
      for (int z = z0; z < z1; ++z) {
        CellIn c;
        c.p = {x, y, z};
        c.mat = mat;
        c.anchored = anchored;
        cells.push_back(c);
      }
}

// Dissipated work to rupture of one bond loaded monotonically in one component.
f64 rupture_work(const BondModel& m, int comp, bool game) {
  const f64 end = comp == 0 ? m.brk.open : m.brk.comp6[comp];
  const int N = 200000;
  f64 W = 0.0, xprev = 0.0, Fprev = 0.0, dc = 0.0;
  for (int k = 1; k <= N; ++k) {
    const f64 x = end * k / N;
    Vec6 d{0, 0, 0, 0, 0, 0};
    d[comp] = x;
    const LawEval e = evaluate_law(m, d, dc, game);
    dc = e.damage;
    const f64 F = m.k[comp] * (1.0 - dc) * x;  // secant without the solver floor
    W += 0.5 * (F + Fprev) * (x - xprev);
    xprev = x;
    Fprev = F;
  }
  return W;
}

// Cantilever slab: anchored wall column at x = 0, slab of length `len` along x.
Lattice cantilever_slab(int len, int width, int thick, f64 h, const LawParams& law) {
  std::vector<CellIn> cells;
  add_box(cells, -1, 0, 0, width, 0, thick, MaterialId::Concrete, true);
  add_box(cells, 0, len, 0, width, 0, thick, MaterialId::Concrete);
  LatticeOptions o;
  o.h = h;
  o.law = law;
  return build_lattice(cells, o);
}

std::set<i32> alive_set(const Lattice& L) {
  std::set<i32> s;
  for (i32 i = 0; i < L.n; ++i)
    if (!L.dead[i]) s.insert(i);
  return s;
}

}  // namespace

TEST_CASE("game law: fracture energy per area is invariant across the cell size (±5%)") {
  LawParams law;
  law.game = true;
  const SectionFixes fx;
  const Material& mc = material(MaterialId::Concrete);
  const f64 s = profile_params(Profile::Solid).strength;
  for (f64 h : {0.0625, 0.125, 0.25, 0.5, 1.0}) {
    const BondModel m = make_bond(0, MaterialId::Concrete, {1, 1, 1}, MaterialId::Concrete, {1, 1, 1}, h,
                                  Profile::Solid, fx, law);
    const f64 wI = rupture_work(m, 0, true) / m.area_min;
    const f64 wII = rupture_work(m, 1, true) / m.area_min;
    CHECK(wI == doctest::Approx(mc.GfI * s).epsilon(0.05));
    CHECK(wII == doctest::Approx(mc.GfII * s).epsilon(0.05));
    CHECK(m.brk.open >= m.onset.open * 1.4);
  }
  // The reference law dissipates far more than G_f at game cell sizes (plan §A1 defect).
  const BondModel r = make_bond(0, MaterialId::Concrete, {1, 1, 1}, MaterialId::Concrete, {1, 1, 1}, 0.125,
                                Profile::Solid, fx, LawParams{});
  CHECK(rupture_work(r, 0, false) / r.area_min > 3.0 * mc.GfI * s);
}

TEST_CASE("corotational kinematics: rigid motions are strain free, small motions match the linear jump") {
  std::vector<CellIn> cells;
  add_box(cells, 0, 3, 0, 2, 0, 2, MaterialId::Concrete);
  LatticeOptions o;
  Lattice L = build_lattice(cells, o);
  // rigid rotation of 0.7 rad about an axis through the origin + translation
  const f64 th[3] = {0.3, -0.5, 0.4};
  const Quat q = quat_from_rotvec(th);
  std::vector<f64> u(6 * size_t(L.n));
  for (i32 i = 0; i < L.n; ++i) {
    const Vec3 x{L.h * L.p[i][0], L.h * L.p[i][1], L.h * L.p[i][2]};
    const Vec3 rx = quat_rotate(q, x);
    for (int k = 0; k < 3; ++k) {
      u[6 * size_t(i) + k] = rx[k] - x[k] + 0.1 * (k + 1);
      u[6 * size_t(i) + 3 + k] = th[k];
    }
  }
  std::vector<f64> f(u.size());
  internal_forces(L, u.data(), true, f.data());
  f64 fmax = 0.0;
  for (f64 v : f) fmax = std::max(fmax, std::abs(v));
  const f64 kref = L.models[0].k[0];
  CHECK(fmax < 1e-9 * kref * L.h);
  // tiny motions: corotational jump == linear jump to second order
  for (auto& v : u) v = 0.0;
  for (i32 i = 0; i < L.n; ++i)
    for (int k = 0; k < 6; ++k) u[6 * size_t(i) + k] = 1e-7 * std::sin(1.0 + i * 7 + k * 3) * (k < 3 ? L.h : 1.0);
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      if (L.nbr[a][i] < 0) continue;
      const Vec6 gl = bond_jump(L, a, i, u.data(), false);
      const Vec6 gc = bond_jump(L, a, i, u.data(), true);
      for (int k = 0; k < 6; ++k) CHECK(std::abs(gl[k] - gc[k]) < 1e-12);
    }
}

TEST_CASE("corotational tangent: exact material tangent at a rotated stress-free state, symmetric") {
  std::vector<CellIn> cells;
  add_box(cells, 0, 4, 0, 2, 0, 3, MaterialId::Concrete);
  LatticeOptions o;
  o.h = 0.5;
  Lattice L = build_lattice(cells, o);
  // rigid rotation by 0.9 rad (stress free): f_int = 0 and d f_int = K_t du exactly
  const f64 th[3] = {0.5, -0.6, 0.45};
  const Quat q = quat_from_rotvec(th);
  std::vector<f64> u(6 * size_t(L.n));
  for (i32 i = 0; i < L.n; ++i) {
    const Vec3 x{L.h * L.p[i][0], L.h * L.p[i][1], L.h * L.p[i][2]};
    const Vec3 rx = quat_rotate(q, x);
    for (int k = 0; k < 3; ++k) {
      u[6 * size_t(i) + k] = rx[k] - x[k];
      u[6 * size_t(i) + 3 + k] = th[k];
    }
  }
  compute_frames(L, u.data());
  std::vector<f64> du(u.size()), y(u.size()), fp(u.size()), fm(u.size());
  for (size_t k = 0; k < du.size(); ++k) du[k] = std::cos(0.3 + 1.7 * k) * ((k % 6) < 3 ? L.h : 1.0);
  apply_stiffness(L, du.data(), y.data());
  const f64 eps = 1e-6;
  std::vector<f64> up = u, um = u;
  apply_increment(L, up.data(), du.data(), eps, true);
  apply_increment(L, um.data(), du.data(), -eps, true);
  internal_forces(L, up.data(), true, fp.data());
  internal_forces(L, um.data(), true, fm.data());
  f64 num = 0.0, den = 0.0;
  for (size_t k = 0; k < y.size(); ++k) {
    const f64 fd = (fp[k] - fm[k]) / (2 * eps);
    num += (fd - y[k]) * (fd - y[k]);
    den += fd * fd;
  }
  CHECK(std::sqrt(num / den) < 1e-5);
  // the unrotated (rest) operator is far from the tangent at this state
  clear_frames(L);
  std::vector<f64> y0(u.size());
  apply_stiffness(L, du.data(), y0.data());
  f64 d0 = 0.0;
  for (size_t k = 0; k < y.size(); ++k) d0 += (y0[k] - y[k]) * (y0[k] - y[k]);
  CHECK(std::sqrt(d0 / den) > 0.1);
  // symmetry of K_t
  compute_frames(L, u.data());
  std::vector<f64> v(u.size()), Kv(u.size());
  for (size_t k = 0; k < v.size(); ++k) v[k] = std::sin(0.1 + 0.37 * k);
  apply_stiffness(L, v.data(), Kv.data());
  f64 a1 = 0.0, a2 = 0.0;
  for (size_t k = 0; k < v.size(); ++k) {
    a1 += v[k] * y[k];
    a2 += du[k] * Kv[k];
  }
  CHECK(a1 == doctest::Approx(a2).epsilon(1e-10));
  clear_frames(L);
}

TEST_CASE("statics: closure with compliance S is invariant under linear kinematics") {
  LawParams law;
  law.game = true;
  law.fragility = 0.02;
  std::vector<std::set<i32>> alive;
  std::vector<std::vector<i64>> rupt;
  for (f64 S : {1.0, 4.0, 16.0}) {
    Lattice L = cantilever_slab(24, 3, 2, 0.125, law);
    L.kscale = 1.0 / S;
    StaticsOptions so;
    so.corot = false;
    std::vector<f64> u;
    const auto f = gravity_vector(L, so.g);
    const ClosureResult cr = solve_to_closure(L, u, f, so);
    CHECK(cr.converged);
    alive.push_back(alive_set(L));
    rupt.push_back(cr.ruptured);
  }
  CHECK(alive[0].size() < 24u * 3u * 2u + 6u);  // the slab does fail at this fragility
  CHECK(alive[0] == alive[1]);
  CHECK(alive[0] == alive[2]);
  CHECK(rupt[0] == rupt[1]);
  CHECK(rupt[0] == rupt[2]);
}

TEST_CASE("statics: an overloaded cantilever breaks at the root and the slab is deleted") {
  LawParams law;
  law.game = true;
  law.fragility = 0.01;
  Lattice L = cantilever_slab(32, 2, 1, 0.125, law);
  StaticsOptions so;
  std::vector<f64> u;
  const auto f = gravity_vector(L, so.g);
  const ClosureResult cr = solve_to_closure(L, u, f, so);
  CHECK(cr.converged);
  CHECK(!cr.ruptured.empty());
  CHECK(!cr.deleted.empty());
  // every surviving free cell is still connected to support
  CHECK(unsupported_components(L).empty());
  // a 1 m slab of the same section stands (the 4 m one sees ~9 MPa at the root)
  law.fragility = 1.0;
  Lattice L2 = cantilever_slab(8, 2, 1, 0.125, law);
  std::vector<f64> u2;
  const ClosureResult c2 = solve_to_closure(L2, u2, gravity_vector(L2, so.g), so);
  CHECK(c2.converged);
  CHECK(c2.ruptured.empty());
  CHECK(c2.passes == 1);
}

TEST_CASE("connectivity: detachment work is O(change), independent of the structure size") {
  for (int n : {48, 160}) {
    std::vector<CellIn> cells;
    add_box(cells, 0, n, 0, n, 0, 1, MaterialId::Concrete);
    // supports: a ring of anchored cells under the slab edge
    for (int x = 0; x < n; ++x)
      for (int y = 0; y < n; ++y)
        if (x == 0 || y == 0 || x == n - 1 || y == n - 1) {
          CellIn c;
          c.p = {x, y, -1};
          c.mat = MaterialId::Concrete;
          c.anchored = true;
          cells.push_back(c);
        }
    Lattice L = build_lattice(cells, LatticeOptions{});
    auto idx = [&](int x, int y) { return x * n + y; };  // slab cells come first, z = 0
    const int c = n / 2;
    // 1. one ruptured bond in the middle of the slab: no island, tiny search
    {
      ConnStats st;
      CutSet cut;
      L.break_bond(idx(c, c), 0);
      cut.seeds = {idx(c, c), idx(c + 1, c)};
      CHECK(detached_islands(L, cut, &st).empty());
      CHECK(st.visited <= 16);
    }
    // 2. a 3x3 hole: no island
    {
      std::vector<i32> hole;
      for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) hole.push_back(idx(c + 10 + dx, c + dy));
      const CutSet cut = removal_cut(L, hole);
      for (i32 h : hole) L.remove_cell(h);
      ConnStats st;
      CHECK(detached_islands(L, cut, &st).empty());
      CHECK(st.visited <= 80);
    }
    // 3. cut a 4x4 piece out of the middle (all its boundary bonds rupture): one island
    {
      CutSet cut;
      const int x0 = c - 12, y0 = c - 12;
      for (int x = x0; x < x0 + 4; ++x)
        for (int y = y0; y < y0 + 4; ++y) {
          const i32 i = idx(x, y);
          for (int a = 0; a < 2; ++a) {
            const i32 j = L.nbr[a][i];
            if (j >= 0) {
              const int xj = L.p[j][0], yj = L.p[j][1];
              if (xj >= x0 + 4 || yj >= y0 + 4) {
                L.break_bond(i, a);
                cut.seeds.push_back(i);
                cut.seeds.push_back(j);
              }
            }
            const i32 m = L.nbrm[a][i];
            if (m >= 0) {
              const int xm = L.p[m][0], ym = L.p[m][1];
              if (xm < x0 || ym < y0) {
                L.break_bond(m, a);
                cut.seeds.push_back(i);
                cut.seeds.push_back(m);
              }
            }
          }
        }
      ConnStats st;
      const auto isl = detached_islands(L, cut, &st);
      REQUIRE(isl.size() == 1);
      CHECK(isl[0].size() == 16);
      CHECK(st.visited <= 120);
    }
  }
}

TEST_CASE("blast: prefracture rules are cell-size invariant when scaled with h") {
  std::vector<std::set<std::pair<i32, int>>> fractured;
  std::vector<std::vector<i32>> removed;
  std::vector<std::vector<f32>> damage;
  for (f64 h : {1.0, 0.125}) {
    std::vector<CellIn> cells;
    add_box(cells, 0, 5, 0, 5, 0, 1, MaterialId::Concrete, true);
    add_box(cells, 0, 5, 0, 5, 1, 6, MaterialId::Concrete);
    LatticeOptions o;
    o.h = h;
    Lattice L = build_lattice(cells, o);
    BlastParams bp;
    bp.center = {2.2 * h, 2.1 * h, 3.3 * h};
    bp.radius = 1.3 * h;
    const BlastResult br = apply_blast(L, nullptr, bp);
    std::set<std::pair<i32, int>> fr;
    for (i64 k : br.fractured) fr.insert({static_cast<i32>(k / 3), static_cast<int>(k % 3)});
    fractured.push_back(fr);
    removed.push_back(br.removed);
    std::vector<f32> d;
    for (int a = 0; a < 3; ++a) d.insert(d.end(), L.dmg[a].begin(), L.dmg[a].end());
    damage.push_back(d);
    CHECK(!br.removed.empty());
    CHECK(!br.fractured.empty());
    CHECK(!br.damaged.empty());
  }
  CHECK(removed[0] == removed[1]);
  CHECK(fractured[0] == fractured[1]);
  REQUIRE(damage[0].size() == damage[1].size());
  for (size_t k = 0; k < damage[0].size(); ++k) CHECK(damage[0][k] == doctest::Approx(damage[1][k]).epsilon(1e-5));
}

TEST_CASE("dynamics: free vibration period of a cantilever and its compliance scaling") {
  // steel cantilever 16 cells long, 1-cell section, released from a tip-loaded static state
  auto period = [](f64 S) {
    std::vector<CellIn> cells;
    add_box(cells, -1, 0, 0, 1, 0, 1, MaterialId::Steel, true);
    add_box(cells, 0, 16, 0, 1, 0, 1, MaterialId::Steel);
    LatticeOptions o;
    o.h = 0.125;
    Lattice L = build_lattice(cells, o);
    DynamicsOptions dop;
    dop.dt = 1.0 / 4000.0;
    dop.g = 0.0;
    dop.compliance = S;
    dop.damage = false;
    dop.settle = false;
    dop.sleep_steps = 1 << 30;
    Dynamics dyn;
    dyn.init(L, dop);
    // static tip-loaded state as the initial condition
    std::vector<f64> f(6 * size_t(L.n), 0.0), u;
    f[6 * size_t(L.n - 1) + 2] = -50.0;
    StaticsOptions so;
    so.damage = false;
    solve_equilibrium(L, u, f, DamageField{}, so, nullptr);
    dyn.set_state(u);
    // zero crossings of the tip deflection
    std::vector<f64> cross;
    f64 prev = dyn.u()[6 * size_t(L.n - 1) + 2];
    for (int s = 0; s < 4000 && cross.size() < 5; ++s) {
      dyn.step();
      const f64 z = dyn.u()[6 * size_t(L.n - 1) + 2];
      if ((prev < 0.0) != (z < 0.0)) cross.push_back(dyn.time() - dop.dt * z / (z - prev));
      prev = z;
    }
    REQUIRE(cross.size() >= 3);
    return cross[2] - cross[0];
  };
  const f64 T1 = period(1.0);
  // Euler-Bernoulli first mode of a clamped-free beam (shear and rotary inertia < 1% here)
  const Material& st = material(MaterialId::Steel);
  const f64 b = 0.125, L = 16 * 0.125 + 0.5 * 0.125;  // clamp at the anchored cell's face
  const f64 EI = st.E * b * b * b * b / 12.0, mbar = st.rho * b * b;
  const f64 f1 = 1.875104 * 1.875104 / (2.0 * M_PI) * std::sqrt(EI / (mbar * L * L * L * L));
  CHECK(T1 == doctest::Approx(1.0 / f1).epsilon(0.06));
  const f64 T4 = period(4.0);
  CHECK(T4 / T1 == doctest::Approx(2.0).epsilon(0.02));  // sqrt(S_p) slower
}

TEST_CASE("dynamics: a carved support drops the slab, emits an island and schedules its impact") {
  LawParams law;
  law.game = true;
  // two columns carrying a beam; the beam overhangs a floor slab
  std::vector<CellIn> cells;
  add_box(cells, 0, 24, 0, 3, 0, 1, MaterialId::Concrete, true);   // anchored ground
  add_box(cells, 0, 24, 0, 3, 1, 2, MaterialId::Concrete);         // floor slab on the ground
  add_box(cells, 2, 4, 0, 3, 2, 12, MaterialId::Concrete);         // column A
  add_box(cells, 20, 22, 0, 3, 2, 12, MaterialId::Concrete);       // column B
  add_box(cells, 2, 22, 0, 3, 12, 14, MaterialId::Concrete);       // beam
  LatticeOptions o;
  o.law = law;
  Lattice L = build_lattice(cells, o);
  DynamicsOptions dop;
  dop.rayleigh_alpha = 1.0;
  Dynamics dyn;
  dyn.init(L, dop);
  dyn.settle_to_equilibrium();
  // cut both columns through their whole section (4 cells high): everything above detaches
  std::vector<i32> cut;
  for (i32 i = 0; i < L.n; ++i)
    if (L.p[i][2] >= 5 && L.p[i][2] < 7 && (L.p[i][0] < 4 || L.p[i][0] >= 20)) cut.push_back(i);
  dyn.carve(cut);
  const auto isl = dyn.take_islands();
  REQUIRE(isl.size() == 1);
  CHECK(isl[0].cells.size() == 2u * (2 * 3 * 5) + 20u * 3u * 2u);
  CHECK(isl[0].fall_time > 0.0);
  // the landing loads act on the floor slab when the island would arrive
  f64 zmin = 0.0;
  bool moved = false;
  for (int s = 0; s < 90; ++s) {
    const StepStats st = dyn.step();
    for (i32 i = 0; i < L.n; ++i)
      if (!L.dead[i] && L.p[i][2] == 1) zmin = std::min(zmin, dyn.u()[6 * size_t(i) + 2]);
    moved = moved || st.max_speed > 0.0;
  }
  CHECK(moved);
  CHECK(zmin < 0.0);
}

TEST_CASE("dynamics: results are bitwise identical for 1 and N threads") {
  auto run = [](int threads) {
    set_num_threads(threads);
    LawParams law;
    law.game = true;
    law.fragility = 0.05;
    Lattice L = cantilever_slab(40, 4, 2, 0.125, law);
    DynamicsOptions dop;
    Dynamics dyn;
    dyn.init(L, dop);
    dyn.settle_to_equilibrium();
    BlastParams bp;
    bp.center = {20 * 0.125, 2 * 0.125, 1 * 0.125};
    bp.radius = 0.3;
    dyn.blast(bp);
    for (int s = 0; s < 40; ++s) dyn.step();
    u64 hsh = 1469598103934665603ull;
    for (f64 v : dyn.u()) {
      u64 bits;
      std::memcpy(&bits, &v, 8);
      hsh = (hsh ^ bits) * 1099511628211ull;
    }
    for (i64 k : dyn.ruptured()) hsh = (hsh ^ u64(k)) * 1099511628211ull;
    return hsh;
  };
  const int nt = num_threads();
  const u64 h1 = run(1);
  const u64 h8 = run(8);
  set_num_threads(nt);
  CHECK(h1 == h8);
  MESSAGE("dynamics hash " << h1);  // compared across native / WASM builds
}
