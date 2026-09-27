#include "svx/engine/engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <optional>
#include <tuple>
#include <unordered_set>

#include "svx/base/parallel.hpp"
#include "svx/base/work.hpp"
#include "svx/engine/replay.hpp"
#include "svx/sim/blast.hpp"
#include "svx/mech/law.hpp"
#include "svx/sim/corot.hpp"
#include "svx/sim/statics.hpp"

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define SVX_NO_THREADS 1
#else
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace svx {

namespace {

using Clock = std::chrono::steady_clock;
inline f64 ms_since(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }

// thread CPU time (ms) for traces: wall clock is meaningless on a loaded machine
inline f64 cpu_now_ms() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return 1e3 * static_cast<f64>(ts.tv_sec) + 1e-6 * static_cast<f64>(ts.tv_nsec);
#else
  return std::chrono::duration<f64, std::milli>(Clock::now().time_since_epoch()).count();
#endif
}

inline IVec3 voxel_of(const std::array<f64, 3>& p, f64 h) {
  return {static_cast<i32>(std::floor(p[0] / h + 0.5)), static_cast<i32>(std::floor(p[1] / h + 0.5)),
          static_cast<i32>(std::floor(p[2] / h + 0.5))};
}

// The solid voxel a debris landing hits: at the mean contact point or up to two voxels further
// along the impulse on the world.
bool impact_voxel(const VoxelGrid& g, const std::array<f64, 3>& pos, const std::array<f64, 3>& J, IVec3& out) {
  const f64 n = std::sqrt(J[0] * J[0] + J[1] * J[1] + J[2] * J[2]);
  if (!(n > 0.0)) return false;
  for (int k = 0; k <= 2; ++k) {
    const IVec3 v = voxel_of({pos[0] + k * g.h * J[0] / n, pos[1] + k * g.h * J[1] / n, pos[2] + k * g.h * J[2] / n}, g.h);
    if (vox_solid(g.get(v))) {
      out = v;
      return true;
    }
  }
  return false;
}

// The cached baseline of a region (the whole-structure equilibrium events solve increments on).
// The rim is pinned at its cached displacement (zero increment), so totals, bond jumps and the
// law stay exact, and the window load is f_int(u0) itself. Where the cache is incomplete, the
// pieces connected to neither support nor rim are removed from the lattice (returned in `loose`:
// they cannot be at rest) and the rest is solved elastically. Reads only `g`.
// (resolve: a complete cache is checked too, and solved from where it is when it is no equilibrium
// - e.g. lazily baked tiles of a structure crossing tile borders, each solved with its rim held,
// meet with a jump; a verification must not read that as a failure)
// A cached baseline u0 in window R: events load a window with f_int(u0) (the increment
// formulation), so a cache that is no exact equilibrium moves nothing; it matters where the law
// reads a jump of the cache as strain - tiles of a progressive bake meeting with a kink, a stale
// cache. Such a cache (relative residual over kCacheResidual - above its f32 rounding - with a
// bond off the rim reading over kCacheDemand of onset from it) is to be solved. A linear bake's
// second-order corotational residual, or an earlier bubble's held rim, reads far below onset.
constexpr f64 kCacheResidual = 1e-3, kCacheDemand = 0.75;
bool cache_needs_solve(const Region& R, const Lattice& L, const std::vector<f64>& u0, bool corot, const char* what) {
  const std::vector<f64> g = gravity_vector(L, 9.81);
  std::vector<f64> fint(u0.size());
  internal_forces(L, u0.data(), corot, fint.data());
  f64 rn = 0.0, gn = 0.0;
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i] || L.dead[i] || R.rim[i]) continue;
    for (int q = 0; q < 6; ++q) {
      const f64 d = g[6 * size_t(i) + q] - fint[6 * size_t(i) + q];
      rn += d * d;
      gn += g[6 * size_t(i) + q] * g[6 * size_t(i) + q];
    }
  }
  f64 demand = 0.0;  // the law's largest reading of the cache (bonds off the rim)
  if (rn > kCacheResidual * kCacheResidual * gn) {
    add_work(i64(L.n) * kWorkLawSweep);
    for (int a = 0; a < 3; ++a)
      for (i32 i = 0; i < L.n; ++i) {
        const i32 j = L.nbr[a][i];
        if (j < 0 || L.dead[i] || L.dead[j] || R.rim[i] || R.rim[j]) continue;
        Vec6 jump = bond_jump(L, a, i, u0.data(), corot);
        for (auto& v : jump) v *= L.kscale;
        const f64 d = L.dmg[a].empty() ? 0.0 : f64(L.dmg[a][i]);
        demand = std::max(demand, evaluate_law(L.bond(a, i), jump, d, L.law.game).phi);
      }
  }
  const bool solve = demand > kCacheDemand;
  if (what && std::getenv("SVX_CONSISTENCY_TRACE"))
    std::printf("      [consistency] %s, %d cells: relative residual %.2e, demand %.2f%s\n", what, L.n,
                std::sqrt(rn / std::max(gn, 1e-300)), demand, solve ? " (solved)" : "");
  return solve;
}

std::vector<f64> baseline_from(const VoxelGrid& g, Region& R, bool corot, std::vector<std::vector<IVec3>>* loose,
                               int* iters, bool trace, bool resolve = false, int max_pcg = 0) {
  Lattice& L = R.L;
  std::vector<f64> u(6 * size_t(L.n), 0.0);
  L.u_fixed.assign(6 * size_t(L.n), 0.0);
  bool complete = true;
  for (i32 i = 0; i < L.n; ++i) {
    f32 b[6];
    const bool have = g.baseline(R.vox[i], b);
    if (R.rim[i]) {
      if (have)
        for (int q = 0; q < 6; ++q) L.u_fixed[6 * size_t(i) + q] = b[q];
      continue;
    }
    if (L.anchored[i]) continue;
    if (have) {
      for (int q = 0; q < 6; ++q) u[6 * size_t(i) + q] = b[q];
    } else {
      complete = false;
    }
  }
  if (iters) *iters = 0;
  if (complete && resolve) {  // a cache the law reads kinks from is solved (cache_needs_solve)
    if (!cache_needs_solve(R, L, u, corot, "structure")) return u;
    if (trace) std::printf("    [baseline] cached state reads kinks: solved\n");
  } else if (complete) {
    return u;
  }
  for (const auto& isl : unsupported_components(L)) {
    std::vector<IVec3> v;
    for (i32 c : isl) {
      v.push_back(R.vox[c]);
      L.remove_cell(c);
    }
    if (loose) loose->push_back(std::move(v));
  }
  if (trace) {
    int missing = 0;
    for (i32 i = 0; i < L.n; ++i) {
      f32 b[6];
      if (!R.rim[i] && !L.anchored[i] && !L.dead[i] && !g.baseline(R.vox[i], b)) ++missing;
    }
    std::printf("    [event] baseline cache incomplete: %d of %d cells to solve, %zu loose pieces dropped\n", missing, L.n,
                loose ? loose->size() : size_t(0));
  }
  StaticsOptions so;
  so.corot = corot;
  so.damage = false;
  so.res_tol = 1e-7;
  so.lin_rtol = 1e-3;
  so.max_pcg_total = max_pcg;  // (a window's re-solve is bounded: an event tick waits for it)
  const DamageField dc = L.dmg;
  const EquilibriumStats es = solve_equilibrium(L, u, gravity_vector(L, 9.81), dc, so, nullptr);
  if (iters) *iters = es.pcg_iters;
  return u;
}

// Live, simulated window cells around (Chebyshev distance 1) a hit voxel.
std::vector<i32> contact_cells(const Region& R, const IVec3& hit) {
  std::vector<i32> out;
  for (i32 dx = -1; dx <= 1; ++dx)
    for (i32 dy = -1; dy <= 1; ++dy)
      for (i32 dz = -1; dz <= 1; ++dz) {
        const i32 c = R.cell({hit[0] + dx, hit[1] + dy, hit[2] + dz});
        if (c >= 0 && !R.L.dead[c] && !R.L.anchored[c]) out.push_back(c);
      }
  return out;
}

// Design strength of hydrated voxels (a verification's regenerated chunks carry none; the lazy
// bake assigns it when a chunk loads): as Engine::design_pass, a bond over the design utilization
// at the baseline raises the class of its hydrated ends. Returns whether any class changed.
bool design_hydrated(VoxelGrid& g, const Region& R, const std::vector<f64>& u, bool corot, f64 design_utilization,
                     const std::function<bool(const IVec3&)>& hydrated) {
  const Lattice& L = R.L;
  std::vector<u8> need(size_t(L.n), 0);
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || L.dead[i] || L.dead[j]) continue;
      const bool hi = hydrated(R.vox[i]), hj = hydrated(R.vox[j]);
      if (!hi && !hj) continue;
      Vec6 jump = bond_jump(L, a, i, u.data(), corot);
      for (auto& v : jump) v *= L.kscale;
      const f64 util = evaluate_law(L.bond(a, i), jump, 0.0, L.law.game).phi;
      if (util <= design_utilization) continue;
      const u8 c = strength_class_for(strength_multiplier(std::min(L.strength[i], L.strength[j])) * util / design_utilization);
      if (hi) need[size_t(i)] = std::max(need[size_t(i)], c);
      if (hj) need[size_t(j)] = std::max(need[size_t(j)], c);
    }
  bool any = false;
  for (i32 i = 0; i < L.n; ++i)
    if (need[size_t(i)] > L.strength[i]) {
      g.set_strength(R.vox[i], need[size_t(i)]);
      any = true;
    }
  return any;
}

// A window's lattice before an event's mutation (what removing cells and breaking or damaging
// bonds changes), for a background job to swap in: the pre-event state of the window.
struct PreState {
  std::array<std::vector<i32>, 3> nbr, nbrm;
  std::vector<u8> dead, fixmask, anchored;
  std::vector<f64> mass;
  std::array<std::vector<f32>, 3> dmg;
  std::array<std::vector<u8>, 3> cracked;
  std::array<std::vector<Vec6>, 3> cscale;
  explicit PreState(const Lattice& L)
      : nbr(L.nbr), nbrm(L.nbrm), dead(L.dead), fixmask(L.fixmask), anchored(L.anchored), mass(L.mass), dmg(L.dmg),
        cracked(L.cracked), cscale(L.cscale) {}
  void swap(Lattice& L) {
    for (int a = 0; a < 3; ++a) {
      std::swap(nbr[a], L.nbr[a]);
      std::swap(nbrm[a], L.nbrm[a]);
      std::swap(dmg[a], L.dmg[a]);
      std::swap(cracked[a], L.cracked[a]);
      std::swap(cscale[a], L.cscale[a]);
    }
    std::swap(dead, L.dead);
    std::swap(fixmask, L.fixmask);
    std::swap(anchored, L.anchored);
    std::swap(mass, L.mass);
  }
};

// (background) The window's cached baseline u0, checked in its pre-event state
// (cache_needs_solve): a cache with kinks the law reads is solved (rim held, bounded) and fpre
// becomes its pre-event internal forces. Returns whether it solved.
bool consistent_baseline(Region& R, PreState& pre, std::vector<f64>& u0, std::vector<f64>& fpre, bool corot,
                         int max_pcg) {
  Lattice& L = R.L;
  pre.swap(L);  // the pre-event window
  const bool solve = cache_needs_solve(R, L, u0, corot, "window");
  if (solve) {
    const std::vector<f64> g = gravity_vector(L, 9.81);
    std::vector<f64> fint(u0.size());
    StaticsOptions so;
    so.corot = corot;
    so.damage = false;
    so.res_tol = 1e-7;
    so.lin_rtol = 1e-3;
    so.max_pcg_total = max_pcg;
    const DamageField dc = L.dmg;
    solve_equilibrium(L, u0, g, dc, so, nullptr);
    internal_forces(L, u0.data(), corot, fint.data());
    fpre = std::move(fint);
  }
  pre.swap(L);  // (back to the post-event window)
  return solve;
}

}  // namespace

// An event merged into a running bubble (merge_target), as the world took it: applied to the
// bubble's lattice by its next job (take_merges, Bubble::apply_event).
struct Engine::Merge {
  std::vector<IVec3> fine, removed, islands;
  std::vector<std::pair<IVec3, int>> fractured;
  std::vector<std::tuple<IVec3, int, f32>> damaged;
  std::vector<std::pair<IVec3, std::array<f64, 6>>> impulse;
};

struct Engine::Active {
  i64 id = 0;
  std::unique_ptr<Region> region;
  // the structure its setup may extract (released on the simulation thread once the setup has
  // committed), and whether the region became that whole structure (read after the setup). The
  // setup builds that region aside (next_region; the simulation thread reads `region`'s cell
  // index while jobs run) and make_ready puts it in place.
  std::shared_ptr<StructureJob> structure;
  std::unique_ptr<Region> next_region;
  bool spans_structure = false;
  bool structure_bubble = false;  // spans (or its setup will try to span) a whole structure: events
                                  // on that structure merge into it (simulation thread)
  std::vector<Merge> merges;      // merged since its last job
  i32 dt_mult = 1;                // its step in ticks (max_dt_multiplier)
  i64 field_snap = -1;            // the commit its cached displacement field shows
  DisplacementField field;
  i64 last_work = 0;              // the counted work of its last step
  Bubble bubble;
  std::vector<f64> u_start;
  std::vector<f64> load;    // f_int_pre(u0): the window's static load (increment formulation)
  int respawns = 0;
  IVec3 center{0, 0, 0};
  f64 radius = 0.0;
  std::vector<u64> chunks;  // chunks overlapped by the region
  std::vector<f64> u_now;   // total displacement cached for meshing
  bool half_rate = false;   // budget manager: stepped every other tick this tick
  bool takeover = false;    // an event waits for it: no more steps, finalized without continuation
  bool finalizing = false;  // its job is the settle projection (finalize at ready_tick)
  Bubble::SettleResult settled;  // (the finalize job's settle and state)
  std::vector<f64> settled_u;
  f64 settle_ms = 0.0;
  // the running job's deterministic work (base/work.hpp): it is committed max(min_ticks,
  // ceil(work / tick_work)) ticks after its launch (Engine::job_due)
  WorkCounter wc;
  std::atomic<bool> finished{false};
  i64 launched = 0, min_ticks = 1;
  char kind = 'u';  // u setup, s step, f finalize (diagnostics)
  // debris landings on it while a job runs: contact impulses added before its next step
  std::vector<std::pair<IVec3, std::array<f64, 3>>> landings;
  // The bubble's background job: its setup (event residual, composite, preconditioner, first
  // step) or its next step, committed at ready_tick (steps are pipelined one tick: launched after
  // this tick's commits, committed at the next). While a job runs the simulation thread touches
  // neither `region` nor `bubble`; meshing, fields and stats read the snapshot of the last commit.
  DamageField dmg0;         // the window's damage as committed to the grid (finalize writes changes)
  i64 ready_tick = -1;      // -1: idle
  i32 est_nodes = 0;        // node estimate before the first commit (budget; deterministic)
  std::function<void()> job;
  // A setup's stages (residual, init, preconditioner, first step). A background worker runs
  // them in one go; without threads they run one per tick (run_setup_stages), the rest at the
  // ready tick, so no tick carries the whole setup. Same results either way.
  std::vector<std::function<void()>> stages;
  size_t next_stage = 0;
  StepStats result;         // the job's step
#ifndef SVX_NO_THREADS
  std::thread worker;
#endif
  // snapshot of the last commit
  std::vector<u8> live_now;  // live, non-anchored cells
  std::vector<u8> level_now; // composite level + 1 per cell (debug view)
  i32 nodes_now = 0;
  bool shown = false;        // the snapshot exists
  i64 snap = 0, meshed_snap = -1;  // commits so far, and the one its chunks were last meshed at
  bool pending() const { return ready_tick >= 0; }
  i32 nodes() const { return shown ? nodes_now : est_nodes; }
  void finish_job() {
#ifndef SVX_NO_THREADS
    if (worker.joinable()) {
      worker.join();  // (the worker ran the job)
      job = nullptr;
      return;
    }
#endif
    if (job) {  // no thread: run it here
      SerialScope serial;
      job();
      job = nullptr;
    }
    run_stages(stages.size());
  }
  // runs up to n of the remaining setup stages (on the calling thread)
  void run_stages(size_t n) {
    for (; next_stage < stages.size() && n > 0; ++next_stage, --n) {
      SerialScope serial;
      WorkScope ws(&wc);
      stages[next_stage]();
    }
    if (next_stage >= stages.size() && !stages.empty()) {
      stages.clear();
      next_stage = 0;
      finished.store(true, std::memory_order_release);
    }
  }
  ~Active() {
#ifndef SVX_NO_THREADS
    if (worker.joinable()) worker.join();
#endif
  }
};

// The structure a rocket's bubble will span (EngineConfig::structure_max_cells): a snapshot of the
// chunks it may reach, taken before the event, and the event as applied to the world, to replay
// on the structure's lattice. Created and released on the simulation thread (the snapshot shares
// baseline bricks copy-on-write with the live grid); the setup reads it in the background.
struct Engine::StructureJob {
  VoxelGrid snap;
  std::vector<u64> chunks;             // the chunks holding structure (sorted keys)
  std::vector<IVec3> seeds;            // the window's cells: the structures it touched
  std::vector<IVec3> removed;          // voxels the event removed or detached
  std::vector<std::pair<IVec3, int>> fractured;  // bonds (lower voxel, axis) the blast broke
  std::vector<std::tuple<IVec3, int, f32>> damaged;  // bonds the blast damaged, with their damage
  std::vector<std::pair<IVec3, std::array<f64, 6>>> impulse;  // one-step blast forces per voxel
  std::vector<IVec3> fine;             // cells that start fine (the event's neighbours)
};

struct Engine::Spawn {
  std::unique_ptr<Region> win;
  std::shared_ptr<StructureJob> structure;  // (a rocket: the structure its bubble may span)
  IVec3 bound_c{0, 0, 0};                   // ... and that structure's bounding sphere (voxels)
  i32 bound_r = 0;
  IVec3 c{0, 0, 0};                  // event voxel
  std::array<f64, 3> pos{0, 0, 0};   // event point (m)
  f64 radius = 0.0;                  // event radius (m)
  std::vector<f64> u0, fpre, fimp;   // baseline, pre-event forces, one-step impulse forces
  std::vector<i32> pre_cells;        // local_pre: the cells fpre holds (the rest: post-event)
  bool local_pre = false;
  std::vector<i32> force_fine;
  std::shared_ptr<PreState> pre;  // the window before the event (consistent_baseline), if kept
};

// A small carve's static settle, solved on a background thread from its window and applied at
// ready_tick (or earlier, before anything that writes to cells of its window).
struct Engine::Settle {
  Spawn spawn;
  IVec3 center{0, 0, 0};
  i32 radius = 0;           // window radius (voxels)
  std::vector<u64> chunks;  // the window's chunks (kept resident while pending)
  std::vector<f64> u;
  std::vector<f64> load;    // an impact's extra static load (on top of the pre-event forces spawn.fpre)
  bool impact = false;      // a debris landing: a pass records damage only (no rebase)
  DamageField dc;
  StaticsOptions so;
  EquilibriumStats es;
  i64 ready_tick = 0;
  // counted work (base/work.hpp): applied max(1, ceil(work / tick_work)) ticks after the launch
  WorkCounter wc;
  std::atomic<bool> finished{false};
  i64 launched = 0;
  std::function<void()> job;
#ifndef SVX_NO_THREADS
  std::thread worker;
#endif
  void finish() {
#ifndef SVX_NO_THREADS
    if (worker.joinable()) {
      worker.join();
      job = nullptr;
      return;
    }
#endif
    if (job) {
      SerialScope serial;
      job();
      job = nullptr;
    }
  }
  ~Settle() {
#ifndef SVX_NO_THREADS
    if (worker.joinable()) worker.join();
#endif
  }
};

struct Engine::Verify {
  IVec3 center{0, 0, 0};
  i32 radius = 0;
  int respawns = 0;
  int requeues = 0;           // queued again after being set aside or going stale
  i64 quiet_since = 0;        // the last tick an event hit the window (or the job was queued)
  // a job queued again after going stale: its structure's chunks as last seen; it starts only
  // once none of them changed for the quiet spell (not again into the same busy structure)
  std::vector<std::pair<u64, u32>> watch;
  // started on the simulation thread once its window has been quiet (verify_quiet_ticks)
  i64 start = -1;             // tick of the snapshot (-1: queued)
  i64 due = -1;               // tick the result is applied at (fixed once the job's size is known)
  VoxelGrid snap;             // the job's world: a copy of the chunks its structures may reach
  std::vector<IVec3> seeds;   // structural voxels of the window
  LatticeOptions lo;
  f64 kscale = 1.0;
  bool corot = true;
  i64 max_cells = 0;
  i64 pcg_cells = 0;  // the linear-solve budget in cell-iterations (0: so.max_pcg_total as set)
  int min_pcg = 0;
  int threads = 1;
  bool trace = false;
  // streamed worlds: chunks the structures reach that were not resident at the snapshot are
  // hydrated - regenerated in the job's snapshot from the source and their archived edits, and
  // designed (never the live world) - so the check spans the streaming boundary (plan §B8)
  std::shared_ptr<const ChunkSource> source;
  std::unordered_map<u64, std::shared_ptr<const std::vector<u8>>> archive;
  std::vector<u64> resident;  // (sorted) chunks resident at the snapshot
  i64 max_hydrate = 0;
  std::vector<u64> hydrated;  // (sorted once extracted)
  f64 design_utilization = 0.5;
  bool design_corot = false;
  // extraction (background; the simulation thread reads it once `extracted`)
  std::unique_ptr<Region> region;  // the structures the window touched
  std::vector<f64> u0, u, g;
  DamageField dc, trial;
  std::vector<std::pair<u64, u32>> versions;  // chunk versions at the snapshot (staleness)
  bool truncated = false;
  size_t loose = 0;
  f64 ms_extract = 0.0;
  StaticsOptions so;
  EquilibriumStats es;
  bool solved = false;
  std::atomic<bool> extracted{false};
  std::atomic<bool> finished{false};
#ifndef SVX_NO_THREADS
  std::thread worker;
  std::mutex mu;
  std::condition_variable cv;
#endif

