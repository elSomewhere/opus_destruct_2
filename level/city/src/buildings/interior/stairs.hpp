// svx_city — U-shaped (switchback) stairs on the voxel grid (voxel_city buildings/interior/stairs.js).
//
// Every riser is 1 voxel (12.5 cm) and every tread 2 voxels (25 cm), so a walker with a 1-2 voxel
// step-up climbs any stair. A stair stacks through floors f0..f1: from each floor f < f1 a first
// flight climbs half a story along lane A to a mid landing at the far end, a second flight
// returns along lane B to the NEAR landing of floor f + 1, which is part of that floor's slab (the
// door into the stairwell is always at the near end).
//
// Local stair frame: s runs along the flights from the near end (s = 0), t runs across (lane A =
// [0, lane - 1], the divider = lane, lane B beyond).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "buildings/interior/grid.hpp"
#include "core/rect.hpp"

namespace svx::city {

constexpr double kStairLane = 8;     // LANE: 1.0 m
constexpr double kStairLanding = 9;  // LANDING: 1.125 m

// stairDims: the inner stairwell size for a story height H (voxels).
struct StairDims {
  double W = 0, L = 0, lane = 0, landing = 0;
};
StairDims stair_dims(double H, double lane = kStairLane, double landing = kStairLanding);

// One U-turn of a stair: from floor f (its slab at z0) a story of height H up.
struct StairFlight {
  double f = 0, z0 = 0, H = 0;
};

// A stair (makeStair's record, and what PlanBuilder.addStair adds: its id and, when the planner
// gave none, a flight per floor f0 .. f1 - 1).
struct Stair {
  Rect rect{};            // the stairwell's interior (building canonical)
  char axis = 'v';        // the run axis, 'u' | 'v'
  double dir = 1;         // +1: the near end at the low coordinate, -1: at the high one
  bool lane_low = true;   // laneLow: lane A at the low across-coordinate
  double lane = kStairLane, landing = kStairLanding;
  double f0 = 0, f1 = 0;  // the floors it joins
  double L = 0, W = 0;    // along and across the run
  bool open = false;      // an open stair (a house's): a rail for a divider, open doors
  std::optional<double> id = std::nullopt;
  std::optional<std::vector<StairFlight>> flights = std::nullopt;
};

// makeStair's arguments ({rect, axis, dir, laneLow = true, lane = LANE, landing = LANDING, f0,
// f1, open = false}).
struct MakeStairOpts {
  Rect rect{};
  char axis = 'v';
  double dir = 1;
  bool lane_low = true;
  double lane = kStairLane, landing = kStairLanding;
  double f0 = 0, f1 = 0;
  bool open = false;
};
Stair make_stair(const MakeStairOpts& o);

// Canonical (u, v) -> local (s, t); null outside the stairwell.
struct StairST {
  double s = 0, t = 0;
};
std::optional<StairST> stair_local(const Stair& st, double u, double v);

// The near-landing rect (canonical): where the stair door must be.
Rect near_landing(const Stair& st);

// Is the slab of floor f open (no floor) at (u, v)?
bool stair_slab_open(const Stair& st, double f, double u, double v);

// The materials of stairBoxes' treads, landings, divider (closed stairs) and rail (open ones).
struct StairMats {
  uint16_t tread = 0, landing = 0, divider = 0, rail = 0;
};
// Geometry boxes for the stair (canonical u, v; absolute z), one U-turn per flight. The stair
// must have its flights (PlanBuilder.addStair).
std::vector<CanonBox> stair_boxes(const Stair& st, const StairMats& mats);

}  // namespace svx::city
