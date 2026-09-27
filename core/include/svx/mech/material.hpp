// structvox — material table (v2: fragment-graph mechanics, docs/V2_DESIGN.md).
//
// SI units (Pa, kg/m^3). Strengths are game-calibrated section strengths of the bonds between
// fragments (pre-scored rubble pieces), not textbook material strengths: a fragment interface is
// a weak plane (cold joint, mortar bed, crack path), and the stiffness only distributes the load
// (an equilibrium solve), it is never seen as deformation.
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
  f64 E;         // Young's modulus
  f64 G;         // shear modulus
  f64 rho;       // density
  f64 ft;        // direct tensile strength of an interface
  f64 fb;        // flexural tensile strength (bending; the rebar reserve of reinforced concrete)
  f64 fc;        // compressive (crushing) strength
  f64 cohesion;  // shear strength at zero normal stress
  f64 friction;  // Mohr-Coulomb friction coefficient of the interface (and of rubble contact)
  f64 Gf;        // fracture energy (J/m^2): what a crack through an interface costs (impacts pay it)
  // Fragment (rubble piece) size: jittered-Voronoi seed spacing in voxels per axis (x, y, z) and
  // the relative jitter of the seam paths.
  f64 frag_x, frag_y, frag_z;
  f64 frag_noise;
};

const Material& material(MaterialId id);
inline const Material& material(u8 id) { return material(static_cast<MaterialId>(id)); }
MaterialId material_from_name(const char* name, bool* ok = nullptr);

}  // namespace svx
