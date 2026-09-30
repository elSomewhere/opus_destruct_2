// svx_stream_bench — streaming cost while flying through the 1 km^2 procedural city (plan Phase 6
// gate: no hitch > 2 ms): the viewer flies along a street at a fixed speed; per tick it reports
// the engine's stream work (chunk generation, eviction, far tiles) on the simulation thread.
//
// usage: svx_stream_bench [--speed M_PER_S] [--seconds S] [--seed N] [--chunks-per-tick C] [--threads T]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/procgen/city.hpp"

using namespace svx;

int main(int argc, char** argv) {
  f64 speed = 20.0, seconds = 20.0;
  u64 seed = 2;
  int cpt = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--speed" && i + 1 < argc) speed = std::atof(argv[++i]);
    else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--chunks-per-tick" && i + 1 < argc) cpt = std::atoi(argv[++i]);
    else if (a == "--threads" && i + 1 < argc) set_num_threads(std::atoi(argv[++i]));
  }
  Game eng;
  auto src = make_city_source(seed, 1000.0, 0.125);
  const auto sp = src->spawn_pos();
  VoxelGrid g;
  eng.load(std::move(g), sp, src->spawn_dir());
  StreamConfig sc;
  if (cpt > 0) sc.chunks_per_tick = cpt;
  eng.load_streaming(std::move(src), eng.grid().h, sc);
  eng.set_viewer(sp);
  const MeshOptions mo{};
  for (int t = 0; t < 600; ++t) {  // the start area streams in and bakes (10 s)
    eng.tick();
    (void)eng.take_events();
    (void)eng.take_meshes(mo);
  }
  const int ticks = static_cast<int>(seconds * 60.0);
  std::vector<f64> ms, mesh;
  ms.reserve(size_t(ticks));
  mesh.reserve(size_t(ticks));
  i64 gen0 = eng.stats().generated_total, ev0 = eng.stats().evicted_total;
  f64 worst_tick = 0.0;
  for (int t = 0; t < ticks; ++t) {
    const f64 x = sp[0] + speed * (t + 1) / 60.0;
    eng.set_viewer({x, sp[1], sp[2]});
    eng.tick();
    (void)eng.take_events();
    const auto t0 = std::chrono::steady_clock::now();
    (void)eng.take_meshes(mo);
    mesh.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
    ms.push_back(eng.stats().stream_ms);
    worst_tick = std::max(worst_tick, eng.stats().tick_ms);
  }
  auto report = [&](const char* what, std::vector<f64> s) {
    std::sort(s.begin(), s.end());
    auto pct = [&](f64 p) { return s.empty() ? 0.0 : s[std::min(s.size() - 1, size_t(p * static_cast<f64>(s.size())))]; };
    int over2 = 0;
    for (f64 v : s) over2 += v > 2.0 ? 1 : 0;
    std::printf("%s ms/tick: p50 %.2f, p95 %.2f, p99 %.2f, max %.2f; %d of %zu ticks over 2 ms\n", what, pct(0.5), pct(0.95),
                pct(0.99), s.empty() ? 0.0 : s.back(), over2, s.size());
  };
  std::printf("flight %.0f m at %.0f m/s: generated %lld chunks, evicted %lld; resident %lld\n", speed * seconds, speed,
              static_cast<long long>(eng.stats().generated_total - gen0), static_cast<long long>(eng.stats().evicted_total - ev0),
              static_cast<long long>(eng.world().stats().resident_chunks));
  report("stream (simulation thread)", ms);
  report("meshing (worker, after the tick)", mesh);
  std::printf("worst tick %.2f ms\n", worst_tick);
  return 0;
}