  bool hydrate(const IVec3& cc) {
    if (!source) return false;
    const IVec3 wlo = source->chunk_lo(), whi = source->chunk_hi();
    for (int q = 0; q < 3; ++q)
      if (cc[q] < wlo[q] || cc[q] >= whi[q]) {  // outside the world: air
        snap.mark_known(cc);
        return true;
      }
    const u64 k = key3(cc[0], cc[1], cc[2]);
    if (std::binary_search(resident.begin(), resident.end(), k)) return false;  // resident, beyond the bound
    if (static_cast<i64>(hydrated.size()) >= max_hydrate) return false;
    std::vector<Vox> v;
    if (source->generate(cc, v)) snap.insert_chunk(cc, std::move(v));
    const auto it = archive.find(k);
    if (it != archive.end()) snap.apply_record(*it->second);
    snap.mark_known(cc);
    hydrated.push_back(k);
    return true;
  }
  bool is_hydrated(const IVec3& p) const {
    const IVec3 cc = chunk_of(p);
    return std::binary_search(hydrated.begin(), hydrated.end(), key3(cc[0], cc[1], cc[2]));
  }
  void extract() {
    const auto t0 = Clock::now();
    const std::vector<IVec3> set =
        structures_of(snap, seeds, max_cells, &truncated, [this](const IVec3& cc) { return hydrate(cc); });
    std::sort(hydrated.begin(), hydrated.end());
    if (!set.empty()) {
      region = std::make_unique<Region>(truncated ? extract_region(snap, center, radius, lo) : extract_set(snap, set, lo));
      region->L.kscale = kscale;
      // the job's linear-solve budget (from its size: deterministic); a cache with kinks is
      // solved within it first, then the check has it again
      const i64 n = std::max<i32>(1, region->L.n);
      if (pcg_cells > 0)
        so.max_pcg_total = static_cast<int>(std::min<i64>(so.max_pcg_total, std::max<i64>(min_pcg, pcg_cells / n)));
      std::vector<std::vector<IVec3>> pieces;
      u0 = baseline_from(snap, *region, corot, &pieces, nullptr, trace, true, so.max_pcg_total);
      if (!hydrated.empty() &&
          design_hydrated(snap, *region, u0, design_corot, design_utilization,
                          [this](const IVec3& p) { return is_hydrated(p); })) {
        // (designed: the lattice again, with the hydrated voxels' strength classes)
        region = std::make_unique<Region>(truncated ? extract_region(snap, center, radius, lo) : extract_set(snap, set, lo));
        region->L.kscale = kscale;
        pieces.clear();
        u0 = baseline_from(snap, *region, corot, &pieces, nullptr, trace, true, so.max_pcg_total);
      }
      Lattice& L = region->L;
      loose = pieces.size();  // never at rest: out of the job (the live world drops them itself)
      u = u0;
      g = gravity_vector(L, 9.81);  // the true load: self-weight (the settle approximates it)
      dc = L.dmg;
      std::vector<u64> ck;
      ck.reserve(region->vox.size() / 64 + 1);
      for (const IVec3& p : region->vox) {
        const IVec3 cc = chunk_of(p);
        const u64 k = key3(cc[0], cc[1], cc[2]);
        if (ck.empty() || ck.back() != k) ck.push_back(k);
      }
      std::sort(ck.begin(), ck.end());
      ck.erase(std::unique(ck.begin(), ck.end()), ck.end());
      for (u64 k : ck) {  // (a hydrated chunk's is the one it has once streamed in, until changed)
        const Chunk* ch = snap.chunk(unkey3(k));
        versions.emplace_back(k, ch ? ch->version : 0u);
      }
      so.corot = corot;
      so.damage = true;
      so.res_tol = 1e-7;
      so.lin_rtol = 1e-3;
      so.candidate_grace = 2;  // detection only: the dynamic continuation resolves the failure
      if (const char* e = std::getenv("SVX_VGAMMA")) so.mg.cycle_gamma = std::atoi(e);
      if (const char* e = std::getenv("SVX_VF32")) so.mg.f32_levels = std::atoi(e) != 0;
      if (const char* e = std::getenv("SVX_VCHEB")) so.mg.cheb_degree = so.mg.cheb_degree_fine = std::atoi(e);
      if (const char* e = std::getenv("SVX_VRTOL")) so.lin_rtol = std::atof(e);
      if (const char* e = std::getenv("SVX_VRESTOL")) so.res_tol = std::atof(e);
    }
    // (the snapshot is released on the simulation thread with the job: its baseline bricks are
    // shared with the live grid, whose copy-on-write reads their use counts)
    ms_extract = ms_since(t0);
  }
  void solve_here() {
    es = solve_equilibrium(region->L, u, g, dc, so, nullptr);
    solved = true;
  }
  void solve() {  // (on the simulation thread: no background worker)
#ifndef SVX_NO_THREADS
    // the team's chunking is the pool's, so the result is bitwise the same for any team size
    ThreadTeam team(threads);
    TeamScope scope(team);
#else
    SerialScope serial;
#endif
    solve_here();
  }
  void run() {  // the background worker
#ifndef SVX_NO_THREADS
    // never shares the pool with the simulation thread, the extraction (its baseline solves)
    // included: the shared pool is not reentrant
    ThreadTeam team(threads);
    TeamScope scope(team);
#else
    SerialScope serial;
#endif
    extract();
#ifndef SVX_NO_THREADS
    {
      std::lock_guard<std::mutex> lk(mu);
      extracted.store(true, std::memory_order_release);
    }
    cv.notify_all();
#else
    extracted.store(true, std::memory_order_release);
#endif
    if (region) solve_here();
    finished.store(true, std::memory_order_release);
  }
  void wait_extracted() {
#ifndef SVX_NO_THREADS
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return extracted.load(std::memory_order_acquire); });
#endif
  }
  bool in_window(const IVec3& p) const {
    const i64 dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
    return dx * dx + dy * dy + dz * dz <= i64(radius) * radius;
  }
  void join() {
#ifndef SVX_NO_THREADS
    if (worker.joinable()) worker.join();
#endif
  }
  ~Verify() { join(); }
};

struct Engine::CoarseField {
  static constexpr i32 kF = 4;  // voxels per coarse cell and axis
  f64 h = 0.125;                // fine pitch
  std::unordered_map<u64, i32> index;
  std::vector<IVec3> cell;
  std::vector<u8> anchored;
  std::vector<f64> u;  // 6 per cell
  static i32 cdiv(i32 a) { return a >= 0 ? a / kF : -((-a + kF - 1) / kF); }
  // the rigid motion of p's coarse cell at voxel p (false: no coarse cell holds p)
  bool at(const IVec3& p, f64 out[6]) const {
    const IVec3 c{cdiv(p[0]), cdiv(p[1]), cdiv(p[2])};
    const auto it = index.find(key3(c[0], c[1], c[2]));
    if (it == index.end()) return false;
    const i32 k = it->second;
    if (anchored[size_t(k)]) {
      for (int q = 0; q < 6; ++q) out[q] = 0.0;
      return true;
    }
    const f64* t = &u[6 * size_t(k)];
    f64 r[3];
    for (int q = 0; q < 3; ++q) r[q] = h * (p[q] - (kF * c[q] + 0.5 * (kF - 1)));
    out[0] = t[0] + t[4] * r[2] - t[5] * r[1];
    out[1] = t[1] + t[5] * r[0] - t[3] * r[2];
    out[2] = t[2] + t[3] * r[1] - t[4] * r[0];
    for (int q = 3; q < 6; ++q) out[q] = t[q];
    return true;
  }
};

Engine::Engine() = default;
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

void Engine::set_params(const EngineParams& p) {
  if (log_) {
    Command c;
    c.tick = st_.ticks;
    c.type = Command::Type::Params;
    c.a = {p.compliance, p.amplification, p.fragility, p.damping, static_cast<f64>(p.debug_view), p.paused ? 1.0 : 0.0};
    log_->push(c);
  }
  const bool look = p.amplification != par_.amplification || p.debug_view != par_.debug_view;
  requested_ = p;
  par_ = p;
  if (compliance_cap_ > 0.0) par_.compliance = std::min(par_.compliance, compliance_cap_);
  if (look) grid_.mark_all_dirty();
}

void Engine::set_compliance_cap(f64 cap) {
  compliance_cap_ = cap > 0.0 ? cap : 0.0;
  par_.compliance = compliance_cap_ > 0.0 ? std::min(requested_.compliance, compliance_cap_) : requested_.compliance;
}

LatticeOptions Engine::lattice_options() const {
  LatticeOptions lo;
  lo.h = grid_.h;
  lo.law.game = true;
  lo.law.fragility = par_.fragility;
  lo.contact = cfg_.contact;
  lo.plate = cfg_.plate;
  return lo;
}

void Engine::load(VoxelGrid&& g, const std::array<f64, 3>& spawn_pos, const std::array<f64, 3>& spawn_dir) {
  set_compliance_cap(0.0);  // (a new world: its own buckling margin, if any)
  active_.clear();
  settles_.clear();
  spawns_.clear();
  // running jobs own their snapshot and finish in the background (reaped by later ticks)
  for (auto& j : verify_)
    if (j->start >= 0) verify_stale_.push_back(std::move(j));
  verify_.clear();
  movers_.clear();
  mover_cols_.clear();
  viewer_set_ = false;
  queue_.clear();
  impacts_.clear();
  debris_.clear();
  events_.clear();
  removed_chunks_.clear();
  animated_.clear();
  for (const auto& [k, c] : grid_.chunks()) removed_chunks_.push_back(k);
  grid_ = std::move(g);
  grid_.mark_all_dirty();
  source_.reset();
  generated_.clear();
  column_count_.clear();
  archive_.clear();
  summaries_.clear();
  for (u64 k : far_sent_) {
    const IVec3 t = unkey3(k);
    far_removed_.push_back({t[0], t[1]});
  }
  far_sent_.clear();
  far_out_.clear();
  spawn_pos_ = spawn_pos;
  spawn_dir_ = spawn_dir;
  viewer_ = spawn_pos;
  st_ = EngineStats{};
  design_ = DesignReport{};
  coarse_.reset();
  unbaked_.clear();
  for (const auto& [k, ch] : grid_.chunks()) {
    bool structural = false;
    if (ch.uniform) {
      structural = vox_solid(ch.value) && !vox_anchored(ch.value);
    } else {
      for (Vox v : ch.v)
        if (vox_solid(v) && !vox_anchored(v)) {
          structural = true;
          break;
        }
    }
    if (structural) unbaked_.push_back(k);
  }
  std::sort(unbaked_.begin(), unbaked_.end());
  grid_.track_changes(true);
}

std::vector<u8> Engine::save_delta() const {
  if (!source_) return grid_.save_delta();
  // streamed worlds: resident modified chunks plus the archive of evicted ones
  std::vector<u64> keys = grid_.modified_chunks();
  for (const auto& [k, r] : archive_) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  std::vector<std::vector<u8>> recs;
  for (u64 k : keys) {
    const auto it = archive_.find(k);
    recs.push_back(it != archive_.end() && !grid_.is_modified(k) ? *it->second : grid_.chunk_record(k));
  }
  return VoxelGrid::pack_delta(recs);
}

void Engine::enable_streaming(std::unique_ptr<ChunkSource> src, const StreamConfig& sc) {
  source_ = std::move(src);
  stream_ = sc;
  generated_.clear();
  column_count_.clear();
  archive_.clear();
  summaries_.clear();
  grid_.lo = {source_->chunk_lo()[0] * kChunk, source_->chunk_lo()[1] * kChunk, source_->chunk_lo()[2] * kChunk};
  grid_.hi = {source_->chunk_hi()[0] * kChunk, source_->chunk_hi()[1] * kChunk, source_->chunk_hi()[2] * kChunk};
  unbaked_.clear();
  // the neighbourhood of the spawn is generated right away (the rest streams in)
  viewer_ = source_->spawn_pos();
  const i32 r = static_cast<i32>(std::ceil(24.0 / grid_.h));
  const IVec3 c = voxel_of(viewer_, grid_.h);
  ensure_chunks({c[0] - r, c[1] - r, grid_.lo[2]}, {c[0] + r, c[1] + r, grid_.hi[2]});
}

bool Engine::generate_chunk(u64 key) {
  if (!source_ || generated_.count(key)) return false;
  const IVec3 cc = unkey3(key);
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  if (cc[0] < lo[0] || cc[1] < lo[1] || cc[2] < lo[2] || cc[0] >= hi[0] || cc[1] >= hi[1] || cc[2] >= hi[2]) return false;
  std::vector<Vox> v;
  const bool any = source_->generate(cc, v);
  insert_generated(key, any, std::move(v));
  return true;
}

void Engine::insert_generated(u64 key, bool any, std::vector<Vox>&& v) {
  const IVec3 cc = unkey3(key);
  generated_.insert(key);
  ++column_count_[key3(cc[0], cc[1], 0)];
  summaries_.erase(key);  // resident now: searches read its voxels
  ++st_.generated_total;
  bool changed = false;
  if (any) {
    grid_.insert_chunk(cc, std::move(v));
    changed = true;
  }
  const auto ait = archive_.find(key);
  if (ait != archive_.end()) {
    grid_.track_changes(true);
    grid_.apply_record(*ait->second);
    archive_.erase(ait);
    changed = true;
  }
  if (!changed) return;
  grid_.mark_dirty(cc);
  for (int d = 0; d < 3; ++d)
    for (int s = -1; s <= 1; s += 2) {
      IVec3 q = cc;
      q[d] += s;
      grid_.mark_dirty(q);
    }
  const Chunk* ch = grid_.chunk(cc);
  bool structural = false;
  if (ch) {
    if (ch->uniform) structural = vox_solid(ch->value) && !vox_anchored(ch->value);
    else
      for (Vox x : ch->v)
        if (vox_solid(x) && !vox_anchored(x)) {
          structural = true;
          break;
        }
  }
  if (structural) unbaked_.push_back(key);
}

void Engine::evict_chunk(u64 k) {
  const IVec3 cc = unkey3(k);
  if (grid_.is_modified(k)) archive_[k] = std::make_shared<const std::vector<u8>>(grid_.chunk_record(k));
  summaries_.erase(k);
  const bool resident = grid_.chunk(cc) != nullptr;
  grid_.remove_chunk(cc);
  generated_.erase(k);
  if (--column_count_[key3(cc[0], cc[1], 0)] <= 0) column_count_.erase(key3(cc[0], cc[1], 0));
  ++st_.evicted_total;
  if (resident) removed_chunks_.push_back(k);
  unbaked_.erase(std::remove(unbaked_.begin(), unbaked_.end(), k), unbaked_.end());
}

void Engine::ensure_chunks(const IVec3& vlo, const IVec3& vhi) {
  if (!source_) return;
  for (i32 x = vlo[0] >> kChunkBits; x <= (vhi[0] - 1) >> kChunkBits; ++x)
    for (i32 y = vlo[1] >> kChunkBits; y <= (vhi[1] - 1) >> kChunkBits; ++y)
      for (i32 z = vlo[2] >> kChunkBits; z <= (vhi[2] - 1) >> kChunkBits; ++z) generate_chunk(key3(x, y, z));
}

// Coarse connectivity of the chunks that are not resident, for detachment searches
// (world/coarse.hpp): a chunk's summary is made from exactly what it holds, its source content
// plus its archived edits, so a search over summaries decides what a search over the whole
// world would. Built on the stack per search (the engine is movable: no stored back pointer).
class Engine::StreamCoarse final : public CoarseWorld {
 public:
  explicit StreamCoarse(Engine& e) : e_(e) {}
  bool resident(const IVec3& cc) const override { return e_.chunk_resident(cc); }
  const ChunkSummary* summary(const IVec3& cc) override {
    const u64 k = key3(cc[0], cc[1], cc[2]);
    auto it = e_.summaries_.find(k);
    if (it != e_.summaries_.end()) return &it->second;
    const auto t0 = Clock::now();
    VoxelGrid tmp;
    tmp.h = e_.grid_.h;
    std::vector<Vox> v;
    if (e_.source_->generate(cc, v)) tmp.insert_chunk(cc, std::move(v));
    const auto ait = e_.archive_.find(k);
    if (ait != e_.archive_.end()) tmp.apply_record(*ait->second);
    it = e_.summaries_.emplace(k, summarize_chunk(tmp, cc)).first;
    ++e_.st_.coarse_summaries;
    e_.st_.coarse_ms += ms_since(t0);
    return &it->second;
  }

 private:
  Engine& e_;
};

bool Engine::chunk_resident(const IVec3& cc) const {
  if (!source_) return true;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  for (int q = 0; q < 3; ++q)
    if (cc[q] < lo[q] || cc[q] >= hi[q]) return true;  // outside the world: air
  return generated_.count(key3(cc[0], cc[1], cc[2])) > 0;
}

std::vector<std::vector<IVec3>> Engine::world_islands(const std::vector<IVec3>& seeds, bool supports_removed) {
  if (!source_) return detached_islands_grid(grid_, seeds, supports_removed, nullptr);
  StreamCoarse coarse(*this);
  std::vector<u64> far;
  auto islands = detached_islands_grid(grid_, seeds, supports_removed, nullptr, &coarse, &far);
  // A piece reaching into chunks that are not resident: those become resident (their voxels go
  // with the piece; eviction archives the result), then the whole piece is searched again.
  for (int round = 0; !far.empty() && round < 8; ++round) {
    for (u64 k : far)
      if (generate_chunk(k)) ++st_.coarse_hydrated;
    far.clear();
    islands = detached_islands_grid(grid_, seeds, supports_removed, nullptr, &coarse, &far);
  }
  return islands;
}

int Engine::stream_update() {
  if (!source_) return 0;
  int generated = 0;
  const auto t0 = Clock::now();
  const f64 cs = grid_.h * kChunk;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  const f64 vx = viewer_[0] / grid_.h + 0.5, vy = viewer_[1] / grid_.h + 0.5;
  auto hdist = [&](const IVec3& cc) {
    const f64 dx = (cc[0] + 0.5) * kChunk - vx, dy = (cc[1] + 0.5) * kChunk - vy;
    return std::sqrt(dx * dx + dy * dy) * grid_.h;
  };
  // generate: nearest missing chunks first, within the budget
  const i32 rl = static_cast<i32>(std::ceil(stream_.load_radius / cs)) + 1;
  const i32 cx = static_cast<i32>(std::floor(vx / kChunk)), cy = static_cast<i32>(std::floor(vy / kChunk));
  std::vector<std::pair<f64, u64>> want;
  for (i32 x = std::max(lo[0], cx - rl); x <= std::min(hi[0] - 1, cx + rl); ++x)
    for (i32 y = std::max(lo[1], cy - rl); y <= std::min(hi[1] - 1, cy + rl); ++y) {
      const f64 d = hdist({x, y, 0});
      if (d > stream_.load_radius) continue;
      const auto cit = column_count_.find(key3(x, y, 0));
      if (cit != column_count_.end() && cit->second >= hi[2] - lo[2]) continue;  // complete column
      for (i32 z = lo[2]; z < hi[2]; ++z) {
        const u64 k = key3(x, y, z);
        if (!generated_.count(k)) want.push_back({d, k});
      }
    }
  std::sort(want.begin(), want.end());
  // deterministic budget: chunks with voxels (meshing / baking work) are the cost; empty sky
  // chunks are nearly free and get their own, larger cap
  int budget = stream_.chunks_per_tick, empty_budget = 16 * stream_.chunks_per_tick;
  // Candidates are generated ahead in parallel batches (sources are pure functions of the chunk)
  // and taken in order under the same budgets: the chunks inserted are those of a serial pass;
  // what a batch generated beyond the budgets is dropped.
  std::vector<std::vector<Vox>> vox;
  std::vector<u8> any;
  for (size_t i = 0; i < want.size() && budget > 0 && empty_budget > 0;) {
    const size_t n = std::min(want.size() - i, size_t(2 * budget + 8));
    vox.assign(n, {});
    any.assign(n, 0);
    const ChunkSource* src = source_.get();
    parallel_for(static_cast<i64>(n), 1, [&](i64 b0, i64 e0) {
      for (i64 j = b0; j < e0; ++j) any[size_t(j)] = src->generate(unkey3(want[i + size_t(j)].second), vox[size_t(j)]) ? 1 : 0;
    });
    for (size_t j = 0; j < n && budget > 0 && empty_budget > 0; ++j) {
      const u64 k = want[i + j].second;
      insert_generated(k, any[j] != 0, std::move(vox[j]));
      ++generated;
      if (grid_.chunk(unkey3(k))) --budget;
      else --empty_budget;
    }
    i += n;
  }
  const f64 ms_gen = ms_since(t0);
  // evict behind the viewer (never under a running bubble)
  std::unordered_set<u64> busy;
  for (const auto& a : active_)
    for (u64 k : a->chunks) busy.insert(k);
  for (const auto& sl : settles_)
    for (u64 k : sl->chunks) busy.insert(k);
  std::vector<u64> keys(generated_.begin(), generated_.end());
  std::sort(keys.begin(), keys.end());
  for (u64 k : keys) {
    const IVec3 cc = unkey3(k);
    if (hdist(cc) <= stream_.evict_radius || busy.count(k)) continue;
    evict_chunk(k);
  }
  // byte budget (plan §B8): beyond it, the farthest chunks outside the load radius go first
  // (deterministic: distance, then key; they come back only once the viewer nears them)
  if (stream_.max_resident_mb > 0.0 && st_.ticks % 30 == 0) {
    const i64 budget = static_cast<i64>(stream_.max_resident_mb * 1048576.0);
    i64 bytes = grid_.memory_bytes();
    if (bytes > budget) {
      std::vector<std::pair<f64, u64>> far;
      for (u64 k : generated_) {
        const IVec3 cc = unkey3(k);
        if (!grid_.chunk(cc)) continue;  // (air: frees nothing)
        const f64 d = hdist(cc);
        if (d > stream_.load_radius && !busy.count(k)) far.push_back({-d, k});
      }
      std::sort(far.begin(), far.end());
      for (const auto& [nd, k] : far) {
        if (bytes <= budget) break;
        const Chunk* ch = grid_.chunk(unkey3(k));  // (its storage; the overlays it drops are a bonus)
        bytes -= ch ? static_cast<i64>(sizeof(Chunk) + ch->v.size() + ch->broken.size() + ch->strength.size()) : 0;
        evict_chunk(k);
        ++st_.budget_evicted;
      }
    }
  }
  const f64 ms_evict = ms_since(t0) - ms_gen;
  far_update();
  st_.stream_ms = ms_since(t0);
  static const bool trace = std::getenv("SVX_STREAM_TRACE") != nullptr;
  if (trace && st_.stream_ms > 1.0)
    std::printf("    [stream] tick %lld: %.2f ms = want+generate %.2f (%d chunks) + evict %.2f + far %.2f (resident %zu)\n",
                static_cast<long long>(st_.ticks), st_.stream_ms, ms_gen, generated, ms_evict, st_.stream_ms - ms_gen - ms_evict,
                generated_.size());
  return generated;
}

void Engine::far_update() {
  if (stream_.far_radius <= stream_.evict_radius || stream_.far_tile <= 0) return;
  const f64 tile_m = grid_.h * kChunk * stream_.far_tile;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  const i32 tlo0 = lo[0] / stream_.far_tile - 1, thi0 = hi[0] / stream_.far_tile + 1;
  const i32 tlo1 = lo[1] / stream_.far_tile - 1, thi1 = hi[1] / stream_.far_tile + 1;
  const f64 vx = viewer_[0] + 0.5 * grid_.h, vy = viewer_[1] + 0.5 * grid_.h;  // world at voxel corners
  auto near_dist = [&](i32 tx, i32 ty) {  // viewer to the tile's rectangle (0 inside)
    const f64 x0 = tx * tile_m, x1 = x0 + tile_m, y0 = ty * tile_m, y1 = y0 + tile_m;
    const f64 dx = vx < x0 ? x0 - vx : vx > x1 ? vx - x1 : 0.0;
    const f64 dy = vy < y0 ? y0 - vy : vy > y1 ? vy - y1 : 0.0;
    return std::sqrt(dx * dx + dy * dy);
  };
  // drop tiles that came near (the detailed chunks take over) or went out of range
  std::vector<u64> keys(far_sent_.begin(), far_sent_.end());
  std::sort(keys.begin(), keys.end());
  for (u64 k : keys) {
    const IVec3 t = unkey3(k);
    const f64 d = near_dist(t[0], t[1]);
    if (d < stream_.evict_radius - 16.0 || d > stream_.far_radius + tile_m) {
      far_sent_.erase(k);
      far_removed_.push_back({t[0], t[1]});
    }
  }
  // new far tiles, nearest first, within the budget
  const i32 r = static_cast<i32>(std::ceil(stream_.far_radius / tile_m)) + 1;
  const i32 cx = static_cast<i32>(std::floor(vx / tile_m)), cy = static_cast<i32>(std::floor(vy / tile_m));
  std::vector<std::pair<f64, u64>> want;
  for (i32 tx = std::max(tlo0, cx - r); tx <= std::min(thi0, cx + r); ++tx)
    for (i32 ty = std::max(tlo1, cy - r); ty <= std::min(thi1, cy + r); ++ty) {
      const f64 d = near_dist(tx, ty);
      if (d <= stream_.evict_radius || d > stream_.far_radius) continue;
      const u64 k = key3(tx, ty, 0);
      if (!far_sent_.count(k)) want.push_back({d, k});
    }
  std::sort(want.begin(), want.end());
  int budget = stream_.far_tiles_per_tick;
  for (const auto& [d, k] : want) {
    if (budget-- <= 0) break;
    const IVec3 t = unkey3(k);
    far_sent_.insert(k);
    ChunkMesh m = far_mesh(t[0], t[1]);
    if (!m.indices.empty()) far_out_.push_back(std::move(m));
  }
}

