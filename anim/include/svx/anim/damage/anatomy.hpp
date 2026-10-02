// svx_anim — procedural anatomy in rest rig space, scaled with the build.
#pragma once
#include "svx/anim/voxel/model.hpp"
namespace svx::anim {
enum class VitalKind : u8 { Brain, Cord, Heart, Lung, Liver, Vessel };
struct VitalRegion {
  std::string name;
  VitalKind kind;
  i32 bone;
  V3 centre, radii;
};
std::vector<VitalRegion> anatomy_regions(const Skeleton& skeleton);
void fill_interior(VoxelModel& model);
f64 tissue_resistance(u8 slot);
f64 segment_distance(const V3& p, const V3& a, const V3& b);
}  // namespace svx::anim
