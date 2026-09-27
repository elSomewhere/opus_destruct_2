#include "svx/mech/material.hpp"

#include <cstring>

namespace svx {

namespace {
// Interface strengths are those of joints and crack planes (weaker than the intact material):
// plain concrete ~1 MPa in tension, reinforced concrete keeps a flexural reserve from its rebar,
// masonry fails along its mortar beds (a fraction of an MPa). The fragility knob divides all of
// them at run time.
//                name        E       G      rho    ft       fb       fc      cohesion friction  frag x y z  noise
constexpr Material kMaterials[static_cast<int>(MaterialId::Count)] = {
    {"rc",       30e9,  12.5e9, 2400, 1.2e6,   3.5e6,   30e6,  1.6e6,   0.7,     3.6, 3.6, 3.6, 0.35},
    {"concrete", 28e9,  11.5e9, 2300, 0.8e6,   1.2e6,   25e6,  1.2e6,   0.7,     3.4, 3.4, 3.4, 0.45},
    {"steel",    200e9, 80e9,   7850, 180e6,   180e6,   200e6, 100e6,   0.35,    6.0, 6.0, 6.0, 0.15},
    {"masonry",  10e9,  4e9,    1900, 0.15e6,  0.3e6,   8e6,   0.35e6,  0.75,    3.0, 2.0, 1.6, 0.25},
    {"soil",     0.1e9, 0.04e9, 1700, 0.01e6,  0.01e6,  0.4e6, 0.03e6,  0.55,    2.5, 2.5, 2.5, 0.5},
    {"rock",     40e9,  16e9,   2600, 2.5e6,   3.0e6,   80e6,  4e6,     0.8,     4.0, 4.0, 4.0, 0.5},
    {"bedrock",  60e9,  25e9,   2800, 10e6,    10e6,    200e6, 15e6,    0.8,     5.0, 5.0, 5.0, 0.5},
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
