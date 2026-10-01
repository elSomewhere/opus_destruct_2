// svx_city — per-column road surface classification (voxel_city network/roadSurface.js).
//
// Roads are signed distance fields round their centre lines: the carriageway union uses a
// circular smooth minimum between different roads, which gives exact curb fillets (corner radii)
// at every junction, T or X, straight or curved; the right-of-way union a sharp minimum, so
// property lines stay square. Markings, crosswalks, stop lines, medians, parking lanes and
// sidewalk patterns are then resolved in the frame of the dominant road.
//
// The sample (makeRoadSample) is written in place and keeps what a call does not set (JS reuses
// one object): along and side are only set where a road is near, q only on a sidewalk.
#pragma once

#include <cstdint>
#include <vector>

#include "network/roadView.hpp"

namespace svx::city {

// KIND
namespace RoadKind {
enum : int { NONE = 0, CARRIAGE = 1, CURB = 2, SIDEWALK = 3, MEDIAN = 4, PLAZA = 5, SHOULDER = 6, STRIP = 7 };
}

struct RoadSample {
  int kind = RoadKind::NONE;
  uint16_t mat = 0;
  double dz = 0;                  // height above the road grade (voxels)
  const RoadSeg* seg = nullptr;   // the dominant segment (of the view the candidates came from)
  double along = 0, side = 0;     // the column in its frame: arc along it, signed lateral offset
  double q = 0;                   // a sidewalk's distance from the curb face
  double sdf_c = 0, sdf_r = 0;    // carriageway and right-of-way signed distances
  double period = 0;              // a wrapping world's size in voxels (texture cells repeat with it), 0 unbounded
};

// makeRoadSample(period)
inline RoadSample make_road_sample(double period = 0) {
  RoadSample s;
  s.period = period;
  return s;
}

// sampleRoadSurface(cands, px, py, out, seed, reach): cands the segments near the column
// (RoadView::near), (px, py) its centre (voxels, x + 0.5).
RoadSample& sample_road_surface(const std::vector<const RoadSeg*>& cands, double px, double py, RoadSample& out, double seed = 0, double reach = 0);

}  // namespace svx::city
