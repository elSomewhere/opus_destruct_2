// svx_city — pitched road pieces (voxel_city network/roadParts.js, ANGLED_WORLD_PLAN.md S4).
//
// Where a street runs at one grade of the pitch table (8.01% ... 20.20%: the grade limits a steep
// street's profile is held at, network/roadLevel) in a direction of the yaw table (every town
// street; not a wandering country road), its right-of-way becomes anchored oriented parts: a slab
// in a lattice of its own, turned to the street and pitched to its grade, so its surface is one
// plane, not a staircase of 12.5 cm steps. A run shares one lattice (its pieces meet without a
// seam) and is cut into pieces within kPartReach chunks of their home chunks. Everything else
// about the street stays the world grid's: the level across, junctions, curbs, markings (sampled
// on the pitched plane from the same road surface), lamps and trees beside it.
#pragma once

#include <vector>

#include "network/road.hpp"
#include "network/roadView.hpp"
#include "world/parts.hpp"

namespace svx::city {

class World;
class ChunkBuffer;

// SLAB: the slab's thickness below the road surface (cells).
constexpr double kRoadSlab = 4;

// A run of one table grade along a segment: arcs a0..a1 along it (voxels), the signed pitch index
// (climbing along the segment when positive) and the segment's table yaw.
struct PitchedRun {
  double a0 = 0, a1 = 0;
  int pitch = 0;
  int yaw = 0;
};

// pitchedRuns(world, seg): the runs of one table grade along a segment of the owner's view whose
// direction is a table yaw (none otherwise). The level at every sample of a run lies on the run's
// line; a run over water (a bridge, a causeway) keeps its deck.
std::vector<PitchedRun> pitched_runs(const World& world, const RoadSeg& seg);

// roadRunParts(world, road, segs, cell): the oriented parts of one road's pitched runs (`segs` its
// segments in its owner cell's view, `cell` the owner), each run one lattice cut into the fewest
// equal pieces within reach (index 0 until the budget grants one): kind "road", with the road, the
// segment, the arc range s0..s1 along the road, the grade and the half right-of-way.
std::vector<Part> road_run_parts(const World& world, const Road& road, const std::vector<const RoadSeg*>& segs, const PartCell& cell);

// rasterizeRoadPart(world, part, chunk): a road piece's content in its own lattice (the chunk
// addressed in the part's local cells: its wx / wy / wz are u, v, w): every column of the
// right-of-way takes the road surface found at its centre on the pitched plane, over the slab.
void rasterize_road_part(const World& world, const Part& part, ChunkBuffer& chunk);

// slabFoot(part, x, y): the world height (continuous) of a road piece's slab foot (its local w = 0
// plane) over world point (x, y), where the ground under it stops in parts mode.
double slab_foot(const Part& part, double x, double y);

}  // namespace svx::city
