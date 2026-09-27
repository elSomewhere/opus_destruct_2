// svx_map_check — plan Phase 5 gate: every Freedoom map imports and bakes with no spontaneous
// collapse.
//
// For each map of the given WADs: voxelize (default options), load, bake the static baseline
// with the design pass (whole world, or tile by tile when the world is larger than the whole-
// world bake limit), and report the self-weight utilization before / after strengthening. A map
// stands at rest when no bond reaches its onset after strengthening (max utilization after < 1,
// no unfixable bonds); then an idle run of the engine must not rupture or detach anything.
// With --movers the map's movers (doors, lifts, floors, ceilings, platforms, crushers, stairs)
// are attached and every move of every mover is run in turn: each must arrive (To), come back
// (Return) or keep cycling (Cycle), and none may rupture or detach anything.
//
//   svx_map_check [--threads T] [--idle-ticks N] [--map NAME] [--movers] file.wad [more.wad ...]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/doom/movers.hpp"
#include "svx/doom/world.hpp"
#include "svx/engine/engine.hpp"

using namespace svx;

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::vector<std::string> wads;
  std::string only;
  int idle = 120;
  bool movers = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--threads" && i + 1 < argc) set_num_threads(std::atoi(argv[++i]));
    else if (a == "--idle-ticks" && i + 1 < argc) idle = std::atoi(argv[++i]);
    else if (a == "--map" && i + 1 < argc) only = argv[++i];
    else if (a == "--movers") movers = true;
    else wads.push_back(a);
  }
  auto secs = [](auto t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  int maps = 0, failed = 0;
  std::printf("%-8s %9s %9s %8s %7s %7s %8s %6s %6s %8s %6s %6s %s\n", "map", "voxels", "struct", "bake s", "util", "after",
              "strength", "unfix", "float", "detached", "rupt", "S_p<=", "verdict");
  for (const std::string& path : wads) {
    std::ifstream f(path, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    doom::Wad wad;
    std::string err;
    if (!wad.load_memory(std::move(bytes), &err)) {
      std::printf("cannot read %s: %s\n", path.c_str(), err.c_str());
      return 1;
    }
    for (const std::string& name : wad.map_names()) {
      if (!only.empty() && name != only) continue;
      ++maps;
      const auto t0 = std::chrono::steady_clock::now();
      doom::DoomWorld dw;
      if (!doom::build_doom_world(wad, name, {}, 0.125, false, &dw, &err)) {
        std::printf("%-8s voxelization failed: %s\n", name.c_str(), err.c_str());
        ++failed;
        continue;
      }
      Engine eng;
      EngineParams par;
      par.fragility = 0.25;
      eng.set_params(par);
      eng.load(std::move(dw.grid), dw.spawn_pos, dw.spawn_dir);
      dw.live = &eng.grid();
      eng.set_compliance_cap(dw.slenderness.max_compliance_p99);
      const auto tb = std::chrono::steady_clock::now();
      if (!eng.bake())
        while (eng.bake_tile()) {
        }
      const f64 bake_s = secs(tb);
      i64 structural = 0;
      for (const auto& [k, ch] : eng.grid().chunks()) {
        if (ch.uniform) {
          if (vox_solid(ch.value) && !vox_anchored(ch.value)) structural += kChunkVox;
          continue;
        }
        for (Vox v : ch.v) structural += (vox_solid(v) && !vox_anchored(v)) ? 1 : 0;
      }
      for (int t = 0; t < idle; ++t) {
        eng.tick();
        (void)eng.take_events();
      }
      const auto& d = eng.design_report();
      const EngineStats s = eng.stats();
      const bool stands = d.unfixable_bonds == 0 && d.max_utilization_after < 1.0 && s.ruptures == 0 && s.detached_voxels == 0;
      if (!stands || std::getenv("SVX_DESIGN_WORST")) {
        static const char* comp[6] = {"N", "V1", "V2", "T", "M1", "M2"};
        std::printf("         worst bond before design: voxel (%d %d %d) axis %d, material %d, %s-dominated\n", d.worst[0],
                    d.worst[1], d.worst[2], d.worst_axis, int(d.worst_mat), comp[d.worst_component]);
      }
      std::string mover_note;
      bool movers_ok = true;
      if (movers) {
        eng.set_viewer({-1e3, -1e3, -1e3});  // nobody in the way
        const int n = doom::attach_doom_movers(eng, dw);
        int kinds[4] = {0, 0, 0, 0}, moves = 0, arrived = 0, cycling = 0, failed_moves = 0;
        size_t kmax = 0;
        for (i32 id = 0; id < n; ++id) {
          ++kinds[static_cast<int>(eng.mover_def(id)->kind)];
          kmax = std::max(kmax, eng.mover_def(id)->moves.size());
        }
        // every move in turn: move k of every mover that has one, then run until nothing moves
        for (size_t k = 0; k < kmax; ++k) {
          std::vector<std::pair<i32, i32>> started;  // (mover, rows expected when done; -1 cycling)
          for (i32 id = 0; id < n; ++id) {
            const MoverDef& md = *eng.mover_def(id);
            if (k >= md.moves.size() || md.moves[k].type == MoverMove::Type::Stop) continue;
            const MoverMove& mv = md.moves[k];
            const i32 before = eng.mover_rows(id);
            if (!eng.activate_mover(id, static_cast<i32>(k))) continue;
            ++moves;
            const i32 H = md.z1 - md.z0;
            const i32 end = mv.type == MoverMove::Type::To ? std::clamp(mv.target, 0, H)
                            : mv.type == MoverMove::Type::Return ? (mv.back >= 0 ? std::clamp(mv.back, 0, H) : before)
                                                                   : -1;
            started.emplace_back(id, end);
          }
          for (int t = 0; t < 60 * 90; ++t) {
            bool busy = false;
            for (const auto& [id, end] : started) busy = busy || (end >= 0 && eng.mover_busy(id));
            if (!busy) break;
            eng.tick();
            (void)eng.take_events();
          }
          for (const auto& [id, end] : started) {
            if (end < 0) {
              ++cycling;
              for (i32 q = 0; q < static_cast<i32>(eng.mover_def(id)->moves.size()); ++q)
                if (eng.mover_def(id)->moves[size_t(q)].type == MoverMove::Type::Stop) eng.activate_mover(id, q);
              continue;
            }
            if (!eng.mover_busy(id) && eng.mover_rows(id) == end) ++arrived;
            else ++failed_moves;
          }
        }
        const EngineStats s2 = eng.stats();
        movers_ok = failed_moves == 0 && s2.ruptures == s.ruptures && s2.detached_voxels == s.detached_voxels;
        char buf[256];
        std::snprintf(buf, sizeof buf, " movers %d (%d doors, %d lifts, %d floors, %d ceilings): %d moves, %d arrived, %d cycling, %d stuck%s",
                      n, kinds[0], kinds[1], kinds[2], kinds[3], moves, arrived, cycling, failed_moves,
                      movers_ok ? "" : " MOVERS FAIL");
        mover_note = buf;
      }
      if (!stands || !movers_ok) ++failed;
      std::printf("%-8s %9lld %9lld %8.1f %7.2f %7.2f %8lld %6lld %6lld %8lld %6lld %6.1f %s (%.0f s)\n", name.c_str(),
                  static_cast<long long>(s.voxels), static_cast<long long>(structural), bake_s, d.max_utilization,
                  d.max_utilization_after, static_cast<long long>(d.strengthened_voxels),
                  static_cast<long long>(d.unfixable_bonds), static_cast<long long>(d.floating_voxels),
                  static_cast<long long>(s.detached_voxels), static_cast<long long>(s.ruptures),
                  std::min(eng.compliance_cap() > 0.0 ? eng.compliance_cap() : 99.9, 99.9), stands ? "stands" : "FAILS",
                  secs(t0));
      if (movers) std::printf("%-8s%s\n", "", mover_note.c_str());
    }
  }
  std::printf("\n%d maps, %d stand, %d fail\n", maps, maps - failed, failed);
  return failed == 0 ? 0 : 1;
}
