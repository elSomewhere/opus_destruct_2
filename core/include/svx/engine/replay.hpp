// structvox — command logs: record / replay / deterministic lockstep (plan §B9, Phase 3 gate
// "bitwise-identical replays", Phase 7 "networking: deterministic").
//
// Everything that changes an Engine between ticks is a command: carve, blast, viewer (the
// streaming / bake focus) and tunables. A command is stamped with the tick it takes effect in
// (the number of ticks completed when it was issued). Applying the same commands at the same
// ticks to the same world reproduces the session bit for bit (Engine::session_hash) on any
// thread count, native or WASM, ARM or x86 — which is what lockstep networking needs: peers
// exchange stamped commands (with an input delay of a few ticks) and simulate locally.
// Multiplayer note: the viewer is part of the shared stream (one authoritative focus), because
// streaming and progressive baking depend on it.
#pragma once

#include <array>
#include <vector>

#include "svx/base/types.hpp"

namespace svx {

class Engine;
struct EngineParams;

struct Command {
  enum class Type : u8 { Carve = 1, Blast = 2, Viewer = 3, Params = 4, Use = 5 };
  i64 tick = 0;
  Type type = Type::Carve;
  // Carve: pos xyz, radius. Blast: pos xyz, radius, energy. Viewer: pos xyz.
  // Params: compliance, amplification, fragility, damping, debug_view, paused.
  // Use: eye xyz, direction xyz.
  std::array<f64, 6> a{};
};

class CommandLog {
 public:
  void clear() { cmds_.clear(); }
  void push(const Command& c) { cmds_.push_back(c); }
  const std::vector<Command>& commands() const { return cmds_; }
  // Binary format "SVXL" v1: magic, version, count, then per command tick (i64), type (u8)
  // and 6 f64 (little-endian, bit-exact).
  std::vector<u8> serialize() const;
  static bool parse(const std::vector<u8>& bytes, CommandLog* out);

 private:
  std::vector<Command> cmds_;
};

// Applies one command to an engine (between ticks).
void apply_command(Engine& e, const Command& c);

// Plays `log` on `e` from its current state until `ticks` ticks have completed, applying each
// command before the tick it is stamped with; calls `on_tick(tick)` after every tick.
template <typename OnTick>
void replay(Engine& e, const CommandLog& log, i64 ticks, OnTick&& on_tick);

}  // namespace svx

#include "svx/engine/engine.hpp"

namespace svx {

template <typename OnTick>
void replay(Engine& e, const CommandLog& log, i64 ticks, OnTick&& on_tick) {
  const auto& cmds = log.commands();
  size_t k = 0;
  for (i64 t = e.ticks(); t < ticks; ++t) {
    while (k < cmds.size() && cmds[k].tick < t) ++k;  // already applied (resumed replays)
    while (k < cmds.size() && cmds[k].tick == t) apply_command(e, cmds[k++]);
    e.tick();
    on_tick(t + 1);
  }
}

}  // namespace svx
