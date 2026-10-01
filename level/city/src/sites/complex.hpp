// svx_city — underground complexes (voxel_city sites/complex.js): research labs, bunkers, mountain
// bases; a reusable planner and emitter of Black Mesa / Doom style layouts.
//
//   sectors   each a stack of levels 15 m apart around a central stair shaft, laid out as rooms
//             of several shapes (rect, octagon, round, cross) joined by corridors (a spanning
//             tree and a few loops)
//   themes    decide the room types and the big signature rooms of a sector (test chamber,
//             reactor, hangar hall with waste channels, mess hall) and its colours (keycard
//             frames, accent lights): COMPLEX_THEMES
//   atriums   a hall spanning two levels, overlooked by a catwalk ring
//   ladders   shortcuts between levels through the floors of rooms
//   tram      a transit loop at the bottom joining every sector's station
//   entries   stair shafts from surface buildings down to a sector
//
// plan_complex draws from the site's stream, and emit_complex goes on drawing from it (room
// props), so a site plans and emits in that order. The emitter writes three box lists (shells:
// replace solid only, carves, details) in world voxels; sites rasterize them through a spatial
// grid (sites/kit finish_structure). A complex is planned once per site and only read after.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "buildings/interior/stairs.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"
#include "sites/kit.hpp"
#include "world/registry.hpp"

namespace svx::city {

// A theme of a sector: its room types by weight, its signature room of level k (last: the
// sector's deepest level), the colours of its keycard door frames and accent lights, and the
// chance of a shaped (octagonal, round, cross) room.
struct ComplexTheme {
  std::string id;
  std::vector<std::pair<std::string, double>> weights;
  std::function<std::string(double k, bool last)> big;
  uint16_t frame = 0, accent = 0;
  double shapes = 0;
};

// COMPLEX_THEMES: read-only once register_all() has run; _mut for registration only.
const Registry<ComplexTheme>& complex_themes();
Registry<ComplexTheme>& complex_themes_mut();
// sites/complex.js's registrations (register_all calls it, in the reference's order).
void register_complex_themes();

constexpr double kLevelGap = 120;  // LEVEL_GAP = vx(15): the levels of a sector, 15 m apart

// segRect: the rect of a corridor w wide along the segment (x0, y0) - (x1, y1).
Rect seg_rect(double x0, double y0, double x1, double y1, double w);

// A room of a complex: its bounds (inclusive), type (the room types of the themes, "hall" for
// an anteroom, "atrium" for an atrium's hall, "atriumTop" for its catwalk level, "station" for a
// tram's platform hall), shape ("rect", "octagon", "round", "cross"; "" where JS leaves it
// undefined: a rect), fixed (planned, never dropped: anterooms, atrium halls, stations) and, on
// an atrium's upper level, the walking z of the hall below (lower; NaN elsewhere).
struct ComplexRoom {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  std::string type;
  std::string shape;
  bool fixed = false;
  double lower = js::kNaN;
  Rect rect() const { return {x0, y0, x1, y1}; }
};

// shapeRects: the floor plan of a room as rects (octagons and circles as rows).
std::vector<Rect> shape_rects(const ComplexRoom& q);

// A level of a sector: its walking z (zf), rooms (room 0 the shaft's anteroom), corridors (centre
// to centre: emit_complex builds the pieces between the rooms), the room pairs no route could
// join (indices into the rooms as they were before unreachable rooms were dropped), the rects of
// the shafts it keeps clear, its theme, and the zones its ladders keep clear of props (keepClear;
// JS leaves it undefined while empty).
struct ComplexLevel {
  double sector = 0, k = 0, zf = 0;
  std::vector<ComplexRoom> rooms;
  std::vector<Rect> corridors;
  std::vector<std::array<double, 2>> blocked;
  std::vector<Rect> keep;
  std::string theme;
  std::vector<Rect> keep_clear;
};

// A sector: its theme, bounds, centre, central shaft and levels (indices into Complex::levels).
struct ComplexSector {
  double id = 0;
  std::string theme;
  Rect bounds{};
  Point2 center{};
  Rect shaft_rect{};
  std::vector<size_t> levels;
  const ComplexTheme* theme_def = nullptr;
};

// A stair stack through walking levels (a U-stair with virtual stories of about 30 voxels;
// every level a landing with an opening).
struct ComplexShaft {
  Rect rect{};
  Stair st;  // (with its flights)
  double dir = 1;
  bool open_top = false;
  double z_low = 0, z_high = 0;
  std::vector<double> levels;  // ascending
};

// A ladder shaft between consecutive levels of a sector, through the floor of a room.
struct ComplexLadder {
  Rect rect{};
  double z_top = 0, z_bot = 0, upper = 0, lower = 0, sector = 0;
};

// A tram station: its sector, its platform hall (a fixed "station" room) and the track point.
struct TramStation {
  double sector = 0;
  ComplexRoom hall;
  Point2 track{};
};

// The tram loop: its level, the stations in loop order, the tunnels' rects and the routes (the
// polylines from station to station).
struct ComplexTram {
  double z = 0;
  std::vector<TramStation> stations;
  std::vector<Rect> segs;
  std::vector<std::vector<Point2>> routes;
};

// planComplex's product.
struct Complex {
  std::vector<ComplexSector> sectors;
  std::vector<ComplexLevel> levels;  // every sector's, in order
  std::vector<ComplexShaft> shafts;
  std::vector<ComplexLadder> ladders;
  std::optional<ComplexTram> tram;
  std::optional<Rect> bounds;  // (none without sectors)
};

// planComplex's spec: sectors { bounds, z0 (level 0's walking z), levels, theme ("": military),
// rooms ([min, max] per level; none: [9, 14]) }, entries { rect (a stair shaft), zTop (the
// walking z at its top), sector, dir (NaN: JS's undefined), openTop (none: true) } and the tram's
// level (none: no tram).
struct ComplexSectorSpec {
  Rect bounds{};
  double z0 = 0;
  double levels = 1;
  std::string theme;
  std::optional<std::array<double, 2>> rooms;
};
struct ComplexEntrySpec {
  Rect rect{};
  double z_top = 0;
  double sector = 0;
  double dir = js::kNaN;
  std::optional<bool> open_top;
};
struct ComplexSpec {
  std::vector<ComplexSectorSpec> sectors;
  std::vector<ComplexEntrySpec> entries;
  std::optional<double> tram_z;
};

// mkShaft: a stair stack through walking levels.
ComplexShaft mk_shaft(const Rect& rect, const std::vector<double>& levels_z, double dir = 1, bool open_top = false);

// Plans a complex (draws from rng).
Complex plan_complex(Rng& rng, const ComplexSpec& spec);

// Emits a planned complex into box lists (draws from rng: room props).
void emit_complex(BoxLists& out, const Complex& cx, Rng& rng);

// A stair shaft's boxes (emitted last: they re-assert the shaft's walls and void, so corridors or
// rooms can never cap or cut a stairwell).
void emit_shaft(std::vector<SiteBox>& details, const ComplexShaft& sh);

}  // namespace svx::city
