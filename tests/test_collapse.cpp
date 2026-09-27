// Global collapse (plan §B6 "the fine level decides", §B7): a structure that can no longer
// stand must come down, however far its failure is from the event that caused it.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/engine/engine.hpp"
#include "svx/sim/statics.hpp"
#include "svx/world/procgen.hpp"
#include "svx/world/region.hpp"

using namespace svx;

namespace {

struct TowerRun {
  EngineStats st;
  i64 voxels_before = 0, voxels_after = 0;
  bool top_stands = true;
};

// The procedural tower (10 storeys on a 4 x 4 grid of ground columns, ~305k voxels): rockets
// take out the ground columns of the south and west rows and of the two middle rows, 0.3 s
// apart, leaving the north row only - the tower's centre of mass is ~5 m off its supports.
TowerRun tower_on_one_row(const EngineConfig* cfg_in, int seconds = 60) {
  ProcWorld w = make_procedural("tower", 1);
  Engine eng;
  if (cfg_in) eng.configure(*cfg_in);
  if (const char* e = std::getenv("SVX_T_DAMPING")) {  // (experiments)
    EngineParams p = eng.params();
    p.damping = std::atof(e);
    eng.set_params(p);
  }
  eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  eng.bake();
  TowerRun r;
  r.voxels_before = eng.stats().voxels;
  const std::array<f64, 3> blasts[] = {
      {5.25, 4.8, 1.5},  {8.25, 4.8, 1.5},   {11.75, 4.8, 1.5},  {14.75, 4.8, 1.5},  // south row
      {4.8, 8.5, 1.5},   {4.8, 11.5, 1.5},   {4.8, 15.0, 1.5},                       // west row
      {8.5, 8.8, 1.5},   {11.75, 8.8, 1.5},  {15.0, 8.8, 1.5},                       // middle rows
      {8.5, 12.05, 1.5}, {11.75, 12.05, 1.5}, {15.0, 12.05, 1.5},
  };
  // (SVX_T_CORNER=1: every ground column but the north-east one, 0.3 s apart)
  std::vector<std::array<f64, 3>> order(std::begin(blasts), std::end(blasts));
  if (std::getenv("SVX_T_CORNER")) {
    order.clear();
    const f64 at[4] = {5.19, 8.44, 11.69, 14.94};
    for (int iy = 0; iy < 4; ++iy)
      for (int ix = 0; ix < 4; ++ix)
        if (ix != 3 || iy != 3) order.push_back({at[ix], iy == 0 ? 4.8 : at[iy], 1.5});
  }
  size_t k = 0;
  const bool progress = std::getenv("SVX_T_PROGRESS") != nullptr;  // (experiments)
  // cracks by storey (z) and by side (y: south, middle, north third of the tower)
  std::array<std::array<int, 3>, 11> cracks{};
  for (int t = 0; t < 60 * seconds; ++t) {
    if (t % 18 == 0 && k < order.size()) eng.blast(order[k++], 1.0, 1.0e6);
    eng.tick();
    for (const EngineEvent& ev : eng.take_events()) {
      if (!progress || ev.kind != EngineEvent::Kind::Crack) continue;
      const int st = std::clamp(static_cast<int>(ev.pos[2] / 3.0), 0, 10);
      const int side = std::clamp(static_cast<int>((ev.pos[1] - 5.0) / (10.1 / 3.0)), 0, 2);
      ++cracks[size_t(st)][size_t(side)];
    }
    if (progress && t % 60 == 59) {
      const EngineStats& s = eng.stats();
      std::printf("  [t %2d s] voxels %lld, ruptures %lld, detached %lld, bubbles %d (%d nodes), merged %lld, steps %lld "
                  "(%lld long); cracks by storey (S/M/N):",
                  (t + 1) / 60, static_cast<long long>(s.voxels), static_cast<long long>(s.ruptures),
                  static_cast<long long>(s.detached_voxels), s.active_bubbles, s.active_nodes,
                  static_cast<long long>(s.merged_events), static_cast<long long>(s.bubble_steps),
                  static_cast<long long>(s.long_steps));
      for (int z = 0; z < 11; ++z)
        if (cracks[size_t(z)][0] + cracks[size_t(z)][1] + cracks[size_t(z)][2] > 0)
          std::printf(" %d:%d/%d/%d", z, cracks[size_t(z)][0], cracks[size_t(z)][1], cracks[size_t(z)][2]);
      std::printf("\n");
      std::fflush(stdout);
    }
  }
  r.st = eng.stats();
  r.voxels_after = r.st.voxels;
  // the roof slab's centre (the tower's top storey)
  const f64 h = eng.grid().h;
  bool any = false;
  for (i32 z = static_cast<i32>(28.0 / h); z < static_cast<i32>(40.0 / h) && !any; ++z)
    any = vox_solid(eng.grid().get({static_cast<i32>(10.0 / h), static_cast<i32>(10.0 / h), z}));
  r.top_stands = any;
  return r;
}

// A small tower: 5 storeys on a 3 x 3 grid of ground columns (2 x 2 bays of 3.25 m; columns
// 3 x 3 voxels, storeys 2.75 m, slabs 2 voxels) with perimeter walls (2 voxels, every window
// open) on its south and north faces, on bedrock - the procedural tower's frame, smaller.
VoxelGrid small_tower() {
  VoxelGrid g;
  g.h = 0.125;
  const Vox rock = make_vox(MaterialId::Rock, true), rc = make_vox(MaterialId::Rc, false);
  for (i32 x = 0; x < 104; ++x)
    for (i32 y = 0; y < 104; ++y) g.fill_column(x, y, -4, 0, rock);
  const i32 ox = 24, oy = 24, bays = 2, bay = 26, col = 3, slab = 2, storey_h = 22, storeys = 5, wall = 2;
  const i32 X = bays * bay + col, Y = bays * bay + col;
  for (i32 s = 0; s < storeys; ++s) {
    const i32 z0 = s * (storey_h + slab);
    for (i32 ix = 0; ix <= bays; ++ix)
      for (i32 iy = 0; iy <= bays; ++iy)
        for (i32 x = ox + ix * bay; x < ox + ix * bay + col; ++x)
          for (i32 y = oy + iy * bay; y < oy + iy * bay + col; ++y) g.fill_column(x, y, z0, z0 + storey_h, rc);
    for (i32 x = ox; x < ox + X; ++x)
      for (i32 y = oy; y < oy + Y; ++y) g.fill_column(x, y, z0 + storey_h, z0 + storey_h + slab, rc);
    for (int side = 0; side < 2; ++side) {
      const i32 yw = side == 0 ? oy : oy + Y - wall;
      for (i32 x = ox; x < ox + X; ++x) {
        const i32 u = (x - ox) % bay;
        const bool bay_x = u > col + 2 && u < bay - 3;
        for (i32 y = yw; y < yw + wall; ++y)
          for (i32 z = z0; z < z0 + storey_h; ++z) {
            const bool window = bay_x && z > z0 + storey_h / 3 && z < z0 + storey_h - 3;
            if (!window) g.fill_column(x, y, z, z + 1, rc);
          }
      }
    }
  }
  g.lo = {0, 0, -4};
  g.hi = {104, 104, storeys * (storey_h + slab) + 8};
  g.compact();
  return g;
}

// The small tower loses all its ground columns but the north-east one, with the north wall
// around the two it loses there, 0.3 s apart: a corner column and a stub of wall remain, 4.6 m
// from its centre of mass.
TowerRun small_tower_on_one_row(const EngineConfig& cfg, int seconds) {
  Engine eng;
  eng.configure(cfg);
  eng.load(small_tower(), {0.5, 0.5, 0.02}, {1, 1, 0});
  eng.bake();
  TowerRun r;
  r.voxels_before = eng.stats().voxels;
  const std::array<f64, 3> blasts[] = {
      {3.19, 2.8, 1.5}, {6.44, 2.8, 1.5}, {9.69, 2.8, 1.5},  // south row (from outside)
      {3.19, 6.44, 1.5}, {6.44, 6.44, 1.5}, {9.69, 6.44, 1.5},  // middle row
      {3.19, 9.69, 1.5}, {6.44, 9.69, 1.5},                    // north-west and north-middle
  };
  size_t k = 0;
  const bool progress = std::getenv("SVX_T_PROGRESS") != nullptr;
  for (int t = 0; t < 60 * seconds; ++t) {
    if (t % 18 == 0 && k < std::size(blasts)) eng.blast(blasts[k++], 1.0, 1.0e6);
    eng.tick();
    (void)eng.take_events();
    if (progress && t % 60 == 59) {
      const EngineStats& s = eng.stats();
      std::printf("  [t %2d s] voxels %lld, ruptures %lld, detached %lld, bubbles %d (%d nodes), merged %lld, steps %lld\n",
                  (t + 1) / 60, static_cast<long long>(s.voxels), static_cast<long long>(s.ruptures),
                  static_cast<long long>(s.detached_voxels), s.active_bubbles, s.active_nodes,
                  static_cast<long long>(s.merged_events), static_cast<long long>(s.bubble_steps));
      std::fflush(stdout);
    }
  }
  r.st = eng.stats();
  r.voxels_after = r.st.voxels;
  bool any = false;  // the roof slab over the middle bay
  for (i32 z = 100; z < 125 && !any; ++z) any = vox_solid(eng.grid().get({51, 51, z}));
  r.top_stands = any;
  return r;
}

}  // namespace

