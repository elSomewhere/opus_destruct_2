// Unilateral cracked contacts (plan §B7 / Phase 7): a crack is not a detachment. Cracked bonds
// carry compression (with friction and rocking limits) and nothing in tension; a crack that
// opens beyond the separation gap is reported for breaking.
#include <algorithm>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/mech/law.hpp"
#include "svx/sim/dynamics.hpp"
#include "svx/sim/statics.hpp"
#include "svx/topo/connectivity.hpp"

using namespace svx;

namespace {

void add_box(std::vector<CellIn>& cells, int x0, int x1, int y0, int y1, int z0, int z1, MaterialId mat, bool anchored) {
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

i32 cell_at(const Lattice& L, int x, int y, int z) {
  for (i32 i = 0; i < L.n; ++i)
    if (L.p[i][0] == x && L.p[i][1] == y && L.p[i][2] == z) return i;
  return -1;
}

Lattice make(const std::vector<CellIn>& cells) {
  LatticeOptions lo;
  lo.h = 0.125;
  lo.law.game = true;
  lo.law.fragility = 0.25;
  lo.contact.enabled = true;
  Lattice L = build_lattice(cells, lo);
  L.enable_damage();
  return L;
}

struct Result {
  EquilibriumStats es;
  f64 max_dz = 0.0;
  int separations = 0;
};

Result solve(Lattice& L) {
  Result r;
  std::vector<f64> u(6 * size_t(L.n), 0.0);
  StaticsOptions so;
  so.corot = true;
  so.damage = true;
  so.max_iters = 120;
  const DamageField dc = L.dmg;
  r.es = solve_equilibrium(L, u, gravity_vector(L, 9.81), dc, so, nullptr);
  for (i32 i = 0; i < L.n; ++i) r.max_dz = std::max(r.max_dz, std::abs(u[6 * size_t(i) + 2]));
  const LawSweep sw = sweep_law(L, u.data(), dc, true, L.kscale);
  for (const RuptureCandidate& c : sw.candidates) r.separations += c.separate ? 1 : 0;
  return r;
}

}  // namespace

TEST_CASE("contacts: a column cracked through a section stands on the crack in compression") {
  std::vector<CellIn> cells;
  add_box(cells, -2, 4, -2, 4, -2, 0, MaterialId::Rock, true);  // anchored footing
  add_box(cells, 0, 2, 0, 2, 0, 20, MaterialId::Rc, false);     // 2 x 2 x 20 column (2.5 m)
  Lattice L = make(cells);
  // crack every bond across z = 9 | 10
  int cracked = 0;
  for (int x = 0; x < 2; ++x)
    for (int y = 0; y < 2; ++y) {
      const i32 c = cell_at(L, x, y, 9);
      REQUIRE(c >= 0);
      L.crack_bond(c, 2);
      ++cracked;
    }
  CHECK(cracked == 4);
  // still one supported piece: a crack is not a detachment
  CHECK(unsupported_components(L).empty());
  const Result r = solve(L);
  CHECK(r.es.converged);
  CHECK(r.separations == 0);
  CHECK(r.max_dz < 1e-3);  // the upper part rests on the crack (elastic shortening only)
}

TEST_CASE("contacts: a cantilever cracked at its root is a mechanism: it hinges open, separates and falls") {
  std::vector<CellIn> cells;
  add_box(cells, -4, 0, 0, 2, 0, 8, MaterialId::Rock, true);  // anchored wall
  add_box(cells, 0, 16, 0, 2, 4, 6, MaterialId::Rc, false);   // 2 m cantilever, 2 x 2 section
  auto crack_root = [](Lattice& L) {
    for (int y = 0; y < 2; ++y)
      for (int z = 4; z < 6; ++z) {
        const i32 c = cell_at(L, -1, y, z);
        REQUIRE(c >= 0);
        L.crack_bond(c, 0);
      }
  };
  {
    Lattice L = make(cells);
    crack_root(L);
    CHECK(unsupported_components(L).empty());  // touching: not detached yet
    // no tension and (without axial force) no rocking moment across the crack: there is no
    // static equilibrium, the static solve must not pretend otherwise
    const Result r = solve(L);
    CHECK_FALSE(r.es.converged);
  }
  // dynamically the arm rotates about the crack's lower edge until the gap passes the
  // separation limit; then it breaks off and falls
  Lattice L = make(cells);
  crack_root(L);
  DynamicsOptions dop;
  dop.rayleigh_alpha = 1.0;
  Dynamics dyn;
  dyn.init(L, dop);
  size_t detached = 0;
  int steps = 0;
  for (; steps < 240 && detached == 0; ++steps) {
    dyn.step();
    for (const DetachedIsland& isl : dyn.take_islands()) detached += isl.cells.size();
  }
  MESSAGE("cantilever separated after " << steps << " steps (" << detached << " cells)");
  CHECK(steps < 120);        // well within 2 s
  CHECK(detached >= 56u);    // the arm (the final break may run one cell out from the crack)
}

TEST_CASE("contacts: the contact secant is compression-only with friction and rocking limits") {
  std::vector<CellIn> cells;
  add_box(cells, 0, 2, 0, 1, 0, 1, MaterialId::Rc, false);
  Lattice L = make(cells);
  const i32 c = cell_at(L, 0, 0, 0);
  L.crack_bond(c, 0);
  const f64 s = L.kscale;
  // pure compression: stick, full stiffness
  for (f64 v : contact_scales(L, 0, c, {-1e-5, 0, 0, 0, 0, 0}, s)) CHECK(v == doctest::Approx(1.0));
  // opening: the floor on every component
  for (f64 v : contact_scales(L, 0, c, {1e-6, 0, 0, 0, 0, 0}, s)) CHECK(v == doctest::Approx(kMinSecant));
  // shear beyond friction: the shear secant limits |V| to mu |N|; the normal stays stiff
  const BondModel& m = L.bond(0, c);
  const f64 dn = -1e-6, dv = 10.0 * L.contact.friction * m.k[0] / m.k[1] * 1e-6;
  const Vec6 sc = contact_scales(L, 0, c, {dn, dv, 0, 0, 0, 0}, s);
  CHECK(sc[0] == doctest::Approx(1.0));
  CHECK(sc[1] * m.k[1] * dv == doctest::Approx(L.contact.friction * m.k[0] * 1e-6).epsilon(1e-6));
  // bending beyond rocking: |M| capped at |N| a / 2
  const f64 side = std::sqrt(m.area_min);
  const f64 dm = 4.0 * 0.5 * m.k[0] * 1e-6 * side / m.k[4];
  const Vec6 sr = contact_scales(L, 0, c, {dn, 0, 0, 0, dm, 0}, s);
  CHECK(sr[4] * m.k[4] * dm == doctest::Approx(0.5 * m.k[0] * 1e-6 * side).epsilon(1e-6));
}

TEST_CASE("plates: slab cells are classified; in-plane bonds get plate stiffness and capacity") {
  std::vector<CellIn> cells;
  add_box(cells, 0, 16, 0, 16, 0, 2, MaterialId::Rc, false);   // slab 2 m x 2 m x 25 cm
  add_box(cells, 20, 22, 0, 2, 0, 16, MaterialId::Rc, false);  // a column: not a plate
  LatticeOptions lo;
  lo.h = 0.125;
  lo.law.game = true;
  lo.plate.enabled = true;
  const Lattice P = build_lattice(cells, lo);
  lo.plate.enabled = false;
  const Lattice B = build_lattice(cells, lo);
  const i32 mid = cell_at(P, 8, 8, 0), col = cell_at(P, 20, 0, 8);
  REQUIRE(mid >= 0);
  REQUIRE(col >= 0);
  CHECK(P.plate[mid] == 3);  // normal along z
  CHECK(P.plate[col] == 0);
  // in-plane bond (x): stiffer by 1 / (1 - nu^2); the bond across the slab (z) unchanged
  const f64 pk = 1.0 / (1.0 - 0.2 * 0.2);
  CHECK(P.bond(0, mid).k[0] == doctest::Approx(B.bond(0, mid).k[0] * pk));
  CHECK(P.bond(2, mid).k[0] == doctest::Approx(B.bond(2, mid).k[0]));
  CHECK(P.bond(2, col).k[0] == doctest::Approx(B.bond(2, col).k[0]));
  // capacity: the plate bond reaches its onset at a larger jump
  const Vec6 g{1e-5, 0, 0, 0, 0, 0};
  const f64 phi_p = evaluate_law(P.bond(0, mid), g, 0.0, true).phi, phi_b = evaluate_law(B.bond(0, mid), g, 0.0, true).phi;
  CHECK(phi_p < phi_b);
}
