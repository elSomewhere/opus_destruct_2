// structvox — rigid voxel bodies (docs/V2_DESIGN.md §4, docs/GRIDS.md).
//
// A body is one or more voxel shapes in its own frame (the "shape frame", or body frame), with a
// pose (centre of mass x, rotation q): a body-frame point s is at x + R (s - com). Each shape is a
// voxel lattice placed in the body frame (BodyShape::xf): a piece that broke off one grid has one
// shape, its lattice the frame (the grid voxel coordinates it had when it was made, metres =
// h p); a piece made of several grids (a rotated beam welded to a wall) has one shape per grid,
// placed as the grids were. Bodies keep their fragments and bond graph (the world's fracture
// layer), so a body is structure that can keep breaking.
//
// Contacts: surface sample points (lattice corners of exposed faces, pulled slightly inwards)
// are tested against the static grids (the world's, and oriented ones: each a lattice with a
// frame of its own) and against other bodies' shapes; a point inside a solid voxel is a contact
// along the face of least penetration that leads to air (in that voxel's lattice). Contacts per
// pair are reduced to a spread manifold. Solver: sequential impulses (projected Gauss-Seidel)
// with warm starting, Coulomb friction, restitution for fast impacts and split-impulse position
// correction (the impulses stay true forces: the fracture layer reads them). Islands sleep.
// No transcendental functions (at whole-numbered substeps of 1/120 s): bit-identical across
// native and WASM.
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <array>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/phys/joint.hpp"
#include "svx/world/grid.hpp"

