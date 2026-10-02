#pragma once
#include <vector>
#include <string_view>
#include "svx/anim/damage/descriptor.hpp"
namespace svx::anim {
class Character;
// Repeatable workbench fixtures, built from the current rig and pose.
std::vector<DamageDescriptor> damage_scenario(const Character& character, std::string_view name);
inline constexpr std::string_view kDamageScenarios[] = {"thighShot",   "femoralBleed", "shatteredKnee", "forearmSever", "backSlash",
                                                        "shotgunLegs", "blast3m",      "gutStab",       "batKnee"};
}  // namespace svx::anim
