// svx_replay — record and replay engine sessions (plan Phase 3 gate: bitwise-identical replays
// across thread counts, x86 / ARM and native / WASM; Phase 7: deterministic lockstep).
//
//   svx_replay record --world W --seconds S --out session.svxl [--threads T] [--seed N]
//                     [--spawn-latency N] [--sync-steps] [--env]
//   svx_replay play   --world W --log session.svxl --seconds S [--threads T]
//
// W: rooms | tower | city (streamed 1 km^2) | wad:PATH:MAP. `record` plays a scripted session
// (a moving viewer; bullet bursts every second and a rocket every 4 s, aimed by a fixed LCG
// through ray casts into the live world; with --env also a fire every 5 s and a bucket of water
// every 7 s) and writes the command log. Both modes print the
// session hash every 10 s of simulated time and at the end: diff the outputs of any two runs.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "svx/base/dmath.hpp"
#include "svx/base/parallel.hpp"
#include "svx/game/doom/movers.hpp"
#include "svx/game/doom/world.hpp"
#include "svx/game/game.hpp"
#include "svx/game/replay.hpp"
#include "svx/game/procgen.hpp"
#include "svx/game/city.hpp"

using namespace svx;

namespace {

std::unique_ptr<doom::DoomWorld> g_doom;  // outlives the engine's door / lift resolver

bool load_world(Game& eng, const std::string& world, std::string* err) {
  if (world.rfind("wad:", 0) == 0) {
    const size_t c = world.find(':', 4);
    if (c == std::string::npos) {
      *err = "expected wad:PATH:MAP";
      return false;
    }
    std::ifstream f(world.substr(4, c - 4), std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    doom::Wad wad;
    g_doom = std::make_unique<doom::DoomWorld>();
    doom::DoomWorld& dw = *g_doom;
    if (!wad.load_memory(std::move(bytes), err) || !doom::build_doom_world(wad, world.substr(c + 1), {}, 0.125, false, &dw, err))
      return false;
    VoxelGrid g = std::move(dw.grid);
    dw.grid = VoxelGrid{};
    eng.load(std::move(g), dw.spawn_pos, dw.spawn_dir);
    dw.live = &eng.grid();
    doom::attach_doom_movers(eng, dw);
    return true;
  }
  if (world == "city") {
    auto src = make_city_source(1, 1000.0, 0.125);
    VoxelGrid g;
    g.h = 0.125;
    const auto sp = src->spawn_pos(), sd = src->spawn_dir();
    eng.load(std::move(g), sp, sd);
    eng.load_streaming(std::move(src), eng.grid().h);
    return true;
  }
  ProcWorld pw = make_procedural(world, 1);
  eng.load(std::move(pw.grid), pw.spawn_pos, pw.spawn_dir);
  add_grids(eng.world(), std::move(pw.grids));
  return true;
}

struct Lcg {
  u64 s = 0x2545F4914F6CDD1Dull;
  f64 next(f64 lo, f64 hi) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return lo + (hi - lo) * static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
  }
};

void checkpoint(const Game& eng, i64 tick, bool last) {
  // (SVX_REPLAY_FROM / SVX_REPLAY_EVERY: hashes every N ticks from a tick, to find a divergence)
  static const i64 from = std::getenv("SVX_REPLAY_FROM") ? std::atoll(std::getenv("SVX_REPLAY_FROM")) : -1;
  static const i64 every = std::getenv("SVX_REPLAY_EVERY") ? std::atoll(std::getenv("SVX_REPLAY_EVERY")) : 0;
  if (from >= 0 && every > 0 && tick >= from && (tick - from) % every == 0 && tick % 600 != 0) {
    std::printf("tick %lld state %016llx session %016llx\n", static_cast<long long>(tick),
                static_cast<unsigned long long>(eng.world().state_hash()), static_cast<unsigned long long>(eng.session_hash()));
    return;
  }
  if (!last && tick % 600 != 0) return;
  const GameStats s = eng.stats();
  std::printf("t=%6.1fs hash %016llx voxels %lld broken %lld detached %lld pieces %d\n", tick / 60.0,
              static_cast<unsigned long long>(eng.session_hash()), static_cast<long long>(s.voxels),
              static_cast<long long>(s.bonds_broken), static_cast<long long>(s.detached_voxels), s.bodies);
  if (std::getenv("SVX_REPLAY_STATS"))  // (a separate line: the gate compares the hash lines)
    std::printf("    stats: events %lld, extractions %lld, solves %lld, piece checks %lld, splits %lld, impact loads %lld\n",
                static_cast<long long>(s.events), static_cast<long long>(s.extractions), static_cast<long long>(s.solves),
                static_cast<long long>(s.body_checks), static_cast<long long>(s.body_splits), static_cast<long long>(s.impacts));
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 2) {
    std::fprintf(stderr, "usage: svx_replay record|play --world W [--seconds S] [--out F | --log F] [--threads T]\n");
    return 2;
  }
  const std::string mode = argv[1];
  std::string world = "rooms", out, in;
  f64 seconds = 60.0;
  // engine configuration is not part of the log: pass the same switches to record and play
  u64 seed = 0;  // record: varies the scripted session
  bool env = false;  // record: fires and water too
  f64 fragility = 1.0;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--world" && i + 1 < argc) world = argv[++i];
    else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--log" && i + 1 < argc) in = argv[++i];
    else if (a == "--threads" && i + 1 < argc) set_num_threads(std::atoi(argv[++i]));
    else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--env") env = true;
    else if (a == "--fragility" && i + 1 < argc) fragility = std::atof(argv[++i]);
  }
  Game eng;
  std::string err;
  if (!load_world(eng, world, &err)) {
    std::fprintf(stderr, "cannot load %s: %s\n", world.c_str(), err.c_str());
    return 1;
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto wall = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  CommandLog log;
  if (mode == "play") {
    std::ifstream f(in, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!CommandLog::parse(bytes, &log)) {
      std::fprintf(stderr, "cannot parse log %s\n", in.c_str());
      return 1;
    }
    // tick-0 tunables precede the bake (they shape the design pass), as when recording
    size_t k = 0;
    const auto& cmds = log.commands();
    for (; k < cmds.size() && cmds[k].tick == 0 && cmds[k].type == Command::Type::Params; ++k) apply_command(eng, cmds[k]);
    CommandLog rest;
    for (; k < cmds.size(); ++k) rest.push(cmds[k]);
    eng.bake();
    const i64 ticks = static_cast<i64>(seconds * 60.0);
    replay(eng, rest, ticks, [&](i64 t) {
      (void)eng.take_events();
      checkpoint(eng, t, t == ticks);
    });
    std::printf("replayed %zu commands, %lld ticks in %.1f s\n", cmds.size(), static_cast<long long>(ticks), wall());
    return 0;
  }
  if (mode != "record") {
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
  }
  eng.record_to(&log);
  GameParams par;
  par.fragility = fragility;
  eng.set_params(par);
  eng.bake();
  Lcg rng;
  rng.s ^= seed * 0x9E3779B97F4A7C15ull;
  const auto sp = eng.spawn_pos(), sd = eng.spawn_dir();
  const f64 dl = std::sqrt(sd[0] * sd[0] + sd[1] * sd[1]);
  const V3 fwd{dl > 0 ? sd[0] / dl : 1.0, dl > 0 ? sd[1] / dl : 0.0, 0.0};
  const bool travel = world == "city";
  const i64 ticks = static_cast<i64>(seconds * 60.0);
  for (i64 t = 0; t < ticks; ++t) {
    const f64 s = t / 60.0;
    const f64 d = travel ? 5.0 * s : 0.0;  // the city flight: 5 m/s along the spawn direction
    const V3 eye{sp[0] + fwd[0] * d, sp[1] + fwd[1] * d, sp[2] + 1.6 + (travel ? 20.0 : 0.0)};
    if (t % 2 == 0) eng.set_viewer(eye);
    auto aim = [&](f64 pitch_lo, f64 pitch_hi) {
      const f64 yaw = rng.next(-3.14159265358979, 3.14159265358979), pitch = rng.next(pitch_lo, pitch_hi);
      const f64 cp = dm::cos(pitch);
      return eng.world().raycast(eye, {cp * dm::cos(yaw), cp * dm::sin(yaw), dm::sin(pitch)}, 80.0);
    };
    if (t % 60 == 0)
      for (int b = 0; b < 3; ++b) {
        const RayHit h = aim(-0.3, 0.4);
        if (h.hit) eng.carve(h.pos, 0.1);
      }
    if (t % 240 == 120) {
      const RayHit h = aim(travel ? -0.9 : -0.1, travel ? -0.2 : 0.6);
      if (h.hit) eng.blast(h.pos, 0.6, 1e6);
    }
    if (env && t % 300 == 60) {
      const RayHit h = aim(-0.4, 0.3);
      if (h.hit) eng.ignite(h.pos, 0.5);
    }
    if (env && t % 420 == 200) {
      const RayHit h = aim(-0.6, 0.0);
      if (h.hit) eng.pour(h.pos + h.normal * 0.4, 0.4);
    }
    eng.tick();
    (void)eng.take_events();
    checkpoint(eng, t + 1, t + 1 == ticks);
  }
  const std::vector<u8> bytes = log.serialize();
  if (!out.empty()) {
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  std::printf("recorded %zu commands (%zu bytes), %lld ticks in %.1f s\n", log.commands().size(), bytes.size(),
              static_cast<long long>(ticks), wall());
  return 0;
}
