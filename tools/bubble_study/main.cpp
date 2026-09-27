// svx_bubble_study — event bubble (composite dynamics) vs full-fine dynamics truth (plan Phase 2).
//
// Scenario: 2-storey column/slab frame at the design pitch (h = 0.125 m); event: an interior
// storey-1 column is carved out. Both runs start from the same fine static baseline. Reported
// per run: node counts, per-step cost (ms, PCG iterations), and over time the near-field peak
// demand (max law phi of the bonds within R0 of the event, bubble / truth), the deflection of
// the slab above the removed column, and (with damage) ruptured / detached counts.
//
// usage: svx_bubble_study [--steps N] [--R0 C] [--g G] [--L L] [--fragility F] [--elastic]
//                         [--compliance S] [--nx BAYS] [--threads T] [--inflation I]
//                         [--no-truth | --no-bubble]
// (--no-truth / --no-bubble: only one side, e.g. for sweeps where the truth does not change)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/bubble/bubble.hpp"
#include "svx/sim/corot.hpp"
#include "svx/sim/dynamics.hpp"
#include "svx/sim/statics.hpp"

using namespace svx;

namespace {

struct ColRec {
  int s, ix, iy, x0, y0, z0;
};

std::vector<CellIn> building(int nxb, int nyb, int stories, std::vector<ColRec>* cols, int bay = 12, int story_h = 10,
                             int slab_t = 2, int col = 2) {
  const int X = nxb * bay + col + 2, Y = nyb * bay + col + 2;
  std::vector<u8> solid;
  const int Z = stories * (story_h + slab_t) + 1;
  solid.assign(size_t(X) * Y * Z, 0);
  auto S = [&](int x, int y, int z) -> u8& { return solid[(size_t(x) * Y + y) * Z + z]; };
  auto fill = [&](int x0, int x1, int y0, int y1, int z0, int z1, u8 v) {
    for (int x = x0; x < x1; ++x)
      for (int y = y0; y < y1; ++y)
        for (int z = z0; z < z1; ++z) S(x, y, z) = v;
  };
  for (int s = 0; s < stories; ++s) {
    const int z0 = 1 + s * (story_h + slab_t);
    for (int ix = 0; ix <= nxb; ++ix)
      for (int iy = 0; iy <= nyb; ++iy) {
        const int x0 = 1 + ix * bay, y0 = 1 + iy * bay;
        fill(x0, x0 + col, y0, y0 + col, z0, z0 + story_h, 1);
        cols->push_back({s, ix, iy, x0, y0, z0});
      }
    const int zs = z0 + story_h;
    fill(1, 1 + nxb * bay + col, 1, 1 + nyb * bay + col, zs, zs + slab_t, 1);
  }
  std::vector<CellIn> cells;
  for (int x = 0; x < X; ++x)
    for (int y = 0; y < Y; ++y)
      for (int z = 0; z < Z; ++z) {
        if (z == 0) {
          CellIn c;
          c.p = {x, y, z};
          c.mat = MaterialId::Concrete;
          c.anchored = true;
          cells.push_back(c);
        } else if (S(x, y, z)) {
          CellIn c;
          c.p = {x, y, z};
          c.mat = MaterialId::Rc;
          cells.push_back(c);
        }
      }
  return cells;
}

f64 near_peak_phi(const Lattice& L, const std::vector<f64>& u, const std::array<f64, 3>& c, f64 R, bool corot,
                  f64 demand_scale = 1.0) {
  f64 pk = 0.0;
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || L.dead[i] || L.dead[j]) continue;
      f64 d2 = 0.0;
      for (int q = 0; q < 3; ++q) {
        const f64 pq = L.h * (L.p[i][q] + (q == a ? 0.5 : 0.0)) - c[q];
        d2 += pq * pq;
      }
      if (d2 > R * R) continue;
      Vec6 g = bond_jump(L, a, i, u.data(), corot);
      for (auto& v : g) v *= L.kscale * demand_scale;
      const LawEval e = evaluate_law(L.bond(a, i), g, 0.0, L.law.game);
      pk = std::max(pk, e.phi);
    }
  return pk;
}

}  // namespace

