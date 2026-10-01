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
  m.passable = m.passable && m.decorative;  // (structure is never passable)
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
  // A smeared section (docs/DAMAGE.md): the real section of a thin-walled member baked into its
  // voxels' material (as rebar bakes its bar into the concrete cell), so members weigh and carry
  // what they would: steel is 10 mm webs and flanges, not solid 12.5 cm cubes.
  // A rolled section (HEB 200: 61 kg/m, 78 cm^2 of S355) as a member of 2 x 2 voxels (625 cm^2):
  // 980 kg/m^3, 355 MPa x 78 / 625 = 44 MPa, its stiffness smeared likewise.
  m_[11] = preset("steel_section", 26e9, 10e9, 980, 44e6, 44e6, 40e6, 25e6, 0.35, 2e5, 6.0, 6.0, 6.0, 0.15, false, true);
  // Penetration resistance of the presets (J/m^3; brittle materials: 0, any impact removes them):
  // steel plate is armour (nothing but a cut goes through it); reinforcing bars bend under
  // bullets and blasts, they are cut; a steel section is holed by a rocket.
  m_[2].penetration = 1e9;
  m_[10].penetration = 5e6;
  m_[11].penetration = 1.5e5;
  for (int i = 0; i < kStandardMaterials; ++i) used_[size_t(i)] = true;
  refresh_kinds();
}

void MaterialTable::refresh_kinds() {
  any_decorative_ = any_passable_ = false;
  kind_[0] = 0;
  for (int v = 1; v < 256; ++v) {
    const int low = v & 0x7F;
    if (low == 0) {
      kind_[size_t(v)] = 0;
      continue;
    }
    const Material& m = (*this)[static_cast<MaterialId>(low - 1)];
    kind_[size_t(v)] = static_cast<u8>((m.decorative ? kVoxDecorative : 0) | (m.passable ? kVoxPassable : 0));
    any_decorative_ = any_decorative_ || m.decorative;
    any_passable_ = any_passable_ || m.passable;
  }
}

bool MaterialTable::add(const Material& m, MaterialId* id) {
  for (int i = kStandardMaterials; i < kMaxMaterials; ++i) {
    if (used_[size_t(i)]) continue;
    m_[size_t(i)] = sanitized(m);
    used_[size_t(i)] = true;
    refresh_kinds();
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
  refresh_kinds();
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