ChunkMesh Engine::far_mesh(i32 tx, i32 ty) const {
  ChunkMesh m;
  m.chunk = {tx, ty, 0};
  const i32 f = stream_.far_factor, T = stream_.far_tile;
  const IVec3 clo = source_->chunk_lo(), chi = source_->chunk_hi();
  const IVec3 lo{tx * T * kChunk, ty * T * kChunk, clo[2] * kChunk};
  const i32 n[3] = {T * kChunk / f, T * kChunk / f, (chi[2] - clo[2]) * kChunk / f};
  std::vector<Vox> occ;
  if (!source_->coarse(lo, {n[0], n[1], n[2]}, f, occ)) return m;
  // outside the tile counts as solid: no underside and no faces on tile borders (neighbour
  // tiles close the gaps; beyond the far radius the fog does); above the tile is air
  auto at = [&](i32 x, i32 y, i32 z) -> Vox {
    if (z >= n[2]) return kAir;
    if (x < 0 || y < 0 || z < 0 || x >= n[0] || y >= n[1]) return make_vox(MaterialId::Rock, true);
    return occ[(size_t(x) * n[1] + y) * n[2] + z];
  };
  const f64 h = grid_.h;
  // greedy meshing per face direction and slice: merge equal-material faces into rectangles
  std::vector<i32> mask;
  for (int face = 0; face < 6; ++face) {
    const int a = face >> 1, s = (face & 1) ? 1 : -1;
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    const i32 nb = n[b], nc = n[c];
    mask.assign(size_t(nb) * nc, 0);
    for (i32 k = 0; k < n[a]; ++k) {
      for (i32 u = 0; u < nb; ++u)
        for (i32 w = 0; w < nc; ++w) {
          i32 p[3];
          p[a] = k;
          p[b] = u;
          p[c] = w;
          const Vox v = at(p[0], p[1], p[2]);
          p[a] += s;
          mask[size_t(u) * nc + w] =
              vox_solid(v) && !vox_solid(at(p[0], p[1], p[2])) ? 1 + static_cast<i32>(vox_mat(v)) : 0;
        }
      for (i32 u = 0; u < nb; ++u)
        for (i32 w = 0; w < nc;) {
          const i32 id = mask[size_t(u) * nc + w];
          if (!id) {
            ++w;
            continue;
          }
          i32 ww = 1;
          while (w + ww < nc && mask[size_t(u) * nc + w + ww] == id) ++ww;
          i32 hh = 1;
          for (bool grow = true; grow && u + hh < nb; ) {
            for (i32 q = 0; q < ww; ++q)
              if (mask[size_t(u + hh) * nc + w + q] != id) {
                grow = false;
                break;
              }
            if (grow) ++hh;
          }
          for (i32 du = 0; du < hh; ++du)
            for (i32 q = 0; q < ww; ++q) mask[size_t(u + du) * nc + w + q] = 0;
          // quad: cells [u, u + hh) x [w, w + ww) on the face plane of slice k
          f64 base[3];
          base[a] = h * (lo[a] + (k + (s > 0 ? 1 : 0)) * f - 0.5);
          base[b] = h * (lo[b] + u * f - 0.5);
          base[c] = h * (lo[c] + w * f - 0.5);
          const f64 eb = h * hh * f, ec = h * ww * f;
          const u32 v0 = static_cast<u32>(m.vertices.size());
          const f64 cr[4][2] = {{0, 0}, {eb, 0}, {eb, ec}, {0, ec}};
          for (const auto& q : cr) {
            MeshVertex mv{};
            f64 pp[3] = {base[0], base[1], base[2]};
            pp[b] += q[0];
            pp[c] += q[1];
            for (int kq = 0; kq < 3; ++kq) mv.pos[kq] = static_cast<f32>(pp[kq]);
            mv.normal[a] = static_cast<i8>(127 * s);
            mv.normal[3] = 127;
            mv.uv[0] = static_cast<f32>(pp[b] * 32.0);
            mv.uv[1] = static_cast<f32>(pp[c] * 32.0);
            mv.texture = static_cast<u16>(0xFF00 + (id - 1));
            mv.light = 255;
            m.vertices.push_back(mv);
          }
          // counter-clockwise seen from outside ((b, c, a) is right-handed)
          if (s > 0) {
            for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(v0 + i);
          } else {
            for (u32 i : {0u, 2u, 1u, 0u, 3u, 2u}) m.indices.push_back(v0 + i);
          }
          w += ww;
        }
    }
  }
  return m;
}

std::vector<ChunkMesh> Engine::take_far_meshes() {
  std::vector<ChunkMesh> out;
  out.swap(far_out_);
  return out;
}

std::vector<std::array<i32, 2>> Engine::take_far_removed() {
  std::vector<std::array<i32, 2>> out;
  out.swap(far_removed_);
  return out;
}

bool Engine::load_delta(const std::vector<u8>& bytes) {
  std::vector<u64> touched;
  if (source_) {
    // streamed worlds: resident chunks take their records now, the rest when they stream in
    std::vector<std::pair<u64, std::vector<u8>>> recs;
    if (!VoxelGrid::unpack_delta(bytes, &recs)) return false;
    for (auto& [k, r] : recs) {
      if (generated_.count(k)) {
        u64 key = 0;
        if (!grid_.apply_record(r, &key)) return false;
        touched.push_back(key);
      } else {
        archive_[k] = std::make_shared<const std::vector<u8>>(std::move(r));
        summaries_.erase(k);
      }
    }
  } else if (!grid_.load_delta(bytes, &touched)) {
    return false;
  }
  // the cached static state around changed geometry is stale: drop it and re-bake lazily
  std::vector<u64> rebake;
  for (u64 k : touched) {
    const IVec3 cc = unkey3(k);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          const IVec3 n{cc[0] + dx, cc[1] + dy, cc[2] + dz};
          const Chunk* ch = grid_.chunk(n);
          if (!ch) continue;
          const IVec3 b{n[0] * kChunk, n[1] * kChunk, n[2] * kChunk};
          for (int i = 0; i < kChunkVox; ++i)
            grid_.clear_baseline({b[0] + i / (kChunk * kChunk), b[1] + (i / kChunk) % kChunk, b[2] + i % kChunk});
          rebake.push_back(key3(n[0], n[1], n[2]));
        }
  }
  unbaked_.insert(unbaked_.end(), rebake.begin(), rebake.end());
  std::sort(unbaked_.begin(), unbaked_.end());
  unbaked_.erase(std::unique(unbaked_.begin(), unbaked_.end()), unbaked_.end());
  // movers are never part of a delta, but a restored chunk record may hold them in another
  // state: put them back as they are now (closed after a load)
  for (Mover& m : movers_) set_mover_rows(m, m.rows);
  return true;
}

template <typename Inner>
void Engine::design_pass(const Lattice& L, const std::vector<IVec3>& vox, const std::vector<f64>& u, Inner&& inner) {
  std::vector<u8> cls(L.n, 0);
  for (i32 i = 0; i < L.n; ++i) cls[i] = grid_.strength(vox[i]);
  std::vector<u8> need(L.n, 0);
  struct Over {
    i32 i, j;
    f64 util, cur;
  };
  std::vector<Over> over;
  f64 after = 0.0;
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || L.dead[i] || L.dead[j] || !inner(i) || !inner(j)) continue;
      Vec6 g = bond_jump(L, a, i, u.data(), cfg_.corot && !cfg_.bake_linear);  // as solved
      for (auto& v : g) v *= L.kscale;
      const LawEval ev = evaluate_law(L.bond(a, i), g, 0.0, L.law.game);
      const f64 util = ev.phi;
      if (util > design_.max_utilization) {
        design_.max_utilization = util;
        design_.worst = vox[i];
        design_.worst_axis = a;
        design_.worst_mat = L.mat[i];
        const BondModel& bm = L.bond(a, i);  // (the component nearest its onset)
        f64 best = -1.0;
        for (int q = 0; q < 6; ++q) {
          const f64 on = q == 0 ? (g[0] >= 0.0 ? bm.onset.open : bm.onset.comp) : bm.onset.comp6[q];
          const f64 r = on > 0.0 ? std::abs(g[q]) / on : 0.0;
          if (r > best) {
            best = r;
            design_.worst_component = q;
          }
        }
      }
      if (util <= cfg_.design_utilization) {
        after = std::max(after, util);
        continue;
      }
      const f64 cur = strength_multiplier(std::min(cls[i], cls[j]));
      const u8 c = strength_class_for(cur * util / cfg_.design_utilization);
      // both ends: a bond's class is the smaller of its voxels' (rock included, so supports on
      // rock can be designed too; rock voxels are never simulated)
      for (i32 k : {i, j}) need[k] = std::max(need[k], c);
      over.push_back({i, j, util, cur});
    }
  for (const Over& o : over) {
    const u8 c = std::min(std::max(cls[o.i], need[o.i]), std::max(cls[o.j], need[o.j]));
    const f64 post = o.util * o.cur / strength_multiplier(c);
    after = std::max(after, post);
    if (post >= 1.0) ++design_.unfixable_bonds;
  }
  design_.max_utilization_after = std::max(design_.max_utilization_after, after);
  for (i32 i = 0; i < L.n; ++i) {
    if (L.dead[i] || need[i] <= cls[i]) continue;
    grid_.set_strength(vox[i], need[i]);
    ++design_.strengthened_voxels;
  }
}

bool Engine::bake_tile() {
  if (unbaked_.empty()) return false;
  // nearest unbaked chunk to the viewer (ties by key: deterministic); in streamed worlds only
  // chunks whose whole neighbourhood is generated (the tile margin reaches into it)
  const f64 h = grid_.h;
  size_t best = unbaked_.size();
  f64 bd = INFINITY;
  auto ready = [&](const IVec3& cc) {
    if (!source_) return true;
    const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          const IVec3 n{cc[0] + dx, cc[1] + dy, cc[2] + dz};
          if (n[0] < lo[0] || n[1] < lo[1] || n[2] < lo[2] || n[0] >= hi[0] || n[1] >= hi[1] || n[2] >= hi[2]) continue;
          if (!generated_.count(key3(n[0], n[1], n[2]))) return false;
        }
    return true;
  };
  std::vector<u64>& queue = unbaked_;
  best = queue.size();
  for (size_t k = 0; k < queue.size(); ++k) {
    const IVec3 cc = unkey3(queue[k]);
    if (!ready(cc)) continue;
    f64 d2 = 0.0;
    for (int q = 0; q < 3; ++q) {
      const f64 c = h * ((cc[q] + 0.5) * kChunk);
      d2 += (c - viewer_[q]) * (c - viewer_[q]);
    }
    if (d2 < bd) {
      bd = d2;
      best = k;
    }
  }
  if (best == queue.size()) return false;  // nothing bakeable yet
  const IVec3 cc = unkey3(queue[best]);
  queue.erase(queue.begin() + static_cast<long>(best));
  bake_chunk(cc);
  return true;
}

// Bakes a chunk's baseline from a tile (the chunk and a margin) with its rim held at the coarse
// field (a world baked progressively), else at cached baselines of baked neighbours.
void Engine::bake_chunk(const IVec3& cc) {
  const auto t0 = Clock::now();
  constexpr i32 kMargin = 8;
  const IVec3 clo{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  const IVec3 chi{clo[0] + kChunk, clo[1] + kChunk, clo[2] + kChunk};
  Region R = extract_box(grid_, {clo[0] - kMargin, clo[1] - kMargin, clo[2] - kMargin},
                         {chi[0] + kMargin, chi[1] + kMargin, chi[2] + kMargin}, lattice_options());
  Lattice& L = R.L;
  L.kscale = 1.0 / std::max(1e-9, par_.compliance);
  // the rim pinned at the coarse field's rigid motions (tiles agree across chunk borders,
  // whatever their order), else at cached baselines where neighbouring tiles already solved them
  L.u_fixed.assign(6 * size_t(L.n), 0.0);
  std::vector<f64> u(6 * size_t(L.n), 0.0);
  for (i32 i = 0; i < L.n; ++i) {
    f64 c6[6];
    if (coarse_ && coarse_->at(R.vox[i], c6)) {
      for (int q = 0; q < 6; ++q) (R.rim[i] ? L.u_fixed : u)[6 * size_t(i) + q] = c6[q];
      continue;
    }
    f32 b[6];
    if (!grid_.baseline(R.vox[i], b)) continue;
    for (int q = 0; q < 6; ++q) {
      if (R.rim[i]) L.u_fixed[6 * size_t(i) + q] = b[q];
      else u[6 * size_t(i) + q] = b[q];
    }
  }
  // pieces reaching neither support nor the tile rim float in the source world: removed
  std::vector<IVec3> floating;
  for (const auto& isl : unsupported_components(L))
    for (i32 c : isl) {
      L.remove_cell(c);
      floating.push_back(R.vox[c]);
    }
  design_.floating_voxels += remove_floating(floating);
  StaticsOptions so;
  so.corot = cfg_.corot && !cfg_.bake_linear;
  so.damage = false;
  so.res_tol = 1e-7;
  so.lin_rtol = 1e-3;
  so.max_iters = 25;  // a tile must never stall the worker (the baseline is refined by events)
  const DamageField dc = L.dmg;
  solve_equilibrium(L, u, gravity_vector(L, 9.81), dc, so, nullptr);
  auto in_chunk = [&](i32 i) {
    const IVec3& p = R.vox[i];
    return p[0] >= clo[0] && p[1] >= clo[1] && p[2] >= clo[2] && p[0] < chi[0] && p[1] < chi[1] && p[2] < chi[2];
  };
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i] || L.dead[i] || !in_chunk(i)) continue;
    f32 b[6];
    for (int q = 0; q < 6; ++q) b[q] = static_cast<f32>(u[6 * size_t(i) + q]);
    grid_.set_baseline(R.vox[i], b);
  }
  design_pass(L, R.vox, u, in_chunk);
  st_.bake_ms += ms_since(t0);
}

void Engine::build_coarse_field() {
  constexpr i32 F = CoarseField::kF;
  const auto t0 = Clock::now();
  struct Block {
    u8 lay[3] = {0, 0, 0};  // occupied layers per axis (bits)
    u32 n = 0;              // structural voxels
    bool rock = false;      // holds bedrock touching structure: a support
    std::array<u32, static_cast<size_t>(MaterialId::Count)> votes{};
  };
  std::unordered_map<u64, Block> blocks;
  auto bkey = [](const IVec3& p) {
    return key3(CoarseField::cdiv(p[0]), CoarseField::cdiv(p[1]), CoarseField::cdiv(p[2]));
  };
  std::vector<u64> ckeys;
  for (const auto& kc : grid_.chunks()) ckeys.push_back(kc.first);
  std::sort(ckeys.begin(), ckeys.end());
  for (u64 k : ckeys) {
    const Chunk& ch = grid_.chunks().at(k);
    if (ch.uniform && (!vox_solid(ch.value) || vox_anchored(ch.value))) continue;  // (air, rock)
    const IVec3 cc = unkey3(k);
    const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
    for (int i = 0; i < kChunkVox; ++i) {
      const Vox v = ch.uniform ? ch.value : ch.v[size_t(i)];
      if (!vox_solid(v) || vox_anchored(v)) continue;
      const IVec3 p{b[0] + i / (kChunk * kChunk), b[1] + (i / kChunk) % kChunk, b[2] + i % kChunk};
      Block& bl = blocks[bkey(p)];
      for (int a = 0; a < 3; ++a) bl.lay[a] |= static_cast<u8>(1u << (p[a] - F * CoarseField::cdiv(p[a])));
      ++bl.n;
      ++bl.votes[static_cast<size_t>(vox_mat(v))];
      for (int a = 0; a < 3; ++a)
        for (int sg = -1; sg <= 1; sg += 2) {
          IVec3 q = p;
          q[a] += sg;
          const Vox w = grid_.get(q);
          if (vox_solid(w) && vox_anchored(w)) blocks[bkey(q)].rock = true;
        }
    }
  }
  std::vector<u64> keys;
  keys.reserve(blocks.size());
  for (const auto& kb : blocks) keys.push_back(kb.first);
  std::sort(keys.begin(), keys.end());
  std::vector<CellIn> cells;
  cells.reserve(keys.size());
  for (u64 k : keys) {
    const Block& bl = blocks.at(k);
    CellIn c;
    const IVec3 cp = unkey3(k);
    c.p = {cp[0], cp[1], cp[2]};
    c.anchored = bl.rock;
    if (bl.rock && bl.n == 0) {
      c.mat = MaterialId::Rock;
    } else {
      size_t best = 0;
      for (size_t m = 1; m < bl.votes.size(); ++m)
        if (bl.votes[m] > bl.votes[best]) best = m;
      c.mat = static_cast<MaterialId>(best);
      for (int a = 0; a < 3; ++a) c.eff[size_t(a)] = std::max(1, __builtin_popcount(bl.lay[a])) / static_cast<f64>(F);
    }
    cells.push_back(c);
  }
  LatticeOptions lo = lattice_options();
  lo.h = grid_.h * F;
  Lattice L = build_lattice(cells, lo);
  L.kscale = 1.0 / std::max(1e-9, par_.compliance);
  for (const auto& isl : unsupported_components(L))
    for (i32 c : isl) L.remove_cell(c);
  std::vector<f64> u(6 * size_t(L.n), 0.0);
  StaticsOptions so;
  so.corot = false;
  so.damage = false;
  so.res_tol = 1e-8;
  so.lin_rtol = 1e-4;
  const DamageField dc = L.dmg;
  const EquilibriumStats es = solve_equilibrium(L, u, gravity_vector(L, 9.81), dc, so, nullptr);
  auto cf = std::make_unique<CoarseField>();
  cf->h = grid_.h;
  cf->cell.reserve(size_t(L.n));
  for (i32 i = 0; i < L.n; ++i) {
    if (L.dead[i]) continue;
    const i32 k = static_cast<i32>(cf->cell.size());
    cf->index.emplace(keys[size_t(i)], k);
    cf->cell.push_back({L.p[i][0], L.p[i][1], L.p[i][2]});
    cf->anchored.push_back(L.anchored[i]);
    for (int q = 0; q < 6; ++q) cf->u.push_back(u[6 * size_t(i) + q]);
  }
  coarse_ = std::move(cf);
  if (std::getenv("SVX_ENGINE_TRACE"))
    std::printf("    [bake] coarse field: %d cells (%zu blocks) solved in %.0f ms (%d pcg)\n", L.n, keys.size(), ms_since(t0),
                es.pcg_iters);
}

bool Engine::bake(f64* ms, int* pcg_iters) {
  const auto t0 = Clock::now();
  if (source_) return false;  // streamed worlds bake tile by tile
  // (SVX_BAKE_ANY_SIZE: bake whole whatever the size - the reference a progressive bake is
  // checked against; MAP01 takes ~9 GB and ~75 s natively)
  static const bool any_size = std::getenv("SVX_BAKE_ANY_SIZE") != nullptr;
  if (!any_size && grid_.solid_count() > cfg_.bake_max_cells) {
    build_coarse_field();  // (the tiles' rims)
    return false;
  }
  // every solid voxel (and bonded rock) of the loaded world as one lattice, in chunk key order:
  // the cell numbering shapes aggregation and summation order, and hash-map order is not
  // portable (32-bit WASM hashes u64 keys differently)
  std::vector<CellIn> cells;
  std::vector<IVec3> vox;
  std::vector<u64> ckeys;
  for (const auto& kc : grid_.chunks()) ckeys.push_back(kc.first);
  std::sort(ckeys.begin(), ckeys.end());
  for (u64 k : ckeys) {
    const Chunk& ch = grid_.chunks().at(k);
    const IVec3 cc = unkey3(k);
    const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
    for (int i = 0; i < kChunkVox; ++i) {
      const Vox v = ch.uniform ? ch.value : ch.v[i];
      if (!vox_solid(v)) continue;
      const IVec3 p{b[0] + i / (kChunk * kChunk), b[1] + (i / kChunk) % kChunk, b[2] + i % kChunk};
      if (vox_anchored(v)) {
        // rock matters only where it touches structure
        bool touches = false;
        for (int a = 0; a < 3 && !touches; ++a)
          for (int s = -1; s <= 1; s += 2) {
            IVec3 q = p;
            q[a] += s;
            const Vox w = grid_.get(q);
            if (vox_solid(w) && !vox_anchored(w)) {
              touches = true;
              break;
            }
          }
        if (!touches) continue;
      }
      CellIn c;
      c.p = p;
      c.mat = vox_mat(v);
      c.anchored = vox_anchored(v);
      cells.push_back(c);
      vox.push_back(p);
    }
  }
  Lattice L = build_lattice(cells, lattice_options());
  L.kscale = 1.0 / std::max(1e-9, par_.compliance);
  L.enable_damage();
  for (i32 i = 0; i < L.n; ++i)
    for (int a = 0; a < 3; ++a) {
      if (L.nbr[a][i] < 0) continue;
      if (grid_.broken(vox[i], a)) L.break_bond(i, a);
      else if (grid_.cracked(vox[i], a)) L.crack_bond(i, a);
      else L.dmg[a][i] = grid_.damage(vox[i], a);
    }
  // unsupported components cannot have a static state: the source world's floating bits (e.g.
  // stray voxels in window openings) are removed, so no window ever meets an unsolvable piece
  std::vector<IVec3> floating;
  for (const auto& isl : unsupported_components(L))
    for (i32 c : isl) {
      L.remove_cell(c);
      floating.push_back(vox[c]);
    }
  const i64 nfloat = remove_floating(floating);
  std::vector<f64> u(6 * size_t(L.n), 0.0);
  StaticsOptions so;
  so.corot = cfg_.corot && !cfg_.bake_linear;
  so.damage = false;
  so.res_tol = 1e-8;
  so.lin_rtol = 1e-4;
  const DamageField dc = L.dmg;
  const EquilibriumStats es = solve_equilibrium(L, u, gravity_vector(L, 9.81), dc, so, nullptr);
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i] || L.dead[i]) continue;
    f32 b[6];
    for (int q = 0; q < 6; ++q) b[q] = static_cast<f32>(u[6 * size_t(i) + q]);
    grid_.set_baseline(vox[i], b);
  }
  // Structural design pass (plan §B10): members whose self-weight demand exceeds the design
  // utilization are strengthened (capacity only — the stiffness and hence the baseline are
  // unchanged), so the level stands and events, not gravity, decide what breaks.
  design_ = DesignReport{};
  design_.floating_voxels = nfloat;
  design_pass(L, vox, u, [](i32) { return true; });
  unbaked_.clear();
  if (ms) *ms = ms_since(t0);
  if (pcg_iters) *pcg_iters = es.pcg_iters;
  return true;
}

void Engine::carve(const std::array<f64, 3>& pos, f64 radius) {
  if (log_) log_->push({st_.ticks, Command::Type::Carve, {pos[0], pos[1], pos[2], radius, 0.0, 0.0}});
  if (shot_resolver && !movers_.empty())
    for (const MoverTrigger& t : shot_resolver(pos)) activate_mover(t.mover, t.move);
  PendingEvent e;
  e.pos = pos;
  e.radius = radius;
  queue_.push_back(std::move(e));
}