int main(int argc, char** argv) {
  int steps = 120, nxb = 3, Lmax = 4;
  f64 R0 = 12, g = 2.0, frag = 1.0, S = 4.0;
  bool elastic = false, tight = false, run_truth = true, run_bubble = true;
  f64 inflation = -1.0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--steps" && i + 1 < argc) steps = std::atoi(argv[++i]);
    else if (a == "--R0" && i + 1 < argc) R0 = std::atof(argv[++i]);
    else if (a == "--g" && i + 1 < argc) g = std::atof(argv[++i]);
    else if (a == "--L" && i + 1 < argc) Lmax = std::atoi(argv[++i]);
    else if (a == "--fragility" && i + 1 < argc) frag = std::atof(argv[++i]);
    else if (a == "--elastic") elastic = true;
    else if (a == "--tight") tight = true;
    else if (a == "--compliance" && i + 1 < argc) S = std::atof(argv[++i]);
    else if (a == "--nx" && i + 1 < argc) nxb = std::atoi(argv[++i]);
    else if (a == "--threads" && i + 1 < argc) set_num_threads(std::atoi(argv[++i]));
    else if (a == "--inflation" && i + 1 < argc) inflation = std::atof(argv[++i]);
    else if (a == "--no-truth") run_truth = false;
    else if (a == "--no-bubble") run_bubble = false;
  }
  std::vector<ColRec> cols;
  const auto cells = building(nxb, nxb, 2, &cols);
  LatticeOptions lo;
  lo.h = 0.125;
  lo.law.game = true;
  lo.law.fragility = frag;
  ColRec target{};
  for (const ColRec& c : cols)
    if (c.s == 0 && c.ix == 1 && c.iy == 1) target = c;
  const std::array<f64, 3> center{lo.h * (target.x0 + 0.5), lo.h * (target.y0 + 0.5), lo.h * (target.z0 + 9)};

  // two identical lattices: truth (full fine dynamics) and bubble
  Lattice LT = build_lattice(cells, lo), LB = build_lattice(cells, lo);
  LT.kscale = LB.kscale = 1.0 / S;
  StaticsOptions so;
  so.damage = false;
  std::vector<f64> u0;
  const auto t0 = std::chrono::steady_clock::now();
  const auto es = solve_equilibrium(LT, u0, gravity_vector(LT, 9.81), DamageField{}, so, nullptr);
  std::printf("frame %dx%d, cells %d (h = 0.125 m, S = %g, fragility %g%s): baseline %.0f ms (conv %d, %d its)\n", nxb,
              nxb, LT.n, S, frag, elastic ? ", elastic" : "",
              std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count(), es.converged ? 1 : 0,
              es.iters);
  std::vector<i32> event;
  for (i32 i = 0; i < LT.n; ++i)
    if (LT.p[i][0] >= target.x0 && LT.p[i][0] < target.x0 + 2 && LT.p[i][1] >= target.y0 &&
        LT.p[i][1] < target.y0 + 2 && LT.p[i][2] >= target.z0 && LT.p[i][2] < target.z0 + 10)
      event.push_back(i);
  // probe: slab cell above the removed column
  i32 probe = -1;
  for (i32 i = 0; i < LT.n; ++i)
    if (LT.p[i][0] == target.x0 && LT.p[i][1] == target.y0 && LT.p[i][2] == target.z0 + 10) probe = i;

  // truth
  DynamicsOptions dop;
  dop.compliance = S;
  dop.rayleigh_alpha = 0.5;
  dop.damage = !elastic;
  dop.settle = false;
  dop.sleep_steps = 1 << 30;
  Dynamics dyn;
  dyn.init(LT, dop);
  dyn.set_state(u0);
  dyn.carve(event);
  // bubble
  BubbleOptions bo;
  bo.comp.R0 = R0;
  bo.comp.grading = g;
  bo.comp.max_level = Lmax;
  bo.damage = !elastic;
  bo.max_steps = 1 << 30;
  bo.sleep_steps = 1 << 30;
  std::vector<i32> nb;  // event neighbours stay fine
  for (i32 c : event)
    for (int a = 0; a < 3; ++a) {
      if (LB.nbr[a][c] >= 0) nb.push_back(LB.nbr[a][c]);
      if (LB.nbrm[a][c] >= 0) nb.push_back(LB.nbrm[a][c]);
    }
  bo.comp.force_fine = nb;
  if (inflation > 0.0) bo.demand_inflation = inflation;
  if (tight) {  // the truth's tolerances
    bo.lin_rtol = dop.lin_rtol;
    bo.lin_atol = dop.lin_atol;
    bo.newton_rtol = dop.newton_rtol;
    bo.newton_atol = dop.newton_atol;
  }
  const std::vector<f64> r = removal_residual(LB, event, u0, bo.corot);
  for (i32 c : event) LB.remove_cell(c);
  Bubble bub;
  const std::array<f64, 3> centers[1] = {center};
  const auto tb = std::chrono::steady_clock::now();
  bub.init(LB, u0, r, centers, bo);
  const f64 init_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - tb).count();
  std::printf("bubble: nodes %d (fine %d) of %d cells, init %.0f ms\n", bub.nodes(), bub.fine_cells(), LB.num_alive(),
              init_ms);
  f64 pk_t = 0.0, pk_b = 0.0, pk_bi = 0.0, zmin_t = 0.0, zmin_b = 0.0, max_zerr = 0.0;
  f64 ms_t = 0.0, ms_b = 0.0, ms_b_first = 0.0;
  i64 pcg_t = 0, pcg_b = 0;
  std::vector<f64> ub;
  std::vector<int> pcg_steady;
  for (int s = 0; s < steps; ++s) {
    const StepStats st = run_truth ? dyn.step() : StepStats{};
    const StepStats sb = run_bubble ? bub.step() : StepStats{};
    ms_t += st.ms_total;
    ms_b += sb.ms_total;
    if (s < 3) ms_b_first += sb.ms_total;
    pcg_t += st.pcg;
    pcg_b += sb.pcg;
    if (s >= 10) pcg_steady.push_back(sb.pcg);
    bub.total_displacement(ub);
    const f64 pt = near_peak_phi(LT, dyn.u(), center, R0 * lo.h, true);
    const f64 pb = near_peak_phi(LB, ub, center, R0 * lo.h, true);
    pk_t = std::max(pk_t, pt);
    pk_b = std::max(pk_b, pb);
    pk_bi = std::max(pk_bi, near_peak_phi(LB, ub, center, R0 * lo.h, true, bo.demand_inflation));
    const f64 zt = probe >= 0 && !LT.dead[probe] ? dyn.u()[6 * size_t(probe) + 2] : 0.0;
    const f64 zb = probe >= 0 && !LB.dead[probe] ? ub[6 * size_t(probe) + 2] : 0.0;
    zmin_t = std::min(zmin_t, zt);
    zmin_b = std::min(zmin_b, zb);
    max_zerr = std::max(max_zerr, std::abs(zt - zb));
    if (std::getenv("SVX_TRACE") && s < 25)
      std::printf("    [trace] step %d bubble max_damage %.4f newton %d pcg %d res %.2e\n", s, sb.max_damage, sb.newton,
                  sb.pcg, sb.residual);
    if (s % 10 == 0 || s == steps - 1)
      std::printf("  step %3d  truth: phi %.3f dz %.3e pcg %3d %6.1f ms | bubble: phi %.3f dz %.3e pcg %3d %6.1f ms  rupt %zu/%zu det %lld/%lld\n",
                  s, pt, zt, st.pcg, st.ms_total, pb, zb, sb.pcg, sb.ms_total, dyn.ruptured().size(),
                  bub.ruptured().size(), static_cast<long long>(dyn.detached_cells()),
                  static_cast<long long>(bub.detached_cells()));
  }
  std::sort(pcg_steady.begin(), pcg_steady.end());
  const int p95 = pcg_steady.empty() ? 0 : pcg_steady[size_t(0.95 * (pcg_steady.size() - 1))];
  std::printf("\npeak-over-time near-field demand: truth %.4f bubble %.4f ratio %.3f (with the bubble's demand "
              "inflation %.2f: %.3f)\n",
              pk_t, pk_b, pk_t > 0 ? pk_b / pk_t : 1.0, bo.demand_inflation, pk_t > 0 ? pk_bi / pk_t : 1.0);
  std::printf("probe min dz: truth %.4e bubble %.4e ratio %.3f  max |dz err| %.3e\n", zmin_t, zmin_b,
              zmin_t != 0 ? zmin_b / zmin_t : 1.0, max_zerr);
  std::printf("ruptured: truth %zu bubble %zu; detached cells: truth %lld bubble %lld\n", dyn.ruptured().size(),
              bub.ruptured().size(), static_cast<long long>(dyn.detached_cells()),
              static_cast<long long>(bub.detached_cells()));
  std::printf("cost: truth %.1f ms/step (%.1f pcg/step); bubble %.1f ms/step (%.1f pcg/step, first 3 steps %.1f ms, "
              "steady pcg p95 %d)\n",
              ms_t / steps, f64(pcg_t) / steps, ms_b / steps, f64(pcg_b) / steps, ms_b_first / 3.0, p95);
  return 0;
}
