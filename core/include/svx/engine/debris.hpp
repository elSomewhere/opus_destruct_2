// structvox — rigid debris (plan Phase 7): detached islands fall as rigid bodies and collide
// with the voxel world instead of vanishing on the spot.
//
// Each body keeps surface sample points in the body frame (at most max_points, chosen
// deterministically), its exact mass and inertia tensor about the centre of mass, and a pose
// (centre, quaternion) relative to its pose at detachment. Every substep: gravity, symplectic
// Euler integration (angular momentum is the state, so free pieces precess), then contacts:
// sample points inside solid world voxels are contacts along the face of their voxel with the
// least penetration that leads to air; restitution and (box) Coulomb friction impulses are
// solved by projected Gauss-Seidel on accumulated impulses, plus a position correction. The
// first landing after a fall is reported (the engine turns heavy landings into impact loads on
// the structure). Bodies sleep when slow, then fade and are removed.
//
// No transcendental functions (first-order quaternion update + normalization): the motion is
// bit-identical across native and WASM builds, like the structural solver.
// Not simulated in v1: debris-debris contact, debris resting weight on the structure.
#pragma once

#include <array>
#include <vector>

#include "svx/world/grid.hpp"

namespace svx {

struct DebrisParams {
  f64 restitution = 0.15;
  f64 friction = 0.6;
  f64 sleep_speed = 0.1;      // m/s (linear + 0.5 m x angular)
  int sleep_ticks = 20;
  f64 fade_after = 3.0;       // s asleep before fading out
  f64 fade_time = 1.0;        // s
  f64 max_age = 10.0;         // s: fade out regardless
  int max_points = 256;       // collision samples per body
  int substeps = 2;
  int iterations = 8;         // contact solver sweeps per substep
  f64 impact_speed = 2.5;     // m/s: landings faster than this are reported
  f64 relaunch_air_time = 0.25;  // s airborne before the next landing is reported again
  f64 kill_depth = 20.0;      // m below the world's lowest voxel: removed
  int hard_cap = 192;         // bodies beyond this are removed at once (oldest first)
  int min_voxels = 4;         // smaller pieces are not simulated (the front end fades them)
};

struct DebrisBody {
  i64 id = 0;
  std::vector<std::array<f32, 3>> pts;  // body-frame collision samples (relative to the centre)
  f64 mass = 0.0;
  std::array<f64, 9> inv_inertia{};     // body frame
  std::array<f64, 3> x0{0, 0, 0};       // centre of mass at detachment
  std::array<f64, 3> x{0, 0, 0}, v{0, 0, 0};
  std::array<f64, 3> L{0, 0, 0};        // angular momentum about the centre (world)
  std::array<f64, 3> w{0, 0, 0};        // angular velocity (world), derived from L and q
  std::array<f64, 4> q{0, 0, 0, 1};     // rotation since detachment (x, y, z, w)
  f64 age = 0.0, air_time = 0.0, fade = 1.0, fade_start = 0.0;
  int slow_ticks = 0;
  bool asleep = false, landed = false;
};

struct DebrisImpact {
  i64 body = 0;
  std::array<f64, 3> pos{0, 0, 0};      // mean contact point
  std::array<f64, 3> impulse{0, 0, 0};  // N s, on the world
  f64 mass = 0.0, speed = 0.0;
};

class DebrisSystem {
 public:
  void clear() {
    bodies_.clear();
    finished_.clear();
  }
  // Voxels in world voxel coordinates with their masses; initial linear / angular velocity
  // (world). Returns false (no body) for empty or massless input.
  bool add(i64 id, const std::vector<IVec3>& voxels, const std::vector<f64>& masses, f64 h, const std::array<f64, 3>& v,
           const std::array<f64, 3>& w, const DebrisParams& p);
  // One tick of dt; appends reported landings to `impacts` (in body order).
  void step(f64 dt, const VoxelGrid& g, const DebrisParams& p, std::vector<DebrisImpact>* impacts);
  // Starts fading the oldest bodies so that at most `n` are not fading; beyond p.hard_cap the
  // oldest are removed at once.
  void limit(int n, const DebrisParams& p);
  const std::vector<DebrisBody>& bodies() const { return bodies_; }
  // Removes (and returns the ids of) bodies that finished fading.
  std::vector<i64> take_finished();

 private:
  std::vector<DebrisBody> bodies_;
  std::vector<i64> finished_;
};

}  // namespace svx
