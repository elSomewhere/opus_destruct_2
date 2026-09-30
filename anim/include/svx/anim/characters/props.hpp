// svx_anim — voxel props held by characters (weapons). A prop is a one-bone model whose origin is
// the grip of the hand that holds it, +y along the barrel, z up; its points place the other hand,
// the shoulder stock and the muzzle.
#pragma once

#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

enum class PropKind : u8 { Rifle, Smg, Lmg, Pistol, Knife };

struct Prop {
  ModelPtr model;
  // prop-space points
  V3 grip, support, stock, muzzle;
  V3 magazine;  // where the magazine sits (reloads)
  PropKind kind = PropKind::Rifle;
  bool one_handed = false;  // (pistols, knives: the support point is on the other hand's side)
  // a long gun: held with both hands (not a pistol, not a knife)
  bool long_gun() const { return kind != PropKind::Pistol && kind != PropKind::Knife; }
};

using PropPtr = std::shared_ptr<const Prop>;

// The voxel size characters are sculpted at (1/32 m: Doom's texel scale in this engine).
constexpr f64 kDefaultVoxelSize = 1.0 / 32.0;

// An assault rifle (~0.86 m), a submachine gun, a light machine gun, a pistol, a knife.
PropPtr make_rifle(f64 voxel_size = kDefaultVoxelSize, i32 variant = 0);
PropPtr make_smg(f64 voxel_size = kDefaultVoxelSize);
PropPtr make_lmg(f64 voxel_size = kDefaultVoxelSize);
PropPtr make_pistol(f64 voxel_size = kDefaultVoxelSize / 2.0);
PropPtr make_knife(f64 voxel_size = kDefaultVoxelSize / 2.0);

}  // namespace svx::anim