void Engine::blast(const std::array<f64, 3>& pos, f64 radius, f64 energy) {
  if (log_) log_->push({st_.ticks, Command::Type::Blast, {pos[0], pos[1], pos[2], radius, energy, 0.0}});
  PendingEvent e;
  e.blast = true;
  e.pos = pos;
  e.radius = radius;
  e.energy = energy;
  queue_.push_back(std::move(e));
}

void Engine::set_viewer(const std::array<f64, 3>& pos) {
  if (log_) log_->push({st_.ticks, Command::Type::Viewer, {pos[0], pos[1], pos[2], 0.0, 0.0, 0.0}});
  if (viewer_set_ && walk_resolver && !movers_.empty())
    for (const MoverTrigger& t : walk_resolver(viewer_, pos)) activate_mover(t.mover, t.move);
  viewer_ = pos;
  viewer_set_ = true;
}

Engine::BaselineCheck Engine::check_baseline(const IVec3& center, i32 radius) {
  BaselineCheck bc;
  Region R = extract_region(grid_, center, radius, lattice_options());
  Lattice& L = R.L;
  L.kscale = 1.0 / std::max(1e-9, par_.compliance);
  std::vector<std::vector<IVec3>> loose;
  const std::vector<f64> u = baseline_from(grid_, R, cfg_.corot, &loose, nullptr, false);
  bc.cells = L.n;
  const std::vector<f64> gv = gravity_vector(L, 9.81);
  std::vector<f64> fint(u.size());
  internal_forces(L, u.data(), cfg_.corot, fint.data());
  f64 rn = 0.0, gn = 0.0;
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i] || L.dead[i] || R.rim[i]) continue;
    for (int q = 0; q < 6; ++q) {
      const f64 d = gv[6 * size_t(i) + q] - fint[6 * size_t(i) + q];
      rn += d * d;
      gn += gv[6 * size_t(i) + q] * gv[6 * size_t(i) + q];
    }
  }
  bc.residual = gn > 0.0 ? std::sqrt(rn / gn) : 0.0;
  if (std::getenv("SVX_RESIDUAL_CELLS")) {  // (diagnostics) the cells with the largest residual
    std::vector<std::pair<f64, i32>> worst;
    for (i32 i = 0; i < L.n; ++i) {
      if (L.anchored[i] || L.dead[i] || R.rim[i]) continue;
      f64 d2 = 0.0;
      for (int q = 0; q < 3; ++q) d2 += (gv[6 * size_t(i) + q] - fint[6 * size_t(i) + q]) * (gv[6 * size_t(i) + q] - fint[6 * size_t(i) + q]);
      worst.push_back({d2, i});
    }
    std::sort(worst.begin(), worst.end(), std::greater<>());
    for (size_t k = 0; k < std::min<size_t>(8, worst.size()); ++k) {
      const i32 i = worst[k].second;
      int nb = 0, anc = 0, rim = 0;
      for (int a = 0; a < 3; ++a)
        for (const i32 j : {L.nbr[a][i], L.nbrm[a][i]})
          if (j >= 0) {
            ++nb;
            anc += L.anchored[j] ? 1 : 0;
            rim += R.rim[j] ? 1 : 0;
          }
      std::printf("      cell (%d %d %d) mat %d: |r| %.3g N (weight %.3g N), bonds %d (anchored %d, rim %d), u %.3g %.3g %.3g\n",
                  R.vox[i][0], R.vox[i][1], R.vox[i][2], int(vox_mat(grid_.get(R.vox[i]))), std::sqrt(worst[k].first),
                  std::abs(gv[6 * size_t(i) + 2]), nb, anc, rim, u[6 * size_t(i)], u[6 * size_t(i) + 1], u[6 * size_t(i) + 2]);
    }
  }
  L.enable_damage();
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || L.dead[i] || L.dead[j] || R.rim[i] || R.rim[j]) continue;
      Vec6 jump = bond_jump(L, a, i, u.data(), cfg_.corot);
      for (auto& v : jump) v *= L.kscale;
      const f64 phi = evaluate_law(L.bond(a, i), jump, 0.0, L.law.game).phi;
      if (phi > 0.5) ++bc.over_half;
      if (phi > 1.0) ++bc.over_one;
      if (phi > bc.max_phi) {
        bc.max_phi = phi;
        bc.worst = R.vox[i];
        bc.worst_axis = a;
      }
    }
  return bc;
}

std::array<f64, 4> Engine::coarse_field_error(IVec3* worst) {
  build_coarse_field();
  std::vector<f64> err;
  f64 zmax = 0.0, emax = 0.0;
  for (const auto& [k, ch] : grid_.chunks()) {
    if (ch.uniform && (!vox_solid(ch.value) || vox_anchored(ch.value))) continue;
    const IVec3 cc = unkey3(k);
    for (int i = 0; i < kChunkVox; ++i) {
      const Vox v = ch.uniform ? ch.value : ch.v[size_t(i)];
      if (!vox_solid(v) || vox_anchored(v)) continue;
      const IVec3 p{cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk};
      f32 b[6];
      f64 c6[6];
      if (!grid_.baseline(p, b) || !coarse_->at(p, c6)) continue;
      zmax = std::max(zmax, std::abs(f64(b[2])));
      const f64 e = std::abs(c6[2] - b[2]);
      err.push_back(e);
      if (e > emax) {
        emax = e;
        if (worst) *worst = p;
      }
    }
  }
  std::sort(err.begin(), err.end());
  auto pct = [&](f64 q) { return err.empty() ? 0.0 : err[std::min(err.size() - 1, size_t(q * f64(err.size())))] / std::max(1e-30, zmax); };
  return {pct(0.5), pct(0.9), pct(0.99), emax / std::max(1e-30, zmax)};
}

std::vector<f64> Engine::window_baseline(Region& R, int* iters) {
  std::vector<std::vector<IVec3>> loose;
  // (a cached baseline that is no equilibrium is solved by the event's background job:
  // consistent_baseline)
  std::vector<f64> u = baseline_from(grid_, R, cfg_.corot, &loose, iters, std::getenv("SVX_ENGINE_TRACE") != nullptr);
  // pieces of the window connected to neither support nor rim cannot be at rest: they fall
  if (!loose.empty()) mirror_islands(loose, nullptr, nullptr, nullptr);
  return u;
}

i64 Engine::remove_floating(const std::vector<IVec3>& vox) {
  if (vox.empty()) return 0;
  const bool tracked = grid_.tracking();
  grid_.track_changes(false);
  const f32 z[3] = {0, 0, 0};
  for (const IVec3& p : vox) {
    grid_.set(p, kAir);
    grid_.clear_baseline(p);
    grid_.set_offset(p, z);
  }
  grid_.track_changes(tracked);
  return static_cast<i64>(vox.size());
}

void Engine::store_baseline(const Region& R, const std::vector<f64>& u, const std::function<bool(i32)>& keep) {
  const Lattice& L = R.L;
  u64 last = ~u64(0);
  std::vector<u64> touched;
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i] || L.dead[i] || (keep && !keep(i))) continue;
    f32 b[6];
    for (int q = 0; q < 6; ++q) b[q] = static_cast<f32>(u[6 * size_t(i) + q]);
    grid_.set_baseline(R.vox[i], b);
    const IVec3 cc = chunk_of(R.vox[i]);
    const u64 k = key3(cc[0], cc[1], cc[2]);
    if (k != last) touched.push_back(last = k);
  }
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  for (u64 k : touched) grid_.touch(unkey3(k));  // verifications that read the old ones are stale
}

