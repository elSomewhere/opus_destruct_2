// structvox game — road networks (docs/VEHICLES.md): the lanes a streamed city's traffic drives.
#pragma once

#include <utility>
#include <vector>

#include "svx/base/vec.hpp"

namespace svx {

// A lane: a straight run of road one way, from where it leaves a junction to where it enters the
// next (its centre line on the road's surface, world metres).
struct Lane {
  u64 id = 0;
  V3 a, b;            // the way it runs: from a to b
  f64 width = 3.5;    // m
  f64 speed = 13.9;   // m/s: its limit
};

// A walkway: a straight run people walk from one street corner to the next - a sidewalk along a
// block's side, or a crossing over a road (its zebra) - between two corners (on the sidewalks'
// surface, world metres). Along a sidewalk people keep to `inset` off the a-b line (towards the
// buildings, clear of the lamps and the trees), coming back onto it at the corners.
struct Walk {
  u64 id = 0;
  V3 a, b;
  V3 inset;           // (a sidewalk) the walking line's offset from a-b in its middle (world)
  f64 width = 3.0;    // m
  bool crossing = false;
};

// A kerbside parking place: where a car stands (on the road's surface) and its heading.
struct ParkingSpot {
  V3 pos;
  f64 yaw = 0.0;
  u64 id = 0;         // (stable: the same place has the same id)
};

class RoadNetwork {
 public:
  virtual ~RoadNetwork() = default;
  // The lanes running through the box (x, y; z ignored), in a stable order.
  virtual void lanes_in(const V3& lo, const V3& hi, std::vector<Lane>& out) const = 0;
  virtual bool lane(u64 id, Lane* out) const = 0;
  // The lanes a car at the end of a lane may go on along through its junction, with the turn:
  // -1 left, 0 straight on, 1 right.
  virtual void next(u64 lane, std::vector<std::pair<u64, int>>& out) const = 0;
  // Whether a lane's traffic may enter its junction at this time (its signal is green).
  virtual bool green(u64 lane, f64 time) const {
    (void)lane, (void)time;
    return true;
  }
  // Kerbside parking places in the box.
  virtual void parking_in(const V3& lo, const V3& hi, std::vector<ParkingSpot>& out) const {
    (void)lo, (void)hi, (void)out;
  }
  // Walkways (none: a network without people) in the box, in a stable order; one by its id; the
  // ones that meet one's end (0: a, 1: b) at its corner, and at which of their ends.
  virtual void walks_in(const V3& lo, const V3& hi, std::vector<Walk>& out) const {
    (void)lo, (void)hi, (void)out;
  }
  virtual bool walk(u64 id, Walk* out) const {
    (void)id, (void)out;
    return false;
  }
  virtual void walk_next(u64 id, int end, std::vector<std::pair<u64, int>>& out) const {
    (void)id, (void)end, (void)out;
  }
  // Whether people may step onto a crossing now (the traffic over it is held by its signal long
  // enough to get across); sidewalks: always.
  virtual bool walk_open(u64 id, f64 time) const {
    (void)id, (void)time;
    return true;
  }
};

}  // namespace svx
