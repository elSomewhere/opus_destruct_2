// svx_game — fight choreography for hand-to-hand brawls and knife fights between two characters:
// a policy on top of the characters' motor control (svx_anim takes intents; this decides them):
// keeping range and circling, choosing strikes by distance (jabs, crosses, hooks, uppercuts, front
// and roundhouse kicks; stabs and slashes with a knife), combinations, blocking what the opponent
// throws, backing off a downed opponent. What the strikes do where they land is
// anim::StrikeResolver's.
//
// The host moves the fighters: `move` is the desired velocity (world, m/s) and `yaw` the facing;
// while a fighter's body leads (knocked back, down) the host follows its root motion. A frame:
// update before the character's frame (its root, its inputs, its strikes), resolve after it.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "svx/anim/character.hpp"
#include "svx/anim/damage/strike.hpp"

namespace svx {

struct BrawlerOptions {
  f64 aggression = 0.5;  // 0 cautious .. 1 relentless
  f64 skill = 0.35;      // the chance to block a strike it sees coming
  f64 seed = 1.0;
};

class Brawler {
 public:
  explicit Brawler(anim::Character& self, const BrawlerOptions& o = {});

  anim::Character& self;
  anim::Character* opponent = nullptr;
  V3 move;        // the desired velocity (world, m/s)
  f64 yaw = 0.0;  // the facing (at the opponent)

  // The fighter's frame: its guard and look, its footwork, blocks and strikes.
  void update(f64 dt);
  // The blows this fighter's strikes landed in the frame just made.
  std::vector<anim::LandedBlow> resolve();

 private:
  anim::Rng rng_;
  f64 aggression_ = 0.5;
  f64 skill_ = 0.35;
  f64 cooldown_ = 0.6;
  f64 circle_ = 1.0;
  f64 circle_t_ = 0.0;
  f64 dt_ = 1.0 / 60;
  std::string reacted_;  // the opponent's strike last seen coming (each answered once, if at all)
  anim::StrikeResolver resolver_;
  V3 aim_point(std::string_view strike) const;
};

}  // namespace svx
