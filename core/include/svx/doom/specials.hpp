// structvox — vanilla Doom linedef specials that move sector planes (plan Phase 7 "doors and
// lifts", extended): doors, lifts, floors, ceilings, platforms, crushers, stairs and the donut.
// The voxelizer reads the targets (each plane's travel envelope); the engine movers read the
// move kinds, speeds and waits (doom/movers).
#pragma once

#include "svx/base/types.hpp"

namespace svx::doom {

enum class LineTrigger : u8 { None, Manual, Use, Walk, Shoot };

// Where a plane goes, by vanilla's P_Find* rules on the map's heights (map units).
enum class PlaneTarget : u8 {
  Start,                // where the plane is when the move starts
  DoorOpen,             // ceiling: lowest neighbouring ceiling - 4
  OwnFloor,             // ceiling: the sector's floor (closed door, lower to floor)
  OwnFloorPlus8,        // ceiling: floor + 8 (crushers)
  HighestCeiling,       // ceiling: highest neighbouring ceiling
  LowestFloor,          // floor: lowest floor around, the sector's own included
  HighestFloor,         // floor: highest neighbouring floor
  TurboFloor,           // floor: highest neighbouring floor + 8 (unless that is the sector's own)
  HighestFloorOrOwn,    // floor: the higher of that and the sector's own (perpetual platforms)
  LowestCeiling,        // floor: lowest neighbouring ceiling, at most the sector's ceiling
  LowestCeilingMinus8,  // floor: that - 8 (crushing floors)
  NextFloor,            // floor: the next higher neighbouring floor
  Plus24,
  Plus32,
  Plus512,
  Stairs8,              // floor: a staircase from the tagged sector, 8 units a step
  Stairs16,             // 16 units a step
  Donut,                // floor: the donut's hole and ring go to the outer floor
};

enum class MoveKind : u8 { To, Return, Cycle, Stop };  // as engine MoverMove::Type

struct SpecialAction {
  bool ceiling = false;  // the plane it moves
  MoveKind kind = MoveKind::To;
  PlaneTarget target = PlaneTarget::Start;
  PlaneTarget back = PlaneTarget::Start;  // Return / Cycle: the other end
  f64 speed = 1.0;                        // map units per tic
  i32 wait = 0;                           // tics
  bool door = false;                      // a door (steel; turns around at the player)
  bool lift = false;                      // a lift (usable)
};

struct SpecialInfo {
  LineTrigger trigger = LineTrigger::None;
  bool once = false;  // W1 / S1 / G1 / D1: the line works once
  int n = 0;
  SpecialAction a[2];
};

// false for specials that move no plane (exits, teleports, lights, scrollers, unknown) and for
// raise-to-texture floors (30, 96: unused by Freedoom).
bool special_info(u16 special, SpecialInfo* out);

}  // namespace svx::doom
