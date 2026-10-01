// svx_anim — what a character needs to know of the world (its only view of it): where the ground
// is (its feet), how to push a sphere out of solid geometry (a body of its own, walls within
// reach) and what a ray hits. A host implements CollisionWorld over its world; WorldCollision
// (world_collision.hpp) does it over the core's World, VoxelCollision over any voxel occupancy
// (voxel p centred at h p, the core's convention), FlatGround is a plane for tests.
#pragma once

#include <functional>
#include <optional>

#include "svx/anim/math.hpp"

namespace svx::anim {

struct SphereContact {
  V3 push;               // the push that frees the sphere (add it to the centre)
  V3 normal{0, 0, 1};    // the unit contact normal (towards the sphere)
};

// A sphere of something else a body collides with, and whose steps clear it: another body, a piece
// of debris. `owner` and `part` say whose it is (-1: nothing that gets pushed back).
struct Obstacle {
  V3 c;
  f64 r = 0.0;
  i32 owner = -1, part = -1;
  V3 v;  // its velocity (a body barged into gives way as fast as the two come together)
};

class CollisionWorld {
 public:
  virtual ~CollisionWorld() = default;
  // The walkable surface under (x, y): the top of the first solid found scanning down from z_top,
  // not below z_bottom.
  virtual std::optional<f64> ground_height(f64 x, f64 y, f64 z_top, f64 z_bottom) const = 0;
  // Resolves a sphere against the static world: false if it touches nothing.
  virtual bool sphere(const V3& c, f64 r, SphereContact* out) const = 0;
  // The distance to the first solid along the unit direction d, or -1 within max_dist.
  virtual f64 raycast(const V3& o, const V3& d, f64 max_dist) const = 0;
};

// A horizontal plane at z.
class FlatGround final : public CollisionWorld {
 public:
  explicit FlatGround(f64 z_ = 0.0) : z(z_) {}
  f64 z;
  std::optional<f64> ground_height(f64, f64, f64 z_top, f64 z_bottom) const override {
    if (z <= z_top && z >= z_bottom) return z;
    return std::nullopt;
  }
  bool sphere(const V3& c, f64 r, SphereContact* out) const override {
    const f64 d = c.z - z;
    if (d >= r) return false;
    out->push = V3{0, 0, r - d};
    out->normal = V3{0, 0, 1};
    return true;
  }
  f64 raycast(const V3& o, const V3& d, f64 max_dist) const override {
    if (d.z >= 0.0 || o.z < z) return -1.0;
    const f64 t = (z - o.z) / d.z;
    return t <= max_dist ? t : -1.0;
  }
};

// Over a voxel occupancy: voxel (i, j, k) is the cube of side h centred at h (i, j, k).
class VoxelCollision : public CollisionWorld {
 public:
  VoxelCollision(f64 h_, std::function<bool(i32, i32, i32)> solid_) : h(h_), solid(std::move(solid_)) {}
  f64 h;
  std::function<bool(i32, i32, i32)> solid;
  std::optional<f64> ground_height(f64 x, f64 y, f64 z_top, f64 z_bottom) const override;
  bool sphere(const V3& c, f64 r, SphereContact* out) const override;
  f64 raycast(const V3& o, const V3& d, f64 max_dist) const override;

 protected:
  i32 idx(f64 v) const { return static_cast<i32>(std::floor(v / h + 0.5)); }
};

}  // namespace svx::anim
