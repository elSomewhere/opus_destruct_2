// Contact features swept over a simulation tick, shared by combat and the workbench.
#pragma once
#include "svx/anim/character.hpp"
namespace svx::anim {
struct StrikeSweep {
  DamageDescriptor descriptor;
  V3 previous_a, previous_b, a, b;
  u64 serial = 0;
};
class StrikeTracker {
 public:
  std::vector<StrikeSweep> sample(const Character& character, f64 dt);
  static std::optional<DamageDescriptor> contact(const StrikeSweep& sweep, const Character& target);

 private:
  std::map<std::string, std::pair<V3, V3>> previous_;
  u64 serial_ = 0;
};
}  // namespace svx::anim
