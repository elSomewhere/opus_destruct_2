// Fine lattice operator: statics against beam theory and exact equilibrium.
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/solve/lattice.hpp"
#include "svx/solve/multigrid.hpp"

using namespace svx;

namespace {

Lattice cantilever(int len, int b, f64 h, MaterialId mat = MaterialId::Concrete) {
  std::vector<CellIn> cells;
  for (int x = 0; x <= len; ++x)
    for (int y = 0; y < b; ++y)
      for (int z = 0; z < b; ++z) {
        CellIn c;
        c.p = {x, y, z};
        c.mat = mat;
        c.anchored = (x == 0);
        cells.push_back(c);
      }
  LatticeOptions o;
  o.h = h;
  return build_lattice(cells, o);
}

std::vector<f64> solve(const Lattice& L, const std::vector<f64>& f, PcgStats* st = nullptr) {
  Multigrid mg;
  mg.build(L, MGOptions{});
  std::vector<f64> u(f.size(), 0.0);
  PcgStats s = pcg_solve(mg, L.n, f.data(), u.data(), 1e-10, 500, false);
  if (st) *st = s;
  return u;
}

}  // namespace

TEST_CASE("bond model: full voxel section properties") {
  const SectionFixes fx;
  const Section s = cell_section(0, {1, 1, 1}, 0.125, fx);
  CHECK(s.A == doctest::Approx(0.125 * 0.125));
  CHECK(s.I1 == doctest::Approx(std::pow(0.125, 4) / 12.0));
  CHECK(s.J == doctest::Approx(0.1407 * std::pow(0.125, 4)).epsilon(0.002));
  SectionFixes proto;
  proto.st_venant_j = false;
  proto.fix_y_axes = false;
  const Section p = cell_section(0, {1, 1, 1}, 0.125, proto);
  CHECK(p.J == doctest::Approx(std::pow(0.125, 4) / 6.0));
}

TEST_CASE("bond model: Y-edge axis fix only matters for anisotropic cells") {
  SectionFixes fixed, proto;
  proto.fix_y_axes = false;
  const std::array<f64, 3> slab = {1.0, 1.0, 0.25};  // rc_floor effDims
  const Section sf = cell_section(1, slab, 1.0, fixed);
  const Section sp = cell_section(1, slab, 1.0, proto);
  // Y edge frame (n,t1,t2) = (y,z,x): bending about t1=z uses fibres along x (extent 1.0).
  CHECK(sf.I1 == doctest::Approx(0.25 * 1.0 / 12.0));   // s1=z extent 0.25, s2=x extent 1 -> s1*s2^3/12
  CHECK(sp.I1 == doctest::Approx(1.0 * 0.25 * 0.25 * 0.25 / 12.0));  // prototype swap
  CHECK(sf.I1 / sp.I1 == doctest::Approx(16.0));
}

TEST_CASE("cantilever under self-weight: exact root reactions and Timoshenko deflection") {
  const int len = 40, b = 4;
  const f64 h = 0.125;
  const Lattice L = cantilever(len, b, h);
  std::vector<f64> f(6 * size_t(L.n));
  gravity_load(L, 9.81, f.data());
  PcgStats st;
  const std::vector<f64> u = solve(L, f, &st);
  REQUIRE(st.converged);
  // Exact equilibrium: vertical shear through the root bonds equals the total weight.
  std::vector<f64> F(6 * size_t(L.n));
  bond_forces(L, 0, u.data(), F.data());
  f64 shear = 0.0, weight = 0.0;
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i]) {
      if (L.nbr[0][i] >= 0) shear += F[6 * size_t(i) + 2];  // V2 = shear along z for x-bonds
    } else {
      weight += L.mass[i] * 9.81;
    }
  }
  CHECK(std::abs(shear) == doctest::Approx(weight).epsilon(1e-6));
  // Tip deflection vs Timoshenko with the lattice's effective span (spike: ratio 1.0453).
  const Material& m = material(MaterialId::Concrete);
  const f64 A = b * b * h * h, I = std::pow(b * h, 4) / 12.0;
  const f64 q = m.rho * A * 9.81;
  const f64 Lb = len * h;
  const f64 w_theory = q * std::pow(Lb, 4) / (8 * m.E * I) + q * Lb * Lb / (2 * (5.0 / 6.0) * m.G * A);
  f64 w_tip = 0.0;
  int ntip = 0;
  for (i32 i = 0; i < L.n; ++i)
    if (L.p[i][0] == len) {
      w_tip += u[6 * size_t(i) + 2];
      ++ntip;
    }
  w_tip /= ntip;
  CHECK(-w_tip / w_theory == doctest::Approx(1.0453).epsilon(0.005));
}

TEST_CASE("operator is symmetric") {
  const Lattice L = cantilever(6, 2, 0.125);
  const size_t m = 6 * size_t(L.n);
  std::vector<f64> x(m), y(m), Kx(m), Ky(m);
  for (size_t i = 0; i < m; ++i) {
    x[i] = std::sin(0.37 * i + 0.1);
    y[i] = std::cos(0.91 * i + 0.3);
  }
  for (i32 i = 0; i < L.n; ++i)
    if (L.anchored[i])
      for (int q = 0; q < 6; ++q) x[6 * size_t(i) + q] = y[6 * size_t(i) + q] = 0.0;
  apply_stiffness(L, x.data(), Kx.data());
  apply_stiffness(L, y.data(), Ky.data());
  f64 a = 0.0, b = 0.0;
  for (size_t i = 0; i < m; ++i) {
    a += y[i] * Kx[i];
    b += x[i] * Ky[i];
  }
  CHECK(a == doctest::Approx(b).epsilon(1e-12));
}
