// Fragment-graph stress (docs/V2_DESIGN.md §2): equilibrium against statics, preconditioners,
// in-place updates, failure modes.
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/stress/stress.hpp"

using namespace svx;

namespace {

// A bond between two nodes (or a node and a support) through a square section of side a.
SBond bond(const StressProblem& P, i32 A, i32 B, const V3& n, const V3& p, f64 a) {
  SBond s;
  s.a = A;
  s.b = B;
  s.ma = s.mb = MaterialId::Concrete;
  s.n = n;
  s.t1 = std::abs(n.x) > 0.5 ? V3{0, 1, 0} : V3{1, 0, 0};
  s.t2 = cross(s.n, s.t1);
  s.area = a * a;
  s.p = p;
  s.s1 = s.s2 = a * a * a * a / 12.0;
  s.c1 = s.c2 = 0.5 * a;
  s.rmax = std::sqrt(0.5) * a;
  const V3 ca = P.nodes[size_t(A)].c;
  s.la = std::abs(dot(p - ca, n));
  s.lb = B >= 0 ? std::abs(dot(P.nodes[size_t(B)].c - p, n)) : s.la;
  s.faces = 16;
  return s;
}

// A cantilever of n cubes of side L along x from a support face at x = 0.
StressProblem cantilever(int n, f64 L, f64 mass) {
  StressProblem P;
  for (int i = 0; i < n; ++i) {
    SNode nd;
    nd.c = V3{(i + 0.5) * L, 0, 0};
    nd.mass = mass;
    P.nodes.push_back(nd);
  }
  P.bonds.push_back(bond(P, 0, -1, V3{-1, 0, 0}, V3{0, 0, 0}, L));
  for (int i = 0; i + 1 < n; ++i) P.bonds.push_back(bond(P, i, i + 1, V3{1, 0, 0}, V3{(i + 1) * L, 0, 0}, L));
  return P;
}

std::vector<f64> gravity(const StressProblem& P) {
  std::vector<f64> f(6 * P.nodes.size(), 0.0);
  for (size_t i = 0; i < P.nodes.size(); ++i) f[6 * i + 2] = -9.81 * P.nodes[i].mass;
  return f;
}

f64 shear(const BondLoad& L) { return std::sqrt(L.V1 * L.V1 + L.V2 * L.V2); }
f64 moment(const BondLoad& L) { return std::sqrt(L.M1 * L.M1 + L.M2 * L.M2); }

}  // namespace

TEST_CASE("stress: a cantilever carries its weight to the support (statics, exactly)") {
  const int n = 12;
  const f64 L = 0.5, m = 300.0;
  StressProblem P = cantilever(n, L, m);
  StressOptions so;
  so.amg_min_nodes = 4;  // (use the multigrid)
  REQUIRE(P.assemble(so));
  std::vector<f64> u;
  const PcgResult r = P.solve(gravity(P), u, 1e-10, 500, false);
  CHECK(r.converged);
  // root: shear = total weight, moment = weight x lever arm (determinate)
  const BondLoad root = P.bond_load(0, u);
  f64 W = 0.0, M = 0.0;
  for (int i = 0; i < n; ++i) {
    W += 9.81 * m;
    M += 9.81 * m * (i + 0.5) * L;
  }
  CHECK(shear(root) == doctest::Approx(W).epsilon(1e-6));
  CHECK(moment(root) == doctest::Approx(M).epsilon(1e-6));
  CHECK(std::abs(root.N) < 1e-6 * W);
  // at the tip joint: the last cube's weight at half a cube
  const BondLoad tip = P.bond_load(n - 1, u);
  CHECK(shear(tip) == doctest::Approx(9.81 * m).epsilon(1e-6));
  CHECK(moment(tip) == doctest::Approx(9.81 * m * 0.5 * L).epsilon(1e-6));
  // and the fibre stress check reads the bending: phi grows towards the root
  const f64 phi_root = bond_utilization(P.bonds[0], root, 1.0);
  const f64 phi_tip = bond_utilization(P.bonds[size_t(n - 1)], tip, 1.0);
  CHECK(phi_root > 50.0 * phi_tip);
}

