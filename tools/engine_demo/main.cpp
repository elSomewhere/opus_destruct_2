// svx_engine_demo — headless end-to-end run of the engine core (plan Phase 3 checks).
//
// Loads a procedural world, meshes it, fires a scripted sequence of bullets (carve) and rockets
// (blast), ticks at 60 Hz until everything sleeps, and reports per-event and per-tick costs,
// triage outcomes, ruptures, detached voxels, mesh sizes, memory and the state hash (run twice
// with different thread counts to check determinism).
//
// usage: svx_engine_demo [--world rooms|city|tower] [--seed N] [--threads T] [--ticks N]
//                        [--rockets N] [--rocket-every T] [--bullets N] [--fragility F]
//                        [--realtime] [--spawn-latency T] [--wad F --map M] [--env KEY=VALUE]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fstream>
#include <iterator>

#include "svx/base/dmath.hpp"
#include "svx/base/parallel.hpp"
#include "svx/doom/movers.hpp"
#include "svx/doom/world.hpp"
#include "svx/engine/engine.hpp"
#include "svx/world/procgen.hpp"

using namespace svx;

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string world = "rooms", wad_path, map_name = "MAP01";
  int bake_tiles = 0;
  bool gpu_disp = false, verify = true, salvo = false;  // salvo: 4 rockets into 4 rooms at tick 30
  bool contacts = true;
  bool realtime = false;  // pace ticks at dt (background work gets real time)
  int spawn_latency = -1;  // < 0: engine default
  int rocket_every = 90;   // ticks between rockets (6: 10 rockets/s)
  f64 compliance = -1.0, damping = -1.0;  // < 0: engine defaults
  u64 seed = 1;
  int ticks = 600, rockets = 3, bullets = 20;
  f64 frag = 0.25;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--world" && i + 1 < argc) world = argv[++i];
    else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--threads" && i + 1 < argc) set_num_threads(std::atoi(argv[++i]));
    else if (a == "--ticks" && i + 1 < argc) ticks = std::atoi(argv[++i]);
    else if (a == "--rockets" && i + 1 < argc) rockets = std::atoi(argv[++i]);
    else if (a == "--bullets" && i + 1 < argc) bullets = std::atoi(argv[++i]);
    else if (a == "--fragility" && i + 1 < argc) frag = std::atof(argv[++i]);
    else if (a == "--wad" && i + 1 < argc) wad_path = argv[++i];
    else if (a == "--map" && i + 1 < argc) map_name = argv[++i];
    else if (a == "--bake-tiles" && i + 1 < argc) bake_tiles = std::atoi(argv[++i]);
    else if (a == "--gpu-displacement") gpu_disp = true;
    else if (a == "--no-verify") verify = false;
    else if (a == "--salvo") salvo = true;
    else if (a == "--no-contacts") contacts = false;
    else if (a == "--compliance" && i + 1 < argc) compliance = std::atof(argv[++i]);
    else if (a == "--damping" && i + 1 < argc) damping = std::atof(argv[++i]);
    else if (a == "--realtime") realtime = true;
    else if (a == "--spawn-latency" && i + 1 < argc) spawn_latency = std::atoi(argv[++i]);
    else if (a == "--rocket-every" && i + 1 < argc) rocket_every = std::max(1, std::atoi(argv[++i]));
    else if (a == "--env" && i + 1 < argc) {  // KEY=VALUE (traces and knobs; WASM under Node sees no shell env)
      const std::string kv = argv[++i];
      const size_t eq = kv.find('=');
      if (eq != std::string::npos) setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
    }
  }
  auto t0 = std::chrono::steady_clock::now();
  Engine eng;
  {
    EngineConfig cfg = eng.config();
    cfg.gpu_displacement = gpu_disp;
    cfg.contact.enabled = contacts;
    cfg.verify = verify;
    if (spawn_latency >= 0) cfg.spawn_latency_ticks = spawn_latency;
    eng.configure(cfg);
  }
  EngineParams par;
  par.fragility = frag;
  if (compliance > 0) par.compliance = compliance;
  if (damping >= 0) par.damping = damping;
  eng.set_params(par);
  doom::DoomWorld dw;  // outlives the engine's door / lift resolver
  if (!wad_path.empty()) {
    std::ifstream f(wad_path, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    doom::Wad wad;
    std::string err;
    if (!wad.load_memory(std::move(bytes), &err) || !doom::build_doom_world(wad, map_name, {}, 0.125, false, &dw, &err)) {
      std::printf("cannot load %s %s: %s\n", wad_path.c_str(), map_name.c_str(), err.c_str());
      return 1;
    }
    world = map_name;
    VoxelGrid g = std::move(dw.grid);
    dw.grid = VoxelGrid{};
    eng.load(std::move(g), dw.spawn_pos, dw.spawn_dir);
    eng.set_compliance_cap(dw.slenderness.max_compliance_p99);
    dw.live = &eng.grid();
    std::printf("movers: %d (doors, lifts, floors, ceilings)\n", doom::attach_doom_movers(eng, dw));
  } else {
    ProcWorld pw = make_procedural(world, seed);
    eng.load(std::move(pw.grid), pw.spawn_pos, pw.spawn_dir);
  }
  auto ms = [](auto t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); };
  {
    f64 bms = 0.0;
    int its = 0;
    const bool ok = eng.bake(&bms, &its);
    std::printf("bake: %s in %.0f ms (%d pcg); design pass: max self-weight utilization %.2f, %lld voxels strengthened, %lld floating removed\n",
                ok ? "whole-world baseline" : "skipped (too large)", bms, its, eng.design_report().max_utilization,
                static_cast<long long>(eng.design_report().strengthened_voxels),
                static_cast<long long>(eng.design_report().floating_voxels));
    if (!ok) {
      std::printf("progressive bake: %lld chunks with structure\n", static_cast<long long>(eng.unbaked_chunks()));
      const auto tb = std::chrono::steady_clock::now();
      int n = 0;
      while (n < bake_tiles && eng.bake_tile()) ++n;
      std::printf("  %d tiles in %.0f ms (%.1f ms/tile), %lld left; design max utilization %.2f, %lld strengthened\n", n,
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb).count(),
                  n ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb).count() / n : 0.0,
                  static_cast<long long>(eng.unbaked_chunks()), eng.design_report().max_utilization,
                  static_cast<long long>(eng.design_report().strengthened_voxels));
    }
  }
  std::printf("world %s: %lld solid voxels, %lld chunks, %.1f MB, generated in %.0f ms\n", world.c_str(),
              static_cast<long long>(eng.stats().voxels), static_cast<long long>(eng.stats().chunks), eng.stats().memory_mb,
              ms(t0));
  t0 = std::chrono::steady_clock::now();
  MeshOptions mo;
  auto meshes = eng.take_meshes(mo);
  size_t nv = 0, ni = 0;
  for (const auto& m : meshes) {
    nv += m.vertices.size();
    ni += m.indices.size();
  }
  std::printf("initial meshing: %zu chunks, %zu vertices, %zu triangles in %.0f ms\n", meshes.size(), nv, ni / 3, ms(t0));
  // targets: ray casts from the spawn in a fan of directions
  const auto sp = eng.spawn_pos();
  std::array<f64, 3> eye{sp[0], sp[1], sp[2] + 1.6};
  int fired_b = 0, fired_r = 0;
  f64 worst_tick = 0.0, sum_tick = 0.0, mesh_sum = 0.0, mesh_worst = 0.0;
  std::vector<f64> tick_times;
  const auto t_start = std::chrono::steady_clock::now();
  i64 mesh_ticks = 0, mesh_chunks = 0;
  f64 field_sum = 0.0;
  i64 detached_events = 0;
  if (std::getenv("SVX_COARSE_ERROR")) {  // (after a whole bake) the coarse field against it
    IVec3 w{0, 0, 0};
    const auto e = eng.coarse_field_error(&w);
    std::printf("coarse field vs baseline, |dz| error / max |dz|: p50 %.4f p90 %.4f p99 %.4f max %.4f at (%d %d %d)\n", e[0],
                e[1], e[2], e[3], w[0], w[1], w[2]);
  }
  if (std::getenv("SVX_CHECK_BASELINE")) {  // the cached baseline around a grid of points
    const i32 R = 40;
    const IVec3 lo = eng.grid().lo, hi = eng.grid().hi;
    i64 windows = 0, bad = 0;
    f64 worst = 0.0;
    for (i32 x = lo[0] + R; x < hi[0]; x += 2 * R)
      for (i32 y = lo[1] + R; y < hi[1]; y += 2 * R) {
        const IVec3 c{x, y, (lo[2] + hi[2]) / 2};
        const auto bc = eng.check_baseline(c, R);
        if (bc.cells < 1000) continue;
        ++windows;
        if (bc.over_one > 0) ++bad;
        worst = std::max(worst, bc.max_phi);
        std::printf("  baseline check at (%d %d %d): %lld cells, residual %.2e, max phi %.3f at (%d %d %d) axis %d, phi>0.5 %lld, phi>1 %lld\n",
                    c[0], c[1], c[2], static_cast<long long>(bc.cells), bc.residual, bc.max_phi, bc.worst[0], bc.worst[1],
                    bc.worst[2], bc.worst_axis, static_cast<long long>(bc.over_half), static_cast<long long>(bc.over_one));
      }
    std::printf("baseline check: %lld windows, %lld with bonds over the law's onset, max phi %.3f\n",
                static_cast<long long>(windows), static_cast<long long>(bad), worst);
  }
  for (int t = 0; t < ticks; ++t) {
    if (fired_b < bullets && t % 3 == 0) {
      const f64 ang = 0.15 * fired_b - 1.2;
      const RayHit hit = eng.raycast(eye, {dm::cos(ang), dm::sin(ang), -0.05 + 0.02 * (fired_b % 5)}, 60.0);
      if (hit.hit) eng.carve(hit.pos, 0.1);
      ++fired_b;
    }
    if (fired_r < rockets && t >= 30 && (t - 30) % rocket_every == 0) {
      const f64 ang = -0.6 + 0.5 * fired_r;
      const RayHit hit = eng.raycast(eye, {dm::cos(ang), dm::sin(ang), 0.25}, 60.0);
      if (hit.hit) {
        eng.blast(hit.pos, 0.6, 1e6);
        std::printf("  tick %3d rocket at (%.2f %.2f %.2f)\n", t, hit.pos[0], hit.pos[1], hit.pos[2]);
      }
      ++fired_r;
    }
    if (salvo && t == 30)
      for (const auto& at : {std::array<f64, 3>{52.5, 20, 12}, {153.5, 60, 12}, {1.5, 70, 12}, {103.5, 20, 12}})
        eng.blast({0.125 * at[0], 0.125 * at[1], 0.125 * at[2]}, 0.6, 1e6);
    if (realtime) std::this_thread::sleep_until(t_start + std::chrono::duration<double>(eng.config().dt * t));
    const auto tt = std::chrono::steady_clock::now();
    eng.tick();
    const f64 tick_ms = ms(tt);
    static const int hash_every = std::getenv("SVX_HASH_EVERY") ? std::atoi(std::getenv("SVX_HASH_EVERY")) : 0;
    if (hash_every > 0 && t % hash_every == 0)
      std::printf("  hash %d %llu %llu\n", t, static_cast<unsigned long long>(eng.state_hash()),
                  static_cast<unsigned long long>(eng.session_hash()));
    tick_times.push_back(tick_ms);
    worst_tick = std::max(worst_tick, tick_ms);
    sum_tick += tick_ms;
    const auto evs = eng.take_events();
    for (const auto& e : evs)
      if (e.kind == EngineEvent::Kind::Detached) ++detached_events;
    const auto tm = std::chrono::steady_clock::now();
    const auto m2 = eng.take_meshes(mo);
    size_t field_bytes = 0;
    if (gpu_disp)
      for (const auto& f : eng.take_fields()) field_bytes += f.rgba.size() * 2;
    const f64 mesh_ms = ms(tm);
    field_sum += static_cast<f64>(field_bytes);
    if (!m2.empty() || field_bytes > 0) {
      mesh_sum += mesh_ms;
      mesh_worst = std::max(mesh_worst, mesh_ms);
      ++mesh_ticks;
      mesh_chunks += static_cast<i64>(m2.size());
    }
    const EngineStats s = eng.stats();
    if (tick_ms > 30.0 || t % 60 == 0)
      std::printf("  tick %3d: %6.1f ms (events %.1f, bubbles %d, nodes %d) meshes %zu\n", t, tick_ms, s.event_ms,
                  s.active_bubbles, s.active_nodes, m2.size());
  }
  const EngineStats s = eng.stats();
  std::printf("\nevents %lld: bubbles %lld, static settles %lld; ruptures %lld; detached voxels %lld (%lld pieces); "
              "voxels now %lld\n",
              static_cast<long long>(s.events), static_cast<long long>(s.bubbles_spawned),
              static_cast<long long>(s.static_settles), static_cast<long long>(s.ruptures),
              static_cast<long long>(s.detached_voxels), static_cast<long long>(detached_events),
              static_cast<long long>(s.voxels));
  std::printf("tick: mean %.2f ms, worst %.1f ms; memory %.1f MB; state hash %llu\n", sum_tick / ticks, worst_tick,
              s.memory_mb, static_cast<unsigned long long>(eng.session_hash()));
  {
    std::vector<f64> tt = tick_times;
    std::sort(tt.begin(), tt.end());
    auto pct = [&](f64 p) { return tt.empty() ? 0.0 : tt[std::min(tt.size() - 1, static_cast<size_t>(p * f64(tt.size())))]; };
    i64 over8 = 0, over16 = 0;
    for (f64 v : tt) {
      over8 += v > 8.0 ? 1 : 0;
      over16 += v > 16.7 ? 1 : 0;
    }
    std::printf("tick percentiles%s: p50 %.2f, p95 %.2f, p99 %.2f ms; %lld ticks > 8 ms, %lld > 16.7 ms (of %zu)\n",
                realtime ? " (real-time paced)" : "", pct(0.5), pct(0.95), pct(0.99), static_cast<long long>(over8),
                static_cast<long long>(over16), tt.size());
  }
  if (s.bubble_steps > 0) {
    const f64 n = static_cast<f64>(s.bubble_steps);
    std::printf("bubble steps %lld: %.2f ms/step (solve %.2f, mg rebuild %.2f in %lld steps, law %.2f, topo %.2f, "
                "rest %.2f); pcg %.1f/step, newton %.2f/step\n",
                static_cast<long long>(s.bubble_steps), s.step_ms / n, s.step_solve_ms / n, s.step_mg_ms / n,
                static_cast<long long>(s.step_rebuilds), s.step_law_ms / n, s.step_topo_ms / n,
                (s.step_ms - s.step_solve_ms - s.step_mg_ms - s.step_law_ms - s.step_topo_ms) / n,
                static_cast<f64>(s.step_pcg) / n, static_cast<f64>(s.step_newton) / n);
  }
  std::printf("render updates%s: %lld ticks, %.1f chunks/tick, mean %.2f ms, worst %.1f ms, fields %.0f KB/tick\n",
              gpu_disp ? " (gpu displacement)" : "", static_cast<long long>(mesh_ticks),
              mesh_ticks ? f64(mesh_chunks) / f64(mesh_ticks) : 0.0, mesh_ticks ? mesh_sum / f64(mesh_ticks) : 0.0, mesh_worst,
              mesh_ticks ? field_sum / 1024.0 / f64(mesh_ticks) : 0.0);
  return 0;
}
