// The city generator's materials in the engine (svx/procgen/city_materials.hpp): its classes
// registered, their fire facets, its looks as an appearance table.
#include "doctest.h"
#include "svx/city/materials.hpp"
#include "svx/env/fire.hpp"
#include "svx/material/material.hpp"
#include "svx/procgen/city_materials.hpp"

using namespace svx;

TEST_CASE("city materials: the city's own classes are registered at their ids, plants decorative") {
  register_city_materials();
  register_city_materials();  // (idempotent)
  const MaterialTable& t = default_materials();
  int own = 0;
  for (const city::PhysicsClass& c : city::physics_classes()) {
    if (!c.own) continue;
    ++own;
    const MaterialId id = static_cast<MaterialId>(c.id);
    REQUIRE(t.registered(id));
    CHECK(t[id].name == "city_" + c.name);
    CHECK(t[id].rho == doctest::Approx(c.rho));
    CHECK(t[id].decorative == (c.name == "foliage"));
    CHECK(t[id].passable == (c.name == "foliage"));
  }
  CHECK(own == 6);
  CHECK(t[static_cast<MaterialId>(city::kCityMaterialBase + 3)].grip == doctest::Approx(0.1));  // (ice: slippery)
  CHECK(t[static_cast<MaterialId>(city::kCityMaterialBase + 2)].crush > 0.0);                  // (soft: it gives)
  CHECK((t.vox_kind(make_vox(static_cast<MaterialId>(city::kCityMaterialBase + 5), false)) & kVoxDecorative) != 0);  // (foliage voxels)
}

TEST_CASE("city materials: fire facets - furnishings and plants burn, partitions weaken, ice does not") {
  FireSystem f;
  set_city_fire_materials(f);
  auto m = [&](int k) { return f.fire_material(static_cast<MaterialId>(city::kCityMaterialBase + k)); };
  CHECK(m(0).combustible);   // roofing
  CHECK(!m(1).combustible);  // partition
  CHECK(m(1).weaken_c > 0.0);
  CHECK(m(2).combustible);   // soft
  CHECK(m(2).ignition_c < f.fire_material(MaterialId::Wood).ignition_c);
  CHECK(!m(3).combustible);  // ice
  CHECK(!m(4).combustible);  // snow
  CHECK(m(5).combustible);   // foliage
  CHECK(m(5).burn_s < m(2).burn_s);
  // (the standard materials are as they were)
  FireSystem plain;
  CHECK(f.fire_material(MaterialId::Wood).burn_s == plain.fire_material(MaterialId::Wood).burn_s);
}

TEST_CASE("city materials: every look of every class has its appearance, glazing see-through, lamps lit") {
  const std::shared_ptr<const AppearanceTable> t = city_appearances();
  REQUIRE(t != nullptr);
  CHECK(t->size() == city::looks().size());
  CHECK(t->size() >= 400);
  int glazing = 0, lit = 0, glow = 0;
  for (const city::Look& l : city::looks()) {
    const int i = t->find(l.class_id, 1 + l.look);
    REQUIRE(i >= 0);
    CHECK(t->find(l.class_id, 0) == -1);  // (look 0: none - the material's own colour)
    const Appearance& a = (*t)[size_t(i)];
    if (l.transparent) {
      CHECK(a.opacity < 1.0f);
      ++glazing;
    }
    lit += a.emissive > 0.0f ? 1 : 0;
    glow += a.glow ? 1 : 0;
    CHECK(a.rgb[0] >= 0.0f);
    CHECK(a.rgb[0] <= 1.0f);
  }
  MESSAGE(t->size() << " appearances: " << glazing << " see-through, " << lit << " self-lit, " << glow << " lit at night");
  CHECK(glazing > 0);
  CHECK(lit > 0);
  CHECK(glow > 0);
}
