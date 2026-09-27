// svx_content_study — Phase 0 Doom content / regime study (plan §C Phase 0).
//
// 1. Voxelizes a Doom map at the design resolution (4 map units = 12.5 cm).
// 2. Finds structural components (run-length connectivity) and anchor distances.
// 3. Solves full-fine self-weight statics per component (multigrid PCG, W-cycle).
// 4. Samples bullet- and rocket-class carve events. Each event is evaluated as a
//    baseline + delta problem: the delta is driven by the released bond forces and solved
//    in a local window (pinned delta at the window rim; valid for local carve events by
//    Saint-Venant — `--validate N` checks N events against a full re-solve). Detachment is
//    checked on the whole component.
//
// usage: svx_content_study --wad FILE [--map NAME] [--mode rock|air] [--events N]
//          [--seed S] [--max-solve CELLS] [--window R] [--validate N] [--json OUT]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/doom/voxelize.hpp"
#include "svx/doom/wad.hpp"
#include "svx/solve/lattice.hpp"
#include "svx/solve/multigrid.hpp"
#include "svx/world/columns.hpp"

using namespace svx;

namespace {

struct Args {
  std::string wad, map, json;
  doom::VoidMode mode = doom::VoidMode::Rock;
  int upv = 4, shell = 8;
  int events = 200;
  u64 seed = 1;
  i64 max_solve = 4000000;
  f64 bullet_r = 1.5, rocket_r = 8.0;
  f64 window = 0.0;  // 0 = auto: max(24, 4 r)
  int validate = 0;
  MGOptions mg;
  int threads = 0;
  bool only_largest = false;
  int maxit = 300;
  f64 rtol = 1e-6;
};

f64 now_s() {
  using namespace std::chrono;
  return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

struct Rng {
  u64 s;
  explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ULL + 1) {}
  u64 next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  f64 uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
  i64 below(i64 n) { return static_cast<i64>(uni() * n) % std::max<i64>(n, 1); }
};

f64 percentile(std::vector<f64> v, f64 p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const f64 idx = p * (v.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(idx));
  const size_t hi = std::min(v.size() - 1, lo + 1);
  return v[lo] + (idx - lo) * (v[hi] - v[lo]);
}

// A solved component: lattice + baseline displacement u0 + per-bond utilization.
struct Solved {
  Lattice L;
  std::vector<f64> u0;
  std::array<std::vector<f32>, 3> util;  // per (axis, lower cell)
  f64 max_util = 0.0, max_disp = 0.0, seconds = 0.0, rel_res = 0.0;
  int iters = 0;
  bool converged = false;
  std::vector<i32> levels;
  std::vector<i32> anchor_dist;  // BFS hops to the nearest anchored cell (-1: none)
};

std::unique_ptr<Solved> solve_component(Lattice&& L, const Args& a) {
  auto S = std::make_unique<Solved>();
  S->L = std::move(L);
  const Lattice& Lr = S->L;
  const f64 t0 = now_s();
  Multigrid mg;
  mg.build(Lr, a.mg);
  S->levels = mg.level_sizes();
  std::vector<f64> f(6 * size_t(Lr.n));
  gravity_load(Lr, 9.81, f.data());
  S->u0.assign(f.size(), 0.0);
  const PcgStats st = pcg_solve(mg, Lr.n, f.data(), S->u0.data(), a.rtol, a.maxit, false);
  S->converged = st.converged;
  S->iters = st.iters;
  S->rel_res = st.rel_res;
  S->seconds = now_s() - t0;
  for (int ax = 0; ax < 3; ++ax) {
    S->util[ax].assign(Lr.n, 0.0f);
    for (i32 i = 0; i < Lr.n; ++i) {
      if (Lr.nbr[ax][i] < 0) continue;
      const f64 u = bond_utilization(bond_force(Lr, ax, i, S->u0.data()), Lr.bond(ax, i).cap);
      S->util[ax][i] = static_cast<f32>(u);
      S->max_util = std::max(S->max_util, u);
    }
  }
  for (i32 i = 0; i < Lr.n; ++i) {
    const f64* ui = &S->u0[6 * size_t(i)];
    S->max_disp = std::max(S->max_disp, std::sqrt(ui[0] * ui[0] + ui[1] * ui[1] + ui[2] * ui[2]));
  }
  // anchor distance (BFS over bonds from anchored cells)
  S->anchor_dist.assign(Lr.n, -1);
  std::deque<i32> q;
  for (i32 i = 0; i < Lr.n; ++i)
    if (Lr.anchored[i]) {
      S->anchor_dist[i] = 0;
      q.push_back(i);
    }
  while (!q.empty()) {
    const i32 c = q.front();
    q.pop_front();
    for (int ax = 0; ax < 3; ++ax) {
      for (i32 nb : {Lr.nbr[ax][c], Lr.nbrm[ax][c]}) {
        if (nb < 0 || S->anchor_dist[nb] >= 0) continue;
        S->anchor_dist[nb] = S->anchor_dist[c] + 1;
        q.push_back(nb);
      }
    }
  }
  return S;
}

// Detachment check after removing cells: BFS from the surviving neighbours of the crater
// until an anchored cell is reached; pieces that exhaust without one are detached.
i64 detached_after(const Lattice& L, const std::vector<u8>& removed, const std::vector<i32>& crater,
                   i64* largest = nullptr) {
  std::vector<u8> seen(L.n, 0);
  i64 detached = 0;
  std::vector<i32> stack, piece;
  for (i32 c : crater)
    for (int ax = 0; ax < 3; ++ax)
      for (i32 s : {L.nbr[ax][c], L.nbrm[ax][c]}) {
        if (s < 0 || removed[s] || L.anchored[s] || seen[s]) continue;
        bool anchored = false;
        stack.assign(1, s);
        piece.clear();
        seen[s] = 1;
        while (!stack.empty() && !anchored) {
          const i32 x = stack.back();
          stack.pop_back();
          piece.push_back(x);
          for (int a2 = 0; a2 < 3; ++a2)
            for (i32 y : {L.nbr[a2][x], L.nbrm[a2][x]}) {
              if (y < 0 || removed[y] || seen[y]) continue;
              if (L.anchored[y]) {
                anchored = true;
                break;
              }
              seen[y] = 1;
              stack.push_back(y);
            }
        }
        if (!anchored) {
          detached += static_cast<i64>(piece.size());
          if (largest) *largest = std::max<i64>(*largest, static_cast<i64>(piece.size()));
        }
      }
  return detached;
}

struct EventResult {
  i64 removed = 0, detached = 0, window_cells = 0;
  i64 largest_piece = 0;           // largest detached piece (voxels)
  i64 crossed1 = 0, crossed5 = 0;  // bonds newly pushed above utilization 1 / 5 (nominal)
  f64 util_before = 0.0, util_after = 0.0, dutil = 0.0, seconds = 0.0;
  int iters = 0;
};

// Delta solve for a carve event. If window <= 0 the whole component is used (reference).
EventResult carve_event(const Solved& S, i32 center, f64 R, f64 window, f64 eval_radius, const Args& a) {
  const Lattice& L = S.L;
  EventResult out;
  const f64 t0 = now_s();
  const auto cp = L.p[center];
  auto d2 = [&](i32 i) {
    const f64 dx = L.p[i][0] - cp[0], dy = L.p[i][1] - cp[1], dz = L.p[i][2] - cp[2];
    return dx * dx + dy * dy + dz * dz;
  };
  std::vector<u8> removed(L.n, 0);
  std::vector<i32> crater;
  for (i32 i = 0; i < L.n; ++i)
    if (!L.anchored[i] && d2(i) <= R * R) {
      removed[i] = 1;
      crater.push_back(i);
    }
  out.removed = static_cast<i64>(crater.size());
  out.detached = detached_after(L, removed, crater, &out.largest_piece);
  // Released-bond residual on the full component (only crater-adjacent cells non-zero).
  std::vector<f64> r0(6 * size_t(L.n));
  released_bond_residual(L, removed, S.u0.data(), r0.data());
  // Window: surviving cells within `window` of the centre; the rim shell is pinned (delta = 0).
  const bool full = window <= 0.0;
  const f64 W2 = window * window, Win2 = (window - 1.5) * (window - 1.5);
  const f64 eval2 = eval_radius * eval_radius;
  std::vector<i32> map_w(L.n, -1);
  std::vector<CellIn> wc;
  std::vector<i32> w2l;
  // Detached pieces make K singular: exclude them from the delta solve (they vanish).
  std::vector<u8> gone = removed;
  if (out.detached > 0) {
    // mark detached pieces (BFS again, cheap relative to the solve)
    std::vector<u8> seen(L.n, 0);
    std::vector<i32> stack, piece;
    for (i32 c : crater)
      for (int ax = 0; ax < 3; ++ax)
        for (i32 s : {L.nbr[ax][c], L.nbrm[ax][c]}) {
          if (s < 0 || removed[s] || L.anchored[s] || seen[s]) continue;
          bool anchored = false;
          stack.assign(1, s);
          piece.clear();
          seen[s] = 1;
          while (!stack.empty()) {
            const i32 x = stack.back();
            stack.pop_back();
            piece.push_back(x);
            for (int a2 = 0; a2 < 3; ++a2)
              for (i32 y : {L.nbr[a2][x], L.nbrm[a2][x]}) {
                if (y < 0 || removed[y] || seen[y]) continue;
                if (L.anchored[y]) {
                  anchored = true;
                  continue;
                }
                seen[y] = 1;
                stack.push_back(y);
              }
          }
          if (!anchored)
            for (i32 x : piece) gone[x] = 1;
        }
  }
  for (i32 i = 0; i < L.n; ++i) {
    if (gone[i]) continue;
    if (!full && d2(i) > W2) continue;
    CellIn c;
    c.p = L.p[i];
    c.mat = L.mat[i];
    c.eff = L.eff[i];
    c.anchored = L.anchored[i] || (!full && d2(i) > Win2);
    map_w[i] = static_cast<i32>(wc.size());
    wc.push_back(c);
    w2l.push_back(i);
  }
  LatticeOptions lo;
  lo.h = L.h;
  const Lattice Lw = build_lattice(wc, lo);
  out.window_cells = Lw.n;
  std::vector<f64> rw(6 * size_t(Lw.n), 0.0), dw(6 * size_t(Lw.n), 0.0);
  for (i32 k = 0; k < Lw.n; ++k)
    if (!Lw.anchored[k])
      for (int q = 0; q < 6; ++q) rw[6 * size_t(k) + q] = r0[6 * size_t(w2l[k]) + q];
  // Load from detached pieces disappears too: their bonds' baseline forces are released.
  if (out.detached > 0) {
    std::vector<f64> rg(6 * size_t(L.n));
    released_bond_residual(L, gone, S.u0.data(), rg.data());
    for (i32 k = 0; k < Lw.n; ++k)
      if (!Lw.anchored[k])
        for (int q = 0; q < 6; ++q) rw[6 * size_t(k) + q] = rg[6 * size_t(w2l[k]) + q];
  }
  Multigrid mg;
  mg.build(Lw, a.mg);
  const PcgStats st = pcg_solve(mg, Lw.n, rw.data(), dw.data(), 1e-6, a.maxit, false);
  out.iters = st.iters;
  // Utilization of surviving bonds within the evaluation radius: before vs after.
  std::vector<f64> u1 = S.u0;
  for (i32 k = 0; k < Lw.n; ++k)
    if (!Lw.anchored[k])
      for (int q = 0; q < 6; ++q) u1[6 * size_t(w2l[k]) + q] += dw[6 * size_t(k) + q];
  for (int ax = 0; ax < 3; ++ax)
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[ax][i];
      if (j < 0 || gone[i] || gone[j]) continue;
      if (map_w[i] < 0 || map_w[j] < 0) continue;
      if (d2(i) > eval2 || d2(j) > eval2) continue;
      const f64 ub = S.util[ax][i];
      const f64 ua = bond_utilization(bond_force(L, ax, i, u1.data()), L.bond(ax, i).cap);
      out.util_before = std::max(out.util_before, ub);
      out.util_after = std::max(out.util_after, ua);
      out.dutil = std::max(out.dutil, ua - ub);
      out.crossed1 += (ub <= 1.0 && ua > 1.0);
      out.crossed5 += (ub <= 5.0 && ua > 5.0);
    }
  out.seconds = now_s() - t0;
  return out;
}

