// svx_env_bench — deterministic environment scenarios, timed: the yard's timber house burning,
// the reservoir breached, the streamed city crossed with fires and water. Prints per scenario
// the tick cost (mean, 99th percentile, max), the environment's share, and the session hash:
// an optimization that changes no result keeps every hash.
//
// usage: svx_env_bench [--scenario fire|flood|city|all] [--threads T] [--repeat N] [--slow MS] [--tune NAME=VALUE ...]
//          [--archive-mb MB]  (the streamed city's change archive; 0: keep every change)
//          [--budget-ms MS]   (a gate: exit status 1 if a scenario's mean tick is slower)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/procgen/city.hpp"
#include "svx/procgen/levels.hpp"

using namespace svx;

namespace {

f64 g_slow = 0.0;         // (--slow MS: the breakdown of ticks slower than this)
f64 g_archive_mb = -1.0;  // (--archive-mb: the streamed city's change archive; < 0: its default)
f64 g_budget_ms = 0.0;    // (--budget-ms: a scenario's mean tick slower than this fails the run; 0: none)

struct Result {
  std::vector<f64> tick_ms;
  f64 env_ms = 0.0;
  u64 hash = 0, world_hash = 0;  // (the session's; the world's alone - docs/BASELINE.md)
  std::string note;
};

// (--tune name=value: the world's tunables, set on every scenario's game before its load)
std::vector<std::pair<std::string, f64>> g_tunes;
void tune(Game& g) {
  for (const auto& [name, value] : g_tunes)
    if (!g.set_tunable(name.c_str(), value)) std::fprintf(stderr, "unknown tunable %s\n", name.c_str());
}

void take_all(Game& g) {
  g.take_meshes(MeshOptions{});
  g.take_removed_chunks();
  g.take_far_meshes();
  g.take_far_removed();
  g.take_events();
  g.take_water_meshes();
  g.take_water_removed();
  g.flames(4096);
  g.smoke(4096);
}

Result run(Game& g, i64 ticks, const std::function<void(Game&, i64)>& script) {
  Result r;
  r.tick_ms.reserve(size_t(ticks));
  for (i64 t = 0; t < ticks; ++t) {
    script(g, t);
    const auto t0 = std::chrono::steady_clock::now();
    g.tick();
    r.tick_ms.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
    const GameStats st = g.stats();
    r.env_ms += st.env_ms;
    if (g_slow > 0.0 && r.tick_ms.back() > g_slow)
      std::printf("  slow tick %lld: %.1f ms (world %.1f: structural %.1f rigid %.1f events %.1f loads %.1f stream %.1f systems %.1f upkeep %.1f; solving %d nodes %lld, extractions %lld, pieces %d awake %d)\n",
                  static_cast<long long>(t), r.tick_ms.back(), st.tick_ms, st.structural_ms, st.rigid_ms, st.event_ms, st.loads_ms, st.stream_ms, st.systems_ms, st.upkeep_ms, st.solving,
                  static_cast<long long>(st.solve_nodes), static_cast<long long>(st.extractions), st.bodies, st.awake);
    take_all(g);
    if (std::getenv("SVX_TRACE_HASH"))  // (each tick's world hash: where two builds part)
      std::printf("[t%lld] world %016llx pieces %d\n", static_cast<long long>(t), static_cast<unsigned long long>(g.world().session_hash()),
                  g.stats().bodies);
  }
  r.hash = g.session_hash();
  r.world_hash = g.world().session_hash();
  return r;
}

void load_yard(Game& g) {
  Level w = make_procedural("yard", 1, 0.125);
  g.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  g.bake();
}

Result fire() {
  Game g;
  tune(g);
  load_yard(g);
  const f64 h = 0.125;
  Result r = run(g, 60 * 120, [&](Game& e, i64 t) {
    if (t == 10) e.ignite({h * 30, h * 30, h * 2.5}, 0.4);
    if (t == 20) e.ignite({h * 56, h * 72, h * 12}, 0.3);
    for (int x : {250, 263})
      for (int y : {92, 105})
        if (t == 30) e.ignite({h * (x + 1), h * (y + 1), h * 2}, 0.35);
  });
  const auto& fs = g.env().fire()->stats();
  char buf[160];
  std::snprintf(buf, sizeof buf, "burnt out %lld, ignited %lld, smoke blocks %d", static_cast<long long>(fs.burnt_out),
                static_cast<long long>(fs.ignited), g.env().smoke()->stats().blocks);
  r.note = buf;
  return r;
}

Result flood() {
  Game g;
  tune(g);
  load_yard(g);
  const f64 h = 0.125;
  Result r = run(g, 60 * 30, [&](Game& e, i64 t) {
    if (t == 30) e.blast({h * 268, h * 149, h * 4}, 1.0, 1e6);
    if (t >= 600 && t < 900 && t % 6 == 0) e.pour({h * 120, h * 60, h * 20}, 0.3);
  });
  char buf[160];
  std::snprintf(buf, sizeof buf, "moving %d, loads %d, afloat %d", g.env().water()->stats().active, g.env().water()->stats().loads,
                g.env().water()->stats().floating);
  r.note = buf;
  return r;
}

Result city() {
  Game g;
  tune(g);
  const f64 h = 0.125;
  StreamConfig sc;
  if (g_archive_mb >= 0.0) sc.archive_mb = g_archive_mb;
  g.load_streaming(make_city_source(7, 2000.0, h), h, sc);
  g.bake();
  const V3 start = g.spawn_pos();
  u64 s = 0x9E3779B97F4A7C15ull;
  auto rnd = [&]() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
  };
  V3 eye = start;
  Result r = run(g, 60 * 90, [&](Game& e, i64 t) {
    const f64 a = 12.0 * t / 60.0 / 200.0;
    eye = V3{start.x + 200.0 * std::sin(a), start.y + 200.0 * (1.0 - std::cos(a)), start.z + 1.6};
    e.set_viewer(eye);
    if (t % 120 == 0) {
      const f64 ang = 6.2831853 * rnd();
      const RayHit hit = e.world().raycast(eye, {std::cos(ang), std::sin(ang), -0.05 + 0.3 * rnd()}, 80.0);
      if (hit.hit) e.blast(hit.pos, 1.0, 4e5);
    }
    if (t % 150 == 75) {
      const f64 ang = 6.2831853 * rnd();
      const RayHit hit = e.world().raycast(eye, {std::cos(ang), std::sin(ang), -0.1 + 0.4 * rnd()}, 60.0);
      if (hit.hit) {
        if (rnd() < 0.6) e.ignite(hit.pos, 0.5);
        else e.pour(hit.pos + hit.normal * 0.4, 0.4);
      }
    }
  });
  const MemoryReport m = g.world().memory();
  char buf[160];
  std::snprintf(buf, sizeof buf, "world %.0f MB (systems %.2f MB), pieces %d", m.total() / 1048576.0, m.systems / 1048576.0, m.piece_count);
  r.note = buf;
  return r;
}

