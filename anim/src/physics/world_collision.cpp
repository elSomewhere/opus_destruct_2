#include "svx/anim/physics/world_collision.hpp"

namespace svx::anim {

WorldCollision::WorldCollision(const World& w)
    : VoxelCollision(w.voxel_size(),
                     [&w](i32 i, i32 j, i32 k) {
                       const IVec3 p{i, j, k};
                       return vox_solid(w.grid().get(p)) || w.grid_solid(p);
                     }),
      world(w) {}

f64 WorldCollision::raycast(const V3& o, const V3& d, f64 max_dist) const {
  if (!world.in_range(o) || !(max_dist > 0.0)) return -1.0;
  const RayHit h = world.raycast(o, d, max_dist);
  return h.hit ? h.distance : -1.0;
}

}  // namespace svx::anim