TEST_CASE("collapse: a small tower left on a corner column comes down") {
  // Its bubble spans the tower (no pinned window rim), the later rockets merge into it, and it
  // keeps pace with the world by longer steps: it comes down within 15 s (measured on the
  // default budget: ~7 s after the last rocket; with one-tick steps 20 s later, unbudgeted 2.6 s).
  EngineConfig cfg;
  if (const char* e = std::getenv("SVX_T_TICK_WORK")) cfg.tick_work = std::atoll(e);  // (experiments)
  if (const char* e = std::getenv("SVX_T_DT_MULT")) cfg.max_dt_multiplier = std::atoi(e);
  const int secs = std::getenv("SVX_T_SECONDS") ? std::atoi(std::getenv("SVX_T_SECONDS")) : 15;
  const TowerRun r = small_tower_on_one_row(cfg, secs);
  MESSAGE("voxels " << r.voxels_before << " -> " << r.voxels_after << ", ruptures " << r.st.ruptures << ", detached "
                    << r.st.detached_voxels << ", bubbles " << r.st.bubbles_spawned << " (structure " << r.st.structure_bubbles
                    << "), merged " << r.st.merged_events << ", top " << std::string(r.top_stands ? "stands" : "gone"));
  CHECK(r.st.merged_events >= 1);
  CHECK_FALSE(r.top_stands);
}

