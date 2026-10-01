// svx_city — a road of a cell's network: the record voxel_city's city/cellNetwork.js makes
// (makeRoad, the pattern streets of city/streets.js and the alleys) and the road network reads
// (network/roadView.js, roadSurface.js, roadLevel.js, roadParts.js, highways.js), as do the cell
// plan, the dressing, the sewers and the export.
//
// A road is complete before anyone else sees it (the cell network fills it in, then shares it):
// the cell network holds its roads as RoadPtr, and every road view of the 3 x 3 cells round a cell
// holds the same ones. What the road network caches on a road (JS: road.prof, road.prof0) are
// Lazy fields, made once from any thread.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/geom2d.hpp"
#include "core/js.hpp"

namespace svx::city {

// A road's vertical profile (network/roadLevel.js roadProfile): its levels z (voxels, as a
// Float32Array keeps them) every 8 m of its arc and at its end, its length L and the number of
// samples n; in the angled world with pitched roads, knots of its own (ks arc, kz level: the
// profile pitched to the grade table; knots false: none).
struct RoadProfile {
  std::vector<float> z;
  double L = 0;
  int n = 0;
  bool knots = false;
  std::vector<double> ks, kz;
};

// A diagonal boulevard's piece (city/diagonals.js): its family f and line k.
struct RoadDiagonal {
  double f = 0, k = 0;
};

// The road record. Numbers are JS numbers: NaN stands for a field JS leaves undefined (a road made
// elsewhere than the cell network may leave out its cross-section; the road view then takes the
// defaults buildSegments gives: sidewalk hr - hc, parking 0, median 0, lanes 2, lane 26,
// shoulder 0). Strings are "" where JS has no value (undefined or null).
struct Road {
  std::string id;           // `${cell}/r${n}` (n: the cell's road counter)
  std::string cell;         // the canonical id of the cell that made it: C{ci}_{cj}
  std::string cls;          // its class (network/roadClasses.js): arterial, collector, local ...
  std::vector<PPoint> pts;  // the centre line (voxels; z NaN: none)
  // the class's cross-section (network/roadClasses.js roadSpecs), voxels
  double hc = js::kNaN, hr = js::kNaN, corner = js::kNaN, median = js::kNaN, parking = js::kNaN;
  double lanes = js::kNaN, lane = js::kNaN, sidewalk = js::kNaN, shoulder = js::kNaN;
  // the angled world: the cell (i, j) that owns it, whose road view sees every road it meets
  std::optional<std::array<double, 2>> home;
  std::string arterial_edge;             // "W" / "N": an arterial edge's road
  std::string paving;                    // "cobble": old-town setts
  std::optional<RoadDiagonal> diagonal;  // a diagonal boulevard's piece
  std::string sub;                       // the sub-cell whose street pattern made it
  std::string strip;                     // the sidewalk's strip: "pits", "grass", "none" ("": pits)

  // (network/roadLevel.js, cached on the road: its profile pinned to its end nodes, and the
  // plain one between the node levels, which a road ending on it reads)
  Lazy<RoadProfile> prof, prof0;
};

using RoadPtr = std::shared_ptr<const Road>;
using RoadList = std::vector<RoadPtr>;

}  // namespace svx::city
