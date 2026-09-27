// structvox — materials (docs/V2_DESIGN.md §1).
//
// A voxel stores a material id (7 bits: up to 127 materials). The properties of each id live in a
// process-wide registry that the host fills at startup: the seven standard presets below are
// registered by default (ids 0..6), and a host may override them or register its own. The
// registry must not change while a World steps (it is read concurrently, without locks).
//
// SI units (Pa, kg/m^3, J/m^2). Strengths are section strengths of the bonds between fragments
// (pre-scored rubble pieces), not textbook material strengths: a fragment interface is a weak
// plane (cold joint, mortar bed, crack path). The stiffness only distributes the load (an
// equilibrium solve); it is never seen as deformation.
#pragma once

#include <string>

#include "svx/base/types.hpp"

namespace svx {

// A material id. The named values are the standard presets; any value 0..kMaxMaterials-1 is an
// id (a registered material, or the fallback properties of id 0).
enum class MaterialId : u8 {
  Rc = 0,     // reinforced concrete
  Concrete,
  Steel,
  Masonry,
  Soil,
  Rock,
  Bedrock,    // indestructible
};
constexpr int kStandardMaterials = 7;
constexpr int kMaxMaterials = 127;

struct Material {
  std::string name;
  f64 E = 30e9;          // Young's modulus
  f64 G = 12.5e9;        // shear modulus
  f64 rho = 2400;        // density
  f64 ft = 1e6;          // direct tensile strength of an interface
  f64 fb = 1e6;          // flexural tensile strength (bending; e.g. the rebar reserve of reinforced concrete)
  f64 fc = 30e6;         // compressive (crushing) strength
  f64 cohesion = 1e6;    // shear strength at zero normal stress
  f64 friction = 0.7;    // Mohr-Coulomb friction coefficient of the interface (and of rubble contact)
  f64 Gf = 400;          // fracture energy (J/m^2): what a crack through an interface costs (impacts pay it)
  // Fragment (rubble piece) size: jittered-Voronoi seed spacing in voxels per axis (x, y, z) and
  // the relative jitter of the seam paths.
  f64 frag_x = 4.0, frag_y = 4.0, frag_z = 4.0;
  f64 frag_noise = 0.4;
  bool indestructible = false;  // carves and blasts leave it (e.g. bedrock)
};

// The properties of id (the fallback, id 0's, for ids never registered).
const Material& material(MaterialId id);
inline const Material& material(u8 id) { return material(static_cast<MaterialId>(id)); }

// Registry (setup time only). register_material takes the next free id after the presets and
// the ones taken; returns false (and leaves *id) when all kMaxMaterials ids are taken.
// Properties that are not positive and finite are replaced by defaults; fragment sizes are
// clamped to 1..64 voxels.
bool register_material(const Material& m, MaterialId* id);
void set_material(MaterialId id, const Material& m);  // override (e.g. a preset's strengths)
bool material_registered(MaterialId id);
MaterialId material_from_name(const char* name, bool* ok = nullptr);  // (not found: Concrete, *ok false)
void reset_materials();  // back to the standard presets only

}  // namespace svx