bool parse(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (k == "--wad") a->wad = val();
    else if (k == "--map") a->map = val();
    else if (k == "--json") a->json = val();
    else if (k == "--mode") a->mode = std::strcmp(val(), "air") == 0 ? doom::VoidMode::Air : doom::VoidMode::Rock;
    else if (k == "--upv") a->upv = std::atoi(val());
    else if (k == "--shell") a->shell = std::atoi(val());
    else if (k == "--events") a->events = std::atoi(val());
    else if (k == "--seed") a->seed = std::strtoull(val(), nullptr, 10);
    else if (k == "--max-solve") a->max_solve = std::atoll(val());
    else if (k == "--bullet-r") a->bullet_r = std::atof(val());
    else if (k == "--rocket-r") a->rocket_r = std::atof(val());
    else if (k == "--window") a->window = std::atof(val());
    else if (k == "--validate") a->validate = std::atoi(val());
    else if (k == "--cheb") a->mg.cheb_degree = std::atoi(val());
    else if (k == "--cheb-fine") a->mg.cheb_degree_fine = std::atoi(val());
    else if (k == "--cheb-ratio") a->mg.cheb_ratio = std::atof(val());
    else if (k == "--gamma") a->mg.cycle_gamma = std::atoi(val());
    else if (k == "--strength") a->mg.strength = std::atof(val());
    else if (k == "--coarse-scale") a->mg.coarse_scale = std::atof(val());
    else if (k == "--coarse-max") a->mg.coarse_max = std::atoi(val());
    else if (k == "--threads") a->threads = std::atoi(val());
    else if (k == "--only-largest") a->only_largest = true;
    else if (k == "--maxit") a->maxit = std::atoi(val());
    else if (k == "--rtol") a->rtol = std::atof(val());
    else {
      std::fprintf(stderr, "unknown arg %s\n", k.c_str());
      return false;
    }
  }
  return !a->wad.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse(argc, argv, &args)) {
    std::fprintf(stderr, "usage: svx_content_study --wad FILE [--map NAME] [--mode rock|air] [--events N]\n");
    return 2;
  }
  if (args.threads > 0) set_num_threads(args.threads);
  doom::Wad wad;
  std::string err;
  if (!wad.load(args.wad, &err)) {
    std::fprintf(stderr, "wad: %s\n", err.c_str());
    return 1;
  }
  if (args.map.empty()) {
    for (const auto& n : wad.map_names()) std::printf("%s\n", n.c_str());
    return 0;
  }
  doom::Map map;
  if (!wad.read_map(args.map, &map, &err)) {
    std::fprintf(stderr, "map: %s\n", err.c_str());
    return 1;
  }
  doom::VoxelizeOptions vo;
  vo.units_per_voxel = args.upv;
  vo.shell_voxels = args.shell;
  vo.void_mode = args.mode;
  ColumnGrid grid;
  doom::VoxelizeStats vs;
  f64 t0 = now_s();
  if (!doom::voxelize(map, vo, &grid, &vs, &err)) {
    std::fprintf(stderr, "voxelize: %s\n", err.c_str());
    return 1;
  }
  const f64 t_vox = now_s() - t0;
  const f64 h = 0.125 * args.upv / 4.0;
  t0 = now_s();
  const RunComponents rc = run_components(grid);
  const f64 t_cc = now_s() - t0;

  // ---- component statistics ---------------------------------------------------------
  std::vector<f64> sizes;
  i64 anchored_comps = 0, floating_comps = 0, floating_vox = 0, biggest = 0;
  for (const auto& c : rc.comps) {
    sizes.push_back(static_cast<f64>(c.voxels));
    biggest = std::max(biggest, c.voxels);
    if (c.anchor_bonds > 0)
      ++anchored_comps;
    else {
      ++floating_comps;
      floating_vox += c.voxels;
    }
  }
  const char* mode_name = args.mode == doom::VoidMode::Rock ? "rock" : "air";
  std::printf("map %s [%s]: %zu sectors, grid %dx%dx%d, %lld structural / %lld anchored voxels (%.2fs voxelize)\n",
              map.name.c_str(), mode_name, map.sectors.size(), vs.nx, vs.ny, vs.nz, (long long)vs.structural_voxels,
              (long long)vs.anchored_voxels, t_vox);
  std::printf("  columns: %lld sector, %lld shell, %lld rock; diagonal fixes %d\n", (long long)vs.sector_columns,
              (long long)vs.shell_columns, (long long)vs.rock_columns, vs.diagonal_fixes);
  std::printf("  components: %zu (%lld anchored, %lld floating with %lld voxels) in %.2fs; size p50=%.0f p90=%.0f p99=%.0f max=%lld\n",
              rc.comps.size(), (long long)anchored_comps, (long long)floating_comps, (long long)floating_vox, t_cc,
              percentile(sizes, 0.5), percentile(sizes, 0.9), percentile(sizes, 0.99), (long long)biggest);

  // ---- statics per component --------------------------------------------------------
  std::vector<i32> order(rc.comps.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<i32>(i);
  std::sort(order.begin(), order.end(), [&](i32 a, i32 b) { return rc.comps[a].voxels > rc.comps[b].voxels; });
  LatticeOptions lo;
  lo.h = h;
  std::map<i32, std::unique_ptr<Solved>> solved_map;
  std::vector<f64> all_util, all_dist;
  i64 solved = 0, solved_vox = 0, skipped = 0, skipped_vox = 0, unconverged = 0, over1 = 0, over5 = 0, nbonds = 0;
  f64 t_solve = 0.0, worst_util = 0.0, worst_disp = 0.0;
  int max_iters = 0;
  for (i32 ci : order) {
    const auto& c = rc.comps[ci];
    if (c.anchor_bonds == 0) continue;
    if (args.only_largest && solved > 0) break;
    if (c.voxels > args.max_solve) {
      ++skipped;
      skipped_vox += c.voxels;
      continue;
    }
    std::vector<CellIn> cells = extract_component_cells(grid, rc, ci);
    auto S = solve_component(build_lattice(cells, lo), args);
    if (!S->converged) ++unconverged;
    ++solved;
    solved_vox += c.voxels;
    t_solve += S->seconds;
    max_iters = std::max(max_iters, S->iters);
    worst_util = std::max(worst_util, S->max_util);
    worst_disp = std::max(worst_disp, S->max_disp);
    for (int ax = 0; ax < 3; ++ax)
      for (i32 i = 0; i < S->L.n; ++i)
        if (S->L.nbr[ax][i] >= 0) {
          const f64 u = S->util[ax][i];
          all_util.push_back(u);
          over1 += u > 1.0;
          over5 += u > 5.0;
          ++nbonds;
        }
    for (i32 i = 0; i < S->L.n; ++i)
      if (!S->L.anchored[i] && S->anchor_dist[i] >= 0) all_dist.push_back(S->anchor_dist[i]);
    if (solved == 1) {
      std::printf("  largest component: %lld voxels, %d PCG iters (res %.1e), %.2fs, max util %.3f, levels",
                  (long long)c.voxels, S->iters, S->rel_res, S->seconds, S->max_util);
      for (i32 s : S->levels) std::printf(" %d", s);
      std::printf("\n");
    }
    solved_map[ci] = std::move(S);
  }
  const f64 u50 = percentile(all_util, 0.5), u99 = percentile(all_util, 0.99), u999 = percentile(all_util, 0.999);
  std::printf("  statics: %lld components solved (%lld voxels) in %.1fs, max PCG iters %d, unconverged %lld; skipped %lld (%lld voxels)\n",
              (long long)solved, (long long)solved_vox, t_solve, max_iters, (long long)unconverged, (long long)skipped,
              (long long)skipped_vox);
  std::printf("  self-weight bond utilization (nominal capacity): p50=%.4f p99=%.4f p99.9=%.4f max=%.3f; bonds >1: %lld, >5 (SOLID onset): %lld of %lld; max disp %.3g m\n",
              u50, u99, u999, worst_util, (long long)over1, (long long)over5, (long long)nbonds, worst_disp);
  std::printf("  anchor distance (bonds): p50=%.0f p90=%.0f p99=%.0f max=%.0f\n", percentile(all_dist, 0.5),
              percentile(all_dist, 0.9), percentile(all_dist, 0.99), percentile(all_dist, 1.0));

  // ---- events -----------------------------------------------------------------------
  std::vector<i32> ev_comp;
  std::vector<f64> ev_w;
  f64 wsum = 0.0;
  for (const auto& [ci, S] : solved_map) {
    ev_comp.push_back(ci);
    wsum += static_cast<f64>(rc.comps[ci].voxels);
    ev_w.push_back(wsum);
  }
  Rng rng(args.seed);
  struct EvOut {
    bool rocket;
    f64 radius;
    EventResult r;
    i64 comp_voxels;
    f64 anchor_dist;
  };
  std::vector<EvOut> evs;
  std::vector<f64> val_err;
  const int n_rocket = args.events / 5;
  for (int e = 0; e < args.events + n_rocket && !ev_comp.empty(); ++e) {
    const bool rocket = e >= args.events;
    const f64 R = rocket ? args.rocket_r : args.bullet_r;
    const f64 pick = rng.uni() * wsum;
    const size_t k = std::lower_bound(ev_w.begin(), ev_w.end(), pick) - ev_w.begin();
    const i32 ci = ev_comp[std::min(k, ev_comp.size() - 1)];
    const Solved& S = *solved_map[ci];
    // surface voxel: structural cell with an air face (fewer than 6 bonds)
    i32 center = -1;
    for (int tries = 0; tries < 8192 && center < 0; ++tries) {
      const i32 c = static_cast<i32>(rng.below(S.L.n));
      if (S.L.anchored[c]) continue;
      int nb = 0;
      for (int ax = 0; ax < 3; ++ax) nb += (S.L.nbr[ax][c] >= 0) + (S.L.nbrm[ax][c] >= 0);
      if (nb < 6) center = c;
    }
    if (center < 0) continue;
    const f64 W = args.window > 0.0 ? args.window : std::max(24.0, 4.0 * R);
    const EventResult er = carve_event(S, center, R, W, W - 4.0, args);
    evs.push_back({rocket, R, er, rc.comps[ci].voxels, static_cast<f64>(S.anchor_dist[center])});
    if (static_cast<int>(val_err.size()) < args.validate) {
      const EventResult ref = carve_event(S, center, R, 0.0, W - 4.0, args);
      val_err.push_back(std::abs(er.util_after - ref.util_after) / std::max(1e-9, ref.util_after));
      std::printf("    validate %s r=%.1f: window util_after=%.4f full=%.4f (window %lld cells %.2fs, full %.2fs)\n",
                  rocket ? "rocket" : "bullet", R, er.util_after, ref.util_after, (long long)er.window_cells, er.seconds,
                  ref.seconds);
    }
  }
  if (!val_err.empty())
    std::printf("  window-vs-full validation: %zu events, rel err of max util_after p50=%.2e max=%.2e\n", val_err.size(),
                percentile(val_err, 0.5), percentile(val_err, 1.0));
  auto summarize = [&](bool rocket) {
    std::vector<f64> du, secs, wcells, pieces;
    i64 n = 0, det_any = 0, det8 = 0, cross1 = 0, cross5 = 0, quiet1 = 0, quiet5 = 0, refined = 0;
    for (const EvOut& e : evs) {
      if (e.rocket != rocket) continue;
      ++n;
      det_any += e.r.detached > 0;
      det8 += e.r.largest_piece >= 8;
      if (e.r.largest_piece > 0) pieces.push_back(static_cast<f64>(e.r.largest_piece));
      du.push_back(e.r.dutil);
      secs.push_back(e.r.seconds);
      wcells.push_back(static_cast<f64>(e.r.window_cells));
      cross1 += e.r.crossed1 > 0;
      cross5 += e.r.crossed5 > 0;
      quiet1 += (e.r.largest_piece < 8 && e.r.crossed1 == 0);
      quiet5 += (e.r.largest_piece < 8 && e.r.crossed5 == 0);
      // v1 semantics: detached pieces vanish; a dynamic bubble is needed only if the event
      // newly overloads bonds or drops a large piece (>= 1000 voxels ~ 2 m^3).
      refined += (e.r.crossed1 == 0 && e.r.largest_piece < 1000);
    }
    if (!n) return;
    std::printf("  %s events: %lld | detach any %lld, piece>=8 vox %lld (largest piece p50=%.0f max=%.0f) | newly >1: %lld, newly >5: %lld | NO-BUBBLE: %.1f%% (onset 1x nominal), %.1f%% (onset 5x = prototype SOLID)\n",
                rocket ? "rocket" : "bullet", (long long)n, (long long)det_any, (long long)det8, percentile(pieces, 0.5),
                percentile(pieces, 1.0), (long long)cross1, (long long)cross5, 100.0 * quiet1 / n, 100.0 * quiet5 / n);
    std::printf("      NO-BUBBLE (v1 refined: no newly-overloaded bond at onset 1x, no piece >= 1000 vox): %.1f%%\n", 100.0 * refined / n);
    std::printf("      dUtil p50=%.4f p90=%.4f p99=%.4f; window cells p50=%.0f; event eval p50=%.3fs\n",
                percentile(du, 0.5), percentile(du, 0.9), percentile(du, 0.99), percentile(wcells, 0.5),
                percentile(secs, 0.5));
  };
  summarize(false);
  summarize(true);

  if (!args.json.empty()) {
    FILE* jf = std::fopen(args.json.c_str(), "w");
    if (jf) {
      std::fprintf(jf, "{\n  \"map\": \"%s\", \"mode\": \"%s\", \"units_per_voxel\": %d, \"h\": %.6f,\n", map.name.c_str(),
                   mode_name, args.upv, h);
      std::fprintf(jf, "  \"grid\": [%d, %d, %d], \"structural_voxels\": %lld, \"anchored_voxels\": %lld,\n", vs.nx, vs.ny,
                   vs.nz, (long long)vs.structural_voxels, (long long)vs.anchored_voxels);
      std::fprintf(jf, "  \"components\": {\"count\": %zu, \"anchored\": %lld, \"floating\": %lld, \"floating_voxels\": %lld, \"p50\": %.1f, \"p90\": %.1f, \"p99\": %.1f, \"max\": %lld},\n",
                   rc.comps.size(), (long long)anchored_comps, (long long)floating_comps, (long long)floating_vox,
                   percentile(sizes, 0.5), percentile(sizes, 0.9), percentile(sizes, 0.99), (long long)biggest);
      std::fprintf(jf, "  \"anchor_distance\": {\"p50\": %.1f, \"p90\": %.1f, \"p99\": %.1f, \"max\": %.1f},\n",
                   percentile(all_dist, 0.5), percentile(all_dist, 0.9), percentile(all_dist, 0.99),
                   percentile(all_dist, 1.0));
      std::fprintf(jf, "  \"statics\": {\"solved\": %lld, \"solved_voxels\": %lld, \"skipped\": %lld, \"skipped_voxels\": %lld, \"seconds\": %.3f, \"max_pcg_iters\": %d, \"unconverged\": %lld, \"util_p50\": %.6f, \"util_p99\": %.6f, \"util_p999\": %.6f, \"util_max\": %.6f, \"bonds_over_1\": %lld, \"bonds_over_5\": %lld, \"bonds\": %lld, \"max_disp_m\": %.6g},\n",
                   (long long)solved, (long long)solved_vox, (long long)skipped, (long long)skipped_vox, t_solve, max_iters,
                   (long long)unconverged, u50, u99, u999, worst_util, (long long)over1, (long long)over5, (long long)nbonds,
                   worst_disp);
      std::fprintf(jf, "  \"events\": [\n");
      for (size_t i = 0; i < evs.size(); ++i) {
        const EvOut& e = evs[i];
        std::fprintf(jf, "    {\"kind\": \"%s\", \"r\": %.2f, \"removed\": %lld, \"detached\": %lld, \"largest_piece\": %lld, \"crossed1\": %lld, \"crossed5\": %lld, \"util_before\": %.6f, \"util_after\": %.6f, \"dutil\": %.6f, \"window_cells\": %lld, \"iters\": %d, \"seconds\": %.4f, \"comp_voxels\": %lld, \"anchor_dist\": %.0f}%s\n",
                     e.rocket ? "rocket" : "bullet", e.radius, (long long)e.r.removed, (long long)e.r.detached,
                     (long long)e.r.largest_piece, (long long)e.r.crossed1, (long long)e.r.crossed5, e.r.util_before, e.r.util_after, e.r.dutil, (long long)e.r.window_cells, e.r.iters, e.r.seconds,
                     (long long)e.comp_voxels, e.anchor_dist, i + 1 < evs.size() ? "," : "");
      }
      std::fprintf(jf, "  ]\n}\n");
      std::fclose(jf);
    }
  }
  return 0;
}
