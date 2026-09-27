// Event bubbles (plan §B5-B6): nominate / decide. A column removed from a 3 x 3-bay, two-storey
// frame overloads the slab at the neighbouring columns, one aggregate beyond the bubble's fine
// region (R0 12). The coarse field cannot fail; with nomination its aggregates there become fine
// and the fine law decides - the collapse of the full-fine truth (60 ruptures, 7016 cells).
#include <vector>

#include "doctest.h"
#include "svx/bubble/bubble.hpp"
#include "svx/sim/statics.hpp"

using namespace svx;

namespace {

// nx x nx bays of 12 voxels, two storeys (10 high, 2-voxel slabs, 2 x 2 columns) on a ground row
std::vector<CellIn> frame(int nx, std::vector<i32>* column) {
  const int bay = 12, story_h = 10, slab_t = 2, col = 2, stories = 2;
  const int X = nx * bay + col + 2, Y = X, Z = stories * (story_h + slab_t) + 1;
  std::vector<u8> solid(size_t(X) * Y * Z, 0);
  auto S = [&](int x, int y, int z) -> u8& { return solid[(size_t(x) * Y + y) * Z + z]; };
  for (int s = 0; s < stories; ++s) {
    const int z0 = 1 + s * (story_h + slab_t);
    for (int ix = 0; ix <= nx; ++ix)
      for (int iy = 0; iy <= nx; ++iy)
        for (int x = 1 + ix * bay; x < 1 + ix * bay + col; ++x)
          for (int y = 1 + iy * bay; y < 1 + iy * bay + col; ++y)
            for (int z = z0; z < z0 + story_h; ++z) S(x, y, z) = 1;
    for (int x = 1; x < 1 + nx * bay + col; ++x)
      for (int y = 1; y < 1 + nx * bay + col; ++y)
        for (int z = z0 + story_h; z < z0 + story_h + slab_t; ++z) S(x, y, z) = 1;
  }
  std::vector<CellIn> cells;
  for (int x = 0; x < X; ++x)
    for (int y = 0; y < Y; ++y)
      for (int z = 0; z < Z; ++z) {
        if (z > 0 && !S(x, y, z)) continue;
        CellIn c;
        c.p = {x, y, z};
        c.mat = z == 0 ? MaterialId::Concrete : MaterialId::Rc;
        c.anchored = z == 0;
        // the storey-1 column of bay (1, 1): the event
        if (z >= 1 && z < 11 && x >= 13 && x < 15 && y >= 13 && y < 15) column->push_back(static_cast<i32>(cells.size()));
        cells.push_back(c);
      }
  return cells;
}

struct Outcome {
  size_t ruptured = 0;
  i64 detached = 0;
  int refinements = 0, coarsenings = 0, grows = 0;
  i64 held = 0;
  f64 dp = 0.0, dke = 0.0;
};

// (impulse > 0: nothing is removed; the slab over the column takes a vertical blow instead, N s)
Outcome remove_column(bool nominate, f64 fragility = 0.040, int steps = 120, f64 impulse = 0.0, bool marginal = true,
                      f64 R0 = 12.0) {
  std::vector<i32> column;
  const auto cells = frame(3, &column);
  LatticeOptions lo;
  lo.h = 0.125;
  lo.law.game = true;
  lo.law.fragility = fragility;
  Lattice L = build_lattice(cells, lo);
  L.kscale = 1.0 / 4.0;
  StaticsOptions so;
  so.damage = false;
  std::vector<f64> u0;
  solve_equilibrium(L, u0, gravity_vector(L, 9.81), DamageField{}, so, nullptr);
  BubbleOptions bo;
  bo.comp.R0 = R0;
  bo.comp.grading = 2.0;
  bo.comp.max_level = 4;
  bo.max_steps = 1 << 30;
  bo.sleep_steps = 1 << 30;
  bo.nominate = nominate;
  if (!marginal) bo.marginal_eps = -1.0;
  for (i32 c : column)  // (the event's neighbours stay fine)
    for (int a = 0; a < 3; ++a) {
      if (L.nbr[a][c] >= 0) bo.comp.force_fine.push_back(L.nbr[a][c]);
      if (L.nbrm[a][c] >= 0) bo.comp.force_fine.push_back(L.nbrm[a][c]);
    }
  std::vector<f64> r;
  if (impulse > 0.0) {
    r.assign(6 * size_t(L.n), 0.0);
    bo.comp.force_fine.clear();
  } else {
    r = removal_residual(L, column, u0, bo.corot);
    for (i32 c : column) L.remove_cell(c);
  }
  Bubble bub;
  const std::array<f64, 3> centers[1] = {{0.125 * 13.5, 0.125 * 13.5, 0.125 * 10}};
  bub.init(L, u0, r, centers, bo);
  i32 hit = -1;  // the slab cell over the column
  for (i32 i = 0; i < L.n && impulse > 0.0; ++i)
    if (L.p[i][0] == 13 && L.p[i][1] == 13 && L.p[i][2] == 12) hit = i;
  for (int s = 0; s < steps; ++s) {
    if (s == 0 && hit >= 0) {
      const f64 f[6] = {0.0, 0.0, -impulse * 60.0, 0.0, 0.0, 0.0};  // (over one 1/60 s step)
      bub.add_force(hit, f);
    }
    bub.step();
  }
  return {bub.ruptured().size(), bub.detached_cells(), bub.refinements(), bub.coarsenings(), bub.grows(), bub.held_bonds(),
          bub.repartition_momentum_error(), bub.repartition_energy_gain()};
}

}  // namespace