// (true: within the budget)
bool report(const char* name, Result r) {
  std::vector<f64> v = r.tick_ms;
  std::sort(v.begin(), v.end());
  f64 sum = 0.0;
  for (f64 x : v) sum += x;
  const f64 mean = v.empty() ? 0.0 : sum / static_cast<f64>(v.size());
  const f64 p99 = v.empty() ? 0.0 : v[std::min(v.size() - 1, v.size() * 99 / 100)];
  std::printf("%-6s ticks %6zu  mean %7.3f ms  p99 %7.2f ms  max %7.1f ms  env %7.3f ms/tick  hash %016llx  world %016llx  %s\n", name, v.size(), mean,
              p99, v.empty() ? 0.0 : v.back(), r.env_ms / std::max<size_t>(1, v.size()), static_cast<unsigned long long>(r.hash),
              static_cast<unsigned long long>(r.world_hash),
              r.note.c_str());
  if (g_budget_ms > 0.0 && mean > g_budget_ms) {
    std::printf("%-6s OVER BUDGET: mean tick %.3f ms > %.3f ms\n", name, mean, g_budget_ms);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string which = "all";
  int threads = 0, repeat = 1;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
    if (arg("--scenario")) which = argv[++i];
    else if (arg("--threads")) threads = std::atoi(argv[++i]);
    else if (arg("--repeat")) repeat = std::atoi(argv[++i]);
    else if (arg("--slow")) g_slow = std::atof(argv[++i]);
    else if (arg("--archive-mb")) g_archive_mb = std::atof(argv[++i]);
    else if (arg("--budget-ms")) g_budget_ms = std::atof(argv[++i]);
    else if (arg("--tune")) {
      const std::string kv = argv[++i];
      const size_t eq = kv.find('=');
      if (eq != std::string::npos) g_tunes.emplace_back(kv.substr(0, eq), std::atof(kv.c_str() + eq + 1));
    }
  }
  if (threads > 0) set_num_threads(threads);
  bool ok = true;
  for (int k = 0; k < std::max(1, repeat); ++k) {
    if (which == "all" || which == "fire") ok = report("fire", fire()) && ok;
    if (which == "all" || which == "flood") ok = report("flood", flood()) && ok;
    if (which == "all" || which == "city") ok = report("city", city()) && ok;
  }
  return ok ? 0 : 1;
}
