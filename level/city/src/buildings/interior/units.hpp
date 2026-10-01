// svx_city — the apartment unit planner (voxel_city buildings/interior/units.js).
//
// A unit is a rect in the building-canonical frame with an ENTRY side (the wall shared with the
// corridor or stair hall) and zero or more FACADE sides. It is planned in a unit-local frame where
// the entry is at v = 0, from templates tried in order of fit, with fewer bedrooms as fallback:
//
//   band     entry opposite the facade: a service row (kitchen, foyer, bath), a hall row, then the
//            living room and bedrooms along the facade
//   through  facades on both ends (walk-up "through" units): living room on one facade, bedrooms
//            on the other, foyer, bath and kitchen between
//   rail     a narrow deep unit: a hall down one side, the living room at the facade
//   open     the studio fallback: foyer, bath and one main room
//
// Every template is painted into the real FloorGrid and must place every required door on a real
// shared wall; otherwise the grid is rolled back and the next candidate tried. So a unit returned
// is always fully connected.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "buildings/interior/grid.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"

namespace svx::city {

// The finishes of a flat (pickUnitStyle): wall paint, bathroom tiles, the wood floor, the wet floor
// (material names).
struct UnitStyle {
  std::string paint{}, tile{}, wood{}, wet{};
};
// Draws paint, tile, wood and wet floor, in that order.
UnitStyle pick_unit_style(Rng& rng);
// floorFor(type, style): the floor finish of a room type.
std::string floor_for(const std::string& type, const UnitStyle& st);

// planUnit's opts: the stream (rng), the unit's id (its rooms' `unit`), where its entry door
// should be (none: centred on the entry wall).
struct UnitOpts {
  Rng* rng = nullptr;
  std::optional<std::string> unit{};
  std::optional<DoorNear> entry_near{};
};

// A planned unit: its rooms (in the template's order), its entry door, the template's name.
struct UnitPlan {
  std::vector<std::shared_ptr<Room>> rooms{};
  std::shared_ptr<Door> entry_door{};
  std::string template_{};
};

// planUnit: plans one unit into the grid. rect is the unit's interior rect (building canonical),
// entry_side the side of rect ('N', 'S', 'E', 'W'; N = -v) touching circ, the circulation room its
// door opens onto, facades the sides of rect lying on the exterior wall. None when no template
// fits (the grid unchanged).
std::optional<UnitPlan> plan_unit(FloorGrid& grid, const Rect& rect, char entry_side, const Room& circ, const std::vector<char>& facades, const UnitOpts& opts);

}  // namespace svx::city
