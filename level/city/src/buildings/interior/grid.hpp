// svx_city — one floor plan as a label grid (voxel_city buildings/interior/grid.js).
//
// FloorGrid is a floor in the building's canonical frame (u along the street facade, v from the
// front); one cell is one voxel column (12.5 cm). Labels (a Uint16Array of U * V):
//
//   OUT    0    outside the footprint
//   EXT    1    exterior wall (EXT_T = 2 cells thick)
//   WALL   2    interior partition (1 cell thick), also the default "unassigned"
//   DOOR   3    carved opening in a wall
//   ROOM0  16+  room index (ROOM0 + room.id)
//
// Rooms are painted as rects; everything interior that no room claims stays WALL. Doors are found
// on real shared walls, so a planned layout is valid only if the grid says two rooms really touch.
//
// Rooms and doors are JS objects that planners keep and change after the grid made them (a room
// that gets no door becomes a "shaft", plan.js gives street doors a sill), and restore() drops
// the newest from the grid's lists while a planner may still hold them: they are shared records
// (std::shared_ptr), as JavaScript's references are. Every coordinate is an integer cell (a
// double). A grid is built by one thread; once its plan is made it is only read.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/rect.hpp"

namespace svx::city {

// A box in a building's canonical frame (x = u, y = v cells, inclusive) with absolute z (voxels)
// and a material (voxel/materials.hpp; 0 carves): what stairs, fixtures and furniture emit
// ({x0, y0, z0, x1, y1, z1, m}).
struct CanonBox {
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  uint16_t m = 0;
};

// A stall of a garage deck (garage.js's stallLayout): its rect, the direction its car's nose
// points along u (noseU, +1 / -1) and whether a car is parked in it.
struct GarageStall {
  double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  double nose_u = 1;
  bool car = false;
};

// A mezzanine's link to a room on another floor (validatePlan's room.linkTo).
struct RoomLink {
  double floor = 0, room = 0;
};

// addRoom's props: the fields planners give a room (JS's {...props}; absent: nullopt, false or
// empty). Names are the JS ones in snake_case.
struct RoomProps {
  std::optional<std::string> paint = std::nullopt;      // wall finish, a material name (voxelize: ?? "PAINT_WHITE")
  std::optional<std::string> floor_mat = std::nullopt;  // floorMat, a material name (?? "FLOOR_CONCRETE")
  std::optional<double> stair = std::nullopt;           // a stairwell's stair id (common.js addStairRoom)
  std::optional<double> elevator = std::nullopt;        // an elevator room's elevator id
  std::optional<std::string> unit = std::nullopt;       // the flat a room belongs to (apartments.js, units.js)
  std::optional<std::string> template_ = std::nullopt;  // the unit template it came from (template)
  std::optional<double> ceiling = std::nullopt;         // (industrial.js; 0: not furnished)
  std::optional<double> deck_h = std::nullopt;          // deckH: a garage deck's story height
  std::optional<char> front = std::nullopt;             // the side its street front is on ('N', 'S'; civic.js)
  bool shop = false;                                    // a shop (storefront windows)
  bool tall = false;                                    // double height (civic halls)
  bool no_windows = false;                              // noWindows
  bool classical = false;                               // (concert halls: a piano on the stage)
  bool fire = false;                                    // (a fire station's garage bay)
  std::vector<GarageStall> stalls{};                    // (garage decks)
  std::vector<Rect> pillars{};                          // (garage decks)
  std::optional<Rect> ramp_rect = std::nullopt;         // rampRect (garage decks)
  std::optional<RoomLink> link_to = std::nullopt;       // linkTo
};

// A room: {id, type, rects, ...props}. type is the furnishing rule's key ("living", "stair",
// "corridor", "ward" ...); a planner may change it (a room without a door becomes a "shaft").
struct Room : RoomProps {
  double id = 0;
  std::string type{};
  std::vector<Rect> rects{};
};

// A door: the opening's cells u0..u1 x v0..v1, the wall's orientation ('v': a wall along v,
// u0..u1 its thickness; 'h': along u), the rooms it joins (b -1: outside), and how it is drawn.
struct Door {
  double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  char orient = 'v';
  double a = 0;
  double b = -1;
  std::string kind{};    // interior, entrance, entry, stair, closet, opening, elevator, sky, shopfront, balcony, window, rollup ...
  std::string side_a{};  // sideA: "low" | "high", the side of the wall room a lies on
  double width = 0;
  std::string leaf{};    // wood, none, metal, glass, elevator, rollup ...
  std::optional<double> height = std::nullopt;
  double id = 0;  // its index in the grid's doors
  // (plan.js streetLevels: the street in front of a ground-floor door, and its raised sill)
  std::optional<double> street = std::nullopt, sill = std::nullopt;
  // (civic.js: a fire station's roll-up doors are painted, a material id; offices.js: a skybridge
  // door the facade could not place where the bridge lands)
  std::optional<uint16_t> color = std::nullopt;
  bool misplaced = false;
};

// A straight wall run between two rooms (or a room and the outside): for orient 'v' the wall
// occupies columns fixed..fixed + thick - 1 along v from t0 to t1; for 'h' rows fixed.. along u.
// side_a: "low" when room a lies on the low side of the wall, "high" otherwise.
struct WallRun {
  char orient = 'v';
  double fixed = 0, t0 = 0, t1 = 0, thick = 1;
  std::string side_a{};
};

// addDoor's options (absent: the default in brackets).
struct DoorNear {
  double u = 0, v = 0;
};
struct DoorOpts {
  std::optional<double> width = std::nullopt;       // opening width in cells [7], plus the grid's doorExtra
  std::optional<double> margin = std::nullopt;      // min distance from wall corners [2]
  std::optional<std::string> place = std::nullopt;  // "center" | "start" | "end" | "near" | "auto" ["auto"]
  std::optional<DoorNear> near = std::nullopt;      // the target point when place is "near"
  std::optional<std::string> kind = std::nullopt;   // [b ? "interior" : "entrance"]
  std::optional<std::string> leaf = std::nullopt;   // [kind "opening" ? "none" : "wood"]
  std::optional<double> height = std::nullopt;      // (kept when truthy)
};

// doorGraph(): room id -> the room ids it has doors to (-1: the outside); keys and each key's set
// in insertion order (JS's Map of Sets).
struct DoorGraph {
  std::vector<double> keys{};
  std::vector<std::vector<double>> sets{};
  const std::vector<double>* get(double key) const;
  std::vector<double>* get(double key);
  void add(double x, double y);   // adj.get(x).add(y)
  bool erase(double x, double y); // adj.get(x)?.delete(y)
};

class FloorGrid {
 public:
  static constexpr int OUT = 0;
  static constexpr int EXT = 1;
  static constexpr int WALL = 2;
  static constexpr int DOOR = 3;
  static constexpr int ROOM0 = 16;
  static constexpr double EXT_T = 2;
  // cut(u, v): cells of the footprint's rects that are outside nonetheless (a chamfered corner,
  // buildings/chamfer.hpp); null: none.
  using Cut = std::function<bool(double u, double v)>;

