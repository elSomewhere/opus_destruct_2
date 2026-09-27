// structvox — rigid voxel bodies (docs/V2_DESIGN.md §4).
//
// A body is a set of voxels in its own grid-aligned frame (the "shape frame": the world voxel
// coordinates it had when it was made, metres = h p), with a pose (centre of mass x, rotation q):
// a shape-frame point s is at x + R (s - com). Bodies keep their fragments and bond graph (the
// world's fracture layer), so a body is structure that can keep breaking.
//
// Contacts: surface sample points (lattice corners of exposed faces, pulled slightly inwards)
// are tested against the world grid and against other bodies' shapes; a point inside a solid
// voxel is a contact along the face of least penetration that leads to air. Contacts per pair
// are reduced to a spread manifold. Solver: sequential impulses (projected Gauss-Seidel) with
// warm starting, Coulomb friction, restitution for fast impacts and split-impulse position
// correction (the impulses stay true forces: the fracture layer reads them). Islands sleep.
// No transcendental functions: bit-identical across native and WASM.
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/world/grid.hpp"

namespace svx {

struct BodyShape {
  IVec3 lo{0, 0, 0};                  // shape-frame voxel coordinates of cell (0, 0, 0)
  std::array<i32, 3> dim{0, 0, 0};
  std::vector<Vox> vox;               // air or a (non-anchored) voxel value
  std::vector<u16> frag;              // body fragment + 1 (0 none)
  std::vector<u8> brk;                // broken face bits (+x, +y, +z), like the grid
  i32 count = 0;                      // solid voxels
  i32 index(const IVec3& p) const {   // -1 outside
    const i32 x = p[0] - lo[0], y = p[1] - lo[1], z = p[2] - lo[2];
    if (x < 0 || y < 0 || z < 0 || x >= dim[0] || y >= dim[1] || z >= dim[2]) return -1;
    return (x * dim[1] + y) * dim[2] + z;
  }
  IVec3 voxel(i32 i) const { return {lo[0] + i / (dim[1] * dim[2]), lo[1] + (i / dim[2]) % dim[1], lo[2] + i % dim[2]}; }
  Vox get(const IVec3& p) const {
    const i32 i = index(p);
    return i < 0 ? kAir : vox[size_t(i)];
  }
  bool broken(const IVec3& p, int axis) const {  // the face between p and p + e_axis
    const i32 i = index(p);
    return i >= 0 && ((brk[size_t(i)] >> axis) & 1) != 0;
  }
};

struct BodyGraph;  // the fracture layer's bond graph of a body (world)

struct BodyFrag {
  V3 com;                             // shape frame
  f64 mass = 0.0;
  M3 inertia;                         // about com
  MaterialId mat = MaterialId::Concrete;
  i32 count = 0;
  f64 strength = 1.0;                 // design strength multiplier (from the world)
};

struct Body {
  i64 id = 0;
  BodyShape shape;
  std::vector<BodyFrag> frags;
  // mass properties (shape frame)
  V3 com;
  f64 mass = 0.0, inv_mass = 0.0;
  M3 inertia, inv_inertia;            // about com, shape axes
  // state
  V3 x, v, w;                         // centre of mass, linear and angular velocity (world)
  Quat q;
  V3 v_pre, w_pre;                    // velocities before this substep's contact solve
  // collision samples: shape-frame points relative to com, and the voxel they belong to
  std::vector<V3> pts;
  std::vector<i32> pt_vox;            // shape voxel index
  f64 radius = 0.0;                   // max |pt|
  V3 box_lo, box_hi;                  // world AABB (samples), refreshed each substep
  // sleep
  bool asleep = false;
  i32 still = 0;                      // still substeps (a jitter takes some back, motion all)
  f64 sleep_ema = 1.0;                // smoothed speed (m/s)
  f64 age = 0.0;
  // (world) reported to the host (PieceAdded); the piece it broke from (0: the static world)
  bool announced = false;
  i64 parent = 0;
  // world data (fracture layer)
  std::shared_ptr<BodyGraph> graph;
  i32 stress_cooldown = 0;
  f64 last_load = 0.0, last_phi = 0.0;
  bool graph_dirty = true;
  bool was_asleep = false;
  // parts of a piece that broke in a collision do not touch each other for the rest of that
  // substep: the failed interface carried the load up to its strength, and then no more
  i64 family = 0;
  i32 family_ticks = 0;
  // (collision) the samples' world positions at pose (wpts_x, wpts_q)
  std::vector<V3> wpts;
  V3 wpts_x;
  Quat wpts_q{0, 0, 0, 0};