TEST_CASE("bubble: nominate / decide refines the overloaded aggregates, and the fine law fails them") {
  const Outcome off = remove_column(false), on = remove_column(true);
  MESSAGE("without nomination: " << off.ruptured << " ruptures, " << off.detached << " detached; with: " << on.ruptured
                                 << " ruptures, " << on.detached << " detached, " << on.refinements
                                 << " refinements (full-fine truth: 60 ruptures, 7016 detached)");
  CHECK(off.ruptured == 0);  // the coarse field cannot fail
  CHECK(on.refinements >= 1);
  CHECK(on.ruptured >= 40);
  CHECK(on.detached > 5000);
}

TEST_CASE("bubble: a refined region that stays quiet goes back into aggregates, momentum exact") {
  // a blow on the slab of a sound frame: its wave nominates aggregates around, nothing fails, it
  // dies down (weaker frames keep aggregates their baseline alone nominates: fine for good)
  const Outcome o = remove_column(true, 0.08, 240, 8000.0);
  MESSAGE(o.refinements << " refinements, " << o.coarsenings << " coarsenings, " << o.ruptured << " ruptures; "
                        << "re-partition momentum error " << o.dp << ", kinetic energy change " << o.dke << " J");
  CHECK(o.refinements >= 1);
  CHECK(o.coarsenings >= 1);
  CHECK(o.ruptured == 0);
  CHECK(o.dp <= 1e-9);    // (plan §B5 gate)
  CHECK(o.dke <= 1e-12);  // no energy gain
}

TEST_CASE("bubble: a step with marginal bonds by the fine/coarse interface is solved again, grown") {
  // Blows on the slab of the sound frame (the default: nomination on). The all-fine truth (the
  // same bubble with R0 over the frame): at F 0.05 a 16,000 N s blow breaks 3 bonds; a 32,000
  // N s one brings the frame down (66 ruptures, 7056 detached). The bonds that decide it lie by
  // the fine/coarse interface: re-solving those steps grown finds both; deciding them where
  // they are misses both (0 ruptures; 4, standing).
  const Outcome local = remove_column(true, 0.05, 120, 16000.0, true), local0 = remove_column(true, 0.05, 120, 16000.0, false);
  const Outcome down = remove_column(true, 0.05, 120, 32000.0, true), down0 = remove_column(true, 0.05, 120, 32000.0, false);
  MESSAGE("16,000 N s: " << local.ruptured << " ruptures (" << local.grows << " re-solves, " << local.held
                         << " marginal bonds), without " << local0.ruptured << "; 32,000 N s: " << down.ruptured << " / "
                         << down.detached << " (" << down.grows << " re-solves), without " << down0.ruptured << " / "
                         << down0.detached << " (truth: 3 / 0 and 66 / 7056)");
  CHECK(local.grows >= 1);
  CHECK(local.ruptured == 3);
  CHECK(local.detached == 0);
  CHECK(local0.ruptured < 3);
  CHECK(down.detached > 5000);
  CHECK(down0.detached == 0);
}
