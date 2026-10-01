// Rigid voxel bodies (docs/V2_DESIGN.md §4): resting contact, stacking, momentum.
#include <cmath>
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/frag/fragments.hpp"
#include "svx/phys/rigid.hpp"

using namespace svx;

namespace {

VoxelGrid ground() {
  VoxelGrid g;
  g.h = 0.125;
  for (i32 x = -32; x < 64; ++x)
    for (i32 y = -32; y < 64; ++y) g.fill_column(x, y, -4, 0, make_vox(MaterialId::Rock, true));
  g.compact();
  g.lo = {-32, -32, -4};
  g.hi = {64, 64, 64};
  return g;
}

// A solid box of voxels [lo, lo + n) as one body of one fragment, at rest.
std::unique_ptr<Body> box(i64 id, const IVec3& lo, const IVec3& n, f64 h) {
  auto b = std::make_unique<Body>();
  b->id = id;
  b->shapes.resize(1);
  BodyShape& S = b->shapes[0];
  S.lo = lo;
  S.dim = {n[0], n[1], n[2]};
  const size_t cells = size_t(n[0]) * size_t(n[1]) * size_t(n[2]);
  S.vox.assign(cells, make_vox(MaterialId::Concrete, false));
  S.frag.assign(cells, 1);
  S.brk.assign(cells, 0);
  S.count = static_cast<i32>(cells);
  f64 sums[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const f64 m = material(MaterialId::Concrete).rho * h * h * h;
  for (i32 i = 0; i < static_cast<i32>(cells); ++i) {
    const IVec3 p = S.voxel(i);
    accumulate_voxel(m, V3{h * p[0], h * p[1], h * p[2]}, h, sums);
  }
  const MassProps mp = finish_mass(sums, h);
  BodyFrag f;
  f.com = mp.com;
  f.mass = mp.mass;
  f.inertia = mp.inertia;
  f.count = static_cast<i32>(cells);
  b->frags.push_back(f);
  body_refresh(*b, h, 512);
  b->x = b->com;
  return b;
}

f64 lowest_point(const Body& b) {
  f64 z = 1e9;
  for (const V3& p : b.pts) z = std::min(z, b.to_world(b.com + p).z);
  return z;
}

}  // namespace

TEST_CASE("rigid: a dropped block comes to rest on the ground and sleeps") {
  const VoxelGrid g = ground();
  RigidWorld w;
  w.add(box(1, {10, 10, 16}, {4, 4, 4}, g.h));  // bottom face 2 m up
  for (int s = 0; s < 2 * 60 * 4; ++s) w.substep(1.0 / 120.0, g, nullptr);
  const Body& b = *w.bodies[0];
  CHECK(b.asleep);
  // resting on the ground's top face (z = -h/2) within the slop and the sample inset
  const f64 bottom = b.x.z - 2.0 * g.h;
  CHECK(bottom == doctest::Approx(-0.5 * g.h).epsilon(0.05).scale(1.0));
  CHECK(std::abs(bottom + 0.5 * g.h) < 0.05);
  CHECK(lowest_point(b) > -0.5 * g.h - 0.05);
}

TEST_CASE("rigid: a block comes to rest and sleeps as soon at any substep length (its rates and counts are per 1/120 s)") {
  const VoxelGrid g = ground();
  f64 base = 0.0;
  for (int per_tick : {2, 4, 8, 6, 1}) {
    RigidWorld w;
    w.add(box(1, {10, 10, 16}, {4, 4, 4}, g.h));  // bottom face 2 m up
    const f64 dt = 1.0 / (60.0 * per_tick);
    int s = 0;
    for (; s < per_tick * 60 * 6 && !w.bodies[0]->asleep; ++s) {
      if (s % per_tick == 0) w.begin_tick();
      w.substep(dt, g, nullptr);
    }
    const f64 t = s * dt;
    MESSAGE(per_tick << " substeps a tick: asleep after " << t << " s");
    REQUIRE(w.bodies[0]->asleep);
    if (per_tick == 2) base = t;
    else CHECK(std::abs(t - base) < 0.06);
  }
}

TEST_CASE("rigid: a stack of two blocks settles and both sleep") {
  const VoxelGrid g = ground();
  RigidWorld w;
  w.add(box(1, {8, 8, 1}, {8, 8, 4}, g.h));
  w.add(box(2, {10, 10, 8}, {4, 4, 4}, g.h));
  for (int s = 0; s < 2 * 60 * 5; ++s) w.substep(1.0 / 120.0, g, nullptr);
  CHECK(w.bodies[0]->asleep);
  CHECK(w.bodies[1]->asleep);
  // the upper block rests on the lower one (not sunk into it)
  const f64 top_lower = w.bodies[0]->x.z + 2.0 * g.h;
  const f64 bottom_upper = w.bodies[1]->x.z - 2.0 * g.h;
  CHECK(std::abs(bottom_upper - top_lower) < 0.06);
}

TEST_CASE("rigid: two free blocks colliding conserve momentum") {
  VoxelGrid empty;
  empty.h = 0.125;
  RigidWorld w;
  w.par.gravity = 0.0;
  w.par.linear_damping = w.par.angular_damping = 0.0;
  w.par.rest_damping = 0.0;
  auto a = box(1, {0, 0, 0}, {4, 4, 4}, empty.h);
  auto b = box(2, {8, 0, 0}, {4, 4, 4}, empty.h);
  a->v = V3{3.0, 0, 0};
  const V3 p0 = a->v * a->mass + b->v * b->mass;
  w.add(std::move(a));
  w.add(std::move(b));
  for (int s = 0; s < 120; ++s) w.substep(1.0 / 120.0, empty, nullptr);
  const Body& A = *w.bodies[0];
  const Body& B = *w.bodies[1];
  const V3 p1 = A.v * A.mass + B.v * B.mass;
  CHECK(p1.x == doctest::Approx(p0.x).epsilon(1e-9));
  CHECK(B.v.x > 1.0);     // the struck block moves on
  CHECK(A.v.x < B.v.x);   // and they separate
}

TEST_CASE("rigid: blocks falling together in contact fall at g") {
  // Rest damping is for held bodies: debris falling in a clump touches and pushes, but nothing
  // holds it up. Two pairs in mid-air, pressed together: side by side (a horizontal contact), and
  // one on the other (the upper one pushed down on the lower one: an upward contact).
  VoxelGrid empty;
  empty.h = 0.125;
  RigidWorld w;
  w.par.linear_damping = 0.0;
  w.add(box(1, {0, 0, 40}, {4, 4, 4}, empty.h));
  w.add(box(2, {4, 0, 40}, {4, 4, 4}, empty.h));
  w.add(box(3, {40, 0, 40}, {4, 4, 4}, empty.h));
  w.add(box(4, {40, 0, 44}, {4, 4, 4}, empty.h));
  const f64 m = w.bodies[0]->mass, g = w.par.gravity;
  w.bodies[0]->force = V3{m * g, 0.0, 0.0};
  w.bodies[1]->force = V3{-m * g, 0.0, 0.0};
  w.bodies[3]->force = V3{0.0, 0.0, -m * g};
  const f64 dt = 1.0 / 120.0;
  i32 touching = 0;
  for (int s = 0; s < 36; ++s) {
    w.substep(dt, empty, nullptr);
    touching += w.contacts().size() >= 2 ? 1 : 0;
  }
  CHECK(touching > 30);  // (both pairs touch as they fall)
  const f64 t = 36 * dt;
  for (const auto& bp : w.bodies) CHECK_FALSE(bp->asleep);
  // (the side by side pair: no net force but gravity; the stack: gravity and the push)
  CHECK(0.5 * (w.bodies[0]->v.z + w.bodies[1]->v.z) == doctest::Approx(-g * t).epsilon(0.03));
  CHECK(0.5 * (w.bodies[2]->v.z + w.bodies[3]->v.z) == doctest::Approx(-1.5 * g * t).epsilon(0.03));
}

TEST_CASE("rigid: a block held against a wall by friction settles and sleeps") {
  // (held up by friction alone: the contact normal is horizontal)
  VoxelGrid g;
  g.h = 0.125;
  for (i32 y = -8; y < 24; ++y) g.fill_column(20, y, 0, 48, make_vox(MaterialId::Rock, true));
  g.compact();
  RigidWorld w;
  auto b = box(1, {16, 4, 24}, {4, 4, 4}, g.h);
  b->force = V3{3.0 * b->mass * w.par.gravity / w.par.friction, 0.0, 0.0};  // (pressed against the wall)
  w.add(std::move(b));
  const f64 z0 = w.bodies[0]->x.z;
  for (int s = 0; s < 2 * 60 * 3; ++s) w.substep(1.0 / 120.0, g, nullptr);
  const Body& B = *w.bodies[0];
  CHECK(B.asleep);
  CHECK(std::abs(B.x.z - z0) < 0.1);
}

TEST_CASE("rigid: a fast block does not pass through a thin wall (continuous collision)") {
  // a sheet one voxel thick at x = 2 m, and a 0.25 m block thrown at it at 28 m/s (a blast's
  // fastest) in a busy world's substeps (one a tick, 60 Hz: 0.47 m - nearly four voxels - a
  // substep: its samples step over the sheet)
  auto run = [](bool speculative, f64* x_end, f64* v_end) {
    VoxelGrid g = ground();
    for (i32 y = -32; y < 64; ++y) g.fill_column(16, y, 0, 40, make_vox(MaterialId::Rock, true));
    g.compact();
    RigidWorld w;
    w.par.speculative = speculative;
    auto b = box(1, {0, 0, 0}, {2, 2, 2}, 0.125);
    b->x = V3{0.5, 0.5, 2.0};
    b->v = V3{28.0, 0.0, 0.0};
    w.add(std::move(b));
    for (int s = 0; s < 30; ++s) w.substep(1.0 / 60.0, g, nullptr);
    *x_end = w.bodies.front()->x.x;
    *v_end = w.bodies.front()->v.x;
  };
  f64 x0, v0, x1, v1;
  run(false, &x0, &v0);
  run(true, &x1, &v1);
  MESSAGE("block at 28 m/s at a sheet one voxel thick: without continuous collision it ends at x " << x0 << " (the sheet at 2.0), with it at "
                                                                                                  << x1 << ", moving " << v1 << " m/s");
  CHECK(x0 > 2.0);  // (it went through)
  CHECK(x1 < 2.0 - 0.0625);
}

TEST_CASE("rigid: a stack stays put through busy mode's switch between two substeps and one (warm starts scaled to the step)") {
  // (a stack of four blocks kept awake, solved with busy mode's iterations, its tick switched
  // between two substeps and one every half second: how fast its top moves after the switches,
  // the warm starts scaled to the step's length or not)
  const VoxelGrid g = ground();
  auto jitter = [&](bool warm_to_step) {
    RigidWorld w;
    w.par.warm_to_step = warm_to_step;
    w.par.iterations = w.par.busy_iterations;
    w.par.position_iterations = 2;
    w.par.sleep_substeps = 1 << 30;  // (awake throughout: a pile in a collapse)
    for (int k = 0; k < 4; ++k) w.add(box(1 + k, {10, 10, 4 * k}, {6, 6, 4}, g.h));
    for (int s = 0; s < 2 * 60 * 2; ++s) w.substep(1.0 / 120.0, g, nullptr);  // (settled)
    f64 worst = 0.0;
    for (int t = 0; t < 240; ++t) {
      const int per_tick = (t / 30) % 2 == 0 ? 1 : 2;
      w.begin_tick();
      for (int s = 0; s < per_tick; ++s) w.substep(1.0 / (60.0 * per_tick), g, nullptr);
      for (const auto& b : w.bodies) worst = std::max(worst, norm(b->v));
    }
    return worst;
  };
  const f64 scaled = jitter(true), raw = jitter(false);
  MESSAGE("the stack's fastest block across the switches: " << scaled << " m/s scaled, " << raw << " m/s not");
  CHECK(scaled < raw);
}
