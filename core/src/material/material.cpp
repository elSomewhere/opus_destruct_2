#include "svx/material/material.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace svx {

namespace {

// Interface strengths are those of joints and crack planes (weaker than the intact material):
// plain concrete ~1 MPa in tension, reinforced concrete keeps a flexural reserve from its rebar,
// masonry fails along its mortar beds (a fraction of an MPa). The fragility knob divides all of
// them at run time. Fracture energies are those of the whole crack (the reinforcement's pull-out
// included for RC).
Material preset(const char* name, f64 E, f64 G, f64 rho, f64 ft, f64 fb, f64 fc, f64 coh, f64 mu, f64 Gf, f64 fx, f64 fy,
                f64 fz, f64 noise, bool indestructible = false, bool ductile = false) {
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
  m.indestructible = indestructible;
  m.ductile = ductile;
  return m;
}

// Properties a solve can use: positive and finite (a zero stiffness or strength would divide
// by zero), fragments at least a voxel.
Material sanitized(const Material& in) {
  Material m = in;
  auto pos = [](f64 v, f64 def) { return std::isfinite(v) && v > 0.0 ? v : def; };
  m.E = pos(m.E, 30e9);
  m.G = pos(m.G, 0.4 * m.E);
  m.rho = pos(m.rho, 2400.0);
  m.ft = pos(m.ft, 1e6);
  m.fb = pos(m.fb, m.ft);
  m.fc = pos(m.fc, 30e6);
  m.cohesion = pos(m.cohesion, 1e6);
  m.friction = std::isfinite(m.friction) ? std::clamp(m.friction, 0.0, 10.0) : 0.7;
  m.Gf = pos(m.Gf, 400.0);
  m.frag_x = std::isfinite(m.frag_x) ? std::clamp(m.frag_x, 1.0, 64.0) : 4.0;
  m.frag_y = std::isfinite(m.frag_y) ? std::clamp(m.frag_y, 1.0, 64.0) : 4.0;
  m.frag_z = std::isfinite(m.frag_z) ? std::clamp(m.frag_z, 1.0, 64.0) : 4.0;
  m.frag_noise = std::isfinite(m.frag_noise) ? std::clamp(m.frag_noise, 0.0, 1.0) : 0.4;
  m.crush = std::isfinite(m.crush) ? std::clamp(m.crush, 0.0, 1e12) : 0.0;
  m.penetration = std::isfinite(m.penetration) ? std::clamp(m.penetration, 0.0, 1e15) : 0.0;
  m.grip = std::isfinite(m.grip) ? std::clamp(m.grip, 0.0, 10.0) : 0.0;
  return m;
}

}  // namespace

MaterialTable::MaterialTable() { reset(); }

