// svx_city — interiors of civic buildings, venues and big shops (voxel_city
// buildings/interior/civic.js). Three planners, each driven by a program table
// (civicPrograms.hpp):
//
//   hall       a foyer band along the street (the entrance, side rooms, the stair), one to three
//              big halls behind it (a supermarket floor, an auditorium, cinema screens, a market
//              hall), a service band at the back (storage with a loading door, dressing rooms,
//              staff rooms, WCs). With two floors the halls are double height: the upper floor
//              keeps a gallery / bar over the foyer and a `void` over the halls (slab open).
//   corridor   a double-loaded corridor with a stair at each end (like the school): rooms by floor
//              from the program (hospital wards and theatres, police cells and interview rooms,
//              museum and gallery halls spanning several bays, hotel rooms ...), a lobby with the
//              street entrance on the ground floor, back or front doors and roll-up doors where the
//              program asks.
//   kiosk      one small shop (a petrol station's shop, a market kiosk).
//
// and the department store: the office core with open sales floors round it on every floor. Every
// room is reachable through doors and stairs; validate_plan checks it like any other building.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "core/hash.hpp"

namespace svx::city {

// A hall building's program (JS keys in brackets; NaN or "": undefined):
//   foyer: the street band's depth (m) [5]; foyer_side: side room types at the end opposite the
//   stair; foyer_type: the foyer's room type ["foyer"]; hall: the halls' room type; hall_open: an
//   opening (no leaf) into the halls; hall_door: its width [14]; halls: [min, max] count (empty:
//   one); hall_min_w (m) [9]; hall_props: the halls' extra props (noWindows, classical); back:
//   service room types (empty: no service band); back_depth (m) [4.5]; back_w (m) [5]; dock:
//   "rollup" | "metal" (the back door of the first back room; "": none); upper_foyer: the upper
//   landing's type ["foyerBar"]; upper_side: the rooms beside it.
struct HallProgram {
  double foyer = js::kNaN;
  std::vector<std::string> foyer_side{};
  std::string foyer_type{};
  std::string hall{};
  bool hall_open = false;
  double hall_door = js::kNaN;
  std::vector<double> halls{};
  double hall_min_w = js::kNaN;
  bool hall_no_windows = false, hall_classical = false;  // (hallProps)
  std::vector<std::string> back{};
  double back_depth = js::kNaN;
  double back_w = js::kNaN;
  std::string dock{};
  std::string upper_foyer{};
  std::vector<std::string> upper_side{};
};

// A room spec of a corridor building: a type, or { type, span (bays), at ("mid" | "start" |
// "end"), ext ("front" | "back" | "rollupFront" | "rollupBack"), split (n rooms side by side in one
// bay), lobby (the entrance and an opening to the corridor), props ({fire: true}: a fire station's
// bay) } (NaN or "": undefined).
struct CorridorSpec {
  std::string type{};
  double span = js::kNaN;
  std::string at{};
  std::string ext{};
  double split = js::kNaN;
  bool lobby = false;
  bool fire = false;  // (props.fire)
};
// One floor's program: the front and back specs and the fill types (none: ["office"]).
struct CorridorFloor {
  std::vector<CorridorSpec> front{}, back{};
  std::optional<std::vector<std::string>> fill{};
};
// A corridor building's program: bay (m) [6], corr (m) [3], the corridor's paint
// ["PAINT_CREAM"] and floor ["FLOOR_TERRAZZO"], and the ground, first and upper floors' programs
// (none: undefined or null).
struct CorridorProgram {
  double bay = js::kNaN, corr = js::kNaN;
  std::string corr_paint{}, corr_floor{};
  std::optional<CorridorFloor> ground{}, first{}, upper{};
};

// A kiosk's program: its shop's type (["grocery"]).
struct KioskProgram {
  std::string shop{};
};

// planHall({env, rng, pb}, P), planCorridor, planKiosk, planDepartmentStore.
void plan_hall(const Envelope& env, Rng& rng, PlanBuilder& pb, const HallProgram& P);
void plan_corridor(const Envelope& env, Rng& rng, PlanBuilder& pb, const CorridorProgram& P);
void plan_kiosk(const Envelope& env, Rng& rng, PlanBuilder& pb, const KioskProgram& P);
void plan_department_store(const Envelope& env, Rng& rng, PlanBuilder& pb);

}  // namespace svx::city
