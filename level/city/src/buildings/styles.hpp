// svx_city — architectural style profiles (voxel_city buildings/styles.js and the STYLES
// registry of world/registry.js).
//
// A style is orthogonal to the building archetype: any archetype can be dressed in any style.
// Values are material choice lists (picked per building: resolve_style) and window-system
// parameters in metres. window.type: "punched" (individual windows in a masonry wall), "ribbon"
// (continuous horizontal bands), "curtain" (full glazing with a mullion grid), "open" (open
// parking decks: a continuous opening between columns).
//
// The nordic styles' optional keys (absent: off, so every other style resolves as before):
// boards (a darker seam every 3 voxels: SEAMS), corners (corner boards / quoins in the trim),
// shop_base (false: storefront ground floors keep the wall above the plinth), base_h (height in
// voxels of the base material at the foot of the ground floor facade, default 6),
// window.casing (a 1-voxel trim surround), window.transom (a horizontal glazing bar).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/hash.hpp"
#include "world/registry.hpp"

namespace svx::city {

// (registrations initialize these by member designators: every member has a default)
struct StyleWindow {
  std::string type{};
  double width = 0, sill = 0, head = 0, bay = 0;  // (m)
  bool lintel = false;
  std::optional<double> shutters{};  // (the chance of shutters: siding, barn, suburbanBrick)
  bool casing = false, transom = false;
};

struct Style {
  std::string id{};
  std::vector<uint16_t> walls{}, base{}, trim{}, glass{}, frame{};  // material choices (voxel/materials.hpp MAT)
  StyleWindow window{};
  bool cornice = false;
  std::vector<uint16_t> roof{}, pitched{};
  double fire_escape = 0, balcony = 0, water_tank = 0, awning = 0;  // chances
  std::vector<uint16_t> storefront{};
  std::vector<uint16_t> joint{};  // (prefab panels' joints; empty: undefined)
  bool accent_strips = false;
  bool boards = false, corners = false;
  std::optional<bool> shop_base{};
  std::optional<double> base_h{};
};

// STYLES: read-only once register_all() has run; _mut for registration only.
const Registry<Style>& style_registry();
Registry<Style>& style_registry_mut();

// buildings/styles.js's registrations (register_all calls it, in the reference's order).
void register_styles();

// SEAMS: the seam shade and direction ('v' board joints, 'h' log grooves) of a board or log wall
// material, or null.
struct Seam {
  uint16_t m = 0;
  char dir = 'v';
};
const Seam* seam_of(uint16_t wall);

// A style resolved into concrete materials for one building (resolveStyle). joint and seam are
// -1, seam_dir '\0' where JS has null.
struct ResolvedStyle {
  std::string id;
  uint16_t wall = 0, base = 0, trim = 0, glass = 0, frame = 0;
  StyleWindow window;
  bool cornice = false;
  uint16_t roof = 0, pitched = 0, storefront = 0;
  bool fire_escape = false, balcony = false, water_tank = false;
  double awning = 0;
  bool shutters = false, accent_strips = false;
  int joint = -1;
  int seam = -1;
  char seam_dir = 0;
  bool corners = false, shop_base = true;
  double base_h = 6;
};
// (draws from rng in the reference's order)
ResolvedStyle resolve_style(const std::string& style_id, Rng& rng);

}  // namespace svx::city
