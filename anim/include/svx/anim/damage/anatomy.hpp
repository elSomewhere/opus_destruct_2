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
// What is inside a model: a cell with a neighbour on all six sides, in the whole body (Body) or in
// its own part only (Part: a part's faces against another stay its surface, as they show when the
// joint between them bends). Its enclosed cells become flesh, and bone where the rig says.
enum class Enclosure : u8 { Body, Part };
void fill_interior(VoxelModel& model, Enclosure enclosure = Enclosure::Body);
f64 tissue_resistance(Tissue t);  // J/m^3: the energy density removing it takes
f64 segment_distance(const V3& p, const V3& a, const V3& b);
}  // namespace svx::anim
