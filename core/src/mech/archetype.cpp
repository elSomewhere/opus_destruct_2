#include "svx/mech/archetype.hpp"

#include <cstring>

namespace svx {

namespace {

const CellArchetype kArchetypes[] = {
    // prototype archetypes.js
    {"rc_floor", MaterialId::Rc, CellKind::Floor, {1.0, 1.0, 0.25}},
    {"rc_column", MaterialId::Rc, CellKind::Column, {0.4, 0.4, 1.0}},
    {"rc_wall_x", MaterialId::Rc, CellKind::WallX, {1.0, 0.3, 1.0}},
    {"rc_wall_y", MaterialId::Rc, CellKind::WallY, {0.3, 1.0, 1.0}},
    {"rc_corner", MaterialId::Rc, CellKind::Corner, {0.35, 0.35, 1.0}},
    {"rc_lintel", MaterialId::Rc, CellKind::Lintel, {1.0, 0.35, 0.55}},
    {"steel_brace", MaterialId::Steel, CellKind::Brace, {0.22, 0.22, 1.0}},
    {"masonry_wall_x", MaterialId::Masonry, CellKind::WallX, {1.0, 0.35, 1.0}},
    {"masonry_wall_y", MaterialId::Masonry, CellKind::WallY, {0.35, 1.0, 1.0}},
    {"generic_concrete", MaterialId::Concrete, CellKind::Generic, {0.5, 0.5, 0.5}},
    {"terrain_soil", MaterialId::Soil, CellKind::Terrain, {1.0, 1.0, 1.0}},
    {"terrain_rock", MaterialId::Rock, CellKind::Terrain, {1.0, 1.0, 1.0}},
    {"terrain_bedrock", MaterialId::Bedrock, CellKind::Terrain, {1.0, 1.0, 1.0}},
    // full voxels (Doom pipeline)
    {"voxel_rc", MaterialId::Rc, CellKind::Voxel, {1.0, 1.0, 1.0}},
    {"voxel_concrete", MaterialId::Concrete, CellKind::Voxel, {1.0, 1.0, 1.0}},
    {"voxel_steel", MaterialId::Steel, CellKind::Voxel, {1.0, 1.0, 1.0}},
    {"voxel_masonry", MaterialId::Masonry, CellKind::Voxel, {1.0, 1.0, 1.0}},
    {"voxel_soil", MaterialId::Soil, CellKind::Voxel, {1.0, 1.0, 1.0}},
    {"voxel_rock", MaterialId::Rock, CellKind::Voxel, {1.0, 1.0, 1.0}},
    {"voxel_bedrock", MaterialId::Bedrock, CellKind::Voxel, {1.0, 1.0, 1.0}},
};

constexpr int kCount = static_cast<int>(sizeof(kArchetypes) / sizeof(kArchetypes[0]));

bool is_wall(CellKind k) { return k == CellKind::WallX || k == CellKind::WallY; }

}  // namespace

int archetype_count() { return kCount; }

const CellArchetype& archetype(int id) {
  SVX_ASSERT(id >= 0 && id < kCount);
  return kArchetypes[id];
}

int archetype_by_name(const char* name) {
  for (int i = 0; i < kCount; ++i)
    if (std::strcmp(kArchetypes[i].name, name) == 0) return i;
  return -1;
}

int voxel_archetype(MaterialId mat) {
  for (int i = 0; i < kCount; ++i)
    if (kArchetypes[i].kind == CellKind::Voxel && kArchetypes[i].mat == mat) return i;
  return -1;
}

Profile resolve_profile(const CellArchetype& a, const CellArchetype& b) {
  if (a.kind == CellKind::Brace || b.kind == CellKind::Brace) return Profile::Brace;
  if (a.mat != b.mat) return Profile::WeakJoint;
  auto has = [&](CellKind k) { return a.kind == k || b.kind == k; };
  if (has(CellKind::Lintel)) return Profile::Monolithic;
  if (has(CellKind::Floor) && has(CellKind::Column)) return Profile::Monolithic;
  if (has(CellKind::Floor) && (is_wall(a.kind) || is_wall(b.kind))) return Profile::Monolithic;
  if (has(CellKind::Column) && (is_wall(a.kind) || is_wall(b.kind))) return Profile::Monolithic;
  if (has(CellKind::Corner)) return Profile::Monolithic;
  return Profile::Solid;
}

}  // namespace svx
