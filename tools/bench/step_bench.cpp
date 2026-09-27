// svx_step_bench — cost of event bubble steps on the rooms world (plan Phase 2 step gate), in
// thread CPU time so that A/B comparisons hold on a loaded machine: bakes once, then fires a
// fixed series of rockets from the spawn (each after the previous bubble slept) and reports, per
// rocket and in total, the CPU time of the ticks that stepped bubbles per bubble step, the PCG
// iterations per step and the composite node count. Verification is off (it runs off-thread).
//
// usage: svx_step_bench [--rockets N] [--steps-per-tick S] [--R0 C] [--threads T] [--setup-trace]
//                       [--spawn-latency T]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "svx/base/parallel.hpp"
#include "svx/engine/engine.hpp"
#include "svx/world/procgen.hpp"

using namespace svx;

namespace {

f64 cpu_ms() {
  timespec ts{};
#if defined(CLOCK_THREAD_CPUTIME_ID)
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  return 1e3 * static_cast<f64>(ts.tv_sec) + 1e-6 * static_cast<f64>(ts.tv_nsec);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // P cores preferred
#endif
  int rockets = 4, spt = -1, threads = 1, spawn_latency = -1;
  f64 R0 = -1.0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--rockets" && i + 1 < argc) rockets = std::atoi(argv[++i]);
    else if (a == "--steps-per-tick" && i + 1 < argc) spt = std::atoi(argv[++i]);
    else if (a == "--R0" && i + 1 < argc) R0 = std::atof(argv[++i]);
    else if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
    else if (a == "--setup-trace") setenv("SVX_SETUP_TRACE", "1", 1);  // (WASM under Node sees no shell env)
    else if (a == "--mg-profile") setenv("SVX_MG_PROFILE", "1", 1);
    else if (a == "--ticks") setenv("SVX_BENCH_TICKS", "1", 1);
    else if (a == "--sgs") setenv("SVX_SGS", "1", 1);
    else if (a == "--env" && i + 1 < argc) {  // KEY=VALUE (experiment knobs; WASM sees no shell env)
      const std::string kv = argv[++i];
      const size_t eq = kv.find('=');
      if (eq != std::string::npos) setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
    }
    else if (a == "--step-trace") setenv("SVX_STEP_TRACE", "1", 1);
    else if (a == "--spawn-latency" && i + 1 < argc) spawn_latency = std::atoi(argv[++i]);
  }
  set_num_threads(threads);
  Engine eng;
  {
    EngineConfig cfg = eng.config();
    cfg.verify = false;
    if (std::getenv("SVX_BAKE_COROT")) cfg.bake_linear = false;  // (diagnostics)
    if (spt > 0) cfg.steps_per_tick = spt;
    if (R0 > 0) cfg.R0 = R0;
    if (spawn_latency >= 0) cfg.spawn_latency_ticks = spawn_latency;
    eng.configure(cfg);
  }
  EngineParams par;
  par.fragility = 0.25;
  eng.set_params(par);
  ProcWorld pw = make_procedural("rooms", 7);
  eng.load(std::move(pw.grid), pw.spawn_pos, pw.spawn_dir);
  {
    f64 bake_ms = 0.0;
    int bake_pcg = 0;
    eng.bake(&bake_ms, &bake_pcg);
    if (std::getenv("SVX_BAKE_TRACE")) std::printf("bake: %.0f ms, %d pcg\n", bake_ms, bake_pcg);
  }
  const auto sp = eng.spawn_pos();
  const std::array<f64, 3> eye{sp[0], sp[1], sp[2] + 1.6};
  f64 all_cpu = 0.0, all_first = 0.0;
  i64 all_steps = 0, all_pcg = 0;
  std::printf("rocket  nodes  steps  cpu ms/step  pcg/step  event-tick cpu ms  worst tick cpu ms\n");
  std::printf("       (steps and cpu ms/step: the ticks after the event tick)\n");
  for (int r = 0; r < rockets; ++r) {
    const f64 ang = -0.6 + 0.5 * r;
    const RayHit hit = eng.raycast(eye, {std::cos(ang), std::sin(ang), 0.25}, 60.0);
    if (!hit.hit) continue;
    const EngineStats s0 = eng.stats();
    if (std::getenv("SVX_CHECK_BASELINE")) {  // the cached baseline around the hit (diagnostics)
      const f64 h = eng.grid().h;
      const IVec3 c{static_cast<i32>(std::lround(hit.pos[0] / h)), static_cast<i32>(std::lround(hit.pos[1] / h)),
                    static_cast<i32>(std::lround(hit.pos[2] / h))};
      if (const char* at = std::getenv("SVX_CHECK_AT")) {  // (x,y,z: one more window, centred there)
        IVec3 p{0, 0, 0};
        std::sscanf(at, "%d,%d,%d", &p[0], &p[1], &p[2]);
        const auto b2 = eng.check_baseline(p, 24);
        std::printf("    [baseline] around (%d %d %d): %lld cells, relative residual %.2e, max phi %.2f\n", p[0], p[1], p[2],
                    static_cast<long long>(b2.cells), b2.residual, b2.max_phi);
      }
      const auto bc = eng.check_baseline(c, 24);
      std::printf("    [baseline] around (%d %d %d): %lld cells, relative residual %.2e, max phi %.2f\n", c[0], c[1], c[2],
                  static_cast<long long>(bc.cells), bc.residual, bc.max_phi);
    }
    eng.blast(hit.pos, 0.5, 1e6);
    f64 cpu = 0.0, first = -1.0, worst = 0.0;
    i64 nodes = 0, first_steps = 0;
    for (int t = 0; t < 1200; ++t) {
      const f64 c0 = cpu_ms();
      eng.tick();
      const f64 dc = cpu_ms() - c0;
      (void)eng.take_events();
      const EngineStats& s = eng.stats();
      if (t == 0) {  // the event tick: triage, window, baseline, composite, first steps
        first = dc;
        first_steps = s.bubble_steps - s0.bubble_steps;
        nodes = s.active_nodes;
        if (s.active_bubbles == 0) break;
        continue;
      }
      if (s.active_bubbles == 0 && s.bubble_steps == eng.stats().bubble_steps && dc < 0.5) break;
      cpu += dc;
      worst = std::max(worst, dc);
      if (std::getenv("SVX_BENCH_TICKS"))
        std::printf("    tick %d: %.2f cpu ms, bubbles %d, nodes %d, steps %lld\n", t, dc, s.active_bubbles, s.active_nodes,
                    static_cast<long long>(s.bubble_steps - s0.bubble_steps));
      nodes = std::max<i64>(nodes, s.active_nodes);
    }
    const EngineStats& s1 = eng.stats();
    const i64 steps = s1.bubble_steps - s0.bubble_steps - first_steps, pcg = s1.step_pcg - s0.step_pcg;
    std::printf("%6d %6lld %6lld %12.2f %9.1f %18.1f %18.1f\n", r, static_cast<long long>(nodes),
                static_cast<long long>(steps), steps ? cpu / static_cast<f64>(steps) : 0.0,
                steps + first_steps ? static_cast<f64>(pcg) / static_cast<f64>(steps + first_steps) : 0.0, first, worst);
    all_cpu += cpu;
    all_first += first;
    all_steps += steps;
    all_pcg += pcg;
    for (int t = 0; t < 30; ++t) {  // quiet spell between rockets
      eng.tick();
      (void)eng.take_events();
    }
  }
  std::printf("total: %lld steps, %.2f cpu ms/step, %.1f pcg/step, event tick %.1f cpu ms; ruptures %lld, detached %lld; "
              "state hash %llu\n",
              static_cast<long long>(all_steps), all_steps ? all_cpu / static_cast<f64>(all_steps) : 0.0,
              all_steps ? static_cast<f64>(all_pcg) / static_cast<f64>(all_steps) : 0.0,
              rockets ? all_first / rockets : 0.0, static_cast<long long>(eng.stats().ruptures),
              static_cast<long long>(eng.stats().detached_voxels), static_cast<unsigned long long>(eng.state_hash()));
  return 0;
}
