// structvox game — movers: the moving parts of a level (Doom's doors, lifts, floors, platforms,
// crushers, stairs). A mover owns columns whose voxel span [z0, z1) holds a solid part `rows`
// levels thick, hanging from z1 (Door, Ceiling) or standing on z0 (Lift, Floor). The solid part
// is written into the world as anchored voxels bonded to nothing (World::set_voxels, untracked
// and isolated): a kinematic support that pieces rest on and structures never bond to.
#pragma once

#include <array>
#include <vector>

#include "svx/world/grid.hpp"

namespace svx {

// What one activation of a mover does. Rows are the solid part's thickness in voxel levels
// (0 .. z1 - z0).
struct MoverMove {
  enum class Type : u8 {
    To,      // go to `target` and stay (floors, ceilings, raising platforms, stairs, open / close doors)
    Return,  // go to `target`, wait, go back to `back` (doors, lifts, close-then-open doors)
    Cycle,   // go to `target`, wait, go to `back`, wait, ... (perpetual platforms, crushers)
    Stop,    // a cycling mover stops where it is; its Cycle move resumes it
  };
  Type type = Type::Return;
  i32 target = 0;
  i32 back = -1;         // Return / Cycle: -1 = the rows when the move started
  f64 speed = 1.0;       // voxel levels per second
  f64 wait = 0.0;        // s at `target` (Return), at each end (Cycle)
  bool reverse = false;  // Return: coming back, turn around at the player (doors)
};

struct MoverDef {
  enum class Kind : u8 { Door, Lift, Floor, Ceiling };
  Kind kind = Kind::Door;
  std::vector<std::array<i32, 2>> cols;
  i32 z0 = 0, z1 = 0;
  i32 rows = -1;         // solid rows at the start (-1: the whole span: closed / raised)
  Vox vox = 0;           // voxel value of the solid part (made anchored)
  bool usable = true;    // "use" on the mover itself runs moves[0] (doors, lifts)
  f64 speed = 1.5;       // span fractions per second
  f64 wait = 4.0;        // s open / lowered before returning (repeatable movers)
  bool repeat = true;    // false: stays open once used
  std::vector<MoverMove> moves;
  bool ceiling() const { return kind == Kind::Door || kind == Kind::Ceiling; }
};

struct MoverTrigger {
  i32 mover = -1;
  i32 move = 0;
};

}  // namespace svx
