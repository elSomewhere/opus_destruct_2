// svx_soak — long sessions and what they cost in memory: a streamed city crossed for minutes
// with continuous destruction, fires and water, the drive city with its traffic and people (the
// front end's characters, meshes and blood taken every tick), or a bounded level shot at and
// reloaded. Prints the world's memory report (World::memory, the environment systems and the
// characters included) and the process's resident size at intervals; the check is that nothing
// keeps growing - with --check, the exit status says so (1: the second half of the session held
// more than the first half's peak allows).
//
// usage: svx_soak [--world city|drive|tower|rooms|yard] [--wad F --map M] [--minutes M] [--report S]
//          [--speed M/S] [--extent KM] [--archive-mb MB] [--forget-s S] [--threads T] [--no-shoot]
//          [--no-env] [--people N] [--check]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/game/doom/world.hpp"
#include "svx/game/game.hpp"
#include "svx/procgen/city.hpp"
#include "svx/procgen/drive_city.hpp"
#include "svx/procgen/levels.hpp"

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

using namespace svx;

namespace {

// The process's memory: its physical footprint on macOS (dirty and compressed pages: what it
// costs the system; the resident size also counts clean allocator pages the system may take back
// at will), the resident size elsewhere.
f64 rss_mb() {
#if defined(__APPLE__)
  task_vm_info_data_t info;
  mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS)
    return static_cast<f64>(info.phys_footprint) / 1048576.0;
  return 0.0;
#elif defined(__linux__)
  long pages = 0, resident = 0;
  if (FILE* f = std::fopen("/proc/self/statm", "r")) {
    if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
    std::fclose(f);
  }
  return static_cast<f64>(resident) * 4096.0 / 1048576.0;
#else
  return 0.0;
#endif
}

f64 mb(i64 b) { return static_cast<f64>(b) / 1048576.0; }

