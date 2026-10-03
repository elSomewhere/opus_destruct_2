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

 private:
  struct FeaturePose { V3 a, b, normal; };
  std::map<std::string, FeaturePose> previous_;
  u64 serial_ = 0;
};
}  // namespace svx::anim
