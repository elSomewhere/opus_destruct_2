// svx_city — office buildings and towers (voxel_city buildings/interior/offices.js).
//
// A central core (two stairs, an elevator bank with a machine shaft behind it, two restrooms)
// stacks from the lowest basement to the roof. Typical floors are an open plan ring around the
// core with enclosed meeting rooms and offices along the end facades. The ground floor is a lobby
// with shops in the front corners; basements are parking levels. A floor a skybridge lands on gets
// a grid of its own with a glass door where the bridge meets the front (env.sky_doors).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/interior/grid.hpp"
#include "buildings/interior/plan.hpp"
#include "buildings/interior/stairs.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"

namespace svx::city {

// officeCore(inner, sd, nF): the core's parts side by side across the middle of the floor
// ({kind, rect}: "stair", "restroom", "elevators"; a compact core has one stair and one restroom),
// its rect and the elevators in its bank (1 up to 5 floors, 2 up to 14, 3 up to 28, else 4).
struct OfficeCoreComp {
  std::string kind{};
  Rect rect{};
};
struct OfficeCore {
  Rect rect{};
  std::vector<OfficeCoreComp> comps{};
  double n_elev = 0;
};
OfficeCore office_core(const Rect& inner, const StairDims& sd, double nF);

// The core's layout as an office (or a department store, civic.js) stacks it: its stairs (their
// rects and the plan's stairs), its elevators (rects and the plan's elevators), its restrooms and
// the machine shafts behind the elevator banks.
struct CoreLayoutStair {
  Rect rect{};
  std::shared_ptr<Stair> stair{};
};
struct CoreLayoutElevator {
  Rect rect{};
  Elevator elev{};
};
struct CoreLayout {
  std::vector<CoreLayoutStair> stairs{};
  std::vector<CoreLayoutElevator> elevators{};
  std::vector<Rect> restrooms{};
  std::vector<Rect> shafts{};
};

// planOfficeBuilding({env, rng, pb}).
void plan_office_building(const Envelope& env, Rng& rng, PlanBuilder& pb);

// The rooms painted before an open floor (planOpenFloor's `cores`): the stairwells with their
// stairs (cores.layout's, else ctx.layout's), the elevator rooms and the restrooms (none: an
// apartment core's).
struct OpenFloorCores {
  std::vector<std::shared_ptr<Room>> stair_rooms{};
  std::vector<const Stair*> stairs{};  // (the stair of each stair room)
  std::vector<std::shared_ptr<Room>> elev_rooms{};
  std::vector<std::shared_ptr<Room>> restrooms{};
};

// planOpenFloor(grid, ctx, cores, kind): fills the rest of a floor with one open-plan room round
// the rooms already painted (cores), carving enclosed rooms (kind "office": meeting rooms, offices
// along the end facades) or shops (kind "lobby": a shop in a front corner, back-of-house behind)
// first, then connects every core room; a lobby gets the street entrance. kind: "office", "lobby",
// "parking" or "store" (a department store's sales floor). Draws from rng (offices and lobbies
// only); env: the building (its flavor: the shops). Returns the open room.
std::shared_ptr<Room> plan_open_floor(FloorGrid& grid, const Envelope& env, Rng& rng, const OpenFloorCores& cores, const std::string& kind);

}  // namespace svx::city
