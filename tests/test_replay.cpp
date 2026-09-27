// Command logs (plan Phase 3 determinism, Phase 7 lockstep networking): a peer that applies
// another peer's stamped commands reproduces its session bit for bit, at every tick.
#include <cmath>
#include <deque>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/engine/engine.hpp"
#include "svx/engine/replay.hpp"
#include "svx/world/procgen.hpp"

using namespace svx;

namespace {

void load_rooms(Engine& eng, CommandLog* log) {
  ProcWorld w = make_procedural("rooms", 3);
  eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  if (log) eng.record_to(log);
  EngineParams p;
  p.fragility = 0.25;
  eng.set_params(p);
  eng.bake();
}

}  // namespace

TEST_CASE("replay: command logs round-trip bit-exactly and reject damaged input") {
  CommandLog log;
  log.push({0, Command::Type::Params, {6.0, 3.0, 0.25, 0.5, 0.0, 0.0}});
  log.push({3, Command::Type::Carve, {1.25, -2.5, 0.1 + 0.2, 0.1, 0.0, 0.0}});
  log.push({3, Command::Type::Blast, {1e-300, 7.0, -0.0, 0.6, 1e6, 0.0}});
  log.push({9, Command::Type::Viewer, {1.0 / 3.0, 2.0, 3.0, 0.0, 0.0, 0.0}});
  const std::vector<u8> bytes = log.serialize();
  CommandLog back;
  REQUIRE(CommandLog::parse(bytes, &back));
  REQUIRE(back.commands().size() == 4);
  for (size_t k = 0; k < 4; ++k) {
    CHECK(back.commands()[k].tick == log.commands()[k].tick);
    CHECK(back.commands()[k].type == log.commands()[k].type);
    for (int q = 0; q < 6; ++q)
      CHECK(std::signbit(back.commands()[k].a[q]) == std::signbit(log.commands()[k].a[q]));
    CHECK(back.commands()[k].a == log.commands()[k].a);
  }
  std::vector<u8> cut(bytes.begin(), bytes.end() - 3);
  CHECK_FALSE(CommandLog::parse(cut, &back));
  std::vector<u8> bad = bytes;
  bad[16 + 8] = 9;  // unknown command type
  CHECK_FALSE(CommandLog::parse(bad, &back));
  bad = bytes;
  bad[0] ^= 1;  // magic
  CHECK_FALSE(CommandLog::parse(bad, &back));
}

TEST_CASE("replay: a lockstep peer on another thread count matches the host at every tick") {
  // Host A plays (1 thread), streaming its stamped commands; peer B receives them over a
  // "network" with a delay of 5 ticks and simulates on 4 threads, lagging A by that delay.
  set_num_threads(1);
  Engine a;
  CommandLog alog;
  load_rooms(a, &alog);
  set_num_threads(4);
  Engine b;
  load_rooms(b, nullptr);
  constexpr int kDelay = 5, kTicks = 420;
  std::vector<u64> host_hash{a.session_hash()};
  std::deque<Command> wire;
  size_t sent = 0;
  const auto sp = a.spawn_pos();
  const std::array<f64, 3> eye{sp[0], sp[1], sp[2] + 1.6};
  int mismatches = 0, checked = 0;
  for (int t = 0; t < kTicks + kDelay; ++t) {
    if (t < kTicks) {
      set_num_threads(1);
      if (t % 20 == 0) {
        const f64 ang = 0.3 * static_cast<f64>(t / 20) - 1.5;
        const RayHit h = a.raycast(eye, {std::cos(ang), std::sin(ang), 0.02 * (t % 7)}, 60.0);
        if (h.hit) a.carve(h.pos, 0.12);
      }
      if (t == 90 || t == 250) {
        const RayHit h = a.raycast(eye, {1.0, 0.15 * (t == 90 ? 1 : -1), 0.3}, 60.0);
        if (h.hit) a.blast(h.pos, 0.6, 1e6);
      }
      if (t == 200) {
        EngineParams p = a.params();
        p.compliance = 5.0;
        a.set_params(p);
      }
      if (t % 2 == 0) a.set_viewer({eye[0] + 0.01 * t, eye[1], eye[2]});
      a.tick();
      (void)a.take_events();
      host_hash.push_back(a.session_hash());
      // the new commands go on the wire (serialized, as a network packet would be)
      CommandLog packet;
      for (; sent < alog.commands().size(); ++sent) packet.push(alog.commands()[sent]);
      CommandLog recv;
      REQUIRE(CommandLog::parse(packet.serialize(), &recv));
      for (const Command& c : recv.commands()) wire.push_back(c);
    }
    if (t >= kDelay) {
      // peer tick: apply every command stamped with its current tick, then simulate
      set_num_threads(4);
      while (!wire.empty() && wire.front().tick == b.ticks()) {
        if (!(b.ticks() == 0 && wire.front().type == Command::Type::Params)) apply_command(b, wire.front());
        wire.pop_front();
      }
      b.tick();
      (void)b.take_events();
      ++checked;
      if (b.session_hash() != host_hash[static_cast<size_t>(b.ticks())]) ++mismatches;
    }
  }
  set_num_threads(1);
  CHECK(checked == kTicks);
  CHECK(mismatches == 0);
  CHECK(a.stats().bubbles_spawned >= 1);
  CHECK(b.session_hash() == a.session_hash());
  CHECK(alog.commands().size() > 20);
}
