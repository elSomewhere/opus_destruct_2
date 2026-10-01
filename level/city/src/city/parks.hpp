// svx_city — landscape parks (voxel_city city/parks.js): a layout per park space, made once from
// its id and size, shared by the ground surface (city/landscape) and the dressing:
//
//   entrances  on the street sides, 2-5 of them, not at the corners
//   hub        a small round plaza off-centre where the main paths meet
//   paths      curved (quadratic) walks from every entrance to the hub, sampled every 2 m into
//              segments on a lookup grid; one or two cross links between entrances
//   pond       in bigger parks: an irregular shore (a few harmonics on the radius), off-centre,
//              with a walk round most of it
//   groves     tree clumps where a coarse noise is high, open lawn and meadow (long grass, left
//              unmown) elsewhere
//
// Coordinates are relative to the space rect (voxels).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/rect.hpp"

namespace svx::city {

struct OpenSpace;    // city/space.hpp
struct SpaceSample;  // city/space.hpp

struct ParkPoint {
  double u = 0, v = 0;
};
// The hub: a round plaza of radius r.
struct ParkHub {
  double u = 0, v = 0, r = 0;
};
// The pond: centre, mean radius, three harmonics of the shore (amplitudes a, phases p), stretched
// along its own u by `stretch` and turned by `rot` (radians).
struct ParkPond {
  double u = 0, v = 0, r = 0;
  std::array<double, 3> a{}, p{};
  double stretch = 1, rot = 0;
};
// A path segment [u0, v0, u1, v1].
using ParkSeg = std::array<double, 4>;

struct ParkLayout {
  double W = 0, H = 0;  // the space's rect: x1 - x0, y1 - y0
  double seed = 0;      // hashString(space.id)
  ParkHub hub;
  std::vector<ParkPoint> ents;
  std::optional<ParkPond> pond;  // (null: none)
  std::vector<ParkSeg> segs;     // the paths (those through the pond dropped)
  // The lookup grid (8 m buckets): key floor(u / G) * 4096 + floor(v / G) -> the segments within
  // 2 m of the bucket (indices into segs, in order). JS's key: two buckets whose keys meet share
  // their list.
  std::unordered_map<int64_t, std::vector<uint32_t>> grid;
  double G = 0;       // the grid's bucket (voxels)
  double path_w = 0;  // the paths' half width (voxels)
};

// The layout of a park of id `id` over `rect` (no cache: park_layout keeps one per space).
ParkLayout plan_park_layout(const std::string& id, const Rect& rect);
// parkLayout(space): the space's layout, made once per space record (JS: a WeakMap by space).
const ParkLayout& park_layout(const OpenSpace& space);

// Signed distance (voxels, approximate) to the pond's shore, negative in the water.
double pond_dist(const ParkPond& pond, double u, double v);
// Distance (voxels) from (u, v) to the nearest path centre line (Infinity off the grid's lists).
double path_dist(const ParkLayout& L, double u, double v);
// 0..1 grove value: trees stand in clumps where it is high.
double grove_at(const ParkLayout& L, double u, double v);

// parkSurface(space, u, v, out, hx, hy): park ground at (u, v) relative to the space rect - lawn,
// unmown meadow, paths, the hub, a pond with reeds, flower beds round the hub. (hx, hy: the
// position for texture hashes, canonical in a wrapping world; JS's default: (u, v).) Sets out.mat,
// and out.water and out.dz in the pond (out.dz, out.water as given elsewhere).
void park_surface(const ParkLayout& L, double u, double v, SpaceSample& out, double hx, double hy);
void park_surface(const OpenSpace& space, double u, double v, SpaceSample& out, double hx, double hy);
inline void park_surface(const OpenSpace& space, double u, double v, SpaceSample& out) { park_surface(space, u, v, out, u, v); }

}  // namespace svx::city
