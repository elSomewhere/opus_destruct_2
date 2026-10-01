// svx_city — the road record (voxel_city: the objects city/cellNetwork.js's makeRoad makes, with
// what city/streets.js and city/diagonals.js give it, and what network/roadView.js, roadLevel.js,
// roadSurface.js, roadParts.js, world/parts.js, city/cellPlan.js, city/dressing.js,
// underground/sewers.js and svx/roads.js read of it).
//
// Every road of the world is made by a cell's network (World::cell_net): the arterial edges a
// cell owns (its west and north ones), diagonal boulevards, collectors, the sub-cells' local
// streets and service alleys. Coordinates are world voxels (doubles: a slanted street of the
// angled world ends anywhere on its centre line). A road is complete when its cell network is:
// the network trims streets and cobbles collectors before it hands them out, and nothing changes
// a road after that except its lazy fields below.
//
// Identity. JS compares roads as objects (roadView's maps, roadLevel's `s.road === road`), and a
// cell network made again after its cache dropped it makes new objects, so there an answer can
// depend on what the caches held. Here a road is the same road as another when it has the same
// `id` and the same cell (`ci`, `cj`): `same_road`. (`id` alone is not enough in a wrapping
// world, whose laps of a cell share their canonical ids; docs/CITY.md §6.)
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache.hpp"

namespace svx::city {

// A point of a road's centre line (voxels). (roadView reads a.z and b.z of a road's points: no
// cell network road has them - undefined.)
struct RoadPt {
  double x = 0, y = 0;
};

// The diagonal boulevard a road is a piece of (city/diagonals.js): its family f and line k.
struct RoadDiagonal {
  double f = 0, k = 0;
};

// Made lazily by the modules that need them (each a pure function of the road and the world):
struct RoadProfile;  // network/roadLevel: the vertical profile (JS road.prof, road.prof0)
struct RoadPieces;   // world/parts: the pitched pieces of an inclined road (JS road._pieces)

// Every field below is JS's of the same name in snake_case (JS type; when JS leaves it undefined).
// (JS's `sea`, a boolean a pending street carries while its cell is planned, is deleted before a
// road is built: no built road has it.)
struct Road {
  // id (string; always): `${cell}/r${n}`, n counting every road the cell's planning made, built or
  // not (a collector over the sea, a street that served no kept block): built roads' numbers have
  // gaps.
  std::string id;
  // cell (string; always): the canonical id of the cell whose network made it, `C${ci}_${cj}` (a
  // wrapping world's laps share it).
  std::string cell;
  // cls (string; always): arterial, collector, local, village, alley, lane, pedestrian or rural
  // (a key of config.roads: network/roadClasses).
  std::string cls;
  // pts ([{x, y}]; always): the centre line - two points for every street, more for a wobbling
  // country road or a bending village street.
  std::vector<RoadPt> pts;
  // hc, hr, corner, median, parking, lanes, lane, sidewalk, shoulder (numbers; always): the cross
  // section of its class (network/roadClasses RoadSpec, voxels; lanes a count) - half carriageway,
  // half right-of-way, corner radius, median, parking lane, lanes, lane width, sidewalk,
  // shoulder. (roadView reads `?? defaults` of the last six: a cell network road has them all.)
  double hc = 0, hr = 0, corner = 0, median = 0, parking = 0, lanes = 0, lane = 0, sidewalk = 0, shoulder = 0;
  // home ([i, j] numbers; undefined unless the angled world's streets are on - world.angles.enabled
  // and features.roads !== false - where every road has it): the cell that owns it, whose road
  // view sees every road it meets (roadLevel's ownSeg, compose's pitched pieces).
  std::optional<std::array<double, 2>> home;
  // arterialEdge ("W" or "N"; undefined but on the arterial edges a cell owns): the edge of its
  // cell it runs along (sewers). "": undefined.
  std::string arterial_edge;
  // paving ("cobble", or another district's paving; undefined unless set): an arterial edge or a
  // diagonal within a flavor's cobbleWithin, the streets of a district that paves (not those of
  // the collector class), its alleys, and a collector (or a village's main street through the
  // cell) both of whose sides' districts pave (set last). roadSurface and dressing read it. "":
  // undefined.
  std::string paving;
  // diagonal ({f, k} numbers; undefined but on a diagonal boulevard's piece): its family and line.
  std::optional<RoadDiagonal> diagonal;
  // sub (string; undefined but on local streets and alleys): the id of the sub-cell whose streets
  // it is. "": undefined.
  std::string sub;
  // strip ("pits", "grass" or "none"; undefined but on local streets - their district's - and
  // alleys - "none"): the verge between kerb and sidewalk (dressing and roadSurface read
  // `strip ?? "pits"`). "": undefined.
  std::string strip;

  // (C++ only) The cell (i, j) whose network made it - not canonical: with `id`, the identity of
  // the road (JS keeps object identity; see above).
  double ci = 0, cj = 0;

  // Lazy products cached on the road (JS: properties a module sets on the shared object, made
  // once whichever thread asks first; a pure function of the road and the world):
  Lazy<std::shared_ptr<const RoadProfile>> prof;   // prof: roadLevel's roadProfile (end levels from the roads met)
  Lazy<std::shared_ptr<const RoadProfile>> prof0;  // prof0: roadLevel's fitProfile between plain node levels
  Lazy<std::shared_ptr<const RoadPieces>> pieces;  // _pieces: world/parts' roadPieces
};

// The same road (JS: the same object, made by the same cell network).
inline bool same_road(const Road& a, const Road& b) { return &a == &b || (a.id == b.id && a.ci == b.ci && a.cj == b.cj); }

}  // namespace svx::city