void MaterialTable::reset() {
  for (int i = 0; i < kMaxMaterials; ++i) {
    m_[size_t(i)] = Material{};
    used_[size_t(i)] = false;
  }
  //                  name        E       G       rho   ft      fb      fc      cohesion friction Gf     frag x y z     noise
  m_[0] = preset("rc",       30e9,  12.5e9, 2400, 1.2e6,  3.5e6,  30e6,  1.6e6,  0.7,  2000, 4.5, 4.5, 4.5, 0.35);
  m_[1] = preset("concrete", 28e9,  11.5e9, 2300, 0.8e6,  1.2e6,  25e6,  1.2e6,  0.7,  400,  4.2, 4.2, 4.2, 0.45);
  m_[2] = preset("steel",    200e9, 80e9,   7850, 180e6,  180e6,  200e6, 100e6,  0.35, 1e6,  6.0, 6.0, 6.0, 0.15);
  m_[3] = preset("masonry",  10e9,  4e9,    1900, 0.15e6, 0.3e6,  8e6,   0.35e6, 0.75, 120,  3.0, 2.0, 1.6, 0.25);
  m_[4] = preset("soil",     0.1e9, 0.04e9, 1700, 0.01e6, 0.01e6, 0.4e6, 0.03e6, 0.55, 20,   2.5, 2.5, 2.5, 0.5);
  m_[5] = preset("rock",     40e9,  16e9,   2600, 2.5e6,  3.0e6,  80e6,  4e6,    0.8,  600,  4.0, 4.0, 4.0, 0.5);
  m_[6] = preset("bedrock",  60e9,  25e9,   2800, 10e6,   10e6,   200e6, 15e6,   0.8,  2000, 5.0, 5.0, 5.0, 0.5, true);
  // Timber: joints and splits along the grain are its weak planes; bending it is strong, and
  // it is tough (splintering absorbs a lot): pieces break into long splinters, rarely to dust.
  m_[7] = preset("wood",     11e9,  0.7e9,  550,  1.2e6,  8e6,    20e6,  2e6,    0.5,  6000, 5.0, 5.0, 5.0, 0.5);
  // Cut stone in lime mortar: blocks that come apart at the joints (regular, little noise).
  m_[8] = preset("stone",    30e9,  12e9,   2600, 0.25e6, 0.4e6,  40e6,  0.5e6,  0.7,  120,  4.0, 4.0, 3.0, 0.15);
  // Glass: very brittle (it pays almost nothing for cracks) and shatters into small shards.
  m_[9] = preset("glass",    70e9,  29e9,   2500, 3e6,    4e6,    100e6, 3e6,    0.4,  4,    2.0, 2.0, 2.0, 0.6);
  // Concrete around a reinforcing bar, per 12.5 cm cell (about 2% steel): stiff and heavy as
  // concrete, strong in tension as its bar (3 cm^2 at 500 MPa over the cell's face), tough
  // (the bar yields), ductile. Concrete bonds to it no more strongly than to itself, so
  // concrete comes off the bars and they stay: the exposed reinforcement of a broken member.
  m_[10] = preset("rebar",   34e9,  13e9,   2550, 9e6,    10e6,   35e6,  4e6,    0.7,  50000, 8.0, 8.0, 8.0, 0.0, false, true);
  m_[2].ductile = true;  // (structural steel)
  m_[10].reinforcement = true;
  // Smeared sections (docs/VEHICLES.md): the real section of a thin-walled member baked into its
  // voxels' material (as rebar bakes its bar into the concrete cell), so members weigh and carry
  // what they would: steel is 1 mm sheets and 10 mm webs, not solid 12.5 cm cubes.
  // A rolled section (HEB 200: 61 kg/m, 78 cm^2 of S355) as a member of 2 x 2 voxels (625 cm^2):
  // 980 kg/m^3, 355 MPa x 78 / 625 = 44 MPa, its stiffness smeared likewise.
  m_[11] = preset("steel_section", 26e9, 10e9, 980, 44e6, 44e6, 40e6, 25e6, 0.35, 2e5, 6.0, 6.0, 6.0, 0.15, false, true);
  // A car's body: pressed 1 mm sheet with its stiffeners, smeared over a voxel (1 / 62.5 of 250
  // MPa steel: 4 MPa in tension; panels buckle long before). It crumples at 90 kPa of contact
  // pressure over the patch it is pressed on (the crush strength of a car's front structure over
  // its frontal area): a 1.2 t car at 50 km/h folds some 0.45 m of its front against a wall, at
  // about 250 kN (20 g).
  m_[12] = preset("sheet", 3.4e9, 1.3e9, 260, 4e6, 6e6, 3e6, 2.5e6, 0.4, 2e4, 3.5, 3.5, 3.5, 0.3, false, true);
  m_[12].crush = 9e4;
  m_[12].penetration = 2e4;
  // A car's frame: rails, floor pan and pillars (box sections of 2 mm steel), smeared.
  m_[13] = preset("car_frame", 10e9, 4e9, 700, 18e6, 20e6, 12e6, 10e6, 0.4, 5e4, 4.0, 4.0, 4.0, 0.25, false, true);
  m_[13].crush = 1.2e6;
  m_[13].penetration = 1.5e5;
  // An engine block and gearbox: cast iron and aluminium with the air in their shape, smeared.
  // It barely crumples (its mounts give, it is pushed back): a car's front folds easily until its
  // engine meets what it hit, then hard.
  m_[14] = preset("engine", 40e9, 16e9, 1200, 30e6, 30e6, 100e6, 20e6, 0.4, 1e5, 6.0, 6.0, 6.0, 0.2, false, true);
  m_[14].crush = 1.5e6;
  m_[14].penetration = 4e5;
  // A car's glazing: 5 mm of glass over a voxel (200 kg/m^3); it shatters in small pieces.
  m_[15] = preset("window", 5.6e9, 2.3e9, 200, 0.3e6, 0.5e6, 5e6, 0.4e6, 0.4, 2, 1.5, 1.5, 1.5, 0.6);
  m_[15].crush = 2e4;  // (pressed, it shatters: it barely resists)
  // A wheel that came off: its tyre on its rim, smeared (20 kg in a 0.66 x 0.22 m disc).
  m_[16] = preset("tyre", 0.05e9, 0.02e9, 265, 5e6, 5e6, 5e6, 3e6, 0.9, 1e5, 8.0, 8.0, 8.0, 0.0, false, true);
  m_[16].penetration = 3e4;
  // Bumpers, trim, lamp housings: plastic on a bumper beam, smeared; it takes a low speed bump.
  m_[17] = preset("plastic", 2e9, 0.8e9, 300, 2e6, 3e6, 2e6, 1.5e6, 0.5, 1e4, 3.0, 3.0, 3.0, 0.4, false, true);
  m_[17].crush = 1.5e5;
  m_[17].penetration = 1e4;
  // Road surface and its markings (anchored ground): tyres grip them.
  m_[18] = preset("asphalt", 10e9, 4e9, 2300, 1e6, 1.5e6, 20e6, 1.2e6, 0.8, 300, 4.0, 4.0, 4.0, 0.45);
  m_[18].grip = 1.0;
  m_[19] = preset("paint", 10e9, 4e9, 2300, 1e6, 1.5e6, 20e6, 1.2e6, 0.7, 300, 4.0, 4.0, 4.0, 0.45);
  m_[19].grip = 0.9;
  // Head and tail lamps' glass.
  m_[20] = preset("lamp", 5.6e9, 2.3e9, 400, 0.3e6, 0.5e6, 5e6, 0.4e6, 0.4, 2, 1.5, 1.5, 1.5, 0.6);
  m_[20].crush = 2e4;
  // Penetration resistance of the presets (J/m^3; brittle materials: 0, any impact removes them):
  // steel plate is armour (nothing but a cut goes through it); reinforcing bars bend under
  // bullets and blasts, they are cut; a steel section is holed by a rocket; sheet by a bullet.
  m_[2].penetration = 1e9;
  m_[10].penetration = 5e6;
  m_[11].penetration = 1.5e5;
  for (int i = 0; i < kStandardMaterials; ++i) used_[size_t(i)] = true;
}

