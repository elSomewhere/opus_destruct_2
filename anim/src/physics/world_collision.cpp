#include "svx/anim/physics/world_collision.hpp"

namespace svx::anim {

WorldCollision::WorldCollision(const World& w)
    : VoxelCollision(w.voxel_size(),
                     [this](i32 i, i32 j, i32 k) {
                       const IVec3 p{i, j, k};
                       return vox_solid(world_->grid().get(p)) || world_->grid_solid(p);
                     }),
      world_(&w) {}

f64 WorldCollision::raycast(const V3& o, const V3& d, f64 max_dist) const {
  if (!world_->in_range(o) || !(max_dist > 0.0)) return -1.0;
  const RayHit h = world_->raycast(o, d, max_dist);
  return h.hit ? h.distance : -1.0;
}

}  // namespace svx::anim