void Engine::emit_detached(const std::vector<IVec3>& vox, const std::array<f64, 3>& vel, const std::array<f64, 3>& ang) {
  EngineEvent ev;
  ev.kind = EngineEvent::Kind::Detached;
  ev.id = next_id_++;
  ev.voxels = static_cast<i32>(vox.size());
  ev.vel = vel;
  ev.ang = ang;
  // mesh the piece in world coordinates from a scratch grid holding only its voxels
  VoxelGrid piece;
  piece.h = grid_.h;
  std::array<f64, 3> c{0, 0, 0};
  f64 mass = 0.0;
  std::vector<f64> masses(vox.size());
  std::unordered_set<u64> chunks;
  const f64 h3 = grid_.h * grid_.h * grid_.h;
  for (size_t k = 0; k < vox.size(); ++k) {
    const IVec3& p = vox[k];
    const Vox v = grid_.get(p) == kAir ? make_vox(MaterialId::Concrete, false) : grid_.get(p);
    piece.set(p, v);
    masses[k] = material(vox_mat(v)).rho * h3;
    mass += masses[k];
    for (int q = 0; q < 3; ++q) c[q] += masses[k] * grid_.h * p[q];
    chunks.insert(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
  }
  // the centre of mass (the pivot of the piece's motion)
  for (int q = 0; q < 3; ++q) ev.pos[q] = mass > 0.0 ? c[q] / mass : 0.0;
  if (cfg_.debris && static_cast<int>(vox.size()) >= cfg_.debris_params.min_voxels) {
    ev.rigid = debris_.add(ev.id, vox, masses, grid_.h, vel, ang, cfg_.debris_params);
    if (ev.rigid) debris_.limit(cfg_.max_debris, cfg_.debris_params);
  }
  std::vector<u64> keys(chunks.begin(), chunks.end());
  std::sort(keys.begin(), keys.end());
  MeshOptions mo;
  for (u64 k : keys) {
    const ChunkMesh m = mesh_chunk(piece, unkey3(k), mo, false);
    const u32 base = static_cast<u32>(ev.mesh.vertices.size());
    ev.mesh.vertices.insert(ev.mesh.vertices.end(), m.vertices.begin(), m.vertices.end());
    for (u32 i : m.indices) ev.mesh.indices.push_back(base + i);
  }
  events_.push_back(std::move(ev));
}

// Removes world islands from the grid (their voxels were read for the event mesh first) and
// from the active bubble's window when they overlap it.
void Engine::mirror_islands(const std::vector<std::vector<IVec3>>& islands, Active* a, const std::vector<f64>* u,
                            const std::vector<f64>* v) {
  for (const auto& isl : islands) {
    std::array<f64, 3> vel{0, 0, 0}, ang{0, 0, 0};
    std::vector<i32> cells;
    if (a) {
      f64 m = 0.0;
      for (const IVec3& p : isl) {
        const i32 c = a->region->cell(p);
        if (c < 0) continue;
        cells.push_back(c);
        if (v) {
          const f64 mc = a->region->L.mass[c];
          m += mc;
          for (int q = 0; q < 3; ++q) {
            vel[q] += mc * (*v)[6 * size_t(c) + q];
            ang[q] += mc * (*v)[6 * size_t(c) + 3 + q];
          }
        }
      }
      if (m > 0.0)
        for (int q = 0; q < 3; ++q) {
          vel[q] /= m;
          ang[q] /= m;
        }
    }
    (void)u;
    emit_detached(isl, vel, ang);
    for (const IVec3& p : isl) {
      grid_.set(p, kAir);
      grid_.clear_baseline(p);
      const f32 z[3] = {0, 0, 0};
      grid_.set_offset(p, z);
    }
    st_.detached_voxels += static_cast<i64>(isl.size());
    if (a && !cells.empty()) a->bubble.remove_cells(cells);
  }
}

void Engine::process(const PendingEvent& e) {
  const auto t0 = Clock::now();
  const f64 h = grid_.h;
  IVec3 c = voxel_of(e.pos, h);
  if (e.impact) {
    // a debris landing: ground and bedrock absorb it; a running bubble takes the impulse
    if (!impact_voxel(grid_, e.pos, e.impulse, c) || vox_anchored(grid_.get(c))) return;
    ++st_.impact_loads;
    if (impact_into_bubble(e)) return;
    if (cfg_.defer_takeovers && cfg_.tick_work > 0)
      for (const auto& ap : active_) {  // beside a running bubble's cells: its load passes
        const f64 dx = ap->center[0] - c[0], dy = ap->center[1] - c[1], dz = ap->center[2] - c[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < ap->region->radius) {
          ++st_.impacts_dropped;
          return;
        }
      }
  } else {
    ++st_.events;
  }
  // a started verification of the window this event hits is moot: set it aside (its thread
  // finishes in the background; the new event's bubble will be verified in turn). Changes
  // elsewhere in its structures drop its result at the due tick (chunk versions).
  std::vector<std::unique_ptr<Verify>> moot;
  for (size_t k = 0; k < verify_.size();) {
    if (verify_[k]->start < 0 && verify_[k]->in_window(c)) verify_[k]->quiet_since = st_.ticks;  // (not quiet yet)
    if (verify_[k]->start >= 0 && verify_[k]->in_window(c)) {
      if (std::getenv("SVX_ENGINE_TRACE"))
        std::printf("    [verify] around (%d %d %d): set aside (an event hit its window)\n", verify_[k]->center[0],
                    verify_[k]->center[1], verify_[k]->center[2]);
      moot.push_back(std::move(verify_[k]));
      verify_.erase(verify_.begin() + static_cast<long>(k));
    } else {
      ++k;
    }
  }
  // queued again: the event may settle statically (no bubble, no verification of its own)
  for (auto& j : moot) {
    enqueue_verify(j->center, j->radius, j->respawns, j->requeues + 1);
    verify_stale_.push_back(std::move(j));
  }
  // an event on a structure a running bubble spans merges into that bubble (below)
  Active* into = merge_target(e, c);
  // an event inside another running bubble's window: commit that bubble first (the new window
  // starts from its state)
  for (size_t k = 0; k < active_.size();) {
    Active& a = *active_[k];
    const f64 dx = a.center[0] - c[0], dy = a.center[1] - c[1], dz = a.center[2] - c[2];
    if (&a != into && std::sqrt(dx * dx + dy * dy + dz * dz) < a.region->radius) {
      a.takeover = true;  // (no continuation: the new event's window takes over)
      finalize(a);
      active_.erase(active_.begin() + static_cast<long>(k));
    } else {
      ++k;
    }
  }
  const i32 er = static_cast<i32>(std::ceil(e.radius / h));
  const bool trace = std::getenv("SVX_ENGINE_TRACE") != nullptr;
  f64 tp = cpu_now_ms();
  auto lap = [&](const char* what) {
    if (!trace) return;
    const f64 now = cpu_now_ms();
    std::printf("    [event] %-11s %8.2f cpu ms\n", what, now - tp);
    tp = now;
  };
  i32 spread = 0;  // extent of merged carves around the first one (voxels)
  for (const auto& x : e.extra) {
    const f64 dx = x[0] - e.pos[0], dy = x[1] - e.pos[1], dz = x[2] - e.pos[2];
    spread = std::max(spread, static_cast<i32>(std::ceil((std::sqrt(dx * dx + dy * dy + dz * dz) + x[3]) / h)));
  }
  const i32 wr = e.impact ? cfg_.impact_window
                 : e.blast ? std::max(cfg_.window, 3 * er + 8)
                           : std::max(cfg_.window_small, 4 * er + 6) + spread;
  apply_settles(false, c, wr + 1);  // (pending settles of this window first)
  ensure_chunks({c[0] - wr - 1, c[1] - wr - 1, c[2] - wr - 1}, {c[0] + wr + 2, c[1] + wr + 2, c[2] + wr + 2});
  auto win = std::make_unique<Region>(extract_region(grid_, c, wr, lattice_options()));
  Region& R = *win;
  Lattice& L = R.L;
  L.kscale = 1.0 / std::max(1e-9, par_.compliance);
  lap("extract");
  int bl_iters = 0;
  const std::vector<f64> u0 = window_baseline(R, &bl_iters);
  if (trace) std::printf("    [event] window %d cells (rim %d), baseline pcg %d\n", L.n, R.rim_count, bl_iters);
  // the window before the event: the background job checks the cached baseline there (and
  // solves it if it is no equilibrium), never the event tick
  std::shared_ptr<PreState> pre = cfg_.resolve_windows && !into ? std::make_shared<PreState>(L) : nullptr;
  lap("baseline");
  // A rocket's bubble spans the whole structure it hits if that is small enough: the chunks it
  // may reach are snapshotted now, before the event changes them; the bubble's setup finds and
  // extracts the structure there and replays the event on it (recorded below).
  std::shared_ptr<StructureJob> sj;
  IVec3 bound_c = c;
  i32 bound_r = wr;
  if (e.blast && !e.impact && !into && cfg_.structure_max_cells > 0) {
    bool complete = false;
    std::vector<u64> skeys;
    const std::vector<u64> keys = structure_chunks(c, wr, cfg_.structure_max_chunks, &complete, &skeys);
    if (complete && !skeys.empty()) {
      sj = std::make_shared<StructureJob>();
      sj->snap = grid_.snapshot(keys);
      sj->chunks = skeys;
      for (i32 i = 0; i < L.n; ++i)
        if (!L.anchored[i] && !L.dead[i]) sj->seeds.push_back(R.vox[i]);
      // the structure's bounding sphere: the box of its chunks (the bubble's reach for events,
      // settles and verification)
      IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
      for (const u64 k : skeys) {
        const IVec3 cc = unkey3(k);
        for (int q = 0; q < 3; ++q) {
          lo[q] = std::min(lo[q], cc[q] * kChunk);
          hi[q] = std::max(hi[q], cc[q] * kChunk + kChunk - 1);
        }
      }
      i64 d2 = 0;
      for (int q = 0; q < 3; ++q) {
        bound_c[q] = lo[q] + (hi[q] - lo[q]) / 2;
        const i64 d = hi[q] - lo[q];
        d2 += d * d;
      }
      bound_r = std::max(wr, static_cast<i32>(std::ceil(std::sqrt(static_cast<f64>(d2)) / 2.0)) + 1);
    }
  }
  const size_t m6 = 6 * size_t(L.n);
  // The pre-event internal forces: the window's static load (increment formulation). A blast
  // changes bonds only near its centre, so there they are taken now and elsewhere they equal the
  // post-event forces the bubble setup computes anyway (bit for bit: a cell's force depends on
  // its own bonds only).
  std::vector<f64> fpre(m6, 0.0), fimp(m6, 0.0);
  std::vector<i32> pre_cells;
  const bool local_pre = e.blast;
  if (!local_pre) internal_forces(L, u0.data(), cfg_.corot, fpre.data());
  std::vector<IVec3> seeds;
  std::vector<i32> seed_cells;
  std::vector<std::pair<IVec3, int>> fractured_vox;         // (recorded for a structure or a merge)
  std::vector<std::tuple<IVec3, int, f32>> damaged_vox;
  const bool record = sj || into;
  bool supports_removed = false;
  i32 removed = 0;
  std::vector<IVec3> removed_vox;
  auto kill = [&](i32 cell) {
    const IVec3 p = R.vox[cell];
    removed_vox.push_back(p);
    for (int a = 0; a < 3; ++a)
      for (int s = -1; s <= 1; s += 2) {
        IVec3 q = p;
        q[a] += s;
        if (vox_solid(grid_.get(q))) seeds.push_back(q);
      }
    if (L.support(cell)) supports_removed = true;
    L.remove_cell(cell);
    grid_.set(p, kAir);
    grid_.clear_baseline(p);
    const f32 z[3] = {0, 0, 0};
    grid_.set_offset(p, z);
    ++removed;
  };
  std::vector<i32> contact;
  if (e.impact) {
    // nothing is removed: the landing impulse acts on the cells around the hit voxel
    contact = contact_cells(R, c);
    if (contact.empty()) return;
    const f64 k = 1.0 / (cfg_.dt * static_cast<f64>(contact.size()));
    for (i32 cell : contact)
      for (int q = 0; q < 3; ++q) fimp[6 * size_t(cell) + q] = e.impulse[q] * k;
  } else if (!e.blast) {
    std::vector<std::array<f64, 4>> spheres{{e.pos[0], e.pos[1], e.pos[2], e.radius}};
    spheres.insert(spheres.end(), e.extra.begin(), e.extra.end());
    std::vector<i32> cells;
    for (const auto& sp : spheres) {
      const IVec3 sc = voxel_of({sp[0], sp[1], sp[2]}, h);
      const i32 sr = static_cast<i32>(std::ceil(sp[3] / h));
      const f64 r2 = (sp[3] / h) * (sp[3] / h);
      for (i32 x = sc[0] - sr; x <= sc[0] + sr; ++x)
        for (i32 y = sc[1] - sr; y <= sc[1] + sr; ++y)
          for (i32 z = sc[2] - sr; z <= sc[2] + sr; ++z) {
            const f64 dx = x - sp[0] / h, dy = y - sp[1] / h, dz = z - sp[2] / h;
            if (dx * dx + dy * dy + dz * dz > r2) continue;
            const i32 cell = R.cell({x, y, z});
            if (cell >= 0 && !L.dead[cell]) cells.push_back(cell);
          }
    }
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    for (i32 cell : cells) kill(cell);
  } else {
    BlastParams bp;
    bp.center = e.pos;
    bp.radius = e.radius;
    {  // the cells whose bonds the blast can change (core, fracture and damage radii, + a cell)
      const f64 reach = std::max({bp.radius, bp.radius + bp.fracture_padding * h, bp.radius + bp.damage_radius_pad * h,
                                  bp.radius * bp.damage_radius_scale}) +
                        h;
      for (i32 i = 0; i < L.n; ++i) {
        if (L.dead[i]) continue;
        const auto x = cell_position(L, u0.data(), i);
        const f64 dx = x[0] - bp.center[0], dy = x[1] - bp.center[1], dz = x[2] - bp.center[2];
        if (dx * dx + dy * dy + dz * dz <= reach * reach) pre_cells.push_back(i);
      }
      internal_forces_cells(L, u0.data(), cfg_.corot, pre_cells, fpre.data());
    }
    add_blast_impulse(L, u0.data(), bp, fimp.data());
    const BlastResult br = apply_blast(L, u0.data(), bp);
    supports_removed = br.supports_removed;
    for (i32 cell : br.removed) {
      const IVec3 p = R.vox[cell];
      removed_vox.push_back(p);
      grid_.set(p, kAir);
      grid_.clear_baseline(p);
      const f32 z[3] = {0, 0, 0};
      grid_.set_offset(p, z);
      for (int q = 0; q < 6; ++q) fimp[6 * size_t(cell) + q] = 0.0;
      ++removed;
    }
    for (i64 key : br.fractured) {
      const i32 cell = static_cast<i32>(key / 3);
      const int ax = static_cast<int>(key % 3);
      grid_.break_bond(R.vox[cell], ax);
      if (record) fractured_vox.push_back({R.vox[cell], ax});
    }
    for (i64 key : br.damaged) {
      const i32 cell = static_cast<i32>(key / 3);
      const int ax = static_cast<int>(key % 3);
      grid_.set_damage(R.vox[cell], ax, L.dmg[ax][cell]);
      if (record) damaged_vox.push_back({R.vox[cell], ax, L.dmg[ax][cell]});
    }
    for (i32 cell : br.seeds) seeds.push_back(R.vox[cell]);
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::Impact;
    ev.id = next_id_++;
    ev.pos = e.pos;
    ev.radius = e.radius;
    ev.strength = e.energy;
    events_.push_back(std::move(ev));
  }
  if (!movers_.empty()) disable_movers_at(removed_vox);  // shot-up doors stay as they are
  // world-level detachment
  std::sort(seeds.begin(), seeds.end());
  seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
  const auto islands = world_islands(seeds, supports_removed);
  if (!islands.empty() && !settles_.empty()) {  // pieces reaching a pending settle's window
    for (const auto& isl : islands)
      for (const IVec3& p : isl) apply_settles(false, p, 1);
  }
  i32 island_voxels = 0;
  for (const auto& isl : islands) {
    island_voxels += static_cast<i32>(isl.size());
    for (const IVec3& p : isl) {
      const i32 cell = R.cell(p);
      if (cell >= 0 && !L.dead[cell]) L.remove_cell(cell);
    }
  }
  mirror_islands(islands, nullptr, nullptr, nullptr);
  for (const IVec3& p : seeds) {
    const i32 cell = R.cell(p);
    if (cell >= 0 && !L.dead[cell] && !L.anchored[cell]) seed_cells.push_back(cell);
  }
  // the event as the world took it: replayed on the structure's lattice (sj), or merged into the
  // running bubble of its structure at that bubble's next job
  std::vector<std::pair<IVec3, std::array<f64, 6>>> impulse_vox;
  if (record)
    for (i32 i = 0; i < L.n; ++i) {
      const f64* f = &fimp[6 * size_t(i)];
      if (f[0] != 0.0 || f[1] != 0.0 || f[2] != 0.0 || f[3] != 0.0 || f[4] != 0.0 || f[5] != 0.0)
        impulse_vox.push_back({R.vox[i], {f[0], f[1], f[2], f[3], f[4], f[5]}});
    }
  if (sj) {
    sj->removed = removed_vox;
    for (const auto& isl : islands) sj->removed.insert(sj->removed.end(), isl.begin(), isl.end());
    sj->fractured = std::move(fractured_vox);
    sj->damaged = std::move(damaged_vox);
    sj->impulse = std::move(impulse_vox);
    for (i32 cell : seed_cells) sj->fine.push_back(R.vox[cell]);
  }
  if (into) {
    Merge mg;
    mg.removed = std::move(removed_vox);
    for (const auto& isl : islands) mg.islands.insert(mg.islands.end(), isl.begin(), isl.end());
    mg.fractured = std::move(fractured_vox);
    mg.damaged = std::move(damaged_vox);
    mg.impulse = std::move(impulse_vox);  // (its fine cells: the bubble's rule, Bubble::apply_event)
    into->merges.push_back(std::move(mg));
    ++st_.merged_events;
    lap("merge");
    st_.event_ms += ms_since(t0);
    return;
  }
  lap("mutation");
  st_.event_ms += ms_since(t0);
  (void)bl_iters;
  // impact triage: the equivalent static force J / duration on top of the baseline load, solved
  // in the background (as a settle); a landing that ruptures nothing only records its damage
  // (the load passes, the baseline stays), else a bubble takes it
  if (e.impact) {
    auto st = std::make_unique<Settle>();
    Settle& sl = *st;
    sl.impact = true;
    sl.center = c;
    sl.radius = wr;
    std::unordered_set<u64> sck;
    for (const IVec3& p : R.vox) sck.insert(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
    sl.chunks.assign(sck.begin(), sck.end());
    std::sort(sl.chunks.begin(), sl.chunks.end());
    sl.so.corot = cfg_.corot;
    sl.so.damage = true;
    sl.so.res_tol = 1e-7;
    sl.so.lin_rtol = 1e-3;
    sl.so.max_pcg_total = cfg_.settle_max_pcg;  // (unconverged: resolved dynamically)
    sl.u = u0;
    sl.load.assign(fpre.size(), 0.0);
    const f64 k = 1.0 / (cfg_.impact_duration * static_cast<f64>(contact.size()));
    for (i32 cell : contact)
      for (int q = 0; q < 3; ++q) sl.load[6 * size_t(cell) + q] += e.impulse[q] * k;
    sl.dc = L.dmg;
    sl.spawn.win = std::move(win);
    sl.spawn.c = c;
    sl.spawn.pos = e.pos;
    sl.spawn.radius = e.radius;
    sl.spawn.u0 = u0;
    sl.spawn.fpre = std::move(fpre);
    sl.spawn.fimp = std::move(fimp);
    sl.spawn.force_fine = contact;
    sl.spawn.pre = std::move(pre);
    launch_settle(std::move(st));
    lap("impact");
    return;
  }
  // severity triage: small carves settle statically when nothing nears failure
  i64 load = 0;
  for (const auto& b : active_) load += b->nodes();
  const bool over = load > cfg_.node_budget;
  const i32 small = over ? std::max(cfg_.small_event, cfg_.degraded_small_event) : cfg_.small_event;
  if (!e.blast && !e.impact && removed + island_voxels <= small) {
    if (small != cfg_.small_event && removed + island_voxels > cfg_.small_event) ++st_.degraded_triage;
    auto st = std::make_unique<Settle>();
    Settle& sl = *st;
    sl.center = c;
    sl.radius = wr;
    std::unordered_set<u64> sck;
    for (const IVec3& p : R.vox) sck.insert(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
    sl.chunks.assign(sck.begin(), sck.end());
    std::sort(sl.chunks.begin(), sl.chunks.end());
    sl.so.corot = cfg_.corot;
    sl.so.damage = true;
    sl.so.res_tol = 1e-7;
    sl.so.lin_rtol = 1e-3;
    sl.so.max_pcg_total = cfg_.settle_max_pcg;  // (unconverged: a bubble decides)
    sl.u = u0;
    sl.dc = L.dmg;
    sl.spawn.win = std::move(win);
    sl.spawn.c = c;
    sl.spawn.pos = e.pos;
    sl.spawn.radius = e.radius;
    sl.spawn.u0 = u0;
    sl.spawn.fpre = std::move(fpre);
    sl.spawn.fimp = std::move(fimp);
    sl.spawn.force_fine = seed_cells;
    sl.spawn.pre = std::move(pre);
    // increment formulation: the load is the pre-event internal force at the baseline
    // (equal to gravity inside the structure; consistent with the pinned window rim)
    launch_settle(std::move(st));
    lap("triage");
    return;
  }
  // dynamic bubble
  Spawn sp;
  sp.win = std::move(win);
  sp.c = c;
  sp.pos = e.pos;
  sp.radius = e.radius;
  sp.u0 = u0;
  sp.fpre = std::move(fpre);
  sp.fimp = std::move(fimp);
  sp.pre_cells = std::move(pre_cells);
  sp.local_pre = local_pre;
  sp.force_fine = e.impact ? contact : seed_cells;
  sp.pre = std::move(pre);
  sp.structure = std::move(sj);
  sp.bound_c = bound_c;
  sp.bound_r = bound_r;
  spawn_bubble(std::move(sp));
  lap("spawn");
}

void Engine::launch_settle(std::unique_ptr<Settle> st) {
  Settle* sp = st.get();
  sp->launched = st_.ticks;
  sp->ready_tick = st_.ticks + 1;
  sp->job = [sp, corot = cfg_.corot, max_pcg = cfg_.settle_max_pcg]() {
    {
      WorkScope ws(&sp->wc);
      Spawn& spn = sp->spawn;
      if (spn.pre && consistent_baseline(*spn.win, *spn.pre, spn.u0, spn.fpre, corot, max_pcg)) {
        sp->u = spn.u0;  // (the settle starts from the consistent baseline)
        spn.local_pre = false;
      }
      spn.pre.reset();
      std::vector<f64> load = spn.fpre;
      if (!sp->load.empty())
        for (size_t k = 0; k < load.size(); ++k) load[k] += sp->load[k];
      sp->es = solve_equilibrium(spn.win->L, sp->u, load, sp->dc, sp->so, nullptr);
    }
    sp->finished.store(true, std::memory_order_release);
  };
#ifndef SVX_NO_THREADS
  if (cfg_.async_triage)
    sp->worker = std::thread([sp] {
      SerialScope serial;  // never shares the pool with the simulation thread
      sp->job();
    });
#endif
  settles_.push_back(std::move(st));
  if (!cfg_.async_triage) {  // within the event tick
    std::unique_ptr<Settle> now = std::move(settles_.back());
    settles_.pop_back();
    apply_settle(*now);
  }
}

// As job_due: due once finished and max(1, ceil(work / tick_work)) ticks after its launch.
bool Engine::settle_due(Settle& sl) {
  const i64 now = st_.ticks;
  if (sl.ready_tick > now) return false;
  if (cfg_.tick_work <= 0) return true;
  auto due_at = [&](i64 w) { return sl.launched + std::max<i64>(1, span_of(w)); };
#ifndef SVX_NO_THREADS
  if (sl.worker.joinable()) {
    while (!sl.finished.load(std::memory_order_acquire)) {
      if (due_at(sl.wc.done.load(std::memory_order_relaxed)) > now) return false;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return due_at(sl.wc.done.load(std::memory_order_relaxed)) <= now;
  }
#endif
  sl.finish();  // no thread: the solve runs now; its result waits
  return due_at(sl.wc.done.load(std::memory_order_relaxed)) <= now;
}

void Engine::apply_settles(bool due_only, const IVec3& at, f64 reach) {
  // the latest settle to apply now: due (a due run from the oldest: they apply in creation
  // order), or its window within `reach` of `at`
  i64 last = -1;
  bool prefix = due_only;
  for (size_t k = 0; k < settles_.size(); ++k) {
    Settle& sl = *settles_[k];
    bool go = prefix && settle_due(sl);
    prefix = go;
    if (!go && reach >= 0.0) {
      const f64 dx = sl.center[0] - at[0], dy = sl.center[1] - at[1], dz = sl.center[2] - at[2];
      const f64 lim = sl.radius + reach;
      go = dx * dx + dy * dy + dz * dz <= lim * lim;
    }
    if (go) last = static_cast<i64>(k);
  }
  if (last < 0) return;
  // it and every earlier one, in creation order; taken out of the list first: applying one may
  // spawn a bubble whose effects apply settles again (they see only the rest)
  std::vector<std::unique_ptr<Settle>> run;
  run.reserve(static_cast<size_t>(last) + 1);
  for (i64 k = 0; k <= last; ++k) run.push_back(std::move(settles_[static_cast<size_t>(k)]));
  settles_.erase(settles_.begin(), settles_.begin() + (last + 1));
  for (auto& sl : run) apply_settle(*sl);
}

void Engine::apply_settle(Settle& sl) {
  sl.finish();
  Region& R = *sl.spawn.win;
  Lattice& L = R.L;
  const EquilibriumStats& es = sl.es;
  if (std::getenv("SVX_ENGINE_TRACE"))
    std::printf("    [settle] %d cells: %d its, pcg %d, mg builds %d, max damage %.3f%s\n", L.n, es.iters, es.pcg_iters,
                es.mg_builds, es.max_damage, es.converged ? "" : " (not converged)");
  bool finite = true;
  for (size_t k = 0; k < sl.u.size() && finite; ++k) finite = std::isfinite(sl.u[k]);
  if (sl.impact && es.converged && es.max_damage < 0.999 && finite) {  // the landing passes
    for (int a = 0; a < 3; ++a)
      for (i32 i = 0; i < L.n; ++i)
        if (L.nbr[a][i] >= 0 && L.dmg[a][i] > sl.dc[a][i]) grid_.set_damage(R.vox[i], a, L.dmg[a][i]);
    ++st_.static_settles;
    return;
  }
  if (!sl.impact && es.converged && es.max_damage < 0.999 && finite) {
    const std::vector<f64>& u = sl.u;
    const std::vector<f64>& u0 = sl.spawn.u0;
    store_baseline(R, u);
    for (int a = 0; a < 3; ++a)
      for (i32 i = 0; i < L.n; ++i)
        if (L.nbr[a][i] >= 0 && L.dmg[a][i] > sl.dc[a][i]) grid_.set_damage(R.vox[i], a, L.dmg[a][i]);
    for (i32 i = 0; i < L.n; ++i) {
      if (L.anchored[i] || L.dead[i]) continue;
      f32 off[3] = {0, 0, 0};
      grid_.offset(R.vox[i], off);
      f64 d2 = 0.0;
      for (int q = 0; q < 3; ++q) {
        const f64 dq = par_.amplification * (u[6 * size_t(i) + q] - u0[6 * size_t(i) + q]);
        off[q] += static_cast<f32>(dq);
        d2 += dq * dq;
      }
      if (d2 > 1e-8) grid_.set_offset(R.vox[i], off);
    }
    ++st_.static_settles;
    return;
  }
  // near failure: a bubble from the pre-event baseline (spawned at the next safe point)
  for (int a = 0; a < 3; ++a) L.dmg[a] = sl.dc[a];
  spawns_.push_back(std::make_unique<Spawn>(std::move(sl.spawn)));
}

void Engine::spawn_pending() {
  while (!spawns_.empty()) {
    std::unique_ptr<Spawn> sp = std::move(spawns_.front());
    spawns_.erase(spawns_.begin());
    spawn_bubble(std::move(*sp));
  }
}

void Engine::spawn_bubble(Spawn&& spn) {
  const f64 h = grid_.h;
  Lattice& L = spn.win->L;
  i64 load = 0;
  for (const auto& b : active_) load += b->nodes();
  const bool over = load > cfg_.node_budget;
  const std::vector<f64>& u0 = spn.u0;
  const std::vector<f64>& fpre = spn.fpre;
  const bool local_pre = spn.local_pre;
  const std::vector<i32>& pre_cells = spn.pre_cells;
  std::vector<f64>& fimp = spn.fimp;
  const IVec3 c = spn.c;
  const f64 radius = spn.radius;
  const std::array<f64, 3> pos = spn.pos;
  auto act = std::make_unique<Active>();
  act->id = next_id_++;
  act->center = c;
  act->radius = spn.win->radius;
  act->u_start = u0;
  if (!local_pre) act->load = fpre;  // (a blast's load is assembled by the setup)
  act->region = std::move(spn.win);
  if (spn.structure) {  // (its reach: the structure's bounding sphere, for events, settles, verification)
    act->structure = spn.structure;
    act->structure_bubble = true;
    act->center = spn.bound_c;
    act->radius = spn.bound_r;
    act->region->center = spn.bound_c;
    act->region->radius = spn.bound_r;
  }
  BubbleOptions bo;
  bo.comp.R0 = std::max(cfg_.R0, 2.0 * radius / h + 2.0);
  bo.comp.max_level = cfg_.max_level;
  bo.comp.force_fine = spn.force_fine;
  if (2 * load > cfg_.node_budget) {  // degrade: coarser far field, then a smaller fine region
    bo.comp.grading = std::max(bo.comp.grading, cfg_.degraded_grading);
    if (over) bo.comp.R0 = std::max(cfg_.degraded_R0, 2.0 * radius / h + 2.0);
    ++st_.degraded_spawns;
  }
  bo.dt = cfg_.dt;
  bo.corot = cfg_.corot;
  bo.rayleigh_alpha = par_.damping;
  bo.nominate_phi = cfg_.nominate_phi;
  bo.marginal_eps = cfg_.marginal_eps;
  bo.demand_inflation = cfg_.demand_inflation;
  bo.max_fine_cells = cfg_.max_fine_cells;
  // chunks the window overlaps (animated while the bubble runs)
  std::unordered_set<u64> ck;
  for (const IVec3& p : act->region->vox) ck.insert(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
  if (spn.structure) ck.insert(spn.structure->chunks.begin(), spn.structure->chunks.end());
  act->chunks.assign(ck.begin(), ck.end());
  std::sort(act->chunks.begin(), act->chunks.end());
  i32 live = 0;
  for (i32 i = 0; i < L.n; ++i) live += (L.dead[i] || L.anchored[i]) ? 0 : 1;
  act->est_nodes = live / 3;
  act->dmg0 = L.dmg;  // (the mutation's damage is in the grid already)
  // The setup reads and writes only the bubble's own window: the released forces (the pre-event
  // internal forces minus those after the mutation), the composite, the first step.
  // Four stages (one per tick without threads; start_setup): 0 the released forces (and a
  // blast's load), 1 the composite and fine sub-lattice, 2 the preconditioner, 3 the first step.
  Active* ap = act.get();
  const bool corot = cfg_.corot;
  const bool trace = std::getenv("SVX_SETUP_TRACE") != nullptr;
  struct SetupState {
    std::vector<f64> u0, fpre, fimp, r;
    std::vector<i32> pre_cells;
    bool local_pre = false;
    std::shared_ptr<PreState> pre;
    BubbleOptions bo;
    f64 ms[5] = {0, 0, 0, 0, 0};
  };
  auto ss = std::make_shared<SetupState>();
  ss->u0 = u0;
  ss->fpre = fpre;
  ss->fimp = std::move(fimp);
  ss->pre_cells = pre_cells;
  ss->local_pre = local_pre;
  ss->pre = std::move(spn.pre);
  ss->bo = bo;
  std::vector<std::function<void()>> stages;
  // the region a setup stage works on: the structure once extracted, else the window
  auto reg = [](Active* a) -> Region& { return a->next_region ? *a->next_region : *a->region; };
  if (spn.structure) {
    // The structure: found in the snapshot from the window's cells; if it fits, extracted whole
    // (its supports included: no pinned rim) with its cached baseline - checked in the window
    // first, as a window bubble's is, and solved there if the law reads kinks from it - the event
    // replayed on it, and it becomes the bubble's region (at make_ready). Otherwise the window
    // stays.
    stages.push_back([ap, ss, sj = spn.structure, lo = lattice_options(), kscale = 1.0 / std::max(1e-9, par_.compliance),
                      corot, trace, max_cells = cfg_.structure_max_cells, max_pcg = cfg_.settle_max_pcg,
                      max_fine = cfg_.structure_max_fine_cells, newton = cfg_.structure_newton_iters,
                      bound_c = spn.bound_c, bound_r = spn.bound_r]() {
      const f64 t0 = trace ? cpu_now_ms() : 0.0;
      bool truncated = false;
      const std::vector<IVec3> set = structures_of(sj->snap, sj->seeds, max_cells, &truncated);
      add_work(static_cast<i64>(set.size()) * kWorkExtract);
      if (truncated || set.empty()) return;
      Region& W = *ap->region;
      bool solved = false;
      if (ss->pre) {  // (the window's baseline as a window bubble would take it)
        std::vector<f64> fw;
        solved = consistent_baseline(W, *ss->pre, ss->u0, fw, corot, max_pcg);
        ss->pre.reset();
      }
      auto big = std::make_unique<Region>(extract_set(sj->snap, set, lo));
      Region& S = *big;
      Lattice& B = S.L;
      B.kscale = kscale;
      std::vector<std::vector<IVec3>> loose;
      std::vector<f64> u0 = baseline_from(sj->snap, S, corot, &loose, nullptr, false, false, max_pcg);
      if (solved)  // (the window's solution, its rim held at the cache: continuous with the rest)
        for (i32 i = 0; i < W.L.n; ++i) {
          if (W.rim[i] || W.L.anchored[i]) continue;
          const i32 c = S.cell(W.vox[i]);
          if (c < 0 || B.anchored[c] || B.dead[c]) continue;
          for (int q = 0; q < 6; ++q) u0[6 * size_t(c) + q] = ss->u0[6 * size_t(i) + q];
        }
      std::vector<f64> fpre(6 * size_t(B.n), 0.0);  // (the structure before the event)
      internal_forces(B, u0.data(), corot, fpre.data());
      for (const IVec3& v : sj->removed) {  // the event, as the world took it
        const i32 c = S.cell(v);
        if (c >= 0 && !B.dead[c]) B.remove_cell(c);
      }
      for (const auto& [v, a] : sj->fractured) {
        const i32 c = S.cell(v);
        if (c >= 0 && B.nbr[a][c] >= 0) B.break_bond(c, a);
      }
      for (const auto& [v, a, d] : sj->damaged) {
        const i32 c = S.cell(v);
        if (c >= 0 && B.nbr[a][c] >= 0) B.dmg[a][c] = d;
      }
      std::vector<f64> fimp(6 * size_t(B.n), 0.0);
      for (const auto& [v, f] : sj->impulse) {
        const i32 c = S.cell(v);
        if (c >= 0 && !B.dead[c])
          for (int q = 0; q < 6; ++q) fimp[6 * size_t(c) + q] = f[q];
      }
      std::vector<i32> fine;
      for (const IVec3& v : sj->fine) {
        const i32 c = S.cell(v);
        if (c >= 0 && !B.dead[c] && !B.anchored[c]) fine.push_back(c);
      }
      S.center = bound_c;
      S.radius = bound_r;
      ss->u0 = std::move(u0);
      ss->fpre = std::move(fpre);
      ss->fimp = std::move(fimp);
      ss->pre_cells.clear();
      ss->local_pre = false;
      ss->bo.comp.force_fine = std::move(fine);
      ss->bo.max_fine_cells = max_fine;
      ss->bo.newton_iters = newton;
      ap->u_start = ss->u0;
      ap->load = ss->fpre;
      ap->dmg0 = B.dmg;  // (the event's damage is in the grid already)
      ap->spans_structure = true;
      ap->next_region = std::move(big);
      if (trace) ss->ms[4] = cpu_now_ms() - t0;
    });
  }
  stages.push_back([ap, ss, reg, corot, trace, max_pcg = cfg_.settle_max_pcg]() {
    const f64 t0 = trace ? cpu_now_ms() : 0.0;
    Lattice& W = reg(ap).L;
    bool local_pre = ss->local_pre;
    if (ss->pre) {  // a cache that is no equilibrium in the window: solved (pre-event) first
      std::vector<f64> fpre_all;
      if (consistent_baseline(reg(ap), *ss->pre, ss->u0, fpre_all, corot, max_pcg)) {
        ss->fpre = std::move(fpre_all);
        local_pre = false;
        ap->load = ss->fpre;
        ap->u_start = ss->u0;  // (offsets from the consistent state: no pop)
      }
      ss->pre.reset();
    }
    const size_t m = 6 * size_t(W.n);
    std::vector<f64> fpost(m);
    ss->r.assign(m, 0.0);
    internal_forces(W, ss->u0.data(), corot, fpost.data());
    if (local_pre) {  // the load: the post-event forces, the pre-event ones near the blast
      std::vector<f64> load = fpost;
      for (i32 c : ss->pre_cells)
        for (int q = 0; q < 6; ++q) load[6 * size_t(c) + q] = ss->fpre[6 * size_t(c) + q];
      ap->load = std::move(load);
    }
    const std::vector<f64>& pre = local_pre ? ap->load : ss->fpre;
    for (i32 i = 0; i < W.n; ++i) {
      if (W.dead[i] || W.anchored[i]) continue;
      for (int q = 0; q < 6; ++q) ss->r[6 * size_t(i) + q] = pre[6 * size_t(i) + q] - fpost[6 * size_t(i) + q];
    }
    std::vector<f64>().swap(ss->fpre);
    if (trace) ss->ms[0] = cpu_now_ms() - t0;
  });
  stages.push_back([ap, ss, reg, center = pos, trace]() {
    const f64 t0 = trace ? cpu_now_ms() : 0.0;
    Lattice& W = reg(ap).L;
    const std::array<f64, 3> centers[1] = {center};
    ap->bubble.init(W, ss->u0, ss->r, centers, ss->bo);
    for (i32 i = 0; i < W.n; ++i) {
      const f64* fi = &ss->fimp[6 * size_t(i)];
      if (fi[0] != 0.0 || fi[1] != 0.0 || fi[2] != 0.0 || fi[3] != 0.0 || fi[4] != 0.0 || fi[5] != 0.0)
        ap->bubble.add_force(i, fi);
    }
    std::vector<f64>().swap(ss->r);
    std::vector<f64>().swap(ss->fimp);
    std::vector<f64>().swap(ss->u0);
    if (trace) ss->ms[1] = cpu_now_ms() - t0;
  });
  stages.push_back([ap, ss, trace]() {
    const f64 t0 = trace ? cpu_now_ms() : 0.0;
    ap->bubble.prepare();
    if (trace) ss->ms[2] = cpu_now_ms() - t0;
  });
  stages.push_back([ap, ss, reg, trace]() {
    const f64 t0 = trace ? cpu_now_ms() : 0.0;
    ap->result = ap->bubble.step();
    if (!trace) return;
    ss->ms[3] = cpu_now_ms() - t0;
    const Lattice& W = reg(ap).L;
    const Bubble::InitProfile& ip = ap->bubble.init_profile();
    const StepStats& s = ap->result;
    std::printf("    [setup] bubble %lld: %d cells%s, %d nodes (%d fine): %.1f cpu ms = structure %.1f + residual %.1f + "
                "init %.1f (composite %.1f, simd %.1f, sublattice %.1f, forces %.1f, restrict %.1f) + preconditioner %.1f "
                "+ first step %.1f (solve %.1f, law %.1f, topo %.1f; pcg %d, newton %d)\n",
                static_cast<long long>(ap->id), W.n, ap->spans_structure ? " (the whole structure)" : "",
                ap->bubble.nodes(), ap->bubble.fine_cells(), ss->ms[4] + ss->ms[0] + ss->ms[1] + ss->ms[2] + ss->ms[3],
                ss->ms[4], ss->ms[0], ss->ms[1], ip.composite, ip.simd, ip.sublattice,
                ip.forces, ip.restrict_residual, ss->ms[2], ss->ms[3], s.ms_solve, s.ms_law, s.ms_topo, s.pcg, s.newton);
    const auto& cm = ap->bubble.composite().ms;
    std::printf("      [composite] levels %.1f, aggregates %.1f, members %.1f, fibres %.1f, pattern %.1f, blocks %.1f, "
                "torsion %.1f, mass %.1f ms\n",
                cm[0], cm[1], cm[2], cm[3], cm[4], cm[5], cm[6], cm[7]);
  });
  start_setup(*act, std::move(stages), act->est_nodes);
  EngineEvent ev;
  ev.kind = EngineEvent::Kind::Bubble;
  ev.id = act->id;
  ev.pos = pos;
  ev.radius = h * bo.comp.R0;
  ev.level = cfg_.max_level;
  events_.push_back(std::move(ev));
  ++st_.bubbles_spawned;
  active_.push_back(std::move(act));
  if (cfg_.tick_work > 0) {  // the oldest stepping bubbles beyond max_bubbles finalize in the background
    i32 stepping = 0;
    for (const auto& ap : active_) stepping += (ap->takeover || ap->finalizing) ? 0 : 1;
    for (auto& ap : active_) {
      if (stepping <= cfg_.max_bubbles) break;
      if (ap->takeover || ap->finalizing) continue;
      ap->takeover = true;
      --stepping;
    }
  }
  while (cfg_.tick_work <= 0 && static_cast<int>(active_.size()) > cfg_.max_bubbles) {
    if (active_.front()->pending()) make_ready(*active_.front());
    finalize(*active_.front());
    active_.erase(active_.begin());
  }
  (void)0;
}

void Engine::step_bubbles() {
  // 1. commit the jobs due this tick (a setup's first step, a step, a finalizing bubble's settle
  //    projection), in list order; a bubble that fell asleep or was taken over by an event is
  //    finalized (its settle projection first runs as its next background job)
  const auto tc = Clock::now();
  const bool async_finalize = cfg_.tick_work > 0;
  static const bool span_trace = std::getenv("SVX_SPAN_TRACE") != nullptr;
  for (size_t k = 0; k < active_.size();) {
    Active& a = *active_[k];
    const auto tw = Clock::now();
    if (a.pending() && !job_due(a)) {  // (its job is not due)
      ++k;
      continue;
    }
    if (span_trace && a.pending()) {
      const f64 wait = ms_since(tw);
      const f64 job = a.kind == 's' ? a.result.ms_total : a.kind == 'f' ? a.settle_ms : 0.0;
      std::printf("      [span] bubble %lld %c: nodes %d, work %lld, span %lld, job %.1f ms, wait %.1f ms%s\n",
                  static_cast<long long>(a.id), a.kind, a.nodes(), static_cast<long long>(a.wc.done.load()),
                  static_cast<long long>(st_.ticks - a.launched), job, wait, wait > 5.0 ? " LATE" : "");
    }
    std::unique_ptr<Active> cont;
    if (a.finalizing) {
      cont = complete_finalize(a);
    } else {
      if (a.pending()) make_ready(a);
      if ((!a.bubble.asleep() || !a.merges.empty()) && !a.takeover) {  // (a merged event wakes it)
        ++k;
        continue;
      }
      if (async_finalize) {
        start_finalize(a);
        ++k;
        continue;
      }
      cont = finalize(a);
    }
    if (cont) active_[k++] = std::move(cont);
    else active_.erase(active_.begin() + static_cast<long>(k));
  }
  prof_commit_ms_ += ms_since(tc);
  // 2. this tick's steps, launched in the background and committed after their span (one tick,
  //    more for heavy steps: the work budget): budget (steps per tick, list order) and, over the
  //    node budget, half rate for bubbles far from the viewer (slower simulated time; the nearest
  //    two always step)
  i64 load = 0;
  for (const auto& ap : active_) {
    load += ap->nodes();
    ap->half_rate = false;
  }
  if (load > cfg_.node_budget && active_.size() > 2) {
    std::vector<std::pair<f64, size_t>> by_dist;
    for (size_t k = 0; k < active_.size(); ++k) {
      f64 d2 = 0.0;
      for (int q = 0; q < 3; ++q) {
        const f64 d = grid_.h * active_[k]->center[q] - viewer_[q];
        d2 += d * d;
      }
      by_dist.emplace_back(d2, k);
    }
    std::sort(by_dist.begin(), by_dist.end());
    for (size_t r = 2; r < by_dist.size(); ++r) active_[by_dist[r].second]->half_rate = true;
  }
  int budget = cfg_.steps_per_tick;
  for (const auto& ap : active_) {
    if (budget <= 0) break;
    if (ap->pending() || ap->takeover) continue;  // (its setup, step or settle still running)
    if (ap->half_rate && (st_.ticks & 1)) {
      ++st_.skipped_steps;
      continue;
    }
    Active* a = ap.get();
    for (const auto& [hit, impulse] : a->landings) {  // debris landed on it: this step's impulse
      const std::vector<i32> cells = contact_cells(*a->region, hit);
      const f64 k = cells.empty() ? 0.0 : 1.0 / (cfg_.dt * static_cast<f64>(cells.size()));
      for (i32 cell : cells) {
        const f64 f[6] = {impulse[0] * k, impulse[1] * k, impulse[2] * k, 0.0, 0.0, 0.0};
        a->bubble.add_force(cell, f);
      }
    }
    a->landings.clear();
    // bubbles are independent (each reads only its own lattice): each steps on its own thread,
    // serial inside, or with a private team when large (its work shared: counted half); results
    // are committed in list order
    const int team = a->nodes() >= cfg_.team_nodes ? 3 : 1;
    // events merged into it since its last job: its lattice takes them before the step
    Bubble::EventMutation mev = take_merges(*a);
    // its step: k ticks, from its last step's span (hysteresis: a factor of two either way)
    if (cfg_.max_dt_multiplier > 1 && cfg_.tick_work > 0 && a->last_work > 0) {
      const i64 span = span_of(a->last_work);
      while (a->dt_mult < cfg_.max_dt_multiplier && span >= 2 * a->dt_mult) a->dt_mult *= 2;
      while (a->dt_mult > 1 && 2 * span <= a->dt_mult) a->dt_mult /= 2;
    }
    if (a->dt_mult > 1) ++st_.long_steps;
    launch_job(*a, [a, team, mev = std::move(mev), dt = cfg_.dt * a->dt_mult]() {
      auto run = [&] {
        a->bubble.set_time_step(dt);
        if (!mev.empty()) a->bubble.apply_event(mev);
        a->result = a->bubble.step();
      };
      if (team > 1) {
        ThreadTeam t(team);
        TeamScope scope(t);
        WorkScope ws(&a->wc, 2);
        run();
      } else {
        run();
      }
    }, 1, 's');
    --budget;
  }
  if (cfg_.pipeline_steps) return;
  // not pipelined: this tick's steps are committed now (list order)
  for (size_t k = 0; k < active_.size();) {
    Active& a = *active_[k];
    if (!a.pending() || a.ready_tick != st_.ticks + 1 || !a.shown || a.finalizing) {
      ++k;
      continue;
    }
    make_ready(a);
    if (a.bubble.asleep() && a.merges.empty()) {
      std::unique_ptr<Active> cont = finalize(a);
      if (cont) {
        active_[k] = std::move(cont);
        ++k;
      } else {
        active_.erase(active_.begin() + static_cast<long>(k));
      }
      continue;
    }
    ++k;
  }
}

i64 Engine::span_of(i64 work) const {
  if (cfg_.tick_work <= 0) return 1;
  return std::clamp<i64>((work + cfg_.tick_work - 1) / cfg_.tick_work, 1, std::max(1, cfg_.max_span));
}

void Engine::launch_job(Active& a, std::function<void()> fn, i64 min_ticks, char kind) {
  a.wc.done.store(0, std::memory_order_relaxed);
  a.finished.store(false, std::memory_order_relaxed);
  a.launched = st_.ticks;
  a.min_ticks = std::max<i64>(1, min_ticks);
  a.ready_tick = st_.ticks + a.min_ticks;
  a.kind = kind;
  Active* ap = &a;
  a.job = [ap, fn = std::move(fn)]() {
    {
      WorkScope ws(&ap->wc);
      fn();
    }
    ap->finished.store(true, std::memory_order_release);
  };
#ifndef SVX_NO_THREADS
  a.worker = std::thread([ap] {
    SerialScope serial;  // never shares the pool with the simulation thread (a team scope inside overrides it)
    ap->job();
  });
#endif
}

// A pending job is due once it has finished and max(min_ticks, ceil(work / tick_work)) ticks
// have passed since its launch, the work counted by the job itself (base/work.hpp): the commit
// tick is the same on every machine. The simulation thread waits only while the job is behind
// that schedule (a machine slower than tick_work assumes). Deterministic (plan §B9).
bool Engine::job_due(Active& a) {
  const i64 now = st_.ticks;
  if (a.ready_tick > now) return false;  // (its minimum latency)
  if (cfg_.tick_work <= 0 || a.min_ticks <= 0) return true;  // (at ready_tick, however long it takes)
  auto due_at = [&](i64 w) { return a.launched + std::max<i64>(a.min_ticks, span_of(w)); };
#ifndef SVX_NO_THREADS
  if (a.worker.joinable()) {
    while (!a.finished.load(std::memory_order_acquire)) {
      if (due_at(a.wc.done.load(std::memory_order_relaxed)) > now) return false;  // (whatever it still does)
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return due_at(a.wc.done.load(std::memory_order_relaxed)) <= now;
  }
#endif
  a.finish_job();  // no thread: the job (or its remaining setup stages) runs now; its result waits
  return due_at(a.wc.done.load(std::memory_order_relaxed)) <= now;
}

void Engine::start_setup(Active& b, std::vector<std::function<void()>> stages, i32 est_nodes) {
  b.est_nodes = est_nodes;
  const i64 latency = std::max(0, cfg_.spawn_latency_ticks);
#ifndef SVX_NO_THREADS
  if (latency > 0) {
    launch_job(b, [st = std::move(stages)]() {
      for (const auto& f : st) f();
    }, latency, 'u');
    return;
  }
#endif
  // one stage per tick from the next tick (run_setup_stages), the rest when due (0: this tick)
  b.wc.done.store(0, std::memory_order_relaxed);
  b.finished.store(false, std::memory_order_relaxed);
  b.launched = st_.ticks;
  b.min_ticks = latency;
  b.ready_tick = st_.ticks + latency;
  b.kind = 'u';
  b.stages = std::move(stages);
  b.next_stage = 0;
}

void Engine::run_setup_stages() {
  for (const auto& a : active_)
    if (a->pending() && !a->stages.empty() && a->ready_tick > st_.ticks) a->run_stages(1);
}

void Engine::make_ready(Active& a) {
  a.finish_job();
  a.ready_tick = -1;
  if (a.kind == 's') a.last_work = a.wc.done.load(std::memory_order_relaxed);
  if (a.next_region) {  // (the setup extracted the structure: its region from now on)
    a.region = std::move(a.next_region);
    ++st_.structure_bubbles;
  }
  a.structure.reset();  // (its snapshot is dropped here, on the simulation thread)
  apply_settles(false, a.center, a.radius + 1);  // (earlier writes to its cells go first)
  commit_step(a, a.result);
  // the snapshot meshing, fields and stats read while the next job runs
  a.bubble.total_displacement(a.u_now);
  const Lattice& L = a.region->L;
  a.live_now.resize(size_t(L.n));
  for (i32 i = 0; i < L.n; ++i) a.live_now[size_t(i)] = (L.dead[i] || L.anchored[i]) ? 0 : 1;
  a.nodes_now = a.bubble.nodes();
  ++a.snap;
  if (par_.debug_view == 2) {
    const Composite& C = a.bubble.composite();
    a.level_now.assign(size_t(L.n), 0);
    for (i32 i = 0; i < L.n && i < static_cast<i32>(C.node_of.size()); ++i) {
      const i32 node = C.node_of[i];
      a.level_now[size_t(i)] = static_cast<u8>(node < 0 ? 0 : 1 + C.level[node]);
    }
  }
  a.shown = true;
}

void Engine::commit_step(Active& a, const StepStats& s) {
  if (std::getenv("SVX_STEP_TRACE"))
    std::printf("    [step] bubble %lld: %.2f ms (solve %.2f pcg %d newton %d, mg %.2f%s, law %.2f, topo %.2f)%s\n",
                static_cast<long long>(a.id), s.ms_total, s.ms_solve, s.pcg, s.newton, s.ms_mg, s.rebuilt ? " rebuilt" : "",
                s.ms_law, s.ms_topo, s.be ? " BE" : "");
  ++st_.bubble_steps;
  st_.step_ms += s.ms_total;
  st_.step_solve_ms += s.ms_solve;
  st_.step_mg_ms += s.ms_mg;
  st_.step_law_ms += s.ms_law;
  st_.step_topo_ms += s.ms_topo;
  st_.step_pcg += s.pcg;
  st_.step_newton += s.newton;
  st_.step_rebuilds += s.rebuilt ? 1 : 0;
  // mirror cracks (contacts: still connected) and breaks into the grid
  for (const auto& [cell, axis] : a.bubble.take_cracked()) {
    const IVec3 p = a.region->vox[cell];
    grid_.crack_bond(p, axis);
    grid_.set_damage(p, axis, 1.0f);
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::Crack;
    ev.id = next_id_++;
    for (int d = 0; d < 3; ++d) ev.pos[d] = grid_.h * (p[d] + (d == axis ? 0.5 : 0.0));
    ev.normal = {0, 0, 0};
    ev.normal[axis] = 1.0;
    ev.strength = 0.6;
    events_.push_back(std::move(ev));
    ++st_.ruptures;
    ++st_.cracks;
  }
  std::vector<IVec3> seeds;
  for (const auto& [cell, axis] : a.bubble.take_broken()) {
    const IVec3 p = a.region->vox[cell];
    grid_.break_bond(p, axis);
    IVec3 q = p;
    q[axis] += 1;
    seeds.push_back(p);
    seeds.push_back(q);
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::Crack;
    ev.id = next_id_++;
    for (int d = 0; d < 3; ++d) ev.pos[d] = grid_.h * (p[d] + (d == axis ? 0.5 : 0.0));
    ev.normal = {0, 0, 0};
    ev.normal[axis] = 1.0;
    ev.strength = 1.0;
    events_.push_back(std::move(ev));
    ++st_.ruptures;
  }
  // pieces the bubble detached inside its window
  for (const DetachedIsland& isl : a.bubble.take_islands()) {
    std::vector<IVec3> vox;
    for (i32 cell : isl.cells)  // (voxels an event merged meanwhile removed are gone already)
      if (vox_solid(grid_.get(a.region->vox[cell]))) vox.push_back(a.region->vox[cell]);
    if (vox.empty()) continue;
    emit_detached(vox, isl.v, isl.w);
    for (const IVec3& p : vox) {
      grid_.set(p, kAir);
      grid_.clear_baseline(p);
      const f32 z[3] = {0, 0, 0};
      grid_.set_offset(p, z);
    }
    st_.detached_voxels += static_cast<i64>(vox.size());
  }
  // pieces connected to the rest of the world only through the window rim
  if (!seeds.empty()) {
    const auto wi = world_islands(seeds, false);
    if (!wi.empty()) mirror_islands(wi, &a, nullptr, nullptr);
  }
}

std::unique_ptr<Engine::Active> Engine::finalize(Active& a) {
  if (!a.finalizing) {  // the settle projection here and now
    if (a.pending()) make_ready(a);
    a.finalizing = true;
    a.job = [ap = &a, pcg = cfg_.settle_max_pcg, mev = take_merges(a)]() {
      const auto t0 = Clock::now();
      if (!mev.empty()) ap->bubble.apply_event(mev);  // (events merged since its last step)
      ap->settled = ap->bubble.settle(12, pcg);
      ap->bubble.total_displacement(ap->settled_u);
      ap->settle_ms = ms_since(t0);
    };
  }
  return complete_finalize(a);
}

void Engine::start_finalize(Active& a) {
  // settle projection on the bubble's composite (bounded): the static state of the window with
  // the law evaluated on the fine bonds (the fine level decides), in the background
  a.finalizing = true;
  Active* ap = &a;
  launch_job(a, [ap, pcg = cfg_.settle_max_pcg, mev = take_merges(a)]() {
    const auto t0 = Clock::now();
    if (!mev.empty()) ap->bubble.apply_event(mev);  // (events merged since its last step)
    ap->settled = ap->bubble.settle(12, pcg);
    ap->bubble.total_displacement(ap->settled_u);
    ap->settle_ms = ms_since(t0);
  }, 1, 'f');
}

std::unique_ptr<Engine::Active> Engine::complete_finalize(Active& a) {
  a.finish_job();  // (the settle projection)
  a.ready_tick = -1;
  apply_settles(false, a.center, a.radius + 1);  // (earlier writes to its cells go first)
  Lattice& L = *&a.region->L;
  std::vector<f64> load = a.load;
  for (i32 i = 0; i < L.n; ++i)
    if (L.dead[i] || L.anchored[i])
      for (int q = 0; q < 6; ++q) load[6 * size_t(i) + q] = 0.0;
  const Bubble::SettleResult& es = a.settled;
  std::vector<f64>& u = a.settled_u;
  bool finite = !a.bubble.failed();
  for (size_t k = 0; k < u.size() && finite; ++k) finite = std::isfinite(u[k]);
  if (!finite) {
    // a numerical breakdown: its state is no equilibrium to keep; the window keeps the
    // baseline and damage it had (the cracks and breaks already committed stay)
    ++st_.bubble_failures;
    if (std::getenv("SVX_ENGINE_TRACE"))
      std::printf("    [finalize] bubble %lld: numerical breakdown, dropped\n", static_cast<long long>(a.id));
    for (u64 k : a.chunks) grid_.mark_dirty(unkey3(k));
    return nullptr;
  }
  const bool failing = es.max_damage >= 0.999;
  if (std::getenv("SVX_ENGINE_TRACE"))
    std::printf("    [finalize] bubble %lld: settle %.1f ms (conv %d, its %d, pcg %d, maxd %.3f)%s\n",
                static_cast<long long>(a.id), a.settle_ms, es.converged ? 1 : 0, es.iters, es.pcg, es.max_damage,
                failing && !a.takeover ? " -> continues dynamically" : (a.takeover ? " (taken over)" : ""));
  if (failing && a.respawns < cfg_.max_respawns && !a.takeover) {
    // The window still fails: continue the cascade dynamically from the settled state (the
    // residual of that state drives the new bubble; the trial damage of the settle is kept).
    std::array<f64, 3> cen{0, 0, 0};
    std::vector<i32> fine;
    for (i32 cell : es.candidate_cells) {
      for (int q = 0; q < 3; ++q) cen[q] += grid_.h * L.p[cell][q];
      fine.push_back(cell);
    }
    const f64 nc = std::max<size_t>(1, es.candidate_cells.size());
    for (auto& v : cen) v /= nc;
    std::vector<f64> fint(u.size()), r(u.size(), 0.0);
    internal_forces(L, u.data(), cfg_.corot, fint.data());
    for (i32 i = 0; i < L.n; ++i) {
      if (L.dead[i] || L.anchored[i]) continue;
      for (int q = 0; q < 6; ++q) r[6 * size_t(i) + q] = load[6 * size_t(i) + q] - fint[6 * size_t(i) + q];
    }
    auto b = std::make_unique<Active>();
    b->id = next_id_++;
    b->center = {static_cast<i32>(std::floor(cen[0] / grid_.h + 0.5)), static_cast<i32>(std::floor(cen[1] / grid_.h + 0.5)),
                 static_cast<i32>(std::floor(cen[2] / grid_.h + 0.5))};
    if (a.spans_structure) b->center = a.center;  // (its reach stays the structure's bounding sphere)
    b->radius = a.radius;
    b->spans_structure = a.spans_structure;
    b->structure_bubble = a.spans_structure;
    b->u_start = a.u_start;
    b->load = a.load;
    b->respawns = a.respawns + 1;
    b->chunks = a.chunks;
    b->dmg0 = std::move(a.dmg0);
    b->region = std::move(a.region);
    BubbleOptions bo;
    bo.comp.R0 = cfg_.R0;
    bo.comp.max_level = cfg_.max_level;
    bo.comp.force_fine = fine;
    bo.dt = cfg_.dt;
    bo.corot = cfg_.corot;
    bo.rayleigh_alpha = par_.damping;
    bo.nominate_phi = cfg_.nominate_phi;
    bo.marginal_eps = cfg_.marginal_eps;
    bo.demand_inflation = cfg_.demand_inflation;
    bo.max_fine_cells = b->spans_structure ? cfg_.structure_max_fine_cells : cfg_.max_fine_cells;
    if (b->spans_structure) bo.newton_iters = cfg_.structure_newton_iters;
    Active* bp = b.get();
    std::vector<std::function<void()>> stages;  // (init, preconditioner, first step)
    stages.push_back([bp, u, r = std::move(r), cen, bo]() {
      const std::array<f64, 3> centers[1] = {cen};
      bp->bubble.init(bp->region->L, u, r, centers, bo);
    });
    stages.push_back([bp]() { bp->bubble.prepare(); });
    stages.push_back([bp]() { bp->result = bp->bubble.step(); });
    start_setup(*b, std::move(stages), a.nodes());
    ++st_.bubbles_spawned;
    return b;
  }
  const bool ftrace = std::getenv("SVX_ENGINE_TRACE") != nullptr;
  f64 fc = cpu_now_ms();
  auto flap = [&](const char* what) {
    if (!ftrace) return;
    const f64 now = cpu_now_ms();
    std::printf("    [finalize] %-9s %7.2f cpu ms\n", what, now - fc);
    fc = now;
  };
  store_baseline(*a.region, u);
  flap("baseline");
  // damage: the bonds the bubble (chain) changed since its window was committed
  const bool known = a.dmg0[0].size() == size_t(L.n) && a.dmg0[1].size() == size_t(L.n) && a.dmg0[2].size() == size_t(L.n);
  for (int ax = 0; ax < 3; ++ax)
    for (i32 i = 0; i < L.n; ++i)
      if (L.nbr[ax][i] >= 0 && !L.dead[i] && (!known || L.dmg[ax][i] != a.dmg0[ax][i]))
        grid_.set_damage(a.region->vox[i], ax, L.dmg[ax][i]);
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i] || L.dead[i]) continue;
    f64 dq[3], d2 = 0.0;
    for (int q = 0; q < 3; ++q) {
      dq[q] = par_.amplification * (u[6 * size_t(i) + q] - a.u_start[6 * size_t(i) + q]);
      d2 += dq[q] * dq[q];
    }
    if (d2 <= 1e-8) continue;
    f32 off[3] = {0, 0, 0};
    grid_.offset(a.region->vox[i], off);
    for (int q = 0; q < 3; ++q) off[q] += static_cast<f32>(dq[q]);
    grid_.set_offset(a.region->vox[i], off);
  }
  flap("overlays");
  for (u64 k : a.chunks) grid_.mark_dirty(unkey3(k));
  if (cfg_.verify && a.region) enqueue_verify(a.region->center, a.region->radius, a.respawns, 0);
  return nullptr;
}

void Engine::enqueue_verify(const IVec3& center, i32 radius, int respawns, int requeues, bool urgent,
                            std::vector<std::pair<u64, u32>> watch) {
  constexpr int kMaxRequeues = 8;
  if (requeues > kMaxRequeues && !urgent) return;
  // a queued (not yet started) job whose window holds this centre verifies the same structures
  if (requeues > 0)
    for (auto& o : verify_) {
      if (o->start >= 0) continue;
      const i64 dx = o->center[0] - center[0], dy = o->center[1] - center[1], dz = o->center[2] - center[2];
      if (dx * dx + dy * dy + dz * dz > i64(o->radius) * o->radius) continue;
      if (o->watch.empty()) o->watch = std::move(watch);  // (it waits for the structure too)
      if (urgent) {  // that job goes first now
        std::unique_ptr<Verify> j = std::move(o);
        verify_.erase(std::find(verify_.begin(), verify_.end(), nullptr));
        size_t at = 0;
        while (at < verify_.size() && verify_[at]->start >= 0) ++at;  // (behind a started job)
        verify_.insert(verify_.begin() + static_cast<long>(at), std::move(j));
      }
      return;
    }
  auto j = std::make_unique<Verify>();
  j->center = center;
  j->radius = radius;
  j->respawns = respawns;
  j->requeues = requeues;
  j->quiet_since = st_.ticks;
  j->watch = std::move(watch);
  // a newer job for (nearly) the same window supersedes an older one (a started one finishes
  // in the background)
  for (size_t k = 0; k < verify_.size();) {
    const Verify& o = *verify_[k];
    const i32 dx = o.center[0] - j->center[0], dy = o.center[1] - j->center[1], dz = o.center[2] - j->center[2];
    if (dx * dx + dy * dy + dz * dz <= 4 && o.radius <= j->radius) {
      if (o.start >= 0) verify_stale_.push_back(std::move(verify_[k]));
      verify_.erase(verify_.begin() + static_cast<long>(k));
    } else {
      ++k;
    }
  }
  if (urgent) {
    size_t at = 0;
    while (at < verify_.size() && verify_[at]->start >= 0) ++at;  // (behind a started job)
    verify_.insert(verify_.begin() + static_cast<long>(at), std::move(j));
  } else {
    verify_.push_back(std::move(j));
  }
}

std::vector<u64> Engine::structure_chunks(const IVec3& c, i32 wr, i64 max_chunks, bool* complete,
                                          std::vector<u64>* structural_keys) const {
  // the window's chunks (and one ring: rim and rock neighbours), then face-connected chunks that
  // hold structure (non-anchored voxels: a street's ground does not join two buildings), with
  // their neighbours (the rock they stand on), up to max_chunks; complete: the search ended
  // before the bound
  const IVec3 clo = chunk_of({c[0] - wr - 1, c[1] - wr - 1, c[2] - wr - 1});
  const IVec3 chi = chunk_of({c[0] + wr + 1, c[1] + wr + 1, c[2] + wr + 1});
  std::vector<u64> keys, queue;
  std::unordered_map<u64, u8> seen;  // 1: in the snapshot, 2: expanded too
  auto structural = [&](const Chunk& ch) { return ch.free_count() > 0; };  // (rock and air alone: no)
  auto add = [&](const IVec3& cc, bool expand) {
    // (a streamed world's chunk that is not resident is unknown, not air: left out of the
    // snapshot, where structures_of stops)
    if (!chunk_resident(cc)) return;
    const u64 k = key3(cc[0], cc[1], cc[2]);
    auto [it, fresh] = seen.emplace(k, u8(0));
    if (it->second == 2) return;
    if (fresh) keys.push_back(k);  // (an absent chunk is known air)
    const Chunk* ch = grid_.chunk(cc);
    if (!ch) return;
    it->second = 1;
    if (expand && structural(*ch)) {
      it->second = 2;
      queue.push_back(k);
    }
  };
  for (i32 x = clo[0] - 1; x <= chi[0] + 1; ++x)
    for (i32 y = clo[1] - 1; y <= chi[1] + 1; ++y)
      for (i32 z = clo[2] - 1; z <= chi[2] + 1; ++z) {
        const bool core = x >= clo[0] && x <= chi[0] && y >= clo[1] && y <= chi[1] && z >= clo[2] && z <= chi[2];
        add({x, y, z}, core);
      }
  size_t h = 0;
  for (; h < queue.size() && static_cast<i64>(keys.size()) < max_chunks; ++h) {
    const IVec3 cc = unkey3(queue[h]);
    for (int a = 0; a < 3; ++a)
      for (int sg = -1; sg <= 1; sg += 2) {
        IVec3 nc = cc;
        nc[a] += sg;
        add(nc, true);
      }
  }
  if (complete) *complete = h >= queue.size();
  if (structural_keys) {
    *structural_keys = queue;
    std::sort(structural_keys->begin(), structural_keys->end());
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

void Engine::verify_update() {
  if (verify_.empty()) return;
  if (verify_.front()->start < 0) {
    // The snapshot reads the grid: a job starts once its window has been quiet for a spell -
    // no event in it, no bubble or settle over it (not between shots there; the world may be
    // busy elsewhere, and changes elsewhere in its structures drop its result as stale). The
    // oldest such job goes first.
    auto overlaps = [](const IVec3& a, i32 ra, const IVec3& b, i32 rb) {
      const i64 dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], r = i64(ra) + rb;
      return dx * dx + dy * dy + dz * dz <= r * r;
    };
    size_t pick = verify_.size();
    for (size_t k = 0; k < verify_.size() && pick == verify_.size(); ++k) {
      Verify& q = *verify_[k];
      if (q.start >= 0) continue;
      for (auto& [key, ver] : q.watch) {  // (a stale job's structure changed again: not quiet)
        const Chunk* ch = grid_.chunk(unkey3(key));
        const u32 now = ch ? ch->version : 0u;
        if (now != ver) {
          ver = now;
          q.quiet_since = st_.ticks;
        }
      }
      if (st_.ticks - q.quiet_since < cfg_.verify_quiet_ticks) continue;
      bool busy = false;
      for (const auto& a : active_)
        if (a->region && overlaps(a->center, a->region->radius, q.center, q.radius)) busy = true;
      for (const auto& sl : settles_)
        if (overlaps(sl->center, sl->radius, q.center, q.radius)) busy = true;
      if (!busy) pick = k;
    }
    if (pick == verify_.size()) return;
    if (pick > 0) std::rotate(verify_.begin(), verify_.begin() + static_cast<long>(pick), verify_.begin() + static_cast<long>(pick) + 1);
  }
  Verify& v = *verify_.front();
  const bool trace = std::getenv("SVX_ENGINE_TRACE") != nullptr;
  if (v.start < 0) {
    const auto t0 = Clock::now();
    const IVec3 c = v.center;
    const i32 wr = v.radius;
    ensure_chunks({c[0] - wr - 1, c[1] - wr - 1, c[2] - wr - 1}, {c[0] + wr + 2, c[1] + wr + 2, c[2] + wr + 2});
    const i64 r2 = i64(wr) * wr;
    for (i32 x = c[0] - wr; x <= c[0] + wr; ++x)
      for (i32 y = c[1] - wr; y <= c[1] + wr; ++y)
        for (i32 z = c[2] - wr; z <= c[2] + wr; ++z) {
          const i64 dx = x - c[0], dy = y - c[1], dz = z - c[2];
          if (dx * dx + dy * dy + dz * dz > r2) continue;
          const Vox w = grid_.get(x, y, z);
          if (vox_solid(w) && !vox_anchored(w)) v.seeds.push_back({x, y, z});
        }
    if (v.seeds.empty()) {
      verify_.erase(verify_.begin());
      return;
    }
    // The chunks the structures may reach (structure_chunks); a structure reaching beyond the
    // snapshot bound is verified around the window (as one above verify_max_cells).
    std::vector<u64> keys = structure_chunks(c, wr, cfg_.verify_max_chunks, nullptr, nullptr);
    v.snap = grid_.snapshot(keys);
    if (source_ && cfg_.verify_hydrate) {  // (streamed worlds: what the snapshot left out is hydrated by the job)
      v.source = source_;
      v.archive = archive_;
      v.resident.assign(generated_.begin(), generated_.end());
      std::sort(v.resident.begin(), v.resident.end());
      v.max_hydrate = std::max<i64>(0, cfg_.verify_max_chunks - static_cast<i64>(keys.size()));
      v.design_utilization = cfg_.design_utilization;
      v.design_corot = cfg_.corot && !cfg_.bake_linear;
    }
    v.lo = lattice_options();
    v.kscale = 1.0 / std::max(1e-9, par_.compliance);
    v.corot = cfg_.corot;
    v.max_cells = cfg_.verify_max_cells;
    v.threads = std::max(1, cfg_.verify_threads);
    v.trace = trace;
    v.so.max_iters = cfg_.verify_max_iters;
    v.so.max_pcg_total = cfg_.verify_max_pcg;
    v.pcg_cells = cfg_.verify_pcg_cells;
    v.min_pcg = cfg_.verify_min_pcg;
    v.start = st_.ticks;
#ifndef SVX_NO_THREADS
    if (cfg_.verify_async) {
      Verify* job = &v;
      v.worker = std::thread([job] { job->run(); });
    }
#endif
    st_.verify_ms += ms_since(t0);
    if (trace)
      std::printf("    [verify] started around (%d %d %d): %zu seeds, snapshot of %zu chunks in %.1f ms\n", c[0], c[1], c[2],
                  v.seeds.size(), keys.size(), ms_since(t0));
    return;
  }
  if (v.due < 0) {
    // the job's size is known once its extraction is done (long before this tick, except on a
    // starved machine): the due tick follows from it
    if (st_.ticks < v.start + cfg_.verify_latency_ticks) return;
    const auto t0 = Clock::now();
#ifndef SVX_NO_THREADS
    if (v.worker.joinable()) v.wait_extracted();
    else if (!v.extracted.load()) v.extract(), v.extracted.store(true);
#else
    v.extract();
    v.extracted.store(true);
#endif
    st_.verify_ms += ms_since(t0);
    if (!v.region) {
      if (trace) std::printf("    [verify] nothing to verify around (%d %d %d)\n", v.center[0], v.center[1], v.center[2]);
      verify_.erase(verify_.begin());
      return;
    }
    const i64 cells = v.region->L.n;
    const i64 per = std::max<i64>(1, cfg_.verify_cells_per_tick);
    v.due = v.start + std::max<i64>(cfg_.verify_latency_ticks, (cells + per - 1) / per);
    if (trace)
      std::printf("    [verify] %s of %lld cells extracted in %.1f ms (background); due in %lld ticks\n",
                  v.truncated ? "window (structure too large)" : "structures", static_cast<long long>(cells),
                  v.ms_extract, static_cast<long long>(v.due - st_.ticks));
  }
  if (st_.ticks < v.due) return;
  const auto t0 = Clock::now();
  v.join();
  if (!v.solved) v.solve();  // no background thread: solve now
  st_.verify_ms += ms_since(t0);
  std::unique_ptr<Verify> job = std::move(verify_.front());
  verify_.erase(verify_.begin());
  // the world changed under the job (a newer event re-verifies): drop it (a hydrated chunk still
  // evicted is as the job saw it; one that streamed in meanwhile is compared as usual)
  for (const auto& [k, ver] : job->versions) {
    if (!chunk_resident(unkey3(k))) continue;
    const Chunk* ch = grid_.chunk(unkey3(k));
    if ((ch ? ch->version : 0u) != ver) {
      if (trace)
        std::printf("    [verify] %d cells: dropped (the structures changed meanwhile; its result: max damage %.3f%s), queued "
                    "again\n",
                    job->region->L.n, job->es.max_damage, job->es.max_damage >= 0.999 ? " FAILS" : "");
      // (a failure found on the old state is checked again first, on fresh data - once its
      // structure has been quiet)
      std::vector<std::pair<u64, u32>> watch;
      watch.reserve(job->versions.size());
      for (const auto& kv : job->versions) {
        if (!chunk_resident(unkey3(kv.first))) continue;
        const Chunk* c2 = grid_.chunk(unkey3(kv.first));
        watch.emplace_back(kv.first, c2 ? c2->version : 0u);
      }
      enqueue_verify(job->center, job->radius, job->respawns, job->requeues + 1, job->es.max_damage >= 0.999,
                     std::move(watch));
      return;
    }
  }
  ++st_.verifications;
  Region& R = *job->region;
  Lattice& L = R.L;
  const EquilibriumStats& es = job->es;
  const bool failed = es.max_damage >= 0.999;
  // undecided (no equilibrium within the budget: near a mechanism): the dynamics decide as well
  const bool undecided = !failed && !es.converged;
  if (trace)
    std::printf("    [verify] %d cells: %s after %d its (pcg %d), max damage %.3f; waited %.1f ms at the due tick"
                " (solve: law %.0f, forces %.0f, mg build %.0f x%d, pcg %.0f ms)\n",
                L.n, failed ? "FAILS" : es.converged ? "verified" : "undecided (budget)", es.iters, es.pcg_iters,
                es.max_damage, ms_since(t0), es.ms_law, es.ms_forces, es.ms_build, es.mg_builds, es.ms_pcg);
  if ((failed || undecided) && job->respawns < cfg_.max_respawns) {
    // (a failure across the streaming boundary: the hydrated chunks stream in with their design)
    for (u64 k : job->hydrated)
      if (!chunk_resident(unkey3(k))) generate_chunk(k);
    for (i32 i = 0; i < L.n; ++i)
      if (job->is_hydrated(R.vox[i]) && L.strength[i] > grid_.strength(R.vox[i])) grid_.set_strength(R.vox[i], L.strength[i]);
    // a failure the bubble's far field missed: it continues dynamically from the cached state
    // and the committed damage, fine at the failing bonds. (The verification's trial damage
    // belongs to a static state near a mechanism: committing it would rupture everything it
    // softened at once.)
    ++st_.verify_failures;
    job->trial = L.dmg;  // the static iterate's trial damage (undecided: where the load path is)
    const LawSweep sw = sweep_law(L, job->u.data(), job->dc, cfg_.corot, L.kscale);
    for (int a = 0; a < 3; ++a) L.dmg[a] = job->dc[a];
    for (int a = 0; a < 3; ++a)
      if (!L.cscale[a].empty())
        for (auto& sc : L.cscale[a]) sc = {1, 1, 1, 1, 1, 1};  // cracks closed, as at the cached rest
    std::vector<i32> fine;
    std::array<f64, 3> cen{0, 0, 0};
    for (const RuptureCandidate& cd : sw.candidates) {
      fine.push_back(cd.cell);
      for (int q = 0; q < 3; ++q) cen[q] += grid_.h * L.p[cd.cell][q];
    }
    if (fine.empty()) {  // undecided: the most damaged bonds of the static iterate
      f32 dmax = 0.0f;
      for (int a = 0; a < 3; ++a)
        for (i32 i = 0; i < L.n; ++i)
          if (L.nbr[a][i] >= 0) dmax = std::max(dmax, job->trial[a][i]);
      for (int a = 0; a < 3 && fine.size() < 64; ++a)
        for (i32 i = 0; i < L.n && fine.size() < 64; ++i)
          if (L.nbr[a][i] >= 0 && dmax > 0.0f && job->trial[a][i] >= 0.9f * dmax) {
            fine.push_back(i);
            for (int q = 0; q < 3; ++q) cen[q] += grid_.h * L.p[i][q];
          }
    }
    if (!fine.empty()) {
      for (auto& x : cen) x /= static_cast<f64>(fine.size());
    } else {
      cen = {grid_.h * job->center[0], grid_.h * job->center[1], grid_.h * job->center[2]};
    }
    const std::vector<f64>& u0 = job->u0;
    std::vector<f64> fint(u0.size()), r(u0.size(), 0.0);
    internal_forces(L, u0.data(), cfg_.corot, fint.data());
    for (i32 i = 0; i < L.n; ++i) {
      if (L.dead[i] || L.anchored[i]) continue;
      for (int q = 0; q < 6; ++q) r[6 * size_t(i) + q] = job->g[6 * size_t(i) + q] - fint[6 * size_t(i) + q];
    }
    auto b = std::make_unique<Active>();
    b->id = next_id_++;
    b->center = voxel_of(cen, grid_.h);
    b->radius = R.radius;
    if (!job->truncated) {  // (whole structures: events on them merge into it)
      b->center = job->center;
      b->spans_structure = b->structure_bubble = true;
    }
    b->u_start = u0;
    b->load = job->g;
    b->respawns = job->respawns + 1;
    for (const auto& kv : job->versions) b->chunks.push_back(kv.first);
    b->dmg0 = L.dmg;  // the committed damage (the grid's)
    b->region = std::move(job->region);
    BubbleOptions bo;
    bo.comp.R0 = cfg_.R0;
    bo.comp.max_level = cfg_.max_level;
    bo.comp.force_fine = fine;
    bo.dt = cfg_.dt;
    bo.corot = cfg_.corot;
    bo.rayleigh_alpha = par_.damping;
    bo.nominate_phi = cfg_.nominate_phi;
    bo.marginal_eps = cfg_.marginal_eps;
    bo.demand_inflation = cfg_.demand_inflation;
    bo.max_fine_cells = b->spans_structure ? cfg_.structure_max_fine_cells : cfg_.max_fine_cells;
    if (b->spans_structure) bo.newton_iters = cfg_.structure_newton_iters;
    Active* bp = b.get();
    std::vector<std::function<void()>> stages;  // (init, preconditioner, first step)
    stages.push_back([bp, u0, r = std::move(r), cen, bo]() {
      const std::array<f64, 3> centers[1] = {cen};
      bp->bubble.init(bp->region->L, u0, r, centers, bo);
    });
    stages.push_back([bp]() { bp->bubble.prepare(); });
    stages.push_back([bp]() { bp->result = bp->bubble.step(); });
    start_setup(*b, std::move(stages), 4000);
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::Bubble;
    ev.id = b->id;
    ev.pos = cen;
    ev.radius = grid_.h * bo.comp.R0;
    ev.level = cfg_.max_level;
    events_.push_back(std::move(ev));
    ++st_.bubbles_spawned;
    active_.push_back(std::move(b));
    return;
  }
  // (a failure or an undecided state beyond the continuation limit changes nothing)
  if (failed || undecided) return;
  // verified: the full-fine state refines the cache; the render offsets stay as settled (cells of
  // hydrated chunks still evicted are left alone: the world holds none of them)
  auto resident = [&](i32 i) { return !job->is_hydrated(R.vox[i]) || chunk_resident(chunk_of(R.vox[i])); };
  store_baseline(R, job->u, resident);
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i)
      if (L.nbr[a][i] >= 0 && !L.dead[i] && L.dmg[a][i] > job->dc[a][i] && resident(i))
        grid_.set_damage(R.vox[i], a, L.dmg[a][i]);
}

bool Engine::impact_into_bubble(const PendingEvent& e) {
  IVec3 hit;
  if (!impact_voxel(grid_, e.pos, e.impulse, hit)) return false;
  for (const auto& ap : active_) {
    Active& a = *ap;
    // (the window's cell index is fixed; its lattice may be stepping)
    bool inside = false;
    for (i32 dx = -1; dx <= 1 && !inside; ++dx)
      for (i32 dy = -1; dy <= 1 && !inside; ++dy)
        for (i32 dz = -1; dz <= 1 && !inside; ++dz) inside = a.region->cell({hit[0] + dx, hit[1] + dy, hit[2] + dz}) >= 0;
    if (!inside) continue;
    if (a.takeover || a.finalizing) {  // (settling: the landing's load passes)
      ++st_.impacts_dropped;
      return true;
    }
    if (a.pending() && cfg_.tick_work > 0) {  // added before its next step (no wait for the running one)
      a.landings.push_back({hit, e.impulse});
      return true;
    }
    if (a.pending()) make_ready(a);
    const std::vector<i32> cells = contact_cells(*a.region, hit);
    if (cells.empty()) continue;
    const f64 k = 1.0 / (cfg_.dt * static_cast<f64>(cells.size()));
    for (i32 cell : cells) {
      const f64 f[6] = {e.impulse[0] * k, e.impulse[1] * k, e.impulse[2] * k, 0.0, 0.0, 0.0};
      a.bubble.add_force(cell, f);
    }
    return true;
  }
  return false;
}

Engine::Active* Engine::merge_target(const PendingEvent& e, const IVec3& c) {
  if (e.impact || cfg_.structure_max_cells <= 0) return nullptr;
  Active* reach = nullptr;
  for (const auto& ap : active_) {
    Active& a = *ap;
    if (!a.structure_bubble || a.takeover || a.finalizing || !a.region) continue;
    const f64 dx = a.center[0] - c[0], dy = a.center[1] - c[1], dz = a.center[2] - c[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) >= a.region->radius) continue;
    // (its cell index is fixed while its jobs run: a setup builds the structure's region aside)
    for (i32 x = -2; x <= 2; ++x)
      for (i32 y = -2; y <= 2; ++y)
        for (i32 z = -2; z <= 2; ++z)
          if (a.region->cell({c[0] + x, c[1] + y, c[2] + z}) >= 0) return &a;
    if (!reach) reach = &a;
  }
  return reach;
}

Bubble::EventMutation Engine::take_merges(Active& a) {
  Bubble::EventMutation ev;
  if (a.merges.empty() || !a.region) return ev;
  const Region& R = *a.region;  // (voxels outside it: the event's other structures, not simulated here)
  for (const Merge& m : a.merges) {
    for (const IVec3& v : m.fine)
      if (const i32 c = R.cell(v); c >= 0) ev.fine.push_back(c);
    for (const IVec3& v : m.removed)
      if (const i32 c = R.cell(v); c >= 0) ev.removed.push_back(c);
    for (const IVec3& v : m.islands)
      if (const i32 c = R.cell(v); c >= 0) ev.islands.push_back(c);
    for (const auto& [v, ax] : m.fractured)
      if (const i32 c = R.cell(v); c >= 0) ev.broken.push_back({c, ax});
    for (const auto& [v, ax, d] : m.damaged)
      if (const i32 c = R.cell(v); c >= 0) ev.damaged.push_back({c, ax, d});
    for (const auto& [v, f] : m.impulse)
      if (const i32 c = R.cell(v); c >= 0) ev.forces.push_back({c, f});
  }
  a.merges.clear();
  return ev;
}

bool Engine::defer_event(const PendingEvent& e) {
  if (!cfg_.defer_takeovers || cfg_.tick_work <= 0) return false;
  const IVec3 c = voxel_of(e.pos, grid_.h);
  // a pending static settle its window reaches: applied first (grid writes keep their order),
  // so the event waits for its due tick rather than the tick for its solve
  const i32 er = static_cast<i32>(std::ceil(e.radius / grid_.h));
  const i32 wr = e.impact ? cfg_.impact_window : e.blast ? std::max(cfg_.window, 3 * er + 8) : std::max(cfg_.window_small, 4 * er + 6);
  for (const auto& sl : settles_) {
    const f64 dx = sl->center[0] - c[0], dy = sl->center[1] - c[1], dz = sl->center[2] - c[2];
    const f64 lim = sl->radius + wr + 2;
    if (dx * dx + dy * dy + dz * dz <= lim * lim) return true;
  }
  if (merge_target(e, c)) return false;  // (it merges into the running bubble of its structure)
  bool held = false;
  for (const auto& ap : active_) {
    Active& a = *ap;
    const f64 dx = a.center[0] - c[0], dy = a.center[1] - c[1], dz = a.center[2] - c[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) < a.region->radius) {
      a.takeover = true;  // (the same test process() takes a bubble over with)
      held = true;
    }
  }
  return held;
}

void Engine::release_deferred() {
  if (deferred_.empty()) return;
  std::vector<PendingEvent> waiting;
  waiting.swap(deferred_);
  bool blasted = false;  // (one rocket's window a tick: released together they would stack up)
  for (PendingEvent& e : waiting) {
    if ((e.blast && blasted) || defer_event(e)) {
      deferred_.push_back(std::move(e));
      continue;
    }
    blasted = blasted || e.blast;
    process(e);
    spawn_pending();
  }
}

void Engine::step_debris() {
  if (!cfg_.debris) {
    debris_.clear();
    return;
  }
  if (debris_.bodies().empty()) return;
  const auto t0 = Clock::now();
  std::vector<DebrisImpact> landed;
  debris_.step(cfg_.dt, grid_, cfg_.debris_params, &landed);
  (void)debris_.take_finished();
  for (const DebrisImpact& im : landed) {
    ++st_.debris_landings;
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::Impact;
    ev.id = next_id_++;
    ev.pos = im.pos;
    ev.strength = 0.5 * im.mass * im.speed * im.speed;
    events_.push_back(std::move(ev));
    const f64 J = std::sqrt(im.impulse[0] * im.impulse[0] + im.impulse[1] * im.impulse[1] + im.impulse[2] * im.impulse[2]);
    if (J < cfg_.impact_min_impulse) continue;
    PendingEvent pe;
    pe.impact = true;
    pe.pos = im.pos;
    pe.impulse = im.impulse;
    pe.tick = st_.ticks;
    impacts_.push_back(std::move(pe));
  }
  st_.debris_ms = ms_since(t0);
}

void Engine::tick() {
  const auto t0 = Clock::now();
  ++st_.ticks;
  // SVX_TICK_PROFILE=<ms>: the phases of every tick longer than that
  static const char* prof_env = std::getenv("SVX_TICK_PROFILE");
  f64 prof[12] = {};
  auto lap = [&, tp = Clock::now()](int k) mutable {
    if (!prof_env) return;
    const auto now = Clock::now();
    prof[k] += std::chrono::duration<f64, std::milli>(now - tp).count();
    tp = now;
  };
  if (summaries_.size() > 1024) summaries_.clear();  // (a cache: searches remake what they need)
  const int streamed = stream_update();
  lap(0);
  if (!par_.paused) {
    run_setup_stages();   // (without threads: this tick's stage of each pending setup)
    lap(1);
    apply_settles(true);  // the static settles of the last tick's small carves
    spawn_pending();
    lap(2);
    std::vector<PendingEvent> q;
    q.swap(queue_);
    // Event merging (plan §B5): carves of the same tick close to an earlier carve (shotgun
    // pellets, bursts) are applied in that carve's window with one triage.
    std::vector<PendingEvent> merged;
    for (const PendingEvent& e : q) {
      bool done = false;
      if (!e.blast)
        for (PendingEvent& m : merged) {
          if (m.blast) continue;
          const f64 dx = e.pos[0] - m.pos[0], dy = e.pos[1] - m.pos[1], dz = e.pos[2] - m.pos[2];
          if (dx * dx + dy * dy + dz * dz <= cfg_.merge_radius * cfg_.merge_radius) {
            m.extra.push_back({e.pos[0], e.pos[1], e.pos[2], e.radius});
            done = true;
            break;
          }
        }
      if (!done) merged.push_back(e);
    }
    for (const PendingEvent& e : merged) {
      // an event in a running bubble's window waits for that bubble's finalize (budget: its
      // settle runs in the background)
      if (defer_event(e)) {
        ++st_.deferred_events;
        deferred_.push_back(e);
        continue;
      }
      process(e);
      spawn_pending();
    }
    lap(3);
    // debris landings of the previous tick: strongest first, a bounded number per tick
    if (!impacts_.empty()) {
      auto j2 = [](const PendingEvent& x) { return x.impulse[0] * x.impulse[0] + x.impulse[1] * x.impulse[1] + x.impulse[2] * x.impulse[2]; };
      std::stable_sort(impacts_.begin(), impacts_.end(), [&](const PendingEvent& a, const PendingEvent& b) { return j2(a) > j2(b); });
      std::vector<PendingEvent> pending;
      pending.swap(impacts_);
      int n = 0;
      for (PendingEvent& e : pending) {
        if (n < cfg_.impacts_per_tick) {
          process(e);
          ++n;
        } else if (st_.ticks - e.tick <= cfg_.impact_max_age) {
          impacts_.push_back(std::move(e));
        } else {
          ++st_.impacts_dropped;
        }
      }
    }
    spawn_pending();
    lap(4);
    const auto t1 = Clock::now();
    step_bubbles();
    spawn_pending();
    release_deferred();  // (events whose bubbles were finalized this tick)
    st_.structural_ms = ms_since(t1);
    lap(5);
    step_debris();
    if (!movers_.empty()) step_movers();
    lap(6);
    // idle: bake the next chunk near the viewer (lazy baselines for large worlds)
    // (never while the neighbourhood is still streaming in: tiles are the longest idle work)
    // verification (eventual consistency of settled bubbles) starts once its window is quiet
    // and completes at its due tick; idle ticks without one bake
    const bool idle = merged.empty() && active_.empty() && impacts_.empty() && settles_.empty();
    // superseded jobs whose background solve has ended
    verify_stale_.erase(std::remove_if(verify_stale_.begin(), verify_stale_.end(),
                                       [](const std::unique_ptr<Verify>& j) {
#ifndef SVX_NO_THREADS
                                         if (j->worker.joinable() && !j->finished.load(std::memory_order_acquire))
                                           return false;
#endif
                                         j->join();
                                         return true;
                                       }),
                        verify_stale_.end());
    const bool verifying = !verify_.empty();
    lap(7);
    verify_update();
    lap(8);
    if (cfg_.idle_bake && !verifying && streamed == 0 && idle && !unbaked_.empty() && (st_.ticks % 2) == 0) bake_tile();
    lap(9);
  }
  st_.tick_ms = ms_since(t0);
  if (prof_env && st_.tick_ms >= std::atof(prof_env))
    std::printf("    [tick %lld] %.1f ms: stream %.1f setup %.1f settles %.1f events %.1f impacts %.1f bubbles %.1f "
                "(commit %.1f) debris/movers %.1f verify %.1f bake %.1f\n",
                static_cast<long long>(st_.ticks), st_.tick_ms, prof[0], prof[1], prof[2], prof[3], prof[4], prof[5],
                prof_commit_ms_, prof[6], prof[8], prof[9]);
  prof_commit_ms_ = 0.0;
}

std::vector<ChunkMesh> Engine::take_meshes(const MeshOptions& base) {
  std::vector<u64> keys = grid_.take_dirty();
  // Chunks under running bubbles are re-meshed every call with the current displacement; with
  // GPU displacement they are meshed per face once (static offsets only) and move by the
  // displacement fields (take_fields) instead.
  const bool gpu = cfg_.gpu_displacement;
  std::unordered_set<u64> anim, moved;
  for (const auto& ap : active_) {
    if (!ap->shown) continue;  // (being set up: static until its first step)
    const bool fresh = ap->snap != ap->meshed_snap;  // (a step spanning ticks moves them once)
    ap->meshed_snap = ap->snap;
    for (u64 k : ap->chunks) {
      anim.insert(k);
      if (fresh) moved.insert(k);
    }
  }
  const std::unordered_set<u64> was(animated_.begin(), animated_.end());
  for (u64 k : anim)
    if (gpu ? !was.count(k) : (moved.count(k) || !was.count(k))) keys.push_back(k);
  // chunks that were animated last call but are not anymore get a final static mesh
  for (u64 k : animated_)
    if (!anim.count(k)) keys.push_back(k);
  animated_.assign(anim.begin(), anim.end());
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  const f64 A = par_.amplification;
  MeshOptions mo = base;
  mo.displacement = [&](const IVec3& p, f32 out[3]) {
    bool any = grid_.offset(p, out);
    if (!any) out[0] = out[1] = out[2] = 0.0f;
    if (gpu) return any;
    for (const auto& ap : active_) {
      if (!ap->shown) continue;
      const i32 cell = ap->region->cell(p);  // (the index is fixed; the lattice may be stepping)
      if (cell < 0 || !ap->live_now[size_t(cell)]) continue;
      for (int q = 0; q < 3; ++q)
        out[q] += static_cast<f32>(A * (ap->u_now[6 * size_t(cell) + q] - ap->u_start[6 * size_t(cell) + q]));
      any = true;
      break;
    }
    return any;
  };
  if (par_.debug_view == 1) {
    mo.debug = [&](const IVec3& p) {
      f32 d = 0.0f;
      for (int a = 0; a < 3; ++a) {
        d = std::max(d, grid_.damage(p, a));
        IVec3 q = p;
        q[a] -= 1;
        d = std::max(d, grid_.damage(q, a));
      }
      return static_cast<u8>(std::lround(255.0f * std::clamp(d, 0.0f, 1.0f)));
    };
  } else if (par_.debug_view == 2) {
    mo.debug = [&](const IVec3& p) -> u8 {
      for (const auto& ap : active_) {
        if (!ap->shown || ap->level_now.empty()) continue;
        const i32 cell = ap->region->cell(p);
        if (cell < 0) continue;
        return ap->level_now[size_t(cell)];
      }
      return 0;
    };
  }
  // chunks mesh independently (reads only: the grid, offsets, the bubbles' commit snapshots), so
  // the pool meshes them in parallel; results are taken in key order (deterministic output)
  std::vector<ChunkMesh> meshes(keys.size());
  std::optional<SerialScope> serial;
  if (!mo.concurrent) serial.emplace();
  parallel_for(static_cast<i64>(keys.size()), 1, [&](i64 b0, i64 e0) {
    for (i64 j = b0; j < e0; ++j) {
      const u64 k = keys[size_t(j)];
      const IVec3 cc = unkey3(k);
      bool displaced = anim.count(k) > 0;
      if (!displaced) {
        // chunks carrying persistent offsets are meshed per face with displaced corners
        const Chunk* ch = grid_.chunk(cc);
        if (ch && !ch->uniform) {
          const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
          f32 tmp[3];
          for (int i = 0; i < kChunkVox && !displaced; i += 1) {
            if (!vox_solid(ch->v[i])) continue;
            const IVec3 p{b[0] + (i / (kChunk * kChunk)), b[1] + (i / kChunk) % kChunk, b[2] + i % kChunk};
            if (grid_.offset(p, tmp)) displaced = true;
          }
        }
      }
      meshes[size_t(j)] = mesh_chunk(grid_, cc, mo, displaced);
    }
  });
  std::vector<ChunkMesh> out;
  for (size_t j = 0; j < keys.size(); ++j) {
    if (meshes[j].vertices.empty()) {
      removed_chunks_.push_back(keys[j]);
      continue;
    }
    out.push_back(std::move(meshes[j]));
  }
  return out;
}

namespace {

// IEEE binary16 from binary32 (round to nearest even; visual data only).
u16 half_of(f32 f) {
  u32 x;
  std::memcpy(&x, &f, 4);
  const u32 sign = (x >> 16) & 0x8000u;
  const i32 e = static_cast<i32>((x >> 23) & 0xffu) - 127 + 15;
  u32 m = x & 0x7fffffu;
  if (((x >> 23) & 0xffu) == 0xffu) return static_cast<u16>(sign | 0x7c00u | (m ? 0x200u : 0u));
  if (e >= 31) return static_cast<u16>(sign | 0x7c00u);
  if (e <= 0) {
    if (e < -10) return static_cast<u16>(sign);
    m |= 0x800000u;
    const int shift = 14 - e;
    u32 hm = m >> shift;
    const u32 rem = m & ((1u << shift) - 1u), halfway = 1u << (shift - 1);
    if (rem > halfway || (rem == halfway && (hm & 1u))) ++hm;
    return static_cast<u16>(sign | hm);
  }
  u32 hm = m >> 13;
  const u32 rem = m & 0x1fffu;
  u32 he = static_cast<u32>(e);
  if (rem > 0x1000u || (rem == 0x1000u && (hm & 1u))) {
    if (++hm == 0x400u) {
      hm = 0;
      ++he;
    }
  }
  if (he >= 31) return static_cast<u16>(sign | 0x7c00u);
  return static_cast<u16>(sign | (he << 10) | hm);
}

}  // namespace

std::vector<DisplacementField> Engine::take_fields() {
  // One field per shown bubble, rebuilt only when it has committed since (a structure-spanning
  // bubble's box holds its whole structure). A box over kMaxTexels voxels is sampled coarser:
  // each texel the mean of its stride^3 voxels' (w d, w), so xyz / w stays the solid-weighted
  // mean displacement the shader interpolates.
  constexpr i64 kMaxTexels = 64 * 64 * 64;
  std::vector<DisplacementField> out;
  const f64 A = par_.amplification;
  for (const auto& ap : active_) {
    if (!ap->shown) continue;
    if (ap->field_snap == ap->snap) {
      if (!ap->field.rgba.empty()) out.push_back(ap->field);
      continue;
    }
    ap->field_snap = ap->snap;
    ap->field = DisplacementField{};
    const Region& R = *ap->region;
    const i32 nc = static_cast<i32>(ap->live_now.size());  // (the snapshot of the last commit)
    // box of the cells that visibly move (plus one voxel of margin for the interpolation)
    IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
    f64 dmax = 0.0;
    for (i32 i = 0; i < nc; ++i) {
      if (!ap->live_now[size_t(i)]) continue;
      f64 d2 = 0.0;
      for (int q = 0; q < 3; ++q) {
        const f64 d = A * (ap->u_now[6 * size_t(i) + q] - ap->u_start[6 * size_t(i) + q]);
        d2 += d * d;
      }
      if (d2 < 1e-8) continue;  // < 0.1 mm
      dmax = std::max(dmax, d2);
      for (int q = 0; q < 3; ++q) {
        lo[q] = std::min(lo[q], R.vox[i][q] - 1);
        hi[q] = std::max(hi[q], R.vox[i][q] + 1);
      }
    }
    if (lo[0] > hi[0]) continue;  // nothing moves visibly
    DisplacementField& f = ap->field;
    f.id = ap->id;
    f.version = ap->snap;
    f.lo = lo;
    const i64 vol = i64(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
    i32 st = 1;
    while (vol > kMaxTexels * i64(st) * st * st) st *= 2;
    f.stride = st;
    for (int q = 0; q < 3; ++q) f.size[q] = (hi[q] - lo[q] + st) / st;
    f.max_disp = std::sqrt(dmax);
    const size_t n = size_t(f.size[0]) * size_t(f.size[1]) * size_t(f.size[2]);
    f.rgba.assign(4 * n, 0);
    const u16 one = half_of(1.0f);
    const f32 inv = 1.0f / static_cast<f32>(st * st * st);
    for (i32 z = 0; z < f.size[2]; ++z)
      for (i32 y = 0; y < f.size[1]; ++y)
        for (i32 x = 0; x < f.size[0]; ++x) {
          u16* t = &f.rgba[4 * ((size_t(z) * size_t(f.size[1]) + size_t(y)) * size_t(f.size[0]) + size_t(x))];
          if (st == 1) {
            const IVec3 p{lo[0] + x, lo[1] + y, lo[2] + z};
            if (!vox_solid(grid_.get(p))) continue;  // air: weight 0
            t[3] = one;  // solid: weight 1 (static solids pin the interpolation at corners)
            const i32 c = R.cell(p);
            if (c < 0 || !ap->live_now[size_t(c)]) continue;
            for (int q = 0; q < 3; ++q)
              t[q] = half_of(static_cast<f32>(A * (ap->u_now[6 * size_t(c) + q] - ap->u_start[6 * size_t(c) + q])));
            continue;
          }
          f32 acc[4] = {0, 0, 0, 0};
          for (i32 dz = 0; dz < st; ++dz)
            for (i32 dy = 0; dy < st; ++dy)
              for (i32 dx = 0; dx < st; ++dx) {
                const IVec3 p{lo[0] + x * st + dx, lo[1] + y * st + dy, lo[2] + z * st + dz};
                if (!vox_solid(grid_.get(p))) continue;
                acc[3] += 1.0f;
                const i32 c = R.cell(p);
                if (c < 0 || !ap->live_now[size_t(c)]) continue;
                for (int q = 0; q < 3; ++q)
                  acc[q] += static_cast<f32>(A * (ap->u_now[6 * size_t(c) + q] - ap->u_start[6 * size_t(c) + q]));
              }
          for (int q = 0; q < 4; ++q) t[q] = half_of(acc[q] * inv);
        }
    out.push_back(f);
  }
  return out;
}

std::vector<u64> Engine::take_removed_chunks() {
  std::vector<u64> out;
  out.swap(removed_chunks_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<EngineEvent> Engine::take_events() {
  std::vector<EngineEvent> out;
  out.swap(events_);
  return out;
}

EngineStats Engine::stats() const {
  EngineStats s = st_;
  s.active_bubbles = static_cast<i32>(active_.size());
  s.active_nodes = 0;
  for (const auto& a : active_) s.active_nodes += a->nodes();
  s.voxels = grid_.solid_count();
  s.chunks = static_cast<i64>(grid_.chunks().size());
  s.memory_mb = f64(grid_.memory_bytes()) / (1024.0 * 1024.0);
  s.unbaked_chunks = static_cast<i64>(unbaked_.size());
  s.resident_chunks = static_cast<i64>(generated_.size());
  s.archived_chunks = static_cast<i64>(archive_.size());
  s.debris_bodies = static_cast<i32>(debris_.bodies().size());
  return s;
}

u64 Engine::state_hash() const {
  std::vector<u64> keys;
  for (const auto& [k, c] : grid_.chunks()) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  u64 hsh = 1469598103934665603ull;
  auto mix = [&](u64 v) { hsh = (hsh ^ v) * 1099511628211ull; };
  for (u64 k : keys) {
    const Chunk& c = grid_.chunks().at(k);
    mix(k);
    if (c.uniform) {
      mix(c.value);
    } else {
      for (Vox v : c.v) mix(v);
    }
    for (u8 b : c.broken) mix(b);
  }
  mix(grid_.overlay_hash());
  return hsh;
}

u64 Engine::session_hash() const {
  u64 hsh = state_hash();
  auto mix = [&](u64 v) { hsh = (hsh ^ v) * 1099511628211ull; };
  auto bits = [](f64 x) {
    u64 u;
    std::memcpy(&u, &x, sizeof u);
    return u;
  };
  for (const DebrisBody& b : debris_.bodies()) {
    mix(static_cast<u64>(b.id));
    for (int q = 0; q < 3; ++q) mix(bits(b.x[q]));
    for (int q = 0; q < 4; ++q) mix(bits(b.q[q]));
  }
  mix(static_cast<u64>(impacts_.size()));
  for (const Mover& m : movers_) {
    mix(bits(m.level));
    mix(bits(m.timer));
    mix(static_cast<u64>(static_cast<u32>(m.move)) | (static_cast<u64>(m.leg) << 32) |
        (static_cast<u64>(m.rows) << 40) | (m.stopped ? 1ull << 62 : 0ull) | (m.disabled ? 1ull << 63 : 0ull));
  }
  return hsh;
}

RayHit Engine::raycast(const std::array<f64, 3>& origin, const std::array<f64, 3>& dir, f64 max_dist) const {
  RayHit hit;
  const f64 h = grid_.h;
  f64 len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  if (len <= 0.0) return hit;
  const f64 d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
  // voxel p spans [p - 1/2, p + 1/2] h: shift by half a voxel to integer boundaries
  f64 o[3];
  i32 v[3];
  int step[3];
  f64 tmax[3], tdelta[3];
  for (int a = 0; a < 3; ++a) {
    o[a] = origin[a] / h + 0.5;
    v[a] = static_cast<i32>(std::floor(o[a]));
    step[a] = d[a] > 0 ? 1 : (d[a] < 0 ? -1 : 0);
    if (step[a] != 0) {
      const f64 next = step[a] > 0 ? (v[a] + 1 - o[a]) : (o[a] - v[a]);
      tdelta[a] = 1.0 / std::abs(d[a]);
      tmax[a] = next * tdelta[a];
    } else {
      tdelta[a] = tmax[a] = INFINITY;
    }
  }
  const f64 tlim = max_dist / h;
  int face = -1;
  f64 t = 0.0;
  for (int it = 0; it < 100000 && t <= tlim; ++it) {
    const Vox vx = grid_.get(v[0], v[1], v[2]);
    if (vox_solid(vx)) {
      hit.hit = true;
      hit.distance = t * h;
      for (int a = 0; a < 3; ++a) hit.pos[a] = origin[a] + d[a] * hit.distance;
      hit.normal = {0, 0, 0};
      if (face >= 0) hit.normal[face] = -step[face];
      hit.material = static_cast<int>(vox_mat(vx));
      hit.voxel = {v[0], v[1], v[2]};
      return hit;
    }
    const int a = (tmax[0] < tmax[1]) ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
    t = tmax[a];
    tmax[a] += tdelta[a];
    v[a] += step[a];
    face = a;
  }
  return hit;
}

CollideResult Engine::collide(const std::array<f64, 3>& mn, const std::array<f64, 3>& mx,
                              const std::array<f64, 3>& move) const {
  CollideResult res;
  const f64 h = grid_.h;
  const f64 eps = 1e-4;
  std::array<f64, 3> lo = mn, hi = mx;
  auto vidx = [&](f64 x) { return static_cast<i32>(std::floor(x / h + 0.5)); };
  const int order[3] = {0, 1, 2};
  for (int a : order) {
    f64 dm = move[a];
    if (dm == 0.0) continue;
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    const i32 b0 = vidx(lo[b] + eps), b1 = vidx(hi[b] - eps);
    const i32 c0 = vidx(lo[c] + eps), c1 = vidx(hi[c] - eps);
    const f64 lead = dm > 0 ? hi[a] : lo[a];
    const f64 target = lead + dm;
    i32 layer = dm > 0 ? vidx(lead - eps) + 1 : vidx(lead + eps) - 1;
    const i32 last = dm > 0 ? vidx(target - eps) : vidx(target + eps);
    bool blocked = false;
    for (; dm > 0 ? layer <= last : layer >= last; layer += dm > 0 ? 1 : -1) {
      for (i32 ib = b0; ib <= b1 && !blocked; ++ib)
        for (i32 ic = c0; ic <= c1 && !blocked; ++ic) {
          i32 p[3];
          p[a] = layer;
          p[b] = ib;
          p[c] = ic;
          if (vox_solid(grid_.get(p[0], p[1], p[2]))) blocked = true;
        }
      if (blocked) break;
    }
    if (blocked) {
      const f64 face = dm > 0 ? (layer - 0.5) * h : (layer + 0.5) * h;
      dm = dm > 0 ? std::max(0.0, face - lead - eps) : std::min(0.0, face - lead + eps);
      if (a == 2 && move[2] < 0) res.on_ground = true;
    }
    lo[a] += dm;
    hi[a] += dm;
    res.move[a] = dm;
  }
  return res;
}

}  // namespace svx
