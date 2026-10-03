// structvox procgen — the city generator's materials in the engine (svx/procgen/city_materials.hpp).
#include "svx/procgen/city_materials.hpp"

#include <mutex>

#include "svx/city/materials.hpp"
#include "svx/material/material.hpp"

namespace svx {

namespace {

enum CityClass : int { Roofing = city::kCityMaterialBase, Partition, Soft, Ice, Snow, Foliage };

}  // namespace

void register_city_materials() {
  static std::once_flag once;
  std::call_once(once, [] {
    MaterialTable& t = default_materials();
    for (const city::PhysicsClass& c : city::physics_classes()) {
      if (!c.own) continue;
      Material m;
      m.name = "city_" + c.name;
      m.E = c.E;
      m.G = c.G;
      m.rho = c.rho;
      m.ft = c.ft;
      m.fb = c.fb;
      m.fc = c.fc;
      m.cohesion = c.cohesion;
      m.friction = c.friction;
      m.Gf = c.Gf;
      m.frag_x = c.frag[0];
      m.frag_y = c.frag[1];
      m.frag_z = c.frag[2];
      m.frag_noise = c.frag_noise;
      m.crush = c.crush;
      m.grip = c.grip;
      // (plants: never structure, burn, shed with what they grow on; leaves and grass let
      // everything through)
      m.non_structural = m.passable = c.id == Foliage;
      t.set(static_cast<MaterialId>(c.id), m);
    }
  });
}

void set_city_fire_materials(FireSystem& fire) {
  FireMaterial roofing = fire.fire_material(MaterialId::Wood);  // (battens under shingles, thatch, sod; tiles and slate shelter them)
  roofing.ignition_c = 380.0;
  roofing.burn_s = 90.0;
  roofing.char_damage = 0.8;
  fire.set_material(static_cast<MaterialId>(Roofing), roofing);
  FireMaterial partition;  // (plasterboard on studs: it does not burn; its gypsum gives up its strength)
  partition.conduct = 0.03;
  partition.cool = 0.05;
  partition.weaken_c = 200.0;
  partition.gone_c = 800.0;
  fire.set_material(static_cast<MaterialId>(Partition), partition);
  FireMaterial soft = fire.fire_material(MaterialId::Wood);  // (upholstery, bedding, paper, goods, hay)
  soft.ignition_c = 260.0;
  soft.burn_s = 25.0;
  soft.flame_c = 850.0;
  soft.char_damage = 1.0;
  fire.set_material(static_cast<MaterialId>(Soft), soft);
  FireMaterial cold;  // (ice and snow: inert)
  cold.conduct = 0.02;
  cold.cool = 0.05;
  fire.set_material(static_cast<MaterialId>(Ice), cold);
  fire.set_material(static_cast<MaterialId>(Snow), cold);
  FireMaterial plant = fire.fire_material(MaterialId::Wood);  // (leaves, grass, crops: quick)
  plant.ignition_c = 280.0;
  plant.burn_s = 6.0;
  plant.flame_c = 750.0;
  plant.char_damage = 1.0;
  fire.set_material(static_cast<MaterialId>(Foliage), plant);
}

std::shared_ptr<const AppearanceTable> city_appearances() {
  static const std::shared_ptr<const AppearanceTable> table = [] {
    auto t = std::make_shared<AppearanceTable>();
    for (const city::Look& l : city::looks()) {
      Appearance a;
      for (int k = 0; k < 3; ++k) a.rgb[k] = srgb_to_linear(l.rgb[size_t(k)]);
      a.opacity = l.transparent ? static_cast<f32>(l.opacity) : 1.0f;
      a.emissive = static_cast<f32>(l.emissive);
      a.noise = static_cast<f32>(l.noise);
      // (glazing mirrors the sky; lamps, screens and polished metal a little)
      a.gloss = l.transparent ? 0.9f : l.emissive > 0.0 ? 0.3f : l.class_id == static_cast<int>(MaterialId::Glass) ? 0.5f : 0.0f;
      a.glow = l.glow;
      t->set(l.class_id, 1 + l.look, a);
    }
    return t;
  }();
  return table;
}

}  // namespace svx