  // A U x V grid: the footprint's rects WALL, then the cut's cells OUT, then the interior cells
  // within EXT_T of the outside EXT.
  FloorGrid(double U, double V, const std::vector<Rect>& footprint, Cut cut = nullptr);
  // (one grid, shared as JS shares it - by floors with the same layout: not copied)
  FloorGrid(const FloorGrid&) = delete;
  FloorGrid& operator=(const FloorGrid&) = delete;
  FloorGrid(FloorGrid&&) = default;
  FloorGrid& operator=(FloorGrid&&) = default;

  double U = 0, V = 0;
  // Extra width (cells) of every door of a turned building (doorExtraOf).
  double door_extra = 0;
  std::vector<uint16_t> cells;
  std::vector<std::shared_ptr<Room>> rooms;  // (by id)
  std::vector<std::shared_ptr<Door>> doors;  // (by id)
  std::vector<Rect> footprint;
  Cut cut;

  // The label at (u, v); OUT beyond the grid.
  int get(double u, double v) const {
    if (u < 0 || v < 0 || u >= U || v >= V) return OUT;
    return cells[static_cast<size_t>(u + v * U)];
  }
  // Sets a label (ignored beyond the grid; stored as a Uint16Array stores it).
  void set(double u, double v, double val) {
    if (u < 0 || v < 0 || u >= U || v >= V) return;
    cells[static_cast<size_t>(u + v * U)] = js::u16(val);
  }
  // Labels a rect's cells (clipped to the grid).
  void fill(const Rect& r, double val);
  // Interior rects of the footprint (inset by the exterior wall).
  std::vector<Rect> inner_rects() const;
  // Is the rect entirely interior, unclaimed (WALL) cells?
  bool is_free(const Rect& r) const;
  // A room of the valid rects (painted inside the walls on a cut floor).
  std::shared_ptr<Room> add_room(const std::string& type, const std::vector<Rect>& rects, const RoomProps& props = {});
  // Labels a rect's cells that are inside the walls (not OUT, not EXT).
  void paint_inside(const Rect& r, double val);
  // Grows a room by a rect of free interior cells; false (and nothing changed) otherwise.
  bool extend_room(Room& room, const Rect& rect);
  // The room at (u, v), or null.
  Room* room_at(double u, double v) const;
  // The cells of a room's rects.
  double area(const Room& room) const;

  struct Snapshot {
    std::vector<uint16_t> cells{};
    size_t rooms = 0, doors = 0;
  };
  Snapshot snapshot() const;
  // The cells as they were, and the rooms and doors made since dropped from the lists.
  void restore(const Snapshot& s);

  // Straight wall runs separating room a from room b (b null: the outside, through the
  // exterior wall), in the order of a's rects, each rect's east, west, south, north sides.
  std::vector<WallRun> wall_runs(const Room& a, const Room* b) const;
  // Carves a door between rooms a and b (b null: the exterior; it faces the street unless told
  // otherwise). Null when no wall run is long enough.
  std::shared_ptr<Door> add_door(const Room& a, const Room* b, DoorOpts opts = {});
  // Where an open door leaf rests: flat against the wall next to the opening, inside room a; a
  // canonical rect (1 cell thick) or null when there is no free wall face.
  std::optional<Rect> door_leaf_rect(const Door& d) const;
  // Room pairs connected by a door (both ways, -1 the outside).
  DoorGraph door_graph() const;
};

// doorExtraOf(env) for a turned building (env.turn): the extra width of its doors. A doorway in a
// turned wall must pass the viewer's 0.5 m square (4 cells), which walks the world's axes, across
// the turn (4 (|cos| + |sin| - 1) cells more), between jambs stepped by the world grid (one
// cell): 2 cells at 8.8-16 degrees, 3 near 37-53. (A building that is not turned: 0.)
double door_extra_of(int turn_yaw);

}  // namespace svx::city
