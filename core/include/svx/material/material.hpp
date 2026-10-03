// structvox — materials (docs/V2_DESIGN.md §1).
//
// A voxel stores a material id (7 bits: up to 127 materials). The properties of each id live in a
// table (MaterialTable): the standard presets below are registered by default, and a host may
// override them or register its own. Every World has its own table (World::materials), made from
// the process's (default_materials: what a host sets up at startup) when the world is made, so
// that worlds may differ in their materials. A world's table must not change while it steps (it
// is read concurrently, without locks); what a world built from it (fragments, structures)
// keeps the properties it was built with.
//
// SI units (Pa, kg/m^3, J/m^2). Strengths are section strengths of the bonds between fragments
// (pre-scored rubble pieces), not textbook material strengths: a fragment interface is a weak
// plane (cold joint, mortar bed, crack path). The stiffness only distributes the load (an
// equilibrium solve); it is never seen as deformation.
#pragma once

#include <array>
#include <string>

#include "svx/base/types.hpp"

namespace svx {

// A material id. The named values are the standard presets; any value 0..kMaxMaterials-1 is an
// id (a registered material, or the fallback properties of id 0).
enum class MaterialId : u8 {
  Rc = 0,     // reinforced concrete (reinforcement smeared: a flexural reserve)
  Concrete,
  Steel,      // structural steel (ductile)
  Masonry,    // brick
  Soil,
  Rock,
  Bedrock,    // indestructible
  Wood,       // structural timber
  Stone,      // cut natural stone in mortar (blocks)
  Glass,
  Rebar,      // concrete around a reinforcing bar (ductile): explicit reinforcement in thick members
  // A smeared section (docs/DAMAGE.md): a member's real section baked into a voxel material, as
  // rebar bakes a bar into its concrete cell - the weight and strength of thin-walled steel, not of
  // solid 12.5 cm cubes of it. Hosts make more of them (a vehicle's sheet metal, its frame) by
  // registering materials with these properties.
  SteelSection,  // a rolled steel section (HEB 200 as 2 x 2 voxels): 980 kg/m^3, 44 MPa, ductile
};
constexpr int kStandardMaterials = 12;
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
  // Ductile (metals, reinforcing bars): it yields and bends where brittle material crushes, so
  // crushing never turns it to dust (pieces keep it; concrete crushed around a bar falls away
  // and leaves the bar).
  bool ductile = false;
  // Reinforcement (bars in concrete): its voxels belong to the fragments of the material around
  // them, so every interface across a member has the bars in its section (a composite section:
  // the bars carry its tension). Bars with nothing around them form fragments of their own.
  bool reinforcement = false;
  // Crumpling (docs/DAMAGE.md): the pressure (Pa) at which it folds where it is pressed in a
  // contact. A contact with a crumpling side carries at most crush x its area: the softer side
  // folds (its voxels dent, buckle and compact), taking the collision's energy out over the
  // distance it folds, and what it hits feels that force, not a rigid body's spike. Sheet metal
  // and thin-walled smeared sections crumple; 0: it does not (brittle material cracks and crushes
  // by its bonds instead; a solid block or steel plate is rigid).
  f64 crush = 0.0;
  // Penetration resistance (J/m^3): the energy density an impact (a bullet's carve, a blast's
  // crater) needs to remove it. 0: any impact removes it (brittle materials). Bullets hole sheet
  // metal, rockets hole a steel section, nothing holes armour plate. A cut (a carve of no given
  // energy) removes everything but indestructible materials.
  f64 penetration = 0.0;
  // Wheel grip on it (the friction coefficient of a wheel's contact on it - a tyre on a road;
  // 0: 1.35 x its friction).
  f64 grip = 0.0;
  // Non-structural (docs/CORE.md §2; plants say: leaves, grass, a hedge): solid to rendering and
  // raycasts, burned and carved like any solid, but never structure - in no fragment, no bond, no
  // support (its voxels are never anchored), never extracted. It follows what it rests on: what
  // has no other voxel next to it after a change near it (what held it came loose, was cut) is
  // shed - removed with a dust event (falling leaves).
  bool non_structural = false;
  // (non-structural) Nothing collides with it: pieces, characters' links, wheels and the player go
  // through (leaves, grass; a hedge is not passable).
  bool passable = false;
};

// A voxel value's material kind (MaterialTable::vox_kind).
constexpr u8 kVoxNonStructural = 1;
constexpr u8 kVoxPassable = 2;

// Other modules keep their own per-material properties (the fire module: combustion and heat;
// renderers: colours), keyed by the same ids and names.

// The properties of every material id. Properties that are not positive and finite are replaced
// by defaults; fragment sizes are clamped to 1..64 voxels.
class MaterialTable {
 public:
  MaterialTable();  // the standard presets
  // The properties of id (the fallback, id 0's, for ids never registered).
  const Material& operator[](MaterialId id) const {
    const int i = static_cast<int>(id);
    return (i < kMaxMaterials && used_[size_t(i)]) ? m_[size_t(i)] : m_[0];
  }
  const Material& operator[](u8 id) const { return (*this)[static_cast<MaterialId>(id)]; }
  // Takes the next free id after the presets and the ones taken; false (and *id left) when all
  // kMaxMaterials ids are taken.
  bool add(const Material& m, MaterialId* id);
  void set(MaterialId id, const Material& m);  // override (e.g. a preset's strengths)
  bool registered(MaterialId id) const;
  MaterialId find(const char* name, bool* ok = nullptr) const;  // (not found: Concrete, *ok false)
  void reset();                                                 // back to the standard presets only
  // The kind of a voxel value's material (kVoxNonStructural, kVoxPassable; air: none) - for the hot
  // paths, a table by value.
  u8 vox_kind(u8 v) const { return kind_[v]; }
  bool any_non_structural() const { return any_non_structural_; }
  bool any_passable() const { return any_passable_; }

 private:
  std::array<Material, kMaxMaterials> m_;
  std::array<bool, kMaxMaterials> used_{};
  std::array<u8, 256> kind_{};
  bool any_non_structural_ = false, any_passable_ = false;
  void refresh_kinds();
};

// The process's table: the materials a World starts with (a host sets it up at startup, before it
// makes worlds). A world reads its own (World::materials).
MaterialTable& default_materials();

// The process's table (setup time only).
inline const Material& material(MaterialId id) { return default_materials()[id]; }
inline const Material& material(u8 id) { return default_materials()[id]; }
inline bool register_material(const Material& m, MaterialId* id) { return default_materials().add(m, id); }
inline void set_material(MaterialId id, const Material& m) { default_materials().set(id, m); }
inline bool material_registered(MaterialId id) { return default_materials().registered(id); }
inline MaterialId material_from_name(const char* name, bool* ok = nullptr) { return default_materials().find(name, ok); }
inline void reset_materials() { default_materials().reset(); }

}  // namespace svx
