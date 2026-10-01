// Decorative materials (Material::decorative, passable; docs/CORE.md §2): plants that render,
// burn and are cut like any solid but are never structure - in no fragment, holding nothing, held
// by nothing - and are shed when what they grow on goes; passable ones let everything through.
#include <algorithm>
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kWood = make_vox(MaterialId::Wood, false);
const Vox kConcrete = make_vox(MaterialId::Concrete, false);

struct Plants {
  MaterialId leaves{}, hedge{};
};

Plants register_plants(World& w) {
  Plants p;
  Material leaves;
  leaves.name = "leaves";
  leaves.rho = 80.0;
  leaves.decorative = true;
  leaves.passable = true;
  REQUIRE(w.register_material(leaves, &p.leaves));
  Material hedge;
  hedge.name = "hedge";
  hedge.rho = 300.0;
  hedge.decorative = true;
  REQUIRE(w.register_material(hedge, &p.hedge));
  return p;
}

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

i64 count(const World& w, const IVec3& lo, const IVec3& hi, Vox v) {
  i64 n = 0;
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y)
      for (i32 z = lo[2]; z < hi[2]; ++z) n += w.grid().get(x, y, z) == v;
  return n;
}

// Rock ground (z < 0) 8 m square; a wooden post (z 0..24) with a crown of leaves on its top
// (z 24..30). The crown touches the post only.
const IVec3 kPostLo{10, 10, 0}, kPostHi{12, 12, 24};
const IVec3 kCrownLo{6, 6, 24}, kCrownHi{16, 16, 30};

World tree(Plants* plants) {
  World w;
  *plants = register_plants(w);
  VoxelGrid g;
  g.h = kH;
  box(g, {-32, -32, -8}, {32, 32, 0}, kRock);
  box(g, kPostLo, kPostHi, kWood);
  box(g, kCrownLo, kCrownHi, make_vox(plants->leaves, false));
  g.compact();
  g.lo = {-32, -32, -8};
  g.hi = {32, 32, 64};
  w.load(std::move(g));
  w.bake();
  return w;
}

}  // namespace

TEST_CASE("decorative: leaves are in no fragment and hold nothing; the tree stands") {
  Plants pl;
  World w = tree(&pl);
  const Vox leaf = make_vox(pl.leaves, false);
  const i64 crown = count(w, kCrownLo, kCrownHi, leaf);
  REQUIRE(crown == 10 * 10 * 6);
  CHECK(w.design_report().floating_voxels == 0);
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.pieces().empty());
  CHECK(count(w, kCrownLo, kCrownHi, leaf) == crown);
  // fragments: the post's voxels have one, the leaves none
  std::vector<u8> frag;
  REQUIRE(w.debug_field({0, 0, 0}, DebugField::Fragment, &frag));
  CHECK(frag[size_t((11 * kChunk + 11) * kChunk + 20)] != 0);  // (the post)
  CHECK(frag[size_t((7 * kChunk + 7) * kChunk + 26)] == 0);    // (a leaf)
  // a beam set down on the crown alone falls through it (nothing holds it: leaves do not, and
  // they let it through)
  std::vector<VoxelEdit> beam;
  for (i32 x = 6; x < 10; ++x) beam.push_back({{x, 7, 30}, kWood});
  for (i32 x = 6; x < 10; ++x) beam.push_back({{x, 8, 30}, kWood});
  for (i32 x = 6; x < 10; ++x)
    for (i32 y = 7; y < 9; ++y) beam.push_back({{x, y, 31}, kWood});
  REQUIRE(w.set_voxels(beam) == 16);
  int added = 0;
  for (int t = 0; t < 120; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events()) added += e.kind == WorldEvent::Kind::PieceAdded;
  }
  CHECK(added == 1);
  REQUIRE(w.pieces().size() == 1);
  CHECK(w.pieces()[0].pos.z < 0.5);  // (on the ground, through the leaves)
  CHECK(count(w, kCrownLo, kCrownHi, leaf) == crown);
}

TEST_CASE("decorative: a decorative voxel is never anchored") {
  Plants pl;
  World w = tree(&pl);
  REQUIRE(w.set_voxels({{{20, 20, 0}, make_vox(pl.hedge, true)}}) == 1);
  CHECK(w.grid().get(20, 20, 0) == make_vox(pl.hedge, false));
  // (a level loaded with anchored leaves: stored free)
  World w2;
  const Plants p2 = register_plants(w2);
  VoxelGrid g;
  g.h = kH;
  box(g, {0, 0, -4}, {8, 8, 0}, kRock);
  box(g, {0, 0, 0}, {8, 8, 2}, make_vox(p2.leaves, true));
  g.compact();
  w2.load(std::move(g));
  CHECK(w2.grid().get(3, 3, 1) == make_vox(p2.leaves, false));
  CHECK(w2.grid().get(3, 3, -1) == kRock);
}