TEST_CASE("stress: multigrid and block-Jacobi preconditioning agree on a 3D block") {
  // 8 x 8 x 8 cubes standing on supports
  const int N = 8;
  const f64 L = 0.4;
  auto build = [&]() {
    StressProblem P;
    auto id = [&](int x, int y, int z) { return (x * N + y) * N + z; };
    for (int x = 0; x < N; ++x)
      for (int y = 0; y < N; ++y)
        for (int z = 0; z < N; ++z) {
          SNode nd;
          nd.c = V3{(x + 0.5) * L, (y + 0.5) * L, (z + 0.5) * L};
          nd.mass = 400.0;
          P.nodes.push_back(nd);
        }
    for (int x = 0; x < N; ++x)
      for (int y = 0; y < N; ++y)
        for (int z = 0; z < N; ++z) {
          const V3 c = P.nodes[size_t(id(x, y, z))].c;
          if (z == 0) P.bonds.push_back(bond(P, id(x, y, z), -1, V3{0, 0, -1}, c - V3{0, 0, 0.5 * L}, L));
          if (x + 1 < N) P.bonds.push_back(bond(P, id(x, y, z), id(x + 1, y, z), V3{1, 0, 0}, c + V3{0.5 * L, 0, 0}, L));
          if (y + 1 < N) P.bonds.push_back(bond(P, id(x, y, z), id(x, y + 1, z), V3{0, 1, 0}, c + V3{0, 0.5 * L, 0}, L));
          if (z + 1 < N) P.bonds.push_back(bond(P, id(x, y, z), id(x, y, z + 1), V3{0, 0, 1}, c + V3{0, 0, 0.5 * L}, L));
        }
    return P;
  };
  StressProblem A = build(), B = build();
  StressOptions sa, sb;
  sa.amg_min_nodes = 16;
  sb.amg_min_nodes = 1 << 30;
  REQUIRE(A.assemble(sa));
  REQUIRE(B.assemble(sb));
  std::vector<f64> ua, ub;
  const PcgResult ra = A.solve(gravity(A), ua, 1e-9, 2000, false);
  const PcgResult rb = B.solve(gravity(B), ub, 1e-9, 5000, false);
  CHECK(ra.converged);
  CHECK(rb.converged);
  CHECK(ra.iters < rb.iters);  // (the multigrid is the better preconditioner)
  f64 err = 0.0, mag = 0.0;
  for (size_t k = 0; k < ua.size(); ++k) {
    err = std::max(err, std::abs(ua[k] - ub[k]));
    mag = std::max(mag, std::abs(ua[k]));
  }
  CHECK(err <= 1e-6 * mag);
  // supports carry the whole weight
  f64 R = 0.0;
  for (i32 b = 0; b < static_cast<i32>(A.bonds.size()); ++b)
    if (A.bonds[size_t(b)].b < 0) R += A.bond_load(b, ua).N;
  CHECK(-R == doctest::Approx(9.81 * 400.0 * N * N * N).epsilon(1e-5));  // (compression: N < 0)
}

TEST_CASE("stress: a free chain under balanced end forces is in uniform tension (pinned reference)") {
  const int n = 9;
  StressProblem P = cantilever(n, 0.5, 100.0);
  P.bonds.erase(P.bonds.begin());  // no support
  P.nodes[4].fixed = true;         // the reference node
  REQUIRE(P.assemble());
  std::vector<f64> f(6 * size_t(n), 0.0), u;
  f[0] = -1000.0;  // pulled apart at both ends
  f[6 * size_t(n - 1)] = 1000.0;
  const PcgResult r = P.solve(f, u, 1e-10, 500, false);
  CHECK(r.converged);
  for (i32 b = 0; b < static_cast<i32>(P.bonds.size()); ++b) CHECK(P.bond_load(b, u).N == doctest::Approx(1000.0).epsilon(1e-6));
}

