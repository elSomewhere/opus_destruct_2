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
  BodyShape& S = b->shape;
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