namespace svx {

// A voxel lattice placed in a frame: lattice point s (metres: voxel p's centre is h p) is at
// off + R s. The identity keeps the arithmetic of an unplaced lattice (the world grid's, a body's
// first shape) exactly: results never change by going through it.
struct LatticeXf {
  V3 off;
  Quat q;                              // R as a quaternion
  M3 R = M3::identity(), Rt = M3::identity();
  bool identity = true;
  static LatticeXf make(const V3& off, const Quat& q);  // (q normalized; the identity when it is one)
  V3 to(const V3& s) const { return identity ? s : off + R * s; }
  V3 from(const V3& X) const { return identity ? X : Rt * (X - off); }
  V3 dir_to(const V3& d) const { return identity ? d : R * d; }
  V3 dir_from(const V3& d) const { return identity ? d : Rt * d; }
};
// b after a: first a, then b (b's frame is a's target frame)
LatticeXf compose(const LatticeXf& b, const LatticeXf& a);
LatticeXf inverse(const LatticeXf& a);

// A broken junction sample of a body shape's voxel (cell index, face, sample; sub
// kJunctionFace: the whole face), sorted in BodyShape::jbrk.
inline u64 shape_junction_code(i32 cell, int face, int sub) {
  return (static_cast<u64>(static_cast<u32>(cell)) << 16) | (static_cast<u64>(face) << 8) | static_cast<u64>(sub);
}

struct BodyShape {
  LatticeXf xf;                       // its lattice in the body frame (identity: the body's first shape)
  u32 grid = 0;                       // the id of the grid its voxels came from (0: the world grid)
  f64 h = 0.0;                        // its voxel size (m; 0: body_refresh's)
  i32 priority = 0;                   // its grid's (junctions between shapes: GridDesc::priority)
  IVec3 lo{0, 0, 0};                  // lattice voxel coordinates of cell (0, 0, 0)
  std::array<i32, 3> dim{0, 0, 0};
  std::vector<Vox> vox;               // air or a (non-anchored) voxel value
  std::vector<u32> frag;              // body fragment + 1 (0 none) (a large piece has more than 2^16)
  std::vector<u8> brk;                // broken face bits (+x, +y, +z), like the grid
  // the voxels' layer values (grid.hpp: damage, heat, ...), per cell; empty: all zero
  std::array<std::vector<u8>, kMaxLayers> layer;
  std::vector<u64> jbrk;              // broken junction samples (shape_junction_code), sorted
  i32 count = 0;                      // solid voxels
  i32 index(const IVec3& p) const {   // -1 outside
    const i64 x = i64(p[0]) - lo[0], y = i64(p[1]) - lo[1], z = i64(p[2]) - lo[2];  // (no overflow at any input)
    if (x < 0 || y < 0 || z < 0 || x >= dim[0] || y >= dim[1] || z >= dim[2]) return -1;
    return static_cast<i32>((x * dim[1] + y) * dim[2] + z);
  }
  IVec3 voxel(i32 i) const { return {lo[0] + i / (dim[1] * dim[2]), lo[1] + (i / dim[2]) % dim[1], lo[2] + i % dim[2]}; }
  Vox get(const IVec3& p) const {
    const i32 i = index(p);
    return i < 0 ? kAir : vox[size_t(i)];
  }
  u8 layer_at(int L, i32 i) const { return i < 0 || layer[size_t(L)].empty() ? 0 : layer[size_t(L)][size_t(i)]; }
  bool broken(const IVec3& p, int axis) const {  // the face between p and p + e_axis
    const i32 i = index(p);
    return i >= 0 && ((brk[size_t(i)] >> axis) & 1) != 0;
  }
  bool junction_broken(i32 cell, int face, int sub) const;
  void break_junction(i32 cell, int face, int sub);
};

struct BodyGraph;  // the fracture layer's bond graph of a body (world)

struct BodyFrag {
  V3 com;                             // body frame
  f64 mass = 0.0;
  M3 inertia;                         // about com (body frame axes)
  MaterialId mat = MaterialId::Concrete;
  i32 count = 0;
  f64 strength = 1.0;                 // design strength multiplier (from the world)
  u16 shape = 0;                      // the shape its voxels are in
};

struct Body {
  i64 id = 0;
  std::vector<BodyShape> shapes;      // one per lattice (at least one)
  i32 count = 0;                      // solid voxels, all shapes (body_refresh)
  std::vector<BodyFrag> frags;
  // mass properties (shape frame)
  V3 com;
  f64 mass = 0.0, inv_mass = 0.0;
  M3 inertia, inv_inertia;            // about com, shape axes
  // state
  V3 x, v, w;                         // centre of mass, linear and angular velocity (world)
  Quat q;
  V3 v_pre, w_pre;                    // velocities before this substep's contact solve
  // collision samples: body-frame points relative to com, and the voxel they belong to
  std::vector<V3> pts;
  std::vector<i32> pt_vox;            // shape voxel index
  std::vector<u16> pt_shape;          // ... of this shape
  f64 radius = 0.0;                   // max |pt|
  V3 box_lo, box_hi;                  // world AABB (samples), refreshed each substep
  // sleep
  bool asleep = false;
  i32 still = 0;                      // still substeps (a jitter takes some back, motion all)
  i32 held = 0;                       // (1/120 s) held up lately (sleep: a hold that flickers in a settling pile)
  f64 sleep_ema = 1.0;                // smoothed speed (m/s)
  f64 age = 0.0;
  // (world) reported to the host (PieceAdded); the piece it broke from (0: the static world)
  bool announced = false;
  // external forces during this tick (World::apply_force; cleared after the tick)
  V3 force, torque;
  bool recheck = false;  // (its strengths changed: its stress is checked again at its next contact)
  i64 parent = 0;
  i64 origin = 0;  // (world) the piece it was split from, until it joins the world (joints follow their voxels)
  bool keep = false;  // (world) never culled (World::set_piece_keep; a joint's pieces are kept too)
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
  // shape k's lattice point (metres) in the world, and back
  V3 lattice_to_world(size_t k, const V3& s) const { return to_world(shapes[k].xf.to(s)); }
  V3 world_to_lattice(size_t k, const V3& X) const { return shapes[k].xf.from(to_shape(X)); }
  // the world rotation of shape k's lattice
  Quat lattice_rot(size_t k) const { return shapes[k].xf.identity ? q : q * shapes[k].xf.q; }
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

// Rebuilds a body's mass properties (from its fragments), voxel count and collision samples
// (from its shapes: each at its own voxel size; h: that of shapes that have none yet).
void body_refresh(Body& b, f64 h, int max_points);

// A static grid the bodies collide with: the world's (the identity, unbounded) or an oriented
// grid (a lattice with a frame, bounded by its world box).
struct StaticGrid {
  const VoxelGrid* g = nullptr;
  LatticeXf xf;                      // lattice -> world
  V3 lo, hi;                         // world box (bounded grids)
  bool unbounded = false;
  u16 slot = 0;                      // (Contact::grid)
};

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
  // joints (phys/joint.hpp): position error taken out per substep beyond a slop (m), and the
  // share of the last substep's impulses a joint starts from
  f64 joint_baumgarte = 0.5;
  f64 joint_slop = 0.0005;
  f64 joint_warm = 0.9;
  f64 rest_damping = 0.2;            // per 1/120 s, for held bodies (not falling) slower than rest_speed
  f64 rest_speed = 0.9;              // m/s (linear + radius x angular): settling rubble (rubble is rough) ...
  f64 rest_radius = 1.5;             // ... of pieces smaller than this (m): a large piece toppling slowly is not held
  f64 linear_damping = 0.02, angular_damping = 0.08;  // 1/s
  f64 sleep_speed = 0.15;            // m/s (linear + radius x angular)
  int sleep_substeps = 30;           // (of 1/120 s) still before sleeping
  int max_points = 1024;             // collision samples per body
  // Continuous collision with the static grids: a body that may move more than half a voxel in a
  // substep (7.5 m/s at 120 Hz) looks along its motion, and the nearest solid faces its samples
  // would reach become speculative contacts (at most speculative_contacts, the nearest): it stops
  // at the face if it would get there, and they do nothing otherwise. A fast piece does not pass
  // through a thin wall or floor.
  bool speculative = true;
  int speculative_contacts = 8;
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
  i16 shape_a = 0, shape_b = 0;      // ... in these shapes
  u16 grid = 0;                      // the static grid hit (b = -1: StaticGrid::slot)
  IVec3 wvox{0, 0, 0};               // its voxel hit (b = -1)
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
  // Joints (phys/joint.hpp), ascending id: solved with the contacts. Their ends are set before
  // each substep by the owner (a body end that is not in `bodies`: the joint is skipped).
  std::vector<Joint> joints;
  // The clock (s) at the end of the substep being solved: joints' drives follow their programs by it.
  f64 time = 0.0;