TEST_CASE("stress: removing a bond in place equals assembling without it") {
  StressProblem A = cantilever(10, 0.5, 200.0), B = cantilever(10, 0.5, 200.0);
  // a second support under node 6 keeps the tail supported once bond 3 is gone
  A.bonds.push_back(bond(A, 6, -1, V3{0, 0, -1}, A.nodes[6].c - V3{0, 0, 0.25}, 0.5));
  B.bonds.push_back(bond(B, 6, -1, V3{0, 0, -1}, B.nodes[6].c - V3{0, 0, 0.25}, 0.5));
  StressOptions so;
  so.amg_min_nodes = 1 << 30;
  REQUIRE(A.assemble(so));
  A.remove_bond(3);
  B.bonds[3].broken = true;
  REQUIRE(B.assemble(so));
  std::vector<f64> ua, ub;
  A.solve(gravity(A), ua, 1e-11, 2000, false);
  B.solve(gravity(B), ub, 1e-11, 2000, false);
  f64 err = 0.0, mag = 0.0;
  for (size_t k = 0; k < ua.size(); ++k) {
    err = std::max(err, std::abs(ua[k] - ub[k]));
    mag = std::max(mag, std::abs(ub[k]));
  }
  CHECK(err <= 1e-6 * mag);
}

TEST_CASE("stress: failure modes follow the fibre stresses") {
  StressProblem P = cantilever(2, 0.5, 1.0);
  const SBond& b = P.bonds[1];
  const BondStrength S = bond_strength(b, 1.0);
  BondLoad L;
  FailMode mode;
  L.N = 1.01 * S.ft * b.area;  // pure tension just over strength
  CHECK(bond_utilization(b, L, 1.0, &mode) == doctest::Approx(1.01));
  CHECK(mode == FailMode::Tension);
  L = BondLoad{};
  L.N = -1.2 * S.fc * b.area;  // crushing
  CHECK(bond_utilization(b, L, 1.0, &mode) == doctest::Approx(1.2));
  CHECK(mode == FailMode::Crush);
  L = BondLoad{};
  L.V1 = S.coh * b.area;  // shear (x 1.5 for the parabolic distribution)
  CHECK(bond_utilization(b, L, 1.0, &mode) == doctest::Approx(1.5));
  CHECK(mode == FailMode::Shear);
  // compression raises the shear strength (friction); fragility divides every strength (not the
  // friction coefficient)
  L.N = -b.area * S.coh / S.mu;
  CHECK(bond_utilization(b, L, 1.0) == doctest::Approx(0.75));
  CHECK(bond_utilization(b, L, 2.0) == doctest::Approx(1.0));
}

namespace {

// n^3 cubes of side L standing on supports; nodes with z >= keep_z are retired (gone) when
// retire_top, else left out.
StressProblem block(int n, f64 L, int keep_z, bool retire_top) {
  StressProblem P;
  const int nz = retire_top ? n : keep_z;
  auto id = [&](int x, int y, int z) { return (x * n + y) * nz + z; };
  for (int x = 0; x < n; ++x)
    for (int y = 0; y < n; ++y)
      for (int z = 0; z < nz; ++z) {
        SNode nd;
        nd.c = V3{(x + 0.5) * L, (y + 0.5) * L, (z + 0.5) * L};
        nd.mass = 400.0;
        nd.gone = z >= keep_z;
        P.nodes.push_back(nd);
      }
  for (int x = 0; x < n; ++x)
    for (int y = 0; y < n; ++y)
      for (int z = 0; z < nz; ++z) {
        const V3 c = P.nodes[size_t(id(x, y, z))].c;
        if (z == 0) P.bonds.push_back(bond(P, id(x, y, z), -1, V3{0, 0, -1}, c - V3{0, 0, 0.5 * L}, L));
        if (x + 1 < n) P.bonds.push_back(bond(P, id(x, y, z), id(x + 1, y, z), V3{1, 0, 0}, c + V3{0.5 * L, 0, 0}, L));
        if (y + 1 < n) P.bonds.push_back(bond(P, id(x, y, z), id(x, y + 1, z), V3{0, 1, 0}, c + V3{0, 0.5 * L, 0}, L));
        if (z + 1 < nz) P.bonds.push_back(bond(P, id(x, y, z), id(x, y, z + 1), V3{0, 0, 1}, c + V3{0, 0, 0.5 * L}, L));
      }
  return P;
}

std::vector<f64> gravity_live(const StressProblem& P) {
  std::vector<f64> f(6 * P.nodes.size(), 0.0);
  for (size_t i = 0; i < P.nodes.size(); ++i)
    if (!P.nodes[i].gone) f[6 * i + 2] = -9.81 * P.nodes[i].mass;
  return f;
}

}  // namespace

