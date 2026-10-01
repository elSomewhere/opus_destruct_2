// svx_city tests — the props' classes (svx/props.hpp, data/city/props.json): every prop, piece of
// furniture and civic fitting the generator draws has one.
#include <doctest.h>

#include "buildings/interior/civicPrefabs.hpp"
#include "buildings/interior/prefabs.hpp"
#include "city/propPrefabs.hpp"
#include "svx/props.hpp"
#include "svx/seams.hpp"
#include "voxel/chunk.hpp"

using namespace svx::city;

TEST_CASE("city props: every prop, piece of furniture and civic fitting has its class") {
  int n = 0;
  for (const PropDef& p : props()) {
    CHECK_MESSAGE(prop_class("props", p.id) != nullptr, "props.", p.id);
    ++n;
  }
  for (const Prefab& p : prefabs()) {
    CHECK_MESSAGE(prop_class("furniture", p.id) != nullptr, "furniture.", p.id);
    ++n;
  }
  for (const Prefab& p : civic_prefabs()) {
    CHECK_MESSAGE(prop_class("civic", p.id) != nullptr, "civic.", p.id);
    ++n;
  }
  CHECK(n == 126);
  CHECK(prop_class("props", "nope") == nullptr);
  CHECK(prop_class("nope", "bench") == nullptr);
  // (the plan's examples, §10.2)
  CHECK(prop_class("props", "streetlight")->attach == Attachment::Fixed);
  CHECK(prop_class("props", "bench")->attach == Attachment::Loose);
  CHECK((prop_class("props", "bench")->uses & kUseSit) != 0);
  CHECK(prop_class("furniture", "sofa")->attach == Attachment::Loose);
  CHECK((prop_class("furniture", "bedDouble")->uses & kUseSleep) != 0);
  CHECK(prop_class("furniture", "counterRun")->attach == Attachment::Fixed);
  CHECK(prop_class("props", "containerStack")->attach == Attachment::Loose);
  const PropClass* car = prop_class("props", "car");
  REQUIRE(car != nullptr);
  CHECK(car->attach == Attachment::Entity);
  CHECK(car->entity == "car");
  CHECK(prop_class("civic", "fireTruck")->entity == "fire_truck");
}

TEST_CASE("city props: the chunk writer marks which object an isolated voxel belongs to") {
  ChunkBuffer c(0, 0, 0, 0);
  c.track_isolated().track_objects();
  c.fill_box(0, 0, 0, 3, 3, 0, 7);  // (structure: no object)
  c.begin_object("furniture", "sofa", "B1/f0/s0");
  c.fill_box(0, 0, 1, 1, 0, 1, 9);
  c.end_object();
  c.begin_object("props", "bench", "C0_0/p3");
  c.fill_box(3, 3, 1, 3, 3, 2, 11);
  c.end_object();
  c.begin_object("furniture", "sofa", "B1/f0/s0");  // (the sofa's back: the same object)
  c.fill_box(0, 1, 1, 1, 1, 2, 9);
  c.end_object();
  CHECK_FALSE(c.isolating);
  REQUIRE(c.objects.size() == 2);
  CHECK(c.objects[0].kind == "sofa");
  CHECK(c.objects[1].kind == "bench");
  auto at = [&](double x, double y, double z) { return c.obj[size_t(c.write_index(x, y, z))]; };
  CHECK(at(0, 0, 0) == 0);
  CHECK(at(0, 0, 1) == 1);
  CHECK(at(1, 1, 2) == 1);
  CHECK(at(3, 3, 2) == 2);
  CHECK(c.iso[size_t(c.write_index(3, 3, 1))] == 11);
  // (without object tracking the reference's isolating writes are unchanged)
  ChunkBuffer d(0, 0, 0, 0);
  d.track_isolated();
  d.begin_object("props", "bench", "x");
  d.fill_box(0, 0, 0, 0, 0, 0, 5);
  d.end_object();
  CHECK(d.objects.empty());
  CHECK(d.iso[size_t(d.write_index(0, 0, 0))] == 5);
}

TEST_CASE("city props: a loose object's voxels have seams on every face to anything else; fixed ones none") {
  ChunkBuffer c(0, 0, 0, 0);
  c.track_isolated().track_objects();
  // a floor (structure) at z 0 over the chunk and its padding; a sofa (loose) on it at x 2..3; a
  // bench (loose) beside it at x 4; a counter (fixed) at x 8; a sofa across the chunk's +x border
  c.fill_box(-1, -1, 0, 32, 32, 0, 7);
  c.begin_object("furniture", "sofa", "s1");
  c.fill_box(2, 0, 1, 3, 0, 1, 9);
  c.end_object();
  c.begin_object("props", "bench", "b1");
  c.fill_box(4, 0, 1, 4, 0, 1, 11);
  c.end_object();
  c.begin_object("furniture", "counterRun", "c1");
  c.fill_box(8, 0, 1, 8, 0, 1, 13);
  c.end_object();
  c.begin_object("furniture", "sofa", "s2");
  c.fill_box(31, 5, 1, 32, 5, 1, 9);
  c.end_object();
  std::vector<uint8_t> s;
  REQUIRE(export_seams(c, [](uint16_t) { return true; }, s));
  auto bits = [&](int x, int y, int z) { return s[(size_t(x) * 32 + size_t(y)) * 32 + size_t(z)]; };
  CHECK(bits(2, 0, 0) == 4);  // (the floor under the sofa: its +z face)
  CHECK(bits(3, 0, 0) == 4);
  CHECK(bits(2, 0, 1) == 0);  // (inside the sofa: bonded; nothing at +y, +z)
  CHECK(bits(3, 0, 1) == 1);  // (the sofa's +x face against the bench)
  CHECK(bits(4, 0, 1) == 0);  // (the bench's +x: air)
  CHECK(bits(1, 0, 1) == 0);
  CHECK(bits(8, 0, 0) == 0);  // (the counter: fixed, bonded to the floor)
  CHECK(bits(7, 0, 1) == 0);
  CHECK(bits(31, 5, 0) == 4);  // (the border sofa: on the floor, and across into the next chunk ...)
  CHECK(bits(31, 5, 1) == 0);  // (... its own voxel there: no seam)
  // (no loose objects: no seams)
  ChunkBuffer f(0, 0, 0, 0);
  f.track_isolated().track_objects();
  f.fill_box(0, 0, 0, 3, 3, 0, 7);
  f.begin_object("furniture", "counterRun", "c1");
  f.fill_box(0, 0, 1, 0, 0, 1, 13);
  f.end_object();
  std::vector<uint8_t> none;
  CHECK_FALSE(export_seams(f, [](uint16_t) { return true; }, none));
}
