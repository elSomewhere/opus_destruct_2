// svx_anim — a character's view of the core's World (CollisionWorld): the static voxels of the
// world grid and of the oriented grids (a turned wall, a bridge deck) for its ground and its
// spheres; rays see the pieces too (a car, a crate, rubble), as World::raycast does.
#pragma once

#include "svx/anim/physics/collision.hpp"
#include "svx/world/world.hpp"

namespace svx::anim {

class WorldCollision final : public VoxelCollision {
 public:
  explicit WorldCollision(const World& w);
  const World& world;
  f64 raycast(const V3& o, const V3& d, f64 max_dist) const override;
};

}  // namespace svx::anim
