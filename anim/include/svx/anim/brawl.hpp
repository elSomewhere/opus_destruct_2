// svx_anim — fight choreography for hand-to-hand brawls and knife fights between two characters:
// keeping range and circling, choosing strikes by distance (jabs, crosses, hooks, uppercuts, front
// and roundhouse kicks; stabs and slashes with a knife), combinations, blocking what the opponent
// throws, backing off a downed opponent, and resolving what lands: a strike event of the motion
// plan becomes a hit where the fist, foot or blade met the opponent's body, with a force by strike
// (a jab stings, a roundhouse to the head drops people).
//
// The host moves the fighters: `move` is the desired velocity (world, m/s) and `yaw` the facing;
// while a fighter's body leads (knocked back, down) the host follows its root motion. A frame:
// update before the character's frame (its root, its inputs, its strikes), resolve after it (the
// strike events it made).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "svx/anim/character.hpp"
#include "svx/anim/damage/strike.hpp"

namespace svx::anim {

struct BrawlerOptions {
  f64 aggression = 0.5;  // 0 cautious .. 1 relentless
  f64 skill = 0.35;      // the chance to block a strike it sees coming
  f64 seed = 1.0;
};

// A strike that reached the opponent, and what it did there.
struct LandedBlow {
  Character* attacker = nullptr;
  Character* victim = nullptr;
  V3 point, dir;
  HitKind kind = HitKind::Blunt;  // the strike's: Blunt (fists, feet, a rifle butt) or Blade
  bool blocked = false;           // it met a block (it landed blunt, and softer)
  DamageDescriptor descriptor;
  WoundResult result;             // (its zone: where it landed)
};

class Brawler {
 public:
  explicit Brawler(Character& self, const BrawlerOptions& o = {});

  Character& self;
  Character* opponent = nullptr;
  V3 move;        // the desired velocity (world, m/s)
  f64 yaw = 0.0;  // the facing (at the opponent)

  // The fighter's frame: its guard and look, its footwork, blocks and strikes.
  void update(f64 dt);
  // Resolves this fighter's animation events: strikes that reach the opponent land.
  std::vector<LandedBlow> resolve(const std::vector<AnimEvent>& events);

 private:
  Rng rng_;
  f64 aggression_ = 0.5;
  f64 skill_ = 0.35;
  f64 cooldown_ = 0.6;
  f64 circle_ = 1.0;
  f64 circle_t_ = 0.0;
  std::string reacted_;  // the opponent's strike last seen coming (each answered once, if at all)
  std::string last_strike_;

  StrikeTracker tracker_;
  f64 dt_ = 1.0 / 60;
  u64 landed_serial_ = ~u64(0);
  V3 aim_point(std::string_view strike) const;
};

}  // namespace svx::anim