TEST_CASE("collapse: a second rocket on a structure merges into its running bubble (plan B5)") {
  // Two rockets into two walls of the rooms (one structure), ten ticks apart: the first one's
  // bubble spans the structure; the second one's crater shows at once and the running bubble
  // takes it (no second bubble, no wait for the first one's settle). Deterministic across
  // thread counts.
  auto run = [](int threads, EngineStats* st, bool* crater_at_once) {
    set_num_threads(threads);
    ProcWorld w = make_procedural("rooms", 7);
    Engine eng;
    EngineParams p;
    p.fragility = 0.25;
    eng.set_params(p);
    eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
    eng.bake();
    const f64 h = eng.grid().h;
    const std::array<f64, 3> a{52.5 * h, 20.0 * h, 12.0 * h}, b{103.5 * h, 60.0 * h, 12.0 * h};
    *crater_at_once = false;
    for (int t = 0; t < 180; ++t) {
      if (t == 0) eng.blast(a, 0.6, 1e6);
      if (t == 10) eng.blast(b, 0.6, 1e6);
      eng.tick();
      (void)eng.take_events();
      if (t == 10)
        *crater_at_once = !vox_solid(eng.grid().get({static_cast<i32>(103.5), static_cast<i32>(60.0), static_cast<i32>(12.0)}));
      if (t == 10) {
        const EngineStats& s = eng.stats();
        CHECK(s.bubbles_spawned == 1);
        CHECK(s.merged_events == 1);
      }
    }
    *st = eng.stats();
    return eng.session_hash();
  };
  const int nt = num_threads();
  EngineStats s1, s4;
  bool c1 = false, c4 = false;
  const u64 h1 = run(1, &s1, &c1), h4 = run(4, &s4, &c4);
  set_num_threads(nt);
  MESSAGE("bubbles " << s1.bubbles_spawned << " (structure " << s1.structure_bubbles << "), merged " << s1.merged_events
                     << ", ruptures " << s1.ruptures << ", steps " << s1.bubble_steps << " (" << s1.long_steps << " long)");
  CHECK(c1);
  CHECK(c4);
  CHECK(s1.structure_bubbles >= 1);
  CHECK(s1.merged_events == 1);
  CHECK(s1.deferred_events == 0);
  CHECK(h1 == h4);
}

