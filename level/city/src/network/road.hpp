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

struct Road {
  // JS `${cell}/r${n}`: n counts every road the cell's planning made, built or not (a collector
  // over the sea, a street that served no kept block): built roads' numbers have gaps.
  std::string id;
  // The canonical id of the cell whose network made it, `C${ci}_${cj}` (a wrapping world's laps
  // share it).
  std::string cell;
  // The class: arterial, collector, local, village, alley, lane, pedestrian or rural (the keys
  // of config.roads; network/roadClasses).
  std::string cls;
  // The centre line: two points for every street, more for a bending country road (wobble).
  std::vector<RoadPt> pts;
  // The cross section of its class (network/roadClasses RoadSpec, voxels; lanes a count): half
  // carriageway, half right-of-way, corner radius, median, parking lane, lanes, lane width,
  // sidewalk, shoulder. (roadView reads `?? defaults` of the last six: a cell network road has
  // them all.)
  double hc = 0, hr = 0, corner = 0, median = 0, parking = 0, lanes = 0, lane = 0, sidewalk = 0, shoulder = 0;
  // The angled world only: the cell (i, j) that owns it, whose road view sees every road it meets
  // (roadLevel's ownSeg, compose's pitched pieces). JS: undefined elsewhere.
  std::optional<std::array<double, 2>> home;
  // "W" or "N": the arterial edge of its cell it runs along (sewers). "": undefined.
  std::string arterial_edge;
  // "cobble" (old towns' streets, lanes and main streets) or "": undefined (cellNetwork sets it
  // from the district's or flavor's paving; roadSurface and dressing read it).
  std::string paving;
  // A diagonal boulevard's piece: its family and line. JS: undefined on every other road.
  std::optional<RoadDiagonal> diagonal;
  // The sub-cell whose streets it is (local streets and alleys), "": undefined (arterial edges,
  // diagonals, collectors).
  std::string sub;
  // The verge between kerb and sidewalk: "pits" (tree pits), "grass" or "none" (local streets
  // and alleys; dressing and roadSurface read `strip ?? "pits"`). "": undefined.
  std::string strip;

  // (C++) The cell (i, j) whose network made it - not canonical: the identity of the road with
  // `id` (JS keeps object identity; see above).
  double ci = 0, cj = 0;

  // Lazy products cached on the road (JS: properties set on the shared object), each made once,
  // whichever thread asks first:
  Lazy<std::shared_ptr<const RoadProfile>> prof;   // roadProfile (end levels from the roads met)
  Lazy<std::shared_ptr<const RoadProfile>> prof0;  // fitProfile between plain node levels
  Lazy<std::shared_ptr<const RoadPieces>> pieces;  // roadPieces
};

// The same road (JS: the same object, made by the same cell network).
inline bool same_road(const Road& a, const Road& b) { return &a == &b || (a.id == b.id && a.ci == b.ci && a.cj == b.cj); }

}  // namespace svx::city