TEST_CASE("stress: retiring one group, then another, never takes a bond out of K twice") {
  // A retired chip keeps its inner bonds intact (its topology) while they are out of K; a later
  // retirement must not subtract them again (K would turn indefinite).
  StressProblem A = cantilever(12, 0.5, 200.0), B = cantilever(12, 0.5, 200.0);
  for (StressProblem* P : {&A, &B})
    for (int i : {3, 6, 9}) P->bonds.push_back(bond(*P, i, -1, V3{0, 0, -1}, P->nodes[size_t(i)].c - V3{0, 0, 0.25}, 0.5));
  StressOptions so;
  so.amg_min_nodes = 1 << 30;
  REQUIRE(A.assemble(so));
  A.retire_nodes({10, 11});  // a chip
  for (SBond& b : A.bonds)
    if (b.a == 10 && b.b == 11) b.broken = false;  // (its inner bond: intact, out of K)
  A.retire_nodes({1});       // another one
  for (int i : {1, 10, 11}) B.nodes[size_t(i)].gone = true;
  REQUIRE(B.assemble(so));
  std::vector<f64> ua, ub;
  const PcgResult ra = A.solve(gravity_live(A), ua, 1e-11, 2000, false);
  const PcgResult rb = B.solve(gravity_live(B), ub, 1e-11, 2000, false);
  CHECK(ra.converged);
  CHECK_FALSE(ra.breakdown);
  CHECK(rb.converged);
  f64 err = 0.0, mag = 0.0;
  for (size_t i = 0; i < A.nodes.size(); ++i) {
    if (A.nodes[i].gone) continue;
    for (int q = 0; q < 6; ++q) {
      err = std::max(err, std::abs(ua[6 * i + size_t(q)] - ub[6 * i + size_t(q)]));
      mag = std::max(mag, std::abs(ub[6 * i + size_t(q)]));
    }
  }
  CHECK(err <= 1e-6 * mag);
  // the chip's bond stays intact for the topology (a split keeps the chip whole)
  bool inner = false;
  for (const SBond& b : A.bonds) inner = inner || (b.a == 10 && b.b == 11 && !b.broken);
  CHECK(inner);
  // ... also through a fresh assembly
  REQUIRE(A.assemble(so));
  for (const SBond& b : A.bonds)
    if (b.a == 10 && b.b == 11) CHECK_FALSE(b.broken);
}

TEST_CASE("stress: retired rows do not slow the multigrid down") {
  StressOptions so;
  so.amg_min_nodes = 16;
  StressProblem with = block(14, 0.4, 9, true), without = block(14, 0.4, 9, false);
  REQUIRE(with.assemble(so));
  REQUIRE(without.assemble(so));
  std::vector<f64> uw, uo;
  const PcgResult rw = with.solve(gravity_live(with), uw, 1e-8, 500, false);
  const PcgResult ro = without.solve(gravity_live(without), uo, 1e-8, 500, false);
  MESSAGE("pcg iterations: " << ro.iters << " without the retired rows, " << rw.iters << " with them");
  CHECK(rw.converged);
  CHECK(ro.converged);
  CHECK(rw.iters <= ro.iters + 4);
}

TEST_CASE("stress: a solve reports no answer for non-finite loads, and u = 0 for none") {
  StressProblem P = cantilever(6, 0.5, 100.0);
  REQUIRE(P.assemble());
  std::vector<f64> u(6 * P.nodes.size(), 1.0);  // (a warm start)
  std::vector<f64> f(6 * P.nodes.size(), 0.0);
  const PcgResult r0 = P.solve(f, u, 1e-9, 100, true);
  CHECK(r0.converged);
  for (f64 v : u) CHECK(v == 0.0);
  f[2] = NAN;
  const PcgResult r1 = P.solve(f, u, 1e-9, 100, false);
  CHECK_FALSE(r1.converged);
  CHECK(r1.breakdown);
}