// deterministic pseudo-random numbers
struct Rng {
  u64 s;
  f64 next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // (line by line, also into a pipe or a file)
  std::string world = "city", wad_path, map_name = "MAP01";
  f64 minutes = 10.0, report_s = 30.0, speed = 12.0, extent_km = 16.0, archive_mb = -1.0, forget_s = -1.0;
  int threads = 0, people = 24;
  bool shoot = true, env = true, check = false;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
    if (arg("--world")) world = argv[++i];
    else if (arg("--wad")) wad_path = argv[++i];
    else if (arg("--map")) map_name = argv[++i];
    else if (arg("--minutes")) minutes = std::atof(argv[++i]);
    else if (arg("--report")) report_s = std::atof(argv[++i]);
    else if (arg("--speed")) speed = std::atof(argv[++i]);
    else if (arg("--extent")) extent_km = std::atof(argv[++i]);
    else if (arg("--archive-mb")) archive_mb = std::atof(argv[++i]);
    else if (arg("--forget-s")) forget_s = std::atof(argv[++i]);
    else if (arg("--threads")) threads = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--no-shoot") == 0) shoot = false;
    else if (arg("--people")) people = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--no-env") == 0) env = false;
    else if (std::strcmp(argv[i], "--check") == 0) check = true;
  }
  if (threads > 0) set_num_threads(threads);
  const f64 h = 0.125;
  Game game;
  std::unique_ptr<doom::DoomWorld> dw;
  bool streamed = false;
  if (!wad_path.empty()) {
    std::ifstream in(wad_path, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    doom::Wad wad;
    std::string err;
    dw = std::make_unique<doom::DoomWorld>();
    if (!wad.load_memory(bytes, &err) || !doom::build_doom_world(wad, map_name, doom::VoxelizeOptions{}, h, false, dw.get(), &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    VoxelGrid g = std::move(dw->grid);
    game.load(std::move(g), dw->spawn_pos, dw->spawn_dir);
    world = map_name;
  } else if (world == "city") {
    StreamConfig sc;
    if (archive_mb >= 0.0) sc.archive_mb = archive_mb;
    if (forget_s >= 0.0) sc.forget_after_s = forget_s;
    game.load_streaming(make_city_source(7, extent_km * 1000.0, h), h, sc);
    streamed = true;
  } else if (world == "drive") {
    // the endless drive city: its traffic and its people about the viewer
    StreamConfig sc;
    if (archive_mb >= 0.0) sc.archive_mb = archive_mb;
    if (forget_s >= 0.0) sc.forget_after_s = forget_s;
    game.load_streaming(make_drive_city(7), h, sc);
    TrafficConfig tc;
    tc.enabled = true;
    game.set_traffic(tc);
    PedestrianConfig pc;
    pc.enabled = people > 0;
    pc.count = std::max(0, people);
    game.set_pedestrians(pc);
    streamed = true;
  } else {
    Level w = make_procedural(world, 1, h);
    game.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
    add_grids(game.world(), std::move(w.grids));
  }
  game.bake();
  const V3 start = game.spawn_pos();
  Rng rng{0x9E3779B97F4A7C15ull};
  const f64 dt = game.config().dt;
  const i64 ticks = static_cast<i64>(minutes * 60.0 / dt);
  const i64 every = std::max<i64>(1, static_cast<i64>(report_s / dt));
  // (--check: the reports' world and process sizes, in order)
  std::vector<std::pair<f64, f64>> sizes;
  std::printf("world %s, %.0f simulated minutes%s\n", world.c_str(), minutes, streamed ? " (streamed: travelling and shooting)" : "");
  std::printf("%7s %8s %8s | %7s %7s %7s %7s %7s %7s %7s %7s | %6s %5s %6s %6s %6s | %6s %6s %6s | %6s\n", "min", "world", "process", "grid",
              "frags", "struct", "pieces", "archive", "caches", "queues", "systems", "chunks", "strs", "pieces", "arch", "forgot", "burn",
              "smoke", "water", "tick");
  const auto t0 = std::chrono::steady_clock::now();
  f64 tick_ms = 0.0;
  i64 n_ticks = 0;
  V3 eye{start.x, start.y, start.z + 1.6};
  for (i64 t = 0; t < ticks; ++t) {
    const f64 s = t * dt;
    if (streamed) {
      // a wide loop around the start (new ground for a long time, then back over old ground)
      const f64 R = std::max(150.0, speed * 60.0 * minutes / (2.0 * 3.14159265358979 * 1.5));
      const f64 a = speed * s / R;
      eye = V3{start.x + R * std::sin(a), start.y + R * (1.0 - std::cos(a)), start.z + 1.6};
      game.set_viewer(eye);
    }
    // destruction: a blast or a burst of carves every two seconds at what is in front (and in
    // the drive city a round into someone near now and then)
    if (shoot && world == "drive" && t % 120 == 60) {
      for (const CharacterView& v : game.character_views()) {
        if (!(v.flags & CharacterView::kAlive) || std::hypot(v.centre.x - eye.x, v.centre.y - eye.y) > 30.0) continue;
        const V3 d{v.centre.x - eye.x, v.centre.y - eye.y, v.centre.z + 0.3 - eye.z};
        const f64 l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const Game::ShotHit hit = game.raycast_shot(eye, V3{d.x / l, d.y / l, d.z / l}, 40.0);
        if (hit.character != 0) game.wound_character(hit.character, hit.pos, 0.15, 500.0);
        break;
      }
    }
    if (shoot && t % 120 == 0) {
      const f64 ang = 6.2831853 * rng.next();
      const V3 dir{std::cos(ang), std::sin(ang), -0.05 + 0.3 * rng.next()};
      const RayHit hit = game.world().raycast(eye, dir, 80.0);
      if (hit.hit) {
        if (rng.next() < 0.5) game.blast(hit.pos, 0.9 + 0.6 * rng.next(), 4e5);
        else
          for (int k = 0; k < 6; ++k) game.carve(hit.pos + V3{0.2 * k, 0.1 * k, 0.15 * k}, 0.35);
      }
    }
    // the environment: a fire set and a bucket of water poured every few seconds
    if (env && t % 300 == 150) {
      const f64 ang = 6.2831853 * rng.next();
      const V3 dir{std::cos(ang), std::sin(ang), -0.1 + 0.4 * rng.next()};
      const RayHit hit = game.world().raycast(eye, dir, 60.0);
      if (hit.hit) {
        if (rng.next() < 0.6) game.ignite(hit.pos, 0.5);
        else game.pour(hit.pos + hit.normal * 0.4, 0.4);
      }
    }
    // a bounded level is reloaded now and then (a level restart)
    if (!streamed && t > 0 && t % (60 * 60 * 3) == 0) {
      if (dw) {
        // (the voxelized grid was moved into the world: re-running the load is the harness's job)
      } else {
        Level w = make_procedural(world, 1 + static_cast<u64>(t), h);
        game.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
        add_grids(game.world(), std::move(w.grids));
        game.bake();
      }
    }
    game.tick();
    // the front end's side: everything is taken every tick
    game.take_meshes(MeshOptions{});
    game.take_removed_chunks();
    game.take_far_meshes();
    game.take_far_removed();
    game.take_events();
    game.take_water_meshes();
    game.take_water_removed();
    game.flames(4096);
    game.smoke(4096);
    if (world == "drive") {
      game.take_character_meshes();
      game.take_removed_character_meshes();
      game.take_character_palettes();
      game.character_views();
      std::vector<f32> drops, stains;
      game.blood(&drops, &stains);
    }
    tick_ms += game.stats().tick_ms;
    ++n_ticks;
    if ((t + 1) % every == 0) {
      const MemoryReport m = game.world().memory();
      const GameStats st = game.stats();
      std::printf("%7.1f %8.1f %8.1f | %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f %7.2f %7.2f | %6d %5d %6d %6d %6lld | %6d %6d %6d | %6.2f\n",
                  (t + 1) * dt / 60.0, mb(m.total()), rss_mb(), mb(m.grid), mb(m.fragments), mb(m.structures), mb(m.pieces), mb(m.archive),
                  mb(m.caches), mb(m.queues), mb(m.systems), m.chunks, m.structure_count, m.piece_count, m.archived_chunks,
                  static_cast<long long>(st.forgotten_regions), st.fire_burning, st.smoke_cells, st.water_active,
                  tick_ms / std::max<i64>(1, n_ticks));
      std::fflush(stdout);
      sizes.emplace_back(mb(m.total()), rss_mb());
      tick_ms = 0.0;
      n_ticks = 0;
    }
  }
  const f64 wall = std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
  std::printf("done: %.0f s wall for %.0f simulated s\n", wall, ticks * dt);
  if (check) {
    // bounded: the second half of the session holds no more than the first half's peak and a
    // margin (the world's own accounting, strictly; the process's resident size, loosely - the
    // allocator keeps pages)
    if (sizes.size() < 4) {
      std::printf("check: too few reports (%zu) - run longer or report more often\n", sizes.size());
      return 1;
    }
    const size_t half = sizes.size() / 2;
    f64 w1 = 0.0, r1 = 0.0, w2 = 0.0, r2 = 0.0;
    for (size_t k = 0; k < sizes.size(); ++k) {
      (k < half ? w1 : w2) = std::max(k < half ? w1 : w2, sizes[k].first);
      (k < half ? r1 : r2) = std::max(k < half ? r1 : r2, sizes[k].second);
    }
    const bool world_ok = w2 <= w1 * 1.25 + 16.0, process_ok = r2 <= r1 * 1.5 + 64.0;
    std::printf("check: world %.1f MB at most in the first half, %.1f in the second (%s); process %.1f, %.1f (%s)\n", w1, w2, world_ok ? "bounded" : "GROWING", r1,
                r2, process_ok ? "bounded" : "GROWING");
    return world_ok && process_ok ? 0 : 1;
  }
  return 0;
}
