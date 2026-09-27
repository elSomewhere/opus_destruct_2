// Rigid-aggregation multigrid: convergence on a frame building (research spike parity).
#include <cmath>
#include <cstring>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/solve/lattice.hpp"
#include "svx/solve/multigrid.hpp"

using namespace svx;

namespace {

// Column + slab frame building on an anchored ground layer (research/mlexp/structures.py).
std::vector<CellIn> building(int bays, int stories, int bay = 12, int story_h = 10, int slab_t = 2, int col = 2) {
  std::vector<CellIn> cells;
  const int X = bays * bay + col + 2, Y = X;
  std::vector<unsigned char> solid(size_t(X) * Y * (stories * (story_h + slab_t) + 1), 0);
  const int Z = stories * (story_h + slab_t) + 1;
  auto at = [&](int x, int y, int z) -> unsigned char& { return solid[(size_t(z) * Y + y) * X + x]; };
  for (int x = 0; x < X; ++x)
    for (int y = 0; y < Y; ++y) at(x, y, 0) = 2;  // anchored ground
  for (int s = 0; s < stories; ++s) {
    const int z0 = 1 + s * (story_h + slab_t);
    for (int ix = 0; ix <= bays; ++ix)
      for (int iy = 0; iy <= bays; ++iy)
        for (int x = 1 + ix * bay; x < 1 + ix * bay + col; ++x)
          for (int y = 1 + iy * bay; y < 1 + iy * bay + col; ++y)
            for (int z = z0; z < z0 + story_h; ++z) at(x, y, z) = 1;
    const int zs = z0 + story_h;
    for (int x = 1; x < 1 + bays * bay + col; ++x)
      for (int y = 1; y < 1 + bays * bay + col; ++y)
        for (int z = zs; z < zs + slab_t; ++z) at(x, y, z) = 1;
  }
  for (int z = 0; z < Z; ++z)
    for (int y = 0; y < Y; ++y)
      for (int x = 0; x < X; ++x)
        if (at(x, y, z)) {
          CellIn c;
          c.p = {x, y, z};
          c.anchored = at(x, y, z) == 2;
          cells.push_back(c);
        }
  return cells;
}

}  // namespace

TEST_CASE("multigrid PCG converges fast to a true-residual solution") {
  const std::vector<CellIn> cells = building(2, 2);
  LatticeOptions o;
  o.h = 0.125;
  const Lattice L = build_lattice(cells, o);
  std::vector<f64> f(6 * size_t(L.n));
  gravity_load(L, 9.81, f.data());

  Multigrid mg;
  mg.build(L, MGOptions{});
  const std::vector<i32> levels = mg.level_sizes();
  CHECK(levels.size() >= 3);
  std::vector<f64> u(f.size(), 0.0);
  const PcgStats st = pcg_solve(mg, L.n, f.data(), u.data(), 1e-8, 200, false);
  CHECK(st.converged);
  CHECK(st.iters <= 60);

  f64 un = 0.0, dn = 0.0;
  // True residual with the plain fine operator: ||f - K u|| / ||f||.
  std::vector<f64> Ku(f.size());
  apply_stiffness(L, u.data(), Ku.data());
  for (size_t i = 0; i < f.size(); ++i) {
    dn += (f[i] - Ku[i]) * (f[i] - Ku[i]);
    un += f[i] * f[i];
  }
  CHECK(std::sqrt(dn / un) < 1e-7);
}

TEST_CASE("multigrid iterations stay flat as the building grows") {
  int prev = 0;
  for (int bays : {2, 4}) {
    const std::vector<CellIn> cells = building(bays, 2);
    LatticeOptions o;
    const Lattice L = build_lattice(cells, o);
    std::vector<f64> f(6 * size_t(L.n));
    gravity_load(L, 9.81, f.data());
    Multigrid mg;
    mg.build(L, MGOptions{});
    std::vector<f64> u(f.size(), 0.0);
    const PcgStats st = pcg_solve(mg, L.n, f.data(), u.data(), 1e-8, 300, false);
    CHECK(st.converged);
    if (prev) CHECK(st.iters <= prev + 15);
    prev = st.iters;
    MESSAGE("bays=" << bays << " cells=" << L.n << " iters=" << st.iters);
  }
}

TEST_CASE("solve is bitwise identical across thread counts (determinism rule, plan B9)") {
  const std::vector<CellIn> cells = building(3, 2);
  LatticeOptions o;
  const Lattice L = build_lattice(cells, o);
  std::vector<f64> f(6 * size_t(L.n));
  gravity_load(L, 9.81, f.data());
  auto solve_with = [&](int threads) {
    set_num_threads(threads);
    Multigrid mg;
    mg.build(L, MGOptions{});
    std::vector<f64> u(f.size(), 0.0);
    pcg_solve(mg, L.n, f.data(), u.data(), 1e-10, 300, false);
    return u;
  };
  const std::vector<f64> u1 = solve_with(1);
  const std::vector<f64> u8 = solve_with(8);
  set_num_threads(num_threads());  // keep current setting
  CHECK(std::memcmp(u1.data(), u8.data(), sizeof(f64) * u1.size()) == 0);
  // FNV-1a over the raw bits: compare this value between native and WASM runs.
  u64 hsh = 1469598103934665603ULL;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(u1.data());
  for (size_t i = 0; i < sizeof(f64) * u1.size(); ++i) hsh = (hsh ^ bytes[i]) * 1099511628211ULL;
  MESSAGE("solution hash " << hsh);
}