  V3 to_world(const V3& s) const { return x + rotate(q, s - com); }
  V3 to_shape(const V3& X) const { return com + rotate_inv(q, X - x); }
  void refresh_box() {
    const f64 r = radius + 0.1;
    box_lo = x - V3{r, r, r};
    box_hi = x + V3{r, r, r};
  }
  M3 inv_inertia_world() const {
    const M3 R = to_matrix(q);
    return R * inv_inertia * transpose(R);
  }
};

// Rebuilds a body's mass properties (from its fragments) and collision samples (from its shape).
void body_refresh(Body& b, f64 h, int max_points);

struct RigidParams {
  f64 gravity = 9.81;
  int substeps = 2;
  int iterations = 10;
  int position_iterations = 4;
  // Busy (the violent part of a collapse: more than busy_bodies pieces faster than busy_speed):
  // one substep a tick, busy_iterations velocity and 2 position iterations. Settling rubble is
  // never busy (full quality, so piles come to rest).
  int busy_bodies = 150;
  f64 busy_speed = 2.0;              // m/s
  int busy_iterations = 6;
  size_t busy_contacts = 6000;       // also beyond this many contacts (a large pile settling): busy
  f64 restitution = 0.1;             // for impacts faster than bounce_speed
  f64 bounce_speed = 2.0;
  f64 friction = 0.65;
  f64 slop = 0.01;                   // m of allowed penetration
  f64 baumgarte = 0.3;
  f64 max_correction = 2.0;          // m/s pseudo velocity cap
  f64 max_speed = 25.0;
  f64 rest_damping = 0.2;            // per 1/120 s, for touching bodies slower than rest_speed
  f64 rest_speed = 0.9;              // m/s (linear + radius x angular): settling rubble (rubble is rough) ...
  f64 rest_radius = 1.5;             // ... of pieces smaller than this (m): a large piece toppling slowly is not held
  f64 linear_damping = 0.02, angular_damping = 0.08;  // 1/s
  f64 sleep_speed = 0.15;            // m/s (linear + radius x angular)
  int sleep_substeps = 30;           // (of 1/120 s) still before sleeping
  int max_points = 1024;             // collision samples per body
  int manifold = 12;                 // contacts kept per body pair ...
  f64 manifold_per_m = 8.0;          // ... plus this per m of the body's radius (a bearing surface)
  f64 kill_depth = 30.0;             // m below the world: removed
};

struct Contact {
  i32 a = -1, b = -1;                // body indices (b = -1: the world)
  V3 p, n;                           // world point, normal (pushes a out of b)
  f64 depth = 0.0;
  i32 vox_a = -1;                    // a's shape voxel of the sample
  i32 vox_b = -1;                    // b's shape voxel hit (b >= 0)
  IVec3 wvox{0, 0, 0};               // world voxel hit (b = -1)
  V3 ra, rb, t1, t2;
  f64 kn = 0, k1 = 0, k2 = 0;
  f64 ln = 0, l1 = 0, l2 = 0;        // accumulated impulses (N s)
  f64 lp = 0;                        // pseudo impulse (position correction)
  f64 bounce = 0, bias = 0;
  f64 approach = 0;                  // normal approach speed before the solve (m/s, > 0 closing)
  f64 mu = 0.6;
  u64 key = 0;
  V3 impulse() const { return n * ln + t1 * l1 + t2 * l2; }  // on a (b receives the opposite)
};

class RigidWorld {
 public:
  RigidParams par;
  std::vector<std::unique_ptr<Body>> bodies;  // ascending id (deterministic order)

  // Contacts of the last substep's final solve (read by the fracture layer / structure loads).
  const std::vector<Contact>& contacts() const { return contacts_; }

  // One substep of dt. `fracture` (optional) runs after the contact solve and returns 0 (nothing
  // changed), 1 (bodies were split / removed / added: the contacts are carried over to the new
  // body list) or 2 (as 1, and velocities are rolled back to their pre-solve values and the step
  // solved again with the new bodies, before positions move).
  void substep(f64 dt, const VoxelGrid& g, const std::function<int(f64 dt)>& fracture);
  void add(std::unique_ptr<Body> b);               // keeps id order
  void remove_if(const std::function<bool(const Body&)>& pred);
  void wake_box(const V3& lo, const V3& hi);       // wakes bodies overlapping a world box
  void wake(Body& b);
  i32 awake_count() const;
  Body* find(i64 id);
  // Busy now? (a function of the bodies' state: the same on every thread count and platform)
  bool busy() const;
  // accumulated wall time (ms) per phase: collide, solve, fracture, rollback (collide + solve),
  // integrate + sleep
  f64 prof_ms[5] = {0, 0, 0, 0, 0};

 private:
  void integrate_velocities(f64 dt);
  void collide(const VoxelGrid& g, const std::vector<u8>* only = nullptr);
  void reduce_manifold(std::vector<Contact>& cs, f64 h) const;
  void solve(f64 dt);
  void integrate_positions(f64 dt);
  void sleep_update(f64 dt);
  void refresh_boxes();
  std::vector<Contact> contacts_;
  bool busy_ = false;
  f64 sleep_speed_ = 0.15;  // (the sleep / wake threshold of the current substep length)
  void set_step(f64 dt);
  std::unordered_map<u64, std::array<f64, 3>> warm_;  // contact key -> (ln, l1, l2)
  std::vector<i32> island_;  // (scratch)
  std::vector<V3> pseudo_v_, pseudo_w_;  // split-impulse pseudo velocities of the last solve
};

}  // namespace svx
