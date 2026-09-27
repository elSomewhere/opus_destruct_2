// structvox — material table.
//
// Values are ported from the prototype oracle (voxel_threed_discrete src/archetypes.js
// material table, SI units: Pa, kg/m^3, J/m^2). They define the fine lattice model.
#pragma once

#include "svx/base/types.hpp"

namespace svx {

enum class MaterialId : u8 {
  Rc = 0,
  Concrete,
  Steel,
  Masonry,
  Soil,
  Rock,
  Bedrock,
  Count
};

struct Material {
  const char* name;
  f64 E;      // Young's modulus
  f64 G;      // shear modulus
  f64 rho;    // density
  f64 ft;     // tensile strength
  f64 fc;     // compressive strength
  f64 tau;    // shear strength
  f64 fb;     // bending (flexural) strength
  f64 taut;   // torsional strength
  f64 GfI;    // mode-I fracture energy
  f64 GfII;   // mode-II fracture energy
};

const Material& material(MaterialId id);
inline const Material& material(u8 id) { return material(static_cast<MaterialId>(id)); }
MaterialId material_from_name(const char* name, bool* ok = nullptr);

}  // namespace svx
