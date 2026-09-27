// structvox — cell archetypes (port of the prototype's archetypes.js cell table) and the
// default interface-profile rule (compiler.js resolveDefaultInterfaceProfileId).
#pragma once

#include <array>

#include "svx/base/types.hpp"
#include "svx/mech/bond.hpp"
#include "svx/mech/material.hpp"

namespace svx {

enum class CellKind : u8 { Voxel = 0, Floor, Column, WallX, WallY, Corner, Lintel, Brace, Generic, Terrain };

struct CellArchetype {
  const char* name;
  MaterialId mat;
  CellKind kind;
  std::array<f64, 3> eff;  // effDims (fractions of the lattice pitch)
};

// Built-in archetypes: the prototype's table plus full-voxel "voxel_<material>" entries
// used by the Doom pipeline. Returns -1 if unknown.
int archetype_count();
const CellArchetype& archetype(int id);
int archetype_by_name(const char* name);
int voxel_archetype(MaterialId mat);  // full-cube archetype for a material

// compiler.js: BRACE if either is a brace; WEAK_JOINT if materials differ; MONOLITHIC for
// lintel, floor+column, floor+wall, column+wall, or corner; otherwise SOLID.
Profile resolve_profile(const CellArchetype& a, const CellArchetype& b);

}  // namespace svx
