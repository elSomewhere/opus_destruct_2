// svx_city_bench — a streamed preset driven through at speed (docs/PROCGEN_MERGE_PLAN.md §11
// phase 4): the viewer moves along a straight line at a fixed speed with the preset's traffic and
// people about it; every few seconds something nearby is shot (a first touch where the structure
// was never touched). Per tick it measures the whole tick (simulation and meshing, as the front
// end's worker runs them), and reports the tick distribution with and without the first-touch
// ticks, the chunks generated a second, and the columns ahead that were not resident.
//
// usage: svx_city_bench [--preset ID] [--seed N] [--speed M/S] [--seconds S] [--warmup S]
//          [--threads T] [--cars N] [--people N] [--shoot-every S] [--heading DEG]
//          [--check MEAN_MS P99_MS]
// --check: the exit status is 1 when the mean tick (all ticks) exceeds MEAN_MS or the 99th
// percentile outside first-touch ticks exceeds P99_MS (the plan's budgets: 16 ms, 33 ms).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/procgen/presets.hpp"

using namespace svx;

namespace {

f64 percentile(std::vector<f64> v, f64 p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<f64>(v.size())))];
}

f64 mean(const std::vector<f64>& v) {
  f64 s = 0.0;
  for (f64 x : v) s += x;
  return v.empty() ? 0.0 : s / static_cast<f64>(v.size());
}

}  // namespace

int main(int argc, char** argv) {
  std::string preset = default_preset_id();
  u64 seed = 0;
  f64 speed = 30.0, seconds = 30.0, warmup = 10.0, shoot_every = 3.0, heading_deg = 0.0;
  int cars = -1, people = -1;
  f64 check_mean = -1.0, check_p99 = -1.0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
    if (a == "--preset") preset = next();
    else if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--speed") speed = std::atof(next());
    else if (a == "--seconds") seconds = std::atof(next());
    else if (a == "--warmup") warmup = std::atof(next());
    else if (a == "--threads") set_num_threads(std::atoi(next()));
    else if (a == "--cars") cars = std::atoi(next());
    else if (a == "--people") people = std::atoi(next());
    else if (a == "--shoot-every") shoot_every = std::atof(next());
    else if (a == "--heading") heading_deg = std::atof(next());
    else if (a == "--check") {
      check_mean = std::atof(next());
      check_p99 = std::atof(next());
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }
  const Preset* p = find_preset(preset);
  if (!p) {
    std::fprintf(stderr, "unknown preset %s\n", preset.c_str());
    return 2;
  }
  Game game;
  std::string err;
  if (!load_preset(game, *p, seed, 0.125, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  if (cars >= 0) {
    TrafficConfig t = game.traffic();
    t.cars = cars;
    t.enabled = cars > 0;
    game.set_traffic(t);
  }
  if (people >= 0) {
    PedestrianConfig q = game.pedestrians();
    q.count = people;
    q.enabled = people > 0;
    game.set_pedestrians(q);
  }
  const V3 sp = game.spawn_pos();
  const f64 hd = heading_deg * 3.141592653589793 / 180.0;
  const V3 dir{std::cos(hd), std::sin(hd), 0.0};
  const V3 eye0{sp.x, sp.y, sp.z + 1.6};
  game.set_viewer(eye0);
  const MeshOptions mo{};
  auto timed_tick = [&]() {
    const auto t0 = std::chrono::steady_clock::now();
    game.tick();
    (void)game.take_events();
    (void)game.take_meshes(mo);
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  for (int t = 0; t < static_cast<int>(warmup * 60.0); ++t) timed_tick();  // (the start streams in)

  const int ticks = static_cast<int>(seconds * 60.0);
  const int shoot_ticks = shoot_every > 0.0 ? std::max(1, static_cast<int>(shoot_every * 60.0)) : 0;
  std::vector<f64> all, ordinary, touches;
  all.reserve(size_t(ticks));
  const i64 gen0 = game.world().stats().generated_total;
  const i64 ext0 = game.world().stats().extractions;
  i64 missing_ahead = 0, ahead_checked = 0, shots = 0, shot_hits = 0;
  const f64 h = game.grid().h;
  for (int t = 0; t < ticks; ++t) {
    const f64 d = speed * (t + 1) / 60.0;
    const V3 eye{eye0.x + dir.x * d, eye0.y + dir.y * d, eye0.z};
    game.set_viewer(eye);
    bool shot = false;
    if (shoot_ticks > 0 && t % shoot_ticks == shoot_ticks / 2) {
      // (something beside the way: a ray to the side, then down a little)
      const V3 side{-dir.y, dir.x, -0.15};
      const RayHit r = game.world().raycast(eye, side, 60.0);
      ++shots;
      if (r.hit) {
        game.shoot(r.pos, 0.25, 2000.0);
        ++shot_hits;
        shot = true;
      }
    }
    const f64 ms = timed_tick();
    all.push_back(ms);
    (shot ? touches : ordinary).push_back(ms);  // (a shot's tick: a first touch where nothing was)
    // the columns 0..64 m ahead at the viewer's height: resident?
    if (t % 30 == 0)
      for (f64 a = 8.0; a <= 64.0; a += 8.0) {
        const V3 q{eye.x + dir.x * a, eye.y + dir.y * a, eye.z - 2.0};
        const IVec3 c{static_cast<i32>(std::floor(q.x / h + 0.5)) >> kChunkBits, static_cast<i32>(std::floor(q.y / h + 0.5)) >> kChunkBits,
                      static_cast<i32>(std::floor(q.z / h + 0.5)) >> kChunkBits};
        ++ahead_checked;
        if (!game.world().chunk_resident(c)) ++missing_ahead;
      }
  }
  const WorldStats st = game.world().stats();
  const f64 gen_per_s = static_cast<f64>(st.generated_total - gen0) / seconds;
  std::printf("%s at %.0f m/s for %.0f s (%d threads): generated %.0f chunks/s; %lld of %lld columns ahead (0-64 m) not resident\n",
              p->id.c_str(), speed, seconds, num_threads(), gen_per_s, static_cast<long long>(missing_ahead), static_cast<long long>(ahead_checked));
  std::printf("ticks: mean %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f\n", mean(all), percentile(all, 0.5), percentile(all, 0.95),
              percentile(all, 0.99), percentile(all, 1.0));
  std::printf("outside first touches: mean %.2f ms, p99 %.2f ms; first touches: %zu ticks (shots %lld, hits %lld), mean %.2f ms, max %.2f ms\n",
              mean(ordinary), percentile(ordinary, 0.99), touches.size(), static_cast<long long>(shots), static_cast<long long>(shot_hits), mean(touches),
              percentile(touches, 1.0));
  std::printf("extractions %lld; cars %d, people %d; memory %.0f MB (sources %.0f MB)\n", static_cast<long long>(st.extractions - ext0),
              game.traffic().enabled ? game.traffic().cars : 0, game.pedestrians().enabled ? game.pedestrians().count : 0,
              static_cast<f64>(game.world().memory().total()) / 1048576.0, static_cast<f64>(game.world().memory().sources) / 1048576.0);
  if (check_mean >= 0.0) {
    const bool ok = mean(all) <= check_mean && percentile(ordinary, 0.99) <= check_p99;
    std::printf("%s: mean %.2f <= %.2f ms, p99 outside first touches %.2f <= %.2f ms\n", ok ? "ok" : "FAIL", mean(all), check_mean,
                percentile(ordinary, 0.99), check_p99);
    return ok ? 0 : 1;
  }
  return 0;
}