  // Contacts of the last substep's final solve (read by the fracture layer / structure loads).
  // Their body indices refer to the body list of that substep: valid until bodies are added or
  // removed.
  const std::vector<Contact>& contacts() const { return contacts_; }
  std::vector<std::array<i32, 2>> joint_bodies() const;  // per joint: its ends' body indices now (-1: none; by id)

  // One substep of dt. `fracture` (optional) runs after the contact solve and returns 0 (nothing
  // changed), 1 (bodies were split / removed / added: the contacts are carried over to the new
  // body list) or 2 (as 1, and velocities are rolled back to their pre-solve values and the step
  // solved again with the new bodies, before positions move).
  void substep(f64 dt, const std::vector<StaticGrid>& statics, const std::function<int(f64 dt)>& fracture);
  void substep(f64 dt, const VoxelGrid& g, const std::function<int(f64 dt)>& fracture);  // (the world grid alone)
  void add(std::unique_ptr<Body> b);               // keeps id order
  void remove_if(const std::function<bool(const Body&)>& pred);
  void wake_box(const V3& lo, const V3& hi);       // wakes bodies overlapping a world box
  void wake(Body& b);
  i32 awake_count() const;
  Body* find(i64 id);
  const Body* find(i64 id) const;
  // Busy now? (a function of the bodies' state: the same on every thread count and platform)
  bool busy() const;
  // accumulated wall time (ms) per phase: collide, solve, fracture, rollback (collide + solve),
  // integrate + sleep
  f64 prof_ms[5] = {0, 0, 0, 0, 0};

