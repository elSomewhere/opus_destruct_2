#include "svx/material/material.hpp"

#include <array>

namespace svx {

namespace {

// Interface strengths are those of joints and crack planes (weaker than the intact material):
// plain concrete ~1 MPa in tension, reinforced concrete keeps a flexural reserve from its rebar,
// masonry fails along its mortar beds (a fraction of an MPa). The fragility knob divides all of
// them at run time. Fracture energies are those of the whole crack (the reinforcement's pull-out
// included for RC).
Material preset(const char* name, f64 E, f64 G, f64 rho, f64 ft, f64 fb, f64 fc, f64 coh, f64 mu, f64 Gf, f64 fx, f64 fy,
                f64 fz, f64 noise, bool indestructible = false) {
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
  return m;
}

struct Registry {
  std::array<Material, kMaxMaterials> m;
  std::array<bool, kMaxMaterials> used{};
  Registry() { reset(); }
  void reset() {
    for (int i = 0; i < kMaxMaterials; ++i) {
      m[size_t(i)] = Material{};
      used[size_t(i)] = false;
    }
    //                  name        E       G       rho   ft      fb      fc      cohesion friction Gf     frag x y z     noise
    m[0] = preset("rc",       30e9,  12.5e9, 2400, 1.2e6,  3.5e6,  30e6,  1.6e6,  0.7,  2000, 4.5, 4.5, 4.5, 0.35);
    m[1] = preset("concrete", 28e9,  11.5e9, 2300, 0.8e6,  1.2e6,  25e6,  1.2e6,  0.7,  400,  4.2, 4.2, 4.2, 0.45);
    m[2] = preset("steel",    200e9, 80e9,   7850, 180e6,  180e6,  200e6, 100e6,  0.35, 1e6,  6.0, 6.0, 6.0, 0.15);
    m[3] = preset("masonry",  10e9,  4e9,    1900, 0.15e6, 0.3e6,  8e6,   0.35e6, 0.75, 120,  3.0, 2.0, 1.6, 0.25);
    m[4] = preset("soil",     0.1e9, 0.04e9, 1700, 0.01e6, 0.01e6, 0.4e6, 0.03e6, 0.55, 20,   2.5, 2.5, 2.5, 0.5);
    m[5] = preset("rock",     40e9,  16e9,   2600, 2.5e6,  3.0e6,  80e6,  4e6,    0.8,  600,  4.0, 4.0, 4.0, 0.5);
    m[6] = preset("bedrock",  60e9,  25e9,   2800, 10e6,   10e6,   200e6, 15e6,   0.8,  2000, 5.0, 5.0, 5.0, 0.5, true);
    for (int i = 0; i < kStandardMaterials; ++i) used[size_t(i)] = true;
  }
};

Registry& registry() {
  static Registry r;
  return r;
}

}  // namespace

const Material& material(MaterialId id) {
  const Registry& r = registry();
  const int i = static_cast<int>(id);
  return (i < kMaxMaterials && r.used[size_t(i)]) ? r.m[size_t(i)] : r.m[0];
}

bool register_material(const Material& m, MaterialId* id) {
  Registry& r = registry();
  for (int i = kStandardMaterials; i < kMaxMaterials; ++i) {
    if (r.used[size_t(i)]) continue;
    r.m[size_t(i)] = m;
    r.used[size_t(i)] = true;
    if (id) *id = static_cast<MaterialId>(i);
    return true;
  }
  return false;
}

void set_material(MaterialId id, const Material& m) {
  const int i = static_cast<int>(id);
  if (i < 0 || i >= kMaxMaterials) return;
  Registry& r = registry();
  r.m[size_t(i)] = m;
  r.used[size_t(i)] = true;
}

bool material_registered(MaterialId id) {
  const int i = static_cast<int>(id);
  return i >= 0 && i < kMaxMaterials && registry().used[size_t(i)];
}

MaterialId material_from_name(const char* name, bool* ok) {
  const Registry& r = registry();
  for (int i = 0; name && i < kMaxMaterials; ++i) {
    if (r.used[size_t(i)] && r.m[size_t(i)].name == name) {
      if (ok) *ok = true;
      return static_cast<MaterialId>(i);
    }
  }
  if (ok) *ok = false;
  return MaterialId::Concrete;
}

void reset_materials() { registry().reset(); }

}  // namespace svx