TEST_CASE("decorative: the post cut, its crown is shed with it (falling leaves)") {
  Plants pl;
  World w = tree(&pl);
  const Vox leaf = make_vox(pl.leaves, false);
  w.carve(V3{11 * kH, 11 * kH, 3 * kH}, 0.35);
  int dust = 0, added = 0;
  i32 shed = 0;
  for (int t = 0; t < 30; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events()) {
      added += e.kind == WorldEvent::Kind::PieceAdded;
      if (e.kind == WorldEvent::Kind::Dust) {
        ++dust;
        shed = std::max(shed, e.voxels);
      }
    }
  }
  CHECK(added == 1);  // (the post above the cut)
  CHECK(count(w, kCrownLo, kCrownHi, leaf) == 0);
  CHECK(shed == 600);
  CHECK(dust >= 1);
}

TEST_CASE("decorative: the crown cut from its post goes; leaves still on it stay") {
  Plants pl;
  World w = tree(&pl);
  const Vox leaf = make_vox(pl.leaves, false);
  // the top of the post cut away (and the leaves about it): the crown left touches nothing
  w.carve(V3{11 * kH, 11 * kH, 23 * kH}, 0.45);
  for (int t = 0; t < 10; ++t) w.tick();
  CHECK(count(w, kCrownLo, kCrownHi, leaf) == 0);
  // leaves on a post that stays are not shed by a cut beside them
  World w2 = tree(&pl);
  w2.carve(V3{14 * kH, 14 * kH, 28 * kH}, 0.2);
  for (int t = 0; t < 10; ++t) w2.tick();
  const i64 left = count(w2, kCrownLo, kCrownHi, leaf);
  CHECK(left > 500);
  CHECK(left < 600);
}

TEST_CASE("decorative: passable grass lets pieces and the player through; a hedge does not; rays hit both") {
  World w;
  const Plants pl = register_plants(w);
  VoxelGrid g;
  g.h = kH;
  box(g, {-32, -32, -8}, {32, 32, 0}, kRock);
  box(g, {-32, -32, 0}, {32, 0, 2}, make_vox(pl.leaves, false));  // grass, 25 cm (y < 0)
  box(g, {-32, 8, 0}, {32, 12, 8}, make_vox(pl.hedge, false));    // a hedge, 1 m (y 8..12)
  g.compact();
  g.lo = {-32, -32, -8};
  g.hi = {32, 32, 64};
  w.load(std::move(g));
  w.bake();
  const f64 ground = -0.5 * kH;  // (the rock's surface)
  // a crate dropped on the grass lands on the rock under it
  std::vector<VoxelEdit> crate;
  for (i32 x = 0; x < 4; ++x)
    for (i32 y = -12; y < -8; ++y)
      for (i32 z = 16; z < 20; ++z) crate.push_back({{x, y, z}, kConcrete});
  REQUIRE(w.set_voxels(crate) == 64);
  for (int t = 0; t < 150; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  CHECK(w.pieces()[0].pos.z == doctest::Approx(ground + 2 * kH).epsilon(0.02));
  // the player's box goes down through the grass to the rock, and stops on the hedge
  const CollideResult down = w.collide(V3{-1.0, -2.0, 1.0}, V3{-0.6, -1.6, 2.8}, V3{0, 0, -3.0});
  CHECK(down.on_ground);
  CHECK(1.0 + down.move.z == doctest::Approx(ground).epsilon(0.01));
  const CollideResult hedge = w.collide(V3{-1.0, 1.2, 2.0}, V3{-0.6, 1.4, 3.8}, V3{0, 0, -3.0});
  CHECK(hedge.on_ground);
  CHECK(2.0 + hedge.move.z == doctest::Approx(7.5 * kH).epsilon(0.01));
  // a ray hits the grass's top
  const RayHit r = w.raycast(V3{-2.0, -2.0, 3.0}, V3{0, 0, -1}, 10.0);
  REQUIRE(r.hit);
  CHECK(r.pos.z == doctest::Approx(1.5 * kH).epsilon(0.02));
}
