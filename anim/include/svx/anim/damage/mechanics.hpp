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
struct MechanicsTuning {
  f64 crush_remove_energy = 1200;  // J: a crush above this removes the tissue it overcomes (CharacterProfile)
};
// What a cause does to a model's cells: removes what it overcomes (marking it in `removed`), deposits
// energy in what it reaches (`tissue`), and stains the rims of what it removed. A prop's material
// (`material`) replaces the tissues' resistance, and under a blunt blow absorbs at most its
// `fracture` energy: the rest remains (remaining_energy) for what the prop rests on.
WoundMechanics wound_mechanics(VoxelModel& model, std::span<const f32> skin, const DamageDescriptor& descriptor, const PropMaterial* material = nullptr,
                               const MechanicsTuning& tuning = {});
}  // namespace svx::anim