TEST_CASE("collapse: a tower left on one row of ground columns comes down (SVX_T_BIG=1)" *
          doctest::skip(std::getenv("SVX_T_BIG") == nullptr)) {
  // The procedural tower's frame, 10 storeys, left on its north wall and three columns: in this
  // model a marginal case - its bake-strengthened base gives way only by progressive dynamic
  // failure. Unbudgeted (each step commits the next tick) it comes down ~8 s after the last
  // rocket; on the default budget the rockets merge within the bubble's first half second and
  // it stood for 90 s (docs/STATUS.md). A physics check, ~5 min on this machine.
  EngineConfig cfg;
  cfg.tick_work = 0;
  if (const char* e = std::getenv("SVX_T_VERIFY_CELLS")) cfg.verify_max_cells = std::atoll(e);  // (experiments)
  if (const char* e = std::getenv("SVX_T_VERIFY_CHUNKS")) cfg.verify_max_chunks = std::atoi(e);
  if (const char* e = std::getenv("SVX_T_TICK_WORK")) cfg.tick_work = std::atoll(e);
  if (const char* e = std::getenv("SVX_T_DT_MULT")) cfg.max_dt_multiplier = std::atoi(e);
  if (const char* e = std::getenv("SVX_T_MAX_LEVEL")) cfg.max_level = std::atoi(e);
  if (const char* e = std::getenv("SVX_T_MAX_FINE")) cfg.structure_max_fine_cells = std::atoi(e);
  const int secs = std::getenv("SVX_T_SECONDS") ? std::atoi(std::getenv("SVX_T_SECONDS")) : 15;
  const TowerRun r = tower_on_one_row(&cfg, secs);
  MESSAGE("voxels " << r.voxels_before << " -> " << r.voxels_after << ", ruptures " << r.st.ruptures << ", detached "
                    << r.st.detached_voxels << ", bubbles " << r.st.bubbles_spawned << ", verifications "
                    << r.st.verifications << " (failures " << r.st.verify_failures << "), top "
                    << std::string(r.top_stands ? "stands" : "gone"));
  CHECK_FALSE(r.top_stands);
}

