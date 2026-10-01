// structvox game — command logs: record / replay / deterministic lockstep.
//
// Everything that changes a Game between ticks is a command: carve, blast, use, fire, water, viewer (the
// streaming focus and walk-over triggers) and tunables. A command is stamped with the tick it
// takes effect in (the number of ticks completed when it was issued). Applying the same commands
// at the same ticks to the same level reproduces the session bit for bit (Game::session_hash) on
// any thread count, native or WASM, ARM or x86 — which is what lockstep networking needs: peers
// exchange stamped commands (with an input delay of a few ticks) and simulate locally.
// Multiplayer note: the viewer is part of the shared stream (one authoritative focus), because
// streaming and first-touch design depend on it.
#pragma once

#include <array>
#include <vector>

#include "svx/base/types.hpp"

namespace svx {

class Game;

struct Command {
  enum class Type : u8 {
    Carve = 1, Blast = 2, Viewer = 3, Params = 4, Use = 5, Ignite = 6, Extinguish = 7, Pour = 8,
    Heat = 9, Drain = 10, EnvParam = 11, Tunable = 12, Shoot = 13, Vehicle = 14, Drive = 15, Traffic = 16,
    Pedestrians = 17, Wound = 18
  };
  i64 tick = 0;
  Type type = Type::Carve;
  // Carve: pos xyz, radius. Blast: pos xyz, radius, energy. Viewer: pos xyz.
  // Params: fragility, impact, dif, (unused), debug_view, paused.
  // Use: eye xyz, direction xyz. Ignite, Extinguish, Pour, Drain: pos xyz, radius. Heat: pos
  // xyz, radius, degC. EnvParam: index (env_param_*), value. Tunable: id (tunable_id: stable across builds), value.
  // Shoot: pos xyz, radius, energy. Vehicle: action (1 spawn: kind + 256 paint + 65536 flags,
  // pos xyz, yaw; 2 remove: id; 3 enter: id; 4 exit). Drive: throttle, brake, steer, handbrake.
  // Traffic: enabled, cars, parked, near radius, radius, speed scale. Pedestrians: enabled, count,
  // near radius, radius, bodies (0 deep, 1 shallow, 2 hybrid), max deep. Wound: character id,
  // pos xyz, radius, energy.
  std::array<f64, 6> a{};
};

class CommandLog {
 public:
  void clear() { cmds_.clear(); }
  void push(const Command& c) { cmds_.push_back(c); }
  const std::vector<Command>& commands() const { return cmds_; }
  // Binary format "SVXL" v3: magic, version, count, then per command tick (i64), type (u8)
  // and 6 f64 (little-endian, bit-exact). (v1 logs - types 1..5 - and v2 logs read the same;
  // their tunables are indices, taken as this build's.)
  std::vector<u8> serialize() const;
  static bool parse(const std::vector<u8>& bytes, CommandLog* out);

 private:
  std::vector<Command> cmds_;
};

// Applies one command to a game (between ticks).
void apply_command(Game& e, const Command& c);

// Plays `log` on `e` from its current state until `ticks` ticks have completed, applying each
// command before the tick it is stamped with; calls `on_tick(tick)` after every tick.
template <typename OnTick>
void replay(Game& e, const CommandLog& log, i64 ticks, OnTick&& on_tick);

}  // namespace svx

#include "svx/game/game.hpp"

namespace svx {

template <typename OnTick>
void replay(Game& e, const CommandLog& log, i64 ticks, OnTick&& on_tick) {
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
