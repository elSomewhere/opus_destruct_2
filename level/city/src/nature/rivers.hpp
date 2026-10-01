// svx_city — rivers (voxel_city nature/rivers.js): meandering channels along the zero isolines
// of a domain-warped, low-frequency noise. A second noise fades rivers in and out (a creek narrows
// to nothing: a spring), so the network has sources and varying widths without any global flow
// computation: every query is a pure function of position, which keeps rivers streamable and
// seamless.
//
// Only the ground tile carves them (valleys with sloped banks in the countryside, stone quays in
// cities); roads crossing a river become bridges there. Terrain heights used for planning stay
// uncarved, so lots, stations and sewers are planned against the street level and simply keep
// clear of river corridors (hits_rect).
//
// The water level follows the local minimum of the terrain sampled around the point, a little
// below it, so the surface never floats above the land and steps only rarely.
//
// Immutable: any thread may query. (The reference's at() returns one shared object that the next
// call overwrites; here a value, which is what its callers read.)
#pragma once

#include <optional>

#include "core/js.hpp"
#include "core/noise.hpp"
#include "core/rect.hpp"

namespace svx::city {

class World;

// Rivers.at's record: the distance (m) from the centre line, the half width (m), the voxel z of
// the water surface and of the bottom at this column, how much of a quay it is (urban, 0..1) and
// the metres of sloped bank.
struct RiverInfo {
  double d = 0, half = 0, water = 0, bed = 0, urban = 0, bank = 0;
};

class Rivers {
 public:
  explicit Rivers(const World& world);
  Rivers(const Rivers&) = delete;
  Rivers& operator=(const Rivers&) = delete;

  const World* world;
  SimplexNoise n, warp, mask;
  // config.rivers (read once): enabled (truthiness), maxHalfWidth
  bool enabled = false;
  double max_half_width = 0;

  // The river field at a field point (fw NaN: a 3D chart).
  double field(double fx, double fy, double fz, double fw = js::kNaN) const;
  // River info at voxel (x, y), or nothing when there is no river within its banks.
  std::optional<RiverInfo> at(double x, double y) const;
  // Water surface z (voxels): below the lowest nearby street / ground.
  double water_level(double x, double y) const;
  // Carved ground height (voxels) for a column with surface h (voxels).
  double ground_at(const RiverInfo& info, double h) const;
  // Distance (m) from a point (voxels) to the channel's edge, negative in the water; Infinity far
  // from any river. Unlike at() it does not stop at the banks (a town river has none), so it can
  // test margins.
  double channel_gap(double x, double y) const;
  // Does the channel (plus a margin in m) touch a rect (voxels)? Sampled every ~10 m.
  bool hits_rect(const Rect& r, double margin_m = 6) const;
};

}  // namespace svx::city
