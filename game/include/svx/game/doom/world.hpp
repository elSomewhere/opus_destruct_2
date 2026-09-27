// structvox — a playable world from a Doom map (plan §B10): voxel grid, player start,
// decoded textures and per-face texturing / sector light for the mesher.
//
// Coordinates: voxel (i, j, k) of the voxelizer's column grid is centred at world
// ((i, j, k) h) metres; map point (x, y, z) maps to ((x - origin_x) / upv - 1/2,
// (y - origin_y) / upv - 1/2, z / upv - 1/2) h.
// Faces: floors (+z faces below sector air) take the sector's floor flat, ceilings its ceiling
// flat, walls the texture of the linedef between the air column and the solid voxel's column
// (one-sided: middle; two-sided: lower below the sector floor, upper above the ceiling), with
// world-aligned texel coordinates (32 texels per metre = 1 texel per map unit).
#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "svx/game/doom/slenderness.hpp"
#include "svx/game/doom/textures.hpp"
#include "svx/game/doom/voxelize.hpp"
#include "svx/base/vec.hpp"
#include "svx/world/grid.hpp"

namespace svx::doom {

struct DoomWorld {
  VoxelGrid grid;
  V3 spawn_pos{0, 0, 0};  // feet
  V3 spawn_dir{1, 0, 0};
  TextureSet textures;
  VoxelizeStats vstats;
  Map map;
  i32 nx = 0, ny = 0;
  std::vector<i32> sector;        // per column
  std::vector<u16> floor_tex, ceil_tex;  // per sector (0xFFFF: untextured)
  std::vector<u8> light;          // per sector (0..255)
  mutable std::unordered_map<u64, std::array<u16, 3>> wall_cache;  // (column, dir) -> upper, mid, lower
  std::vector<std::vector<i32>> sector_lines;  // linedefs bordering each sector
  const VoxelGrid* live = nullptr;  // the world's grid once `grid` was moved into it
  std::vector<MoverInfo> movers;    // moving sector planes (voxelized most open)
  // self-weight buckling margin of the walls (concrete strips): cap S_p at
  // slenderness.max_compliance_p99 (v1)
  SlendernessReport slenderness;

  u16 face_texture(const IVec3& p, int face) const;
  u8 face_light(const IVec3& p, int face) const;
  // The linedef between the air column in front of face `face` of voxel p and p's column
  // (-1: none, e.g. floors / ceilings or void).
  i32 face_linedef(const IVec3& p, int face) const;
};

bool build_doom_world(const Wad& wad, const std::string& map_name, const VoxelizeOptions& vo, f64 h, bool textures,
                      DoomWorld* out, std::string* err);

}  // namespace svx::doom
