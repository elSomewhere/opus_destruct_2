// svx_city tests — the props' classes (svx/props.hpp, data/city/props.json): every prop, piece of
// furniture and civic fitting the generator draws has one.
#include <doctest.h>

#include "buildings/interior/civicPrefabs.hpp"
#include "buildings/interior/prefabs.hpp"
#include "city/propPrefabs.hpp"
#include "svx/props.hpp"
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