// Diagnostic (SVX_T_STATIC=1): the full-fine static truth of the tower after the 13 blasts'
// craters (removal only, no prefracture): the closure cascade (ruptures to a quiet pass or
// detachment) on the whole structure.
TEST_CASE("collapse: static truth of the tower on one row (diagnostic)" * doctest::skip(std::getenv("SVX_T_STATIC") == nullptr)) {
  ProcWorld w = make_procedural("tower", 1);
  Engine eng;
  eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  eng.bake();
  std::vector<u64> keys;
  for (const auto& kv : eng.grid().chunks()) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  VoxelGrid g = eng.grid().snapshot(keys);
  const f64 h = g.h;
  const std::array<f64, 3> blasts[] = {
      {5.25, 4.8, 1.5},  {8.25, 4.8, 1.5},   {11.75, 4.8, 1.5},  {14.75, 4.8, 1.5},
      {4.8, 8.5, 1.5},   {4.8, 11.5, 1.5},   {4.8, 15.0, 1.5},
      {8.5, 8.8, 1.5},   {11.75, 8.8, 1.5},  {15.0, 8.8, 1.5},
      {8.5, 12.05, 1.5}, {11.75, 12.05, 1.5}, {15.0, 12.05, 1.5},
  };
  const f64 r = std::getenv("SVX_T_RADIUS") ? std::atof(std::getenv("SVX_T_RADIUS")) : 1.0;
  i64 removed = 0;
  for (const auto& b : blasts) {
    const i32 cx = static_cast<i32>(b[0] / h), cy = static_cast<i32>(b[1] / h), cz = static_cast<i32>(b[2] / h);
    const i32 rr = static_cast<i32>(std::ceil(r / h)) + 1;
    for (i32 x = cx - rr; x <= cx + rr; ++x)
      for (i32 y = cy - rr; y <= cy + rr; ++y)
        for (i32 z = cz - rr; z <= cz + rr; ++z) {
          const f64 dx = h * x - b[0], dy = h * y - b[1], dz = h * z - b[2];
          if (dx * dx + dy * dy + dz * dz > r * r) continue;
          const Vox v = g.get(x, y, z);
          if (!vox_solid(v) || vox_anchored(v)) continue;
          g.set(x, y, z, kAir);
          ++removed;
        }
  }
  std::vector<IVec3> seeds;
  for (i32 x = 40; x < 125; ++x)
    for (i32 y = 40; y < 125; ++y)
      for (i32 z = 0; z < 8; ++z)
        if (vox_solid(g.get(x, y, z)) && !vox_anchored(g.get(x, y, z))) seeds.push_back({x, y, z});
  bool truncated = false;
  const std::vector<IVec3> set = structures_of(g, seeds, 2000000, &truncated);
  LatticeOptions lo;
  lo.h = h;
  lo.law.game = true;
  lo.law.fragility = eng.params().fragility;
  lo.contact = EngineConfig{}.contact;
  lo.plate = EngineConfig{}.plate;
  Region R = extract_set(g, set, lo);
  Lattice& L = R.L;
  L.kscale = 1.0 / eng.params().compliance;
  std::vector<f64> u(6 * size_t(L.n), 0.0);
  for (i32 i = 0; i < L.n; ++i) {  // (from the cached baseline where there is one)
    f32 b[6];
    if (!L.anchored[i] && g.baseline(R.vox[i], b))
      for (int q = 0; q < 6; ++q) u[6 * size_t(i) + q] = b[q];
  }
  StaticsOptions so;
  so.corot = true;
  so.damage = true;
  so.max_passes = std::getenv("SVX_T_PASSES") ? std::atoi(std::getenv("SVX_T_PASSES")) : 40;
  so.max_iters = 60;
  so.res_tol = 1e-7;
  so.lin_rtol = 1e-3;
  so.max_pcg_total = 3000;
  const auto t0 = std::chrono::steady_clock::now();
  const ClosureResult cr = solve_to_closure(L, u, gravity_vector(L, 9.81), so);
  const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  for (const PassLog& p : cr.log)
    MESSAGE("pass " << p.pass << ": " << (p.eq.converged ? "converged" : "unconverged") << " after " << p.eq.iters
                    << " its (pcg " << p.eq.pcg_iters << "), max damage " << p.eq.max_damage << ", phi " << p.eq.max_phi
                    << ", ruptured " << p.ruptured << ", deleted " << p.deleted << (p.quiet ? " (quiet)" : ""));
  MESSAGE("removed " << removed << " voxels; structure " << L.n << " cells" << (truncated ? " (truncated)" : "") << "; "
                     << cr.passes << " passes, " << cr.ruptured.size() << " ruptured, " << cr.deleted.size()
                     << " deleted; closure " << (cr.converged ? "quiet" : "not reached") << " in " << ms << " ms");
}
