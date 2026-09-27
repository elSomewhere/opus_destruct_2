// structvox game — reinforcement for generated concrete members: bars (MaterialId::Rebar
// voxels) embedded in columns, walls, slabs and beams.
#pragma once

#include "svx/world/grid.hpp"

namespace svx {

// Places bars inside the box member [lo, hi): along each axis the member is long in (at least
// 2 x spacing), on a lattice `spacing` voxels apart in its cross-section, one voxel in from the
// faces (a cross-section thinner than 3 voxels gets none: nothing would cover the bar). Only
// the member's solid free voxels become bars (openings stay open). Returns the bar voxels.
i64 reinforce(VoxelGrid& g, const IVec3& lo, const IVec3& hi, i32 spacing = 4);

}  // namespace svx
