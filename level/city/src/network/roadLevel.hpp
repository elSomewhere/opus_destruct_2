// svx_city — road levels (voxel_city network/roadLevel.js): streets are built, not draped over the
// terrain.
//
// Every road gets a vertical profile along its centre line (the natural ground every 8 m, lightly
// smoothed), fitted to the ground within a grade limit and pinned at its ends to the ground there
// (the node level: the ground averaged over a disc round the node, so every road ending there
// agrees) or, where it ends on a road that goes on through (a T), to that road's level there. The
// fit balances cut and fill: the mean of the highest grade-limited profile nowhere above the
// ground and the lowest nowhere below it. The grade limit is the class's comfortable grade,
// relaxed where the land is steeper up to the class's steepest; the angled world takes them from
// the pitch table and, with pitched roads, holds each steep stretch at one table grade with level
// landings (knots of its own). A road is level across its whole width. At a junction the more
// important road keeps its profile and the other meets it: both flatten to the dominant road's
// level over the junction box and blend back to their own profiles (the angled world: junction by
// junction, continuous however their boxes overlap).
//
// Profiles are cached on the roads, a junction's level and blend on its annotation (Lazy fields:
// pure functions of the roads and the terrain, made once from any thread). World::street_level is
// defined here.
#pragma once

#include <optional>
#include <vector>

#include "network/road.hpp"
#include "network/roadView.hpp"

namespace svx::city {

class World;

// A road's vertical profile (roadProfile; network/road.hpp's Road::prof, prof0): its levels z
// (voxels, as a Float32Array keeps them) every 8 m of its arc and at its end, its length L and the
// number of samples n; in the angled world with pitched roads, knots of its own (ks arc, kz level:
// the profile pitched to the grade table; knots false: none).
struct RoadProfile {
  std::vector<float> z;
  double L = 0;
  int n = 0;
  bool knots = false;
  std::vector<double> ks, kz;
};

// roadProfile(world, road): the road's profile, pinned at its ends (cached on the road: Road::prof).
const RoadProfile& road_profile(const World& world, const Road& road);

// The level (voxels) of a profile at arc s (profileAt: the knots where it has them, else the
// samples, held flat past the ends).
double profile_at(const RoadProfile& prof, double s);

// segmentLevel(world, seg, along): the road surface level (voxels, carriageway top) at `along` on
// a segment (of the view `seg` belongs to: hold it while asking).
double segment_level(const World& world, const RoadSeg& seg, double along);

// roadLevelAt: the level of the nearest road at a point and how the point relates to it (seg: a
// segment of `view`, along it, the distance from its centre line, whether it has sidewalks), or
// nothing when no road's right-of-way is within `reach`.
struct RoadLevel {
  double z = 0;
  const RoadSeg* seg = nullptr;
  double along = 0, dist = 0;
  bool sidewalk = false;
};
std::optional<RoadLevel> road_level_at(const World& world, const RoadView& view, double x, double y, double reach = 8);

}  // namespace svx::city
