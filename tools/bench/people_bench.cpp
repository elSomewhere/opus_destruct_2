// svx_people_bench — what the drive city's people cost (docs/ANIM.md): the viewer goes along an
// avenue at driving speed (the population streams in ahead and out behind), with the traffic,
// for each way of simulating the bodies and each crowd:
//   deep     every physical body an articulation of the world (every contact real, both ways),
//   shallow  every physical body its own (the original's XPBD; the world through its collision),
//   hybrid   deep near the viewer and near moving pieces, shallow further, on the plan alone far;
// and reports per tick the whole tick and the characters' part (mean, 95th percentile, worst),
// the bodies' split, the people made and gone, and the memory.
//
// usage: svx_people_bench [--seconds S] [--speed M_PER_S] [--counts 24,48,96] [--policies deep,shallow,hybrid]
//                         [--threads T] [--seed N] [--no-traffic]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "svx/anim/system.hpp"
#include "svx/base/parallel.hpp"
#include "svx/game/drive_city.hpp"
#include "svx/game/game.hpp"

using namespace svx;

namespace {

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a <= s.size()) {
    const size_t b = s.find(',', a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

f64 pct(std::vector<f64> v, f64 q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<f64>(v.size())))];
}

}  // namespace

int main(int argc, char** argv) {
  f64 seconds = 30.0, speed = 8.0;
  u64 seed = 11;
  bool traffic = true;
  std::vector<std::string> counts = {"24", "48", "96"}, policies = {"deep", "shallow", "hybrid"};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    else if (a == "--speed" && i + 1 < argc) speed = std::atof(argv[++i]);
    else if (a == "--counts" && i + 1 < argc) counts = split(argv[++i]);
    else if (a == "--policies" && i + 1 < argc) policies = split(argv[++i]);
    else if (a == "--threads" && i + 1 < argc) set_num_threads(std::atoi(argv[++i]));
    else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--no-traffic") traffic = false;
  }
  std::printf("%-8s %6s | %8s %8s %8s | %8s %8s %8s | %5s %5s %5s %5s | %5s %5s | %8s %8s\n", "bodies", "people", "tick", "p95", "worst", "chars", "p95", "worst", "deep",
              "shal", "plan", "rest", "made", "gone", "chars MB", "world MB");
  for (const std::string& pol : policies) {
    for (const std::string& cs : counts) {
      const i32 n = std::atoi(cs.c_str());
      Game game;
      std::shared_ptr<GameSource> src = make_drive_city(seed);
      StreamConfig sc;
      sc.load_radius = 100.0;
      sc.evict_radius = 130.0;
      game.load_streaming(src, 0.125, sc);
      TrafficConfig tc;
      tc.enabled = traffic;
      game.set_traffic(tc);
      PedestrianConfig pc;
      pc.count = n;
      pc.bodies = pol == "deep" ? 0 : pol == "shallow" ? 1 : 2;
      pc.max_deep = n;
      game.set_pedestrians(pc);
      V3 eye = src->spawn_pos();
      game.set_viewer(eye);
      // (the ground about the start, and the people)
      for (int t = 0; t < 60 * 15; ++t) game.tick();
      std::vector<f64> tick_ms, char_ms;
      f64 deep = 0.0, shallow = 0.0, plan = 0.0, rest = 0.0;
      std::set<u32> seen, before;
      for (const CharacterView& v : game.character_views()) before.insert(v.id);
      const int ticks = static_cast<int>(seconds * 60.0);
      for (int t = 0; t < ticks; ++t) {
        eye.x += speed / 60.0;
        game.set_viewer(eye);
        const auto t0 = std::chrono::steady_clock::now();
        game.tick();
        tick_ms.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
        const anim::CharacterSystem* chars = game.characters();
        if (!chars) continue;
        const anim::CharacterStats st = chars->stats();
        char_ms.push_back(st.pre_ms + st.step_ms);
        deep += st.deep;
        shallow += st.shallow;
        plan += st.plan_only;
        rest += st.asleep;
        if (t % 30 == 0)
          for (const CharacterView& v : game.character_views()) seen.insert(v.id);
      }
      std::set<u32> now;
      for (const CharacterView& v : game.character_views()) now.insert(v.id);
      i32 made = 0, gone = 0;
      for (u32 id : seen) made += before.count(id) ? 0 : 1;
      for (u32 id : before) gone += now.count(id) ? 0 : 1;
      auto mean = [](const std::vector<f64>& v) {
        f64 s = 0.0;
        for (f64 x : v) s += x;
        return v.empty() ? 0.0 : s / static_cast<f64>(v.size());
      };
      const MemoryReport mem = game.world().memory();
      const f64 k = 1.0 / std::max(1, ticks);
      std::printf("%-8s %6d | %8.2f %8.2f %8.1f | %8.2f %8.2f %8.1f | %5.1f %5.1f %5.1f %5.1f | %5d %5d | %8.2f %8.1f\n", pol.c_str(), n, mean(tick_ms), pct(tick_ms, 0.95),
                  pct(tick_ms, 1.0), mean(char_ms), pct(char_ms, 0.95), pct(char_ms, 1.0), deep * k, shallow * k, plan * k, rest * k, made, gone,
                  static_cast<f64>(mem.systems) / 1048576.0, static_cast<f64>(mem.total()) / 1048576.0);
    }
  }
  return 0;
}
