// svx_anim characters in a core World (the deep path's own): what the world's pieces do to the
// bodies - a heavy piece driven into someone knocks them down, and the body knows the blow it took
// (its links' senses: what the host's injuries go by).
#include <algorithm>
#include <cmath>

#include "doctest.h"
#include "scene.hpp"

using namespace scene;

namespace {

// A block of timber (w x d x h voxels) dropped into the world at `at` (its lattice's corner), loose.
i64 block(World& w, const V3& at, i32 nx, i32 ny, i32 nz) {
  VoxelGrid g;
  g.h = kH;
  const Vox wood = make_vox(MaterialId::Wood, false);
  for (i32 x = 0; x < nx; ++x)
    for (i32 y = 0; y < ny; ++y)
      for (i32 z = 0; z < nz; ++z) g.set(x, y, z, wood);
  g.compact();
  const GridId id = w.add_grid(GridFrame{at, Quat{}}, std::move(g), false);
  return id ? w.loosen_grid(id) : 0;
}

}  // namespace

TEST_CASE("world: a heavy piece driven into a standing body knocks it down, and the body feels the blow") {
  Scene s(Path::Deep);
  Character& c = s.civilian(3.0, 0.0);
  run(s, c, 1.0);
  REQUIRE(c.bound());
  const V3 p0 = c.pose.p[H::pelvis];
  // a timber block (1.5 x 1.25 x 0.75 m: some 700 kg) flying at 9 m/s into the body's middle
  const i64 b = block(*s.world, V3{p0.x - 3.2, p0.y - 0.6, p0.z - 0.35}, 12, 10, 6);
  REQUIRE(b != 0);
  s.frame({&c});
  const Body* piece = s.world->piece(b);
  REQUIRE(piece);
  REQUIRE(s.world->apply_impulse(b, piece->x, V3{piece->mass * 9.0, 0.0, piece->mass * 1.4}));
  f64 blow = 0.0;
  bool down = false;
  run(s, c, 2.5, 0.0, [&](Character& x, f64, Host&) {
    f64 bump = 0.0;
    for (const RigidBody* p : x.body.parts) bump += p->bumped;
    blow = std::max(blow, bump / x.body.total_mass);
    down = down || x.down();
  });
  const V3 p1 = c.pose.p[H::pelvis];
  MESSAGE("a " << piece->mass << " kg block at 9 m/s: the body's largest blow " << blow << " m/s, it went " << p1.x - p0.x << " m; down " << down);
  CHECK(blow > 2.2);  // (a blow that hurts: Pedestrians' injuries start there)
  CHECK(down);
  CHECK(p1.x - p0.x > 0.5);
}
