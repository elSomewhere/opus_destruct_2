// svx_city — the city's materials as a structvox host registers and draws them (the export's
// physics classes and looks: voxel_city svx/materials.js svxMaterials()). Public: the adapter in
// svx_procgen registers the classes and builds the appearance table from the looks.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace svx::city {

// Where the city's own physics materials start: after the core's 12 standard presets and the
// game's 9 materials.
inline constexpr int kCityMaterialBase = 21;

// A physics class: its structvox material id; for the city's own classes (own: roofing,
// partition, soft, ice, snow, foliage) their properties (SI units: section strengths of the bonds
// between fragments, fragment sizes in voxels; crush and grip 0 where the class has none).
struct PhysicsClass {
  std::string name;
  int id = 0;
  bool own = false;
  double E = 0, G = 0, rho = 0, ft = 0, fb = 0, fc = 0, cohesion = 0, friction = 0, Gf = 0;
  std::array<double, 3> frag{};
  double frag_noise = 0, crush = 0, grip = 0;
};
// The classes in the export's order.
const std::vector<PhysicsClass>& physics_classes();

// A look: a class and the index of the look within it, and the city material it draws as.
struct Look {
  int class_id = 0, look = 0;
  std::string material;
  std::array<uint8_t, 3> rgb{};  // sRGB
  double noise = 0, opacity = 1, emissive = 0;
  bool transparent = false, glow = false;
  bool flora = false;  // (a plant: decorative)
};
// Every (class, look) pair: the classes in the order the export's LOOKS holds them, each one's
// looks in order.
const std::vector<Look>& looks();

}  // namespace svx::city
