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
};

}  // namespace svx
