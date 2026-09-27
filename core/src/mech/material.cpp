#include "svx/mech/material.hpp"

#include <cstring>

namespace svx {

namespace {
// name, E, G, rho, ft, fc, tau, fb, taut, GfI, GfII  (prototype archetypes.js)
constexpr Material kMaterials[static_cast<int>(MaterialId::Count)] = {
    {"rc", 35e9, 14e9, 2500, 8e6, 35e6, 8e6, 12e6, 4e6, 250, 450},
    {"concrete", 30e9, 12.5e9, 2400, 3e6, 30e6, 5e6, 4e6, 2e6, 120, 220},
    {"steel", 200e9, 80e9, 7800, 250e6, 250e6, 145e6, 200e6, 100e6, 5e4, 8e4},
    {"masonry", 15e9, 6e9, 1800, 0.5e6, 15e6, 1.5e6, 1e6, 0.5e6, 40, 60},
    {"soil", 60e6, 22e6, 1600, 0.02e6, 0.4e6, 0.05e6, 0.03e6, 0.02e6, 5, 8},
    {"rock", 50e9, 20e9, 2700, 4e6, 120e6, 10e6, 6e6, 4e6, 80, 140},
    {"bedrock", 80e9, 33e9, 2900, 20e6, 400e6, 40e6, 30e6, 20e6, 400, 700},
};
}  // namespace

const Material& material(MaterialId id) {
  const int i = static_cast<int>(id);
  SVX_ASSERT(i >= 0 && i < static_cast<int>(MaterialId::Count));
  return kMaterials[i];
}

MaterialId material_from_name(const char* name, bool* ok) {
  for (int i = 0; i < static_cast<int>(MaterialId::Count); ++i) {
    if (std::strcmp(kMaterials[i].name, name) == 0) {
      if (ok) *ok = true;
      return static_cast<MaterialId>(i);
    }
  }
  if (ok) *ok = false;
  return MaterialId::Concrete;
}

}  // namespace svx
