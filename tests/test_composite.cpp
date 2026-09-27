// Composite assembly (plan §B4): the per-face moment assembly equals the per-bond Galerkin
// products it replaces, for stiffness and mass, with damaged bonds and partially fixed cells.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/bubble/composite.hpp"
#include "svx/world/region.hpp"

using namespace svx;

namespace {

// A two-storey frame of slabs, columns and a wall on an anchored footing (voxels).
VoxelGrid frame_grid() {
  VoxelGrid g;
  g.h = 0.125;
  const Vox rc = make_vox(MaterialId::Rc, false), rock = make_vox(MaterialId::Rock, true);
  for (i32 x = 0; x < 48; ++x)
    for (i32 y = 0; y < 40; ++y)
      for (i32 z = -3; z < 0; ++z) g.set(x, y, z, rock);
  for (int storey = 0; storey < 2; ++storey) {
    const i32 z0 = storey * 20;
    for (i32 cx : {2, 22, 42})
      for (i32 cy : {2, 32})
        for (i32 x = cx; x < cx + 3; ++x)
          for (i32 y = cy; y < cy + 3; ++y)
            for (i32 z = z0; z < z0 + 17; ++z) g.set(x, y, z, rc);
    for (i32 x = 0; x < 48; ++x)
      for (i32 y = 0; y < 40; ++y)
        for (i32 z = z0 + 17; z < z0 + 20; ++z) g.set(x, y, z, rc);
  }
  for (i32 x = 2; x < 25; ++x)  // a wall between two columns
    for (i32 y = 2; y < 4; ++y)
      for (i32 z = 0; z < 17; ++z) g.set(x, y, z, rc);
  g.lo = {0, 0, -3};
  g.hi = {48, 40, 40};
  return g;
}

}  // namespace

TEST_CASE("composite: face-moment assembly equals the per-bond Galerkin products") {
  const VoxelGrid g = frame_grid();
  Region R = extract_box(g, g.lo, g.hi, LatticeOptions{});
  Lattice& L = R.L;
  REQUIRE(L.n > 10000);
  // damaged bonds (varying secant stiffness) and a few partially fixed cells (masked rows)
  L.enable_damage();
  u64 seed = 7;
  auto rnd = [&]() {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<f64>(seed >> 11) / 9007199254740992.0;
  };
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i)
      if (L.nbr[a][i] >= 0 && rnd() < 0.2) L.dmg[a][i] = static_cast<f32>(0.9 * rnd());
  i32 masked = 0;
  for (i32 i = 0; i < L.n && masked < 12; i += 997)
    if (!L.anchored[i]) {
      L.fixmask[i] = 0b000101;  // x and z translations fixed
      ++masked;
    }
  const std::array<f64, 3> centers[1] = {{0.125 * 22, 0.125 * 20, 0.125 * 18}};
  for (bool fine_bonds : {false, true}) {
    CompositeOptions co;
    co.R0 = 6.0;  // (levels 1-3 all present in this frame)
    co.max_level = 3;
    co.fine_bonds = fine_bonds;
    co.moment_assembly = false;
    const Composite ref = build_composite(L, centers, co);
    co.moment_assembly = true;
    const Composite got = build_composite(L, centers, co);
    REQUIRE(ref.n == got.n);
    REQUIRE(ref.A.col == got.A.col);
    REQUIRE(ref.A.rowptr == got.A.rowptr);
    CHECK(ref.crossing_bonds == got.crossing_bonds);
    i32 levels[4] = {0, 0, 0, 0};
    for (i32 k = 0; k < got.n; ++k) ++levels[std::min<int>(3, got.level[k])];
    CHECK(levels[1] > 0);
    CHECK(levels[2] > 0);
    CHECK(levels[3] > 0);
    // entries relative to their row's scale
    f64 worst = 0.0, worst_m = 0.0;
    for (i32 r = 0; r < got.n; ++r) {
      f64 scale = 0.0;
      for (i32 e = ref.A.rowptr[r]; e < ref.A.rowptr[r + 1]; ++e)
        for (int q = 0; q < 36; ++q) scale = std::max(scale, std::abs(ref.A.val[36 * size_t(e) + q]));
      for (i32 e = ref.A.rowptr[r]; e < ref.A.rowptr[r + 1]; ++e)
        for (int q = 0; q < 36; ++q)
          worst = std::max(worst, std::abs(got.A.val[36 * size_t(e) + q] - ref.A.val[36 * size_t(e) + q]) / scale);
      f64 ms = 0.0;
      for (int q = 0; q < 36; ++q) ms = std::max(ms, std::abs(ref.M[36 * size_t(r) + q]));
      for (int q = 0; q < 36; ++q)
        worst_m = std::max(worst_m, std::abs(got.M[36 * size_t(r) + q] - ref.M[36 * size_t(r) + q]) / std::max(ms, 1e-300));
    }
    MESSAGE("fine bonds " << fine_bonds << ": " << got.n << " nodes (" << levels[0] << "/" << levels[1] << "/" << levels[2]
                          << "/" << levels[3] << " by level), " << got.crossing_bonds << " crossing bonds; max rel diff A "
                          << worst << ", M " << worst_m);
    CHECK(worst < 1e-10);
    CHECK(worst_m < 1e-10);
  }
}
