// Contact features swept over a simulation tick, shared by combat and the workbench.
#pragma once
#include "svx/anim/character.hpp"
namespace svx::anim {
struct StrikeSweep {
  DamageDescriptor descriptor;
  V3 previous_a, previous_b, a, b;
  V3 previous_normal, normal;  // the blade's flat-side normal, in world space
  f64 speed_limit = 22;
  u64 serial = 0;
  V3 velocity(f64 along) const;
  DamageDescriptor impact(f64 along, const V3& point, const V3& target_velocity = {}, f64 time = 1) const;
};
class StrikeTracker {
 public:
  std::vector<StrikeSweep> sample(const Character& character, f64 dt);
  static std::optional<DamageDescriptor> contact(const StrikeSweep& sweep, const Character& target);
  // The sweep's first contact with a still box (world, lo..hi: a pad, a target), traced at `step`
  // (m) along the feature: the impact there, if the feature meets it moving (over .05 m/s).
  static std::optional<DamageDescriptor> box_contact(const StrikeSweep& sweep, const V3& lo, const V3& hi, f64 step = .015);

 private:
  struct FeaturePose { V3 a, b, normal; };
  std::map<std::string, FeaturePose> previous_;
  u64 serial_ = 0;
};

// A strike that reached the opponent, and what it did there.
struct LandedBlow {
  Character* attacker = nullptr;
  Character* victim = nullptr;
  V3 point, dir;
  HitKind kind = HitKind::Blunt;  // the strike's: Blunt (fists, feet, a rifle butt) or Blade
  bool blocked = false;           // it met a block (it landed blunt, and softer)
  DamageDescriptor descriptor;
  WoundResult result;
};

// What one character's strikes do to another, physically: the contact features its strikes
// sweep this frame, met with the opponent's body or what it holds, land as damage there - and the
// momentum the opponent took loads the striker's grip or striking limb back. It decides nothing:
// whom to fight and what to throw are the host's.
class StrikeResolver {
 public:
  explicit StrikeResolver(Character& self) : self(self) {}
  Character& self;
  Character* opponent = nullptr;
  // (after the striker's frame of dt)
  std::vector<LandedBlow> resolve(f64 dt);

 private:
  StrikeTracker tracker_;
  u64 landed_serial_ = ~u64(0);
};
}  // namespace svx::anim