 private:
  void integrate_velocities(f64 dt);
  void collide(const std::vector<StaticGrid>& statics, const std::vector<u8>* only = nullptr);
  void reduce_manifold(std::vector<Contact>& cs, f64 h) const;
  void solve(f64 dt);
  void integrate_positions(f64 dt);
  void sleep_update(f64 dt);
  static constexpr i32 kWorldRest = -1, kMachine = -2;
  std::vector<i32> resting_on() const;  // (per body: kWorldRest, kMachine, or the body index it rests on)
  const std::vector<u8>& support(f64 dt);  // (per body: held up by the world, a sleeping body or a held body)
  void refresh_boxes();
  std::vector<Contact> contacts_;
  bool busy_ = false;
  f64 sleep_speed_ = 0.15;  // (the sleep / wake threshold of the current substep length)
  f64 step_dt_ = 0.0;       // (the current substep's length)
  void set_step(f64 dt);
  std::unordered_map<u64, std::array<f64, 3>> warm_;  // contact key -> (ln, l1, l2)
  std::vector<i32> island_;  // (scratch)
  // (support: scratch)
  struct SupportPair {
    i32 a, b;
    f64 jz;  // (upward impulse on a)
    u8 up_a, up_b;
  };
  struct SupportEdge {
    i32 to;
    u8 up;
    f64 jz;
  };
  std::vector<u8> held_;
  std::vector<f64> lift_;
  std::vector<i32> sup_start_, sup_fill_, sup_queue_;
  std::vector<SupportPair> sup_pairs_;
  std::vector<SupportEdge> sup_edges_;
  std::vector<V3> pseudo_v_, pseudo_w_;  // split-impulse pseudo velocities of the last solve
  // joints (joint.cpp): per joint, its rows this substep
  struct JointPrep {
    bool on = false;                   // (both ends there, not both immovable)
    i32 ia = -1, ib = -1;              // awake bodies (-1: a frame, or a sleeping body: immovable)
    f64 ma = 0.0, mb = 0.0;
    M3 Ia, Ib;                         // inverse inertias (world)
    V3 pa, pb;                         // anchors (world)
    V3 ra, rb;                         // arms to where the linear rows act (a slider's a: to pb)
    V3 ax, t1, t2;                     // a's axis and two directions square to it (world)
    V3 n;                              // (distance) from a's anchor to b's
    M3 Kp, Ka;                         // inverted point and angular blocks
    f64 K2[4] = {0, 0, 0, 0};          // inverted 2x2: a hinge's angular rows, a slider's linear rows
    f64 kax = 0.0;                     // inverted: the axial row (distance, limit, motor)
    f64 gamma = 0.0, soft = 0.0;       // (a stretching distance joint) softness, and its stretch as a target rate
    f64 ksoft = 0.0;                   // ... its row's inverted mass with the softness
    f64 value = 0.0;                   // hinge angle, slider offset, distance
    f64 goal = 0.0, goal_rate = 0.0;   // (a drive to a target) where it is to be, and how fast that moves
    i8 lim = 0;                        // the limit bearing: -1 lower, +1 upper, 2 both (locked), 0 none
    // position errors to remove (pseudo targets)
    V3 ep, ea;                         // point, angular
    f64 e2[2] = {0, 0};                // hinge angular / slider linear
    f64 eax = 0.0;                     // axial (distance beyond its range, a limit passed)
    V3 J, L;                           // impulses on b this substep (linear, angular about its anchor)
  };
  std::vector<JointPrep> jprep_;
  f64 joint_dt_ = 1.0 / 120.0;
  void prepare_joints(f64 dt, const std::vector<M3>& Iw);
  void solve_joints();
  void solve_joints_position(std::vector<V3>& pv, std::vector<V3>& pw);
  void finish_joints(f64 dt);
  void joint_partner_speeds(std::vector<f64>& partner) const;  // (the squeeze guard)
  void wake_jointed();     // a sleeper joined to a body in motion, or to a moving frame, wakes
  void joint_stillness();  // joined bodies count towards sleep together (sleep_update)
  static bool driving(const Joint& j, const JointPrep& P);  // its drive at work (running, or short of its target)
  std::vector<u8> hanging(const std::vector<u8>& held) const;  // bodies a joint holds up (to what is held or immovable)
  std::vector<u8> machine_parts() const;                        // per body: an end of a drive at work
};

}  // namespace svx
