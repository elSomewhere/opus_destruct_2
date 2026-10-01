// svx_city — the city's materials as a structvox world holds them (voxel_city svx/materials.js).
//
// A structvox voxel is a byte: 1 + a physics material id (up to 127 of them: how it carries,
// breaks and burns) and bit 7 for anchored (a support, never simulated). The city's 400 looks
// are far more than a physics engine needs, so each maps to a physics class, and what it looks
// like travels in a persistent voxel layer:
//
//   vox     (1 + class id) | 0x80 where anchored
//   look    its look's index within its class: the class and the index give back the city
//           material, for colour
//   flora   decorative voxels (leaves, flowers, grass blades, crops): air to the physics, 1 +
//           their index in FLORA in an air-bound layer
//   water   liquids (water, sewage, sludge): air, 255 in the "water" layer
//
// Class ids: the core's standard presets keep theirs, the game's materials theirs (at
// kStandardMaterials + 0..8), and the city registers its own right after them, at kCityBase + k.
//
// The tables (CLASSES, CLASSIFY, LOOKS, FLORA) are made once from the palette by the reference's
// rules (first match wins, the rules' regular expressions matched as JS matches them), on first
// use, from any thread.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/value.hpp"

namespace svx::city {

// Where the city's own physics materials start: kStandardMaterials (12) + mat::kGameMaterials (9).
constexpr int kCityBase = 21;

// A city physics material's own properties (structvox Material fields, SI units: section
// strengths of the bonds between fragments, fragment sizes in voxels).
struct SvxOwnMaterial {
  double E = 0, G = 0, rho = 0, ft = 0, fb = 0, fc = 0, cohesion = 0, friction = 0, Gf = 0;
  std::array<double, 3> frag{};
  double frag_noise = 0;
  std::optional<double> crush, grip;
};

// A physics class: its name, its structvox id, its own properties (none for the core's and the
// game's, which the engine already has).
struct SvxClass {
  std::string name;
  int id = 0;
  std::optional<SvxOwnMaterial> own;
};

// CLASSES (in the reference's order) and CLASS[name] (null: none).
const std::vector<SvxClass>& svx_classes();
const SvxClass* svx_class(std::string_view name);

// CLASSIFY[material]: air; liquid (the water layer); else its class as structure (s, its look
// s_look) and as ground (g, g_look), and for decorative plants flora with flora_idx (-1 where
// JS leaves a field undefined).
struct SvxClassify {
  bool air = false, liquid = false, flora = false;
  int s = -1, g = -1, s_look = -1, g_look = -1, flora_idx = -1;
};
// (material < kMaterialCount)
const SvxClassify& svx_classify(int material);

// LOOKS: per class id, the city materials of that class in look order (look index -> material);
// svx_look_classes() the classes in the order LOOKS (a Map) holds them.
const std::vector<int>& svx_looks(int class_id);
const std::vector<int>& svx_look_classes();
// FLORA: flora layer value - 1 -> city material id.
const std::vector<int>& svx_flora();

// The voxel byte of a physics class (1 + id, bit 7 anchored).
inline int vox(int class_id, bool anchored) { return (1 + class_id) | (anchored ? 0x80 : 0); }

// svxMaterials(): what a structvox host registers and draws with - { cityBase, register (the
// city's own physics materials at their ids), classes, looks (class id -> material names),
// flora (material names), palette (name -> { rgb, transparent, opacity, emissive, glow, noise }) }
// as JSON would hold it.
Value svx_materials();

}  // namespace svx::city
