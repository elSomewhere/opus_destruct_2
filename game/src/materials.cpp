#include "svx/game/materials.hpp"

#include "svx/env/fire.hpp"

namespace svx {

namespace {

Material preset(const char* name, f64 E, f64 G, f64 rho, f64 ft, f64 fb, f64 fc, f64 coh, f64 mu, f64 Gf, f64 fx, f64 fy, f64 fz, f64 noise, bool ductile = false) {
  Material m;
  m.name = name;
  m.E = E;
  m.G = G;
  m.rho = rho;
  m.ft = ft;
  m.fb = fb;
  m.fc = fc;
  m.cohesion = coh;
  m.friction = mu;
  m.Gf = Gf;
  m.frag_x = fx;
  m.frag_y = fy;
  m.frag_z = fz;
  m.frag_noise = noise;
  m.ductile = ductile;
  return m;
}

struct Entry {
  MaterialId id;
  Material m;
};

// Smeared sections (docs/VEHICLES.md): the real section of a thin-walled part baked into its
// voxels' material, so a car weighs and carries what it would: its steel is 1 mm sheets and 2 mm
// box sections, not solid 12.5 cm cubes. (Values as the core's standard presets were before they
// moved here: the same ids, the same bits.)
std::array<Entry, mat::kGameMaterials> entries() {
  //                               name       E       G       rho   ft      fb      fc      cohesion friction Gf  frag x y z     noise
  // A car's body: pressed 1 mm sheet with its stiffeners, smeared over a voxel (1 / 62.5 of 250
  // MPa steel: 4 MPa in tension; panels buckle long before). It crumples at 90 kPa of contact
  // pressure over the patch it is pressed on (the crush strength of a car's front structure over
  // its frontal area): a 1.2 t car at 50 km/h folds some 0.45 m of its front against a wall, at
  // about 250 kN (20 g).
  Material sheet = preset("sheet", 3.4e9, 1.3e9, 260, 4e6, 6e6, 3e6, 2.5e6, 0.4, 2e4, 3.5, 3.5, 3.5, 0.3, true);
  sheet.crush = 9e4;
  sheet.penetration = 2e4;
  // A car's frame: rails, floor pan and pillars (box sections of 2 mm steel), smeared.
  Material frame = preset("car_frame", 10e9, 4e9, 700, 18e6, 20e6, 12e6, 10e6, 0.4, 5e4, 4.0, 4.0, 4.0, 0.25, true);
  frame.crush = 1.2e6;
  frame.penetration = 1.5e5;
  // An engine block and gearbox: cast iron and aluminium with the air in their shape, smeared.
  // It barely crumples (its mounts give, it is pushed back): a car's front folds easily until its
  // engine meets what it hit, then hard.
  Material engine = preset("engine", 40e9, 16e9, 1200, 30e6, 30e6, 100e6, 20e6, 0.4, 1e5, 6.0, 6.0, 6.0, 0.2, true);
  engine.crush = 1.5e6;
  engine.penetration = 4e5;
  // A car's glazing: 5 mm of glass over a voxel (200 kg/m^3); it shatters in small pieces.
  Material window = preset("window", 5.6e9, 2.3e9, 200, 0.3e6, 0.5e6, 5e6, 0.4e6, 0.4, 2, 1.5, 1.5, 1.5, 0.6);
  window.crush = 2e4;  // (pressed, it shatters: it barely resists)
  // A wheel that came off: its tyre on its rim, smeared (20 kg in a 0.66 x 0.22 m disc).
  Material tyre = preset("tyre", 0.05e9, 0.02e9, 265, 5e6, 5e6, 5e6, 3e6, 0.9, 1e5, 8.0, 8.0, 8.0, 0.0, true);
  tyre.penetration = 3e4;
  // Bumpers, trim, lamp housings: plastic on a bumper beam, smeared; it takes a low speed bump.
  Material plastic = preset("plastic", 2e9, 0.8e9, 300, 2e6, 3e6, 2e6, 1.5e6, 0.5, 1e4, 3.0, 3.0, 3.0, 0.4, true);
  plastic.crush = 1.5e5;
  plastic.penetration = 1e4;
  // Road surface and its markings (anchored ground): tyres grip them.
  Material asphalt = preset("asphalt", 10e9, 4e9, 2300, 1e6, 1.5e6, 20e6, 1.2e6, 0.8, 300, 4.0, 4.0, 4.0, 0.45);
  asphalt.grip = 1.0;
  Material paint = preset("paint", 10e9, 4e9, 2300, 1e6, 1.5e6, 20e6, 1.2e6, 0.7, 300, 4.0, 4.0, 4.0, 0.45);
  paint.grip = 0.9;
  // Head and tail lamps' glass.
  Material lamp = preset("lamp", 5.6e9, 2.3e9, 400, 0.3e6, 0.5e6, 5e6, 0.4e6, 0.4, 2, 1.5, 1.5, 1.5, 0.6);
  lamp.crush = 2e4;
  return {{{mat::Sheet, sheet},
           {mat::CarFrame, frame},
           {mat::Engine, engine},
           {mat::Window, window},
           {mat::Tyre, tyre},
           {mat::Plastic, plastic},
           {mat::Asphalt, asphalt},
           {mat::Paint, paint},
           {mat::Lamp, lamp}}};
}

}  // namespace

void register_game_materials() {
  static const std::array<Entry, mat::kGameMaterials> all = entries();
  MaterialTable& t = default_materials();
  for (const Entry& e : all)
    if (!t.registered(e.id) || t[e.id].name != e.m.name) t.set(e.id, e.m);
}

void set_game_fire_materials(FireSystem& fire) {
  // (sheet steel heats through fast and weakens as steel; a car's plastic and tyres burn)
  FireMaterial sheet = fire.fire_material(MaterialId::Steel);
  sheet.conduct = 0.5;
  sheet.cool = 0.06;
  fire.set_material(mat::Sheet, sheet);
  fire.set_material(mat::CarFrame, sheet);
  FireMaterial plastic = fire.fire_material(MaterialId::Wood);
  plastic.ignition_c = 350.0;
  plastic.burn_s = 25.0;
  plastic.flame_c = 850.0;
  plastic.char_damage = 1.0;
  fire.set_material(mat::Plastic, plastic);
  FireMaterial tyre = plastic;
  tyre.ignition_c = 400.0;
  tyre.burn_s = 70.0;
  fire.set_material(mat::Tyre, tyre);
}

}  // namespace svx
