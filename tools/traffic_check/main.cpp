// svx_traffic_check — the traffic's acceptance on a streamed world (docs/PROCGEN_MERGE_PLAN.md
// §10.3): its cars drive for minutes with none stuck - a driver that moved less than 2 m over a
// window longer than a signal cycle (45 s: a red light does not explain it), a wreck - and no lane
// near the spawn leads nowhere (its `next` is empty: a dead end the traffic cannot leave).
//
// usage: svx_traffic_check [--preset ID] [--seed N] [--minutes M] [--cars N] [--radius M]
//                          [--threads T]
// The viewer stands at the spawn (the traffic about it). Exit status 1 when a car got stuck or a
// lane within --radius (250 m) of the spawn has no way on.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/procgen/presets.hpp"

using namespace svx;

int main(int argc, char** argv) {
  std::string preset = default_preset_id();
  u64 seed = 0;
  f64 minutes = 10.0, radius = 250.0;
  int cars = 14;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
    if (a == "--preset") preset = next();
    else if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--minutes") minutes = std::atof(next());
    else if (a == "--cars") cars = std::atoi(next());
    else if (a == "--radius") radius = std::atof(next());
    else if (a == "--threads") set_num_threads(std::atoi(next()));
    else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }
  const Preset* p = find_preset(preset);
  Game game;
  std::string err;
  if (!p || !load_preset(game, *p, seed, 0.125, &err)) {
    std::fprintf(stderr, "cannot load preset %s: %s\n", preset.c_str(), p ? err.c_str() : "unknown");
    return 2;
  }
  TrafficConfig t = game.traffic();
  t.enabled = true;
  t.cars = cars;
  game.set_traffic(t);
  const V3 sp = game.spawn_pos();
  game.set_viewer(V3{sp.x, sp.y, sp.z + 1.6});

  // the lanes near the spawn that lead nowhere
  i32 lanes = 0, dead = 0;
  if (const RoadNetwork* roads = game.roads()) {
    std::vector<Lane> ls;
    roads->lanes_in(sp - V3{radius, radius, 0.0}, sp + V3{radius, radius, 0.0}, ls);
    std::vector<std::pair<u64, int>> nx;
    for (const Lane& l : ls) {
      nx.clear();
      roads->next(l.id, nx);
      ++lanes;
      if (nx.empty()) {
        ++dead;
        if (dead <= 5) std::printf("  dead end: lane %llu (%.1f, %.1f) -> (%.1f, %.1f)\n", static_cast<unsigned long long>(l.id), l.a.x, l.a.y, l.b.x, l.b.y);
      }
    }
  } else {
    std::fprintf(stderr, "%s has no roads\n", preset.c_str());
    return 2;
  }

  // the drivers' positions every second; stuck: under 2 m in 45 s
  constexpr int kWindow = 45;
  std::map<u32, std::deque<V3>> track;
  std::map<u32, bool> reported;
  i32 stuck = 0, wrecks = 0, seen = 0;
  f64 moving_km = 0.0;
  const int seconds = static_cast<int>(minutes * 60.0);
  for (int s = 0; s < seconds; ++s) {
    for (int k = 0; k < 60; ++k) {
      game.tick();
      (void)game.take_events();
    }
    std::map<u32, bool> alive;
    for (const VehicleView& v : game.vehicles()) {
      if (!(v.flags & VehicleView::kNpc) || (v.flags & VehicleView::kParked)) continue;
      alive[v.id] = true;
      std::deque<V3>& q = track[v.id];
      if (q.empty()) ++seen;
      if (!q.empty()) moving_km += std::hypot(v.pos.x - q.back().x, v.pos.y - q.back().y) / 1000.0;
      q.push_back(v.pos);
      if (q.size() > kWindow + 1) q.pop_front();
      const bool wreck = (v.flags & VehicleView::kWreck) != 0;
      if (reported[v.id]) continue;
      if (wreck) {
        ++wrecks;
        reported[v.id] = true;
        std::printf("  t=%4d s: car %u a wreck at (%.1f, %.1f)\n", s, v.id, v.pos.x, v.pos.y);
      } else if (q.size() == kWindow + 1 && std::hypot(q.back().x - q.front().x, q.back().y - q.front().y) < 2.0) {
        ++stuck;
        reported[v.id] = true;
        std::printf("  t=%4d s: car %u stuck %d s at (%.1f, %.1f)\n", s, v.id, kWindow, v.pos.x, v.pos.y);
      }
    }
    for (auto it = track.begin(); it != track.end();)
      it = alive.count(it->first) ? std::next(it) : track.erase(it);
  }
  const bool ok = stuck == 0 && wrecks == 0 && dead == 0;
  std::printf("%s: %d lanes within %.0f m of the spawn, %d without a way on; %.0f min of traffic: %d drivers, %.1f km driven, %d stuck, %d wrecks: %s\n",
              preset.c_str(), lanes, radius, dead, minutes, seen, moving_km, stuck, wrecks, ok ? "ok" : "FAIL");
  return ok ? 0 : 1;
}
