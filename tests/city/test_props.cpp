// svx_city tests — the props' classes (svx/props.hpp, data/city/props.json): every prop, piece of
// furniture and civic fitting the generator draws has one.
#include <doctest.h>

#include "buildings/interior/civicPrefabs.hpp"
#include "buildings/interior/prefabs.hpp"
#include "city/propPrefabs.hpp"
#include "svx/props.hpp"

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