bool MaterialTable::add(const Material& m, MaterialId* id) {
  for (int i = kStandardMaterials; i < kMaxMaterials; ++i) {
    if (used_[size_t(i)]) continue;
    m_[size_t(i)] = sanitized(m);
    used_[size_t(i)] = true;
    if (id) *id = static_cast<MaterialId>(i);
    return true;
  }
  return false;
}

void MaterialTable::set(MaterialId id, const Material& m) {
  const int i = static_cast<int>(id);
  if (i < 0 || i >= kMaxMaterials) return;
  m_[size_t(i)] = sanitized(m);
  used_[size_t(i)] = true;
}

bool MaterialTable::registered(MaterialId id) const {
  const int i = static_cast<int>(id);
  return i >= 0 && i < kMaxMaterials && used_[size_t(i)];
}

MaterialId MaterialTable::find(const char* name, bool* ok) const {
  for (int i = 0; name && i < kMaxMaterials; ++i) {
    if (used_[size_t(i)] && m_[size_t(i)].name == name) {
      if (ok) *ok = true;
      return static_cast<MaterialId>(i);
    }
  }
  if (ok) *ok = false;
  return MaterialId::Concrete;
}

MaterialTable& default_materials() {
  static MaterialTable t;
  return t;
}

}  // namespace svx
