#pragma once
#include "svx/anim/damage/descriptor.hpp"
#include "svx/anim/characters/props.hpp"
#include "svx/anim/voxel/damage.hpp"
namespace svx::anim {
struct TissueImpact {
  i32 bone = 0;
  V3 rest;
  f64 energy = 0, removed = 0;
  bool bone_hit = false;
};
struct WoundMechanics {
  std::vector<RemovedVoxel> removed;
  std::vector<TissueImpact> tissue;
  std::vector<i32> changed_bones;
  f64 remaining_energy = 0;
  bool exited = false;
  V3 exit, exit_direction;
  // Linear momentum left in the source is not delivered to the target.
  V3 impulse(const DamageDescriptor& source) const {
    return vnorm(source.direction) * source.momentum() - exit_direction * std::sqrt(2 * source.mass * remaining_energy);
  }
};
WoundMechanics wound_mechanics(VoxelModel& model, std::span<const f32> skin, const DamageDescriptor& descriptor, const PropMaterial* material = nullptr);
}  // namespace svx::anim
