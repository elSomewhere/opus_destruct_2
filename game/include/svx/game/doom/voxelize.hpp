// structvox — Doom map -> run-length voxel columns (plan §B10).
//
// Sectors are rasterized with a scanline walk over linedef crossings (robust to
// non-convex sectors and holes). Every playable column becomes: anchored bedrock below,
// a structural floor slab, air up to the ceiling, and (unless sky) a structural ceiling
// slab — extended upward where a neighbouring sector's ceiling is higher so upper walls
// exist and connect. Void columns within `shell_voxels` of the playable space form a
// structural wall shell; farther void is anchored rock ("rock" mode) or air ("air" mode).
// Diagonal-only contacts in the 2D sector map are thickened so walls stay face-connected
// (6-connectivity: diagonal voxels share no bond).
#pragma once

#include <string>

#include "svx/game/doom/wad.hpp"
#include "svx/game/columns.hpp"

namespace svx::doom {

enum class VoidMode { Rock, Air };

struct VoxelizeOptions {
  int units_per_voxel = 4;  // 4 map units = 12.5 cm (plan design point)
  int shell_voxels = 8;     // ~1 m wall shell
  int floor_slab = 2;       // structural voxels under each floor before bedrock
  int ceil_slab = 3;        // structural ceiling slab thickness
  VoidMode void_mode = VoidMode::Rock;
  bool fix_diagonals = true;
  MaterialId material = MaterialId::Concrete;
  bool movers = true;  // sector planes moved by line specials become movers (voxelized most open)
};

// One way a line special moves a sector plane (doom/specials): the plane's solid rows at the
// destination and, for Return / Cycle moves, at the other end (-1: where the move starts).
struct PlaneMove {
  u16 special = 0;
  u16 tag = 0;      // lines with this special and tag run it (0: a manual door, used directly)
  i32 target = 0;
  i32 back = -1;
};

// A moving sector plane (plan Phase 7 "doors and lifts", extended): the ceiling (Door, Ceiling)
// or the floor (Lift, Floor) of `sector`, travelling through voxel levels [z0, z1) - the plane's
// start height and every height a special can take it to. The sector is voxelized with the
// plane at its most open (a floor at z0, a ceiling at z1); the game fills `rows` solid levels,
// hanging from z1 (ceilings) or standing on z0 (floors). Doors: planes moved by door specials
// (steel, usable when a manual door line opens them); lifts: by lift specials (usable).
struct MoverInfo {
  enum class Kind : u8 { Door, Lift, Floor, Ceiling };
  Kind kind = Kind::Door;
  i32 sector = -1;
  i32 z0 = 0, z1 = 0;
  i32 rows = 0;          // solid rows at map start
  u16 special = 0;       // the first special that moves it
  u16 tag = 0;           // the sector's tag
  bool manual = false;   // moves[0] is a manual door's (DR / D1 lines on its sides)
  std::vector<PlaneMove> moves;
  bool ceiling() const { return kind == Kind::Door || kind == Kind::Ceiling; }
};

struct VoxelizeStats {
  i32 nx = 0, ny = 0, nz = 0;
  i64 sector_columns = 0, shell_columns = 0, rock_columns = 0;
  i32 diagonal_fixes = 0;
  i64 structural_voxels = 0, anchored_voxels = 0;
  i32 map_min_x = 0, map_min_y = 0, map_max_x = 0, map_max_y = 0;
  // column (i, j) is centred at map (origin_x + (i + 1/2) upv, origin_y + (j + 1/2) upv);
  // voxel level k spans map heights [k upv, (k + 1) upv)
  i32 origin_x = 0, origin_y = 0, units_per_voxel = 4;
};

// sector_map (optional): per column (j * nx + i) the sector index, -1 for void columns.
bool voxelize(const Map& map, const VoxelizeOptions& opt, ColumnGrid* out, VoxelizeStats* stats, std::string* err,
              std::vector<i32>* sector_map = nullptr, std::vector<MoverInfo>* movers = nullptr);

}  // namespace svx::doom
