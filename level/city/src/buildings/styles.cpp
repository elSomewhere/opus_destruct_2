// svx_city — voxel_city buildings/styles.js.
#include "buildings/styles.hpp"

#include "voxel/materials.hpp"

namespace svx::city {

const Registry<Style>& style_registry() { return style_registry_mut(); }

Registry<Style>& style_registry_mut() {
  static Registry<Style> r("style");
  return r;
}

void register_styles() {
  using namespace MAT;
  Registry<Style>& R = style_registry_mut();
  R.add({.id = "brick",
         .walls = {BRICK_RED, BRICK_BROWN, BRICK_YELLOW, BRICK_DARK},
         .base = {GRANITE, LIMESTONE, CONCRETE_DARK},
         .trim = {TRIM_STONE, LIMESTONE, FRAME_WHITE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE, FRAME_DARK, FRAME_WOOD},
         .window = {.type = "punched", .width = 1.25, .sill = 0.875, .head = 2.375, .bay = 3.0, .lintel = true},
         .cornice = true,
         .roof = {ROOF_MEMBRANE, ROOF_GRAVEL},
         .pitched = {ROOF_TILE_DARK, ROOF_SHINGLE},
         .fire_escape = 0.45,
         .balcony = 0.15,
         .water_tank = 0.35,
         .awning = 0.6,
         .storefront = {FRAME_DARK, FRAME_WOOD}});
  R.add({.id = "plaster",
         .walls = {PLASTER_CREAM, PLASTER_PEACH, PLASTER_WHITE, PLASTER_BLUE, PLASTER_GREEN, PLASTER_OCHRE, PLASTER_PINK},
         .base = {CONCRETE_DARK, GRANITE, PLASTER_WHITE},
         .trim = {PLASTER_WHITE, TRIM_STONE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE, FRAME_DARK},
         .window = {.type = "punched", .width = 1.25, .sill = 0.875, .head = 2.375, .bay = 3.25, .lintel = false},
         .cornice = true,
         .roof = {ROOF_MEMBRANE, ROOF_GRAVEL},
         .pitched = {ROOF_TILE_RED, ROOF_TILE_DARK},
         .fire_escape = 0,
         .balcony = 0.55,
         .water_tank = 0,
         .awning = 0.5,
         .storefront = {FRAME_DARK, FRAME_WHITE}});
  R.add({.id = "concrete",
         .walls = {CONCRETE, CONCRETE_LIGHT, CINDERBLOCK, LIMESTONE},
         .base = {CONCRETE_DARK, GRANITE},
         .trim = {CONCRETE_LIGHT, CONCRETE_DARK},
         .glass = {GLASS, GLASS_TINT},
         .frame = {FRAME_DARK, MULLION_SILVER},
         .window = {.type = "ribbon", .width = 2.5, .sill = 0.875, .head = 2.625, .bay = 3.0, .lintel = false},
         .cornice = false,
         .roof = {ROOF_MEMBRANE, ROOF_GRAVEL},
         .pitched = {ROOF_SHINGLE},
         .fire_escape = 0,
         .balcony = 0.35,
         .water_tank = 0.05,
         .awning = 0.25,
         .storefront = {FRAME_DARK, MULLION_SILVER}});
  R.add({.id = "glass",
         .walls = {METAL_PANEL, METAL_PANEL_DARK, CONCRETE_LIGHT},
         .base = {GRANITE, FLOOR_MARBLE_DARK, METAL_PANEL_DARK},
         .trim = {MULLION_SILVER, MULLION},
         .glass = {GLASS_TINT, GLASS_GREEN, GLASS_BRONZE, GLASS},
         .frame = {MULLION, MULLION_SILVER},
         .window = {.type = "curtain", .width = 1.5, .sill = 0.125, .head = 3.0, .bay = 1.5, .lintel = false},
         .cornice = false,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_SHINGLE},
         .fire_escape = 0,
         .balcony = 0.05,
         .water_tank = 0,
         .awning = 0.1,
         .storefront = {MULLION, MULLION_SILVER}});
  R.add({.id = "deco",
         .walls = {LIMESTONE, BRICK_YELLOW, TRIM_STONE, BRICK_BROWN},
         .base = {GRANITE, FLOOR_MARBLE_DARK},
         .trim = {CORNICE, TRIM_STONE},
         .glass = {GLASS, GLASS_TINT},
         .frame = {FRAME_DARK, MULLION},
         .window = {.type = "punched", .width = 1.25, .sill = 0.75, .head = 2.5, .bay = 2.25, .lintel = true},
         .cornice = true,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_METAL_GREEN},
         .fire_escape = 0.1,
         .balcony = 0.05,
         .water_tank = 0.4,
         .awning = 0.3,
         .storefront = {FRAME_DARK, MULLION}});
  R.add({.id = "futurist",
         .walls = {PANEL_WHITE, PANEL_GRAPHITE},
         .base = {PANEL_GRAPHITE, FLOOR_MARBLE_DARK},
         .trim = {LIGHT_STRIP, NEON_CYAN, NEON_MAGENTA},
         .glass = {GLASS_TINT, GLASS_GREEN},
         .frame = {PANEL_GRAPHITE, MULLION},
         .window = {.type = "ribbon", .width = 3.0, .sill = 0.5, .head = 2.75, .bay = 3.0, .lintel = false},
         .cornice = false,
         .roof = {ROOF_GREEN, ROOF_MEMBRANE},
         .pitched = {ROOF_SHINGLE},
         .fire_escape = 0,
         .balcony = 0.25,
         .water_tank = 0,
         .awning = 0,
         .storefront = {PANEL_GRAPHITE},
         .accent_strips = true});
  R.add({.id = "siding",
         .walls = {SIDING_WHITE, SIDING_BLUE, SIDING_GRAY, SIDING_YELLOW, WOOD_SIDING},
         .base = {CONCRETE, BRICK_RED},
         .trim = {FRAME_WHITE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE},
         .window = {.type = "punched", .width = 1.0, .sill = 0.875, .head = 2.25, .bay = 2.75, .lintel = false, .shutters = 0.5},
         .cornice = false,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_SHINGLE, ROOF_TILE_DARK, ROOF_TILE_RED, ROOF_METAL_GREEN},
         .fire_escape = 0,
         .balcony = 0.1,
         .water_tank = 0,
         .awning = 0,
         .storefront = {FRAME_WHITE}});
  R.add({.id = "barn",
         .walls = {BARN_RED, BARN_RED, WOOD_SIDING, WOOD_DARK},
         .base = {STONE, CONCRETE},
         .trim = {FRAME_WHITE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE},
         .window = {.type = "punched", .width = 1.0, .sill = 1.5, .head = 2.5, .bay = 5.5, .lintel = false, .shutters = 0},
         .cornice = false,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_METAL_GREEN, CORRUGATED_RUST, ROOF_SHINGLE, CORRUGATED},
         .fire_escape = 0,
         .balcony = 0,
         .water_tank = 0,
         .awning = 0,
         .storefront = {FRAME_WHITE}});
  // Soviet prefab panel blocks (khrushchyovka / brezhnevka): ribbed panel joints, loggias.
  R.add({.id = "panel",
         .walls = {PANEL_BEIGE, PANEL_GRAYBLUE, CONCRETE_LIGHT, PANEL_BEIGE, PLASTER_WHITE},
         .base = {CONCRETE, CONCRETE_DARK},
         .trim = {CONCRETE_DARK},
         .glass = {GLASS},
         .frame = {FRAME_WHITE, FRAME_WOOD, FRAME_DARK},
         .window = {.type = "punched", .width = 1.5, .sill = 0.875, .head = 2.25, .bay = 3.2, .lintel = false},
         .cornice = false,
         .roof = {ROOF_MEMBRANE, ROOF_GRAVEL},
         .pitched = {ROOF_METAL_GREEN},
         .fire_escape = 0,
         .balcony = 0.55,
         .water_tank = 0,
         .awning = 0,
         .storefront = {FRAME_DARK},
         .joint = {PANEL_JOINT}});
  // Stalinist blocks on the avenues of Soviet towns: ochre plaster, cornices, tall windows.
  R.add({.id = "stalinist",
         .walls = {PLASTER_OCHRE, PLASTER_CREAM, PLASTER_PEACH, BRICK_BROWN},
         .base = {GRANITE, TRIM_STONE},
         .trim = {TRIM_STONE, PLASTER_WHITE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE, FRAME_WOOD},
         .window = {.type = "punched", .width = 1.25, .sill = 0.875, .head = 2.75, .bay = 2.9, .lintel = true},
         .cornice = true,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_METAL_GREEN, ROOF_TILE_DARK},
         .fire_escape = 0,
         .balcony = 0.3,
         .water_tank = 0,
         .awning = 0.2,
         .storefront = {FRAME_WOOD, FRAME_DARK}});
  // Housing projects: dark brick slabs and towers, small windows, no ornament.
  R.add({.id = "projects",
         .walls = {BRICK_DARK, BRICK_BROWN, BRICK_RED, CONCRETE_DARK},
         .base = {CONCRETE_DARK},
         .trim = {CONCRETE},
         .glass = {GLASS},
         .frame = {FRAME_DARK, FRAME_WHITE},
         .window = {.type = "punched", .width = 1.1, .sill = 0.875, .head = 2.125, .bay = 2.9, .lintel = false},
         .cornice = false,
         .roof = {ROOF_MEMBRANE, ROOF_GRAVEL},
         .pitched = {ROOF_SHINGLE},
         .fire_escape = 0,
         .balcony = 0.1,
         .water_tank = 0.2,
         .awning = 0,
         .storefront = {FRAME_DARK}});
  R.add({.id = "suburbanBrick",
         .walls = {BRICK_RED, BRICK_BROWN, BRICK_WHITE},
         .base = {CONCRETE},
         .trim = {FRAME_WHITE, TRIM_STONE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE, FRAME_DARK},
         .window = {.type = "punched", .width = 1.0, .sill = 0.875, .head = 2.25, .bay = 2.75, .lintel = true, .shutters = 0.3},
         .cornice = false,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_SHINGLE, ROOF_TILE_DARK},
         .fire_escape = 0,
         .balcony = 0,
         .water_tank = 0,
         .awning = 0,
         .storefront = {FRAME_WHITE}});
  R.add({.id = "industrial",
         .walls = {CORRUGATED, CORRUGATED_BLUE, CORRUGATED_RUST, CINDERBLOCK, METAL_PANEL},
         .base = {CONCRETE, CINDERBLOCK},
         .trim = {CONCRETE_DARK, HAZARD_YELLOW},
         .glass = {GLASS, GLASS_TINT},
         .frame = {FRAME_DARK},
         .window = {.type = "ribbon", .width = 2.0, .sill = 2.0, .head = 3.0, .bay = 6.0, .lintel = false},
         .cornice = false,
         .roof = {ROOF_MEMBRANE, CORRUGATED},
         .pitched = {CORRUGATED, CORRUGATED_RUST},
         .fire_escape = 0,
         .balcony = 0,
         .water_tank = 0.1,
         .awning = 0,
         .storefront = {FRAME_DARK}});
  R.add({.id = "adobe",
         .walls = {SANDSTONE, PLASTER_OCHRE, PLASTER_CREAM, CLAY, PLASTER_WHITE},
         .base = {SANDSTONE, CLAY},
         .trim = {WOOD_DARK, PLASTER_WHITE},
         .glass = {GLASS},
         .frame = {FRAME_WOOD, WOOD_DARK},
         .window = {.type = "punched", .width = 0.875, .sill = 1.0, .head = 2.125, .bay = 2.75, .lintel = true},
         .cornice = false,
         .roof = {ROOF_MEMBRANE, CLAY},
         .pitched = {ROOF_TILE_RED},
         .fire_escape = 0,
         .balcony = 0.25,
         .water_tank = 0.3,
         .awning = 0.55,
         .storefront = {FRAME_WOOD, WOOD_DARK}});
  R.add({.id = "parkingDeck",
         .walls = {CONCRETE, CONCRETE_LIGHT, CONCRETE_DARK},
         .base = {CONCRETE_DARK},
         .trim = {CONCRETE_LIGHT, PAINT_WHITE},
         // open decks: the "glazing" of an open parking level is air
         .glass = {AIR},
         .frame = {CONCRETE, CONCRETE_LIGHT},
         .window = {.type = "open", .width = 7.0, .sill = 1.0, .head = 2.5, .bay = 7.5, .lintel = false},
         .cornice = false,
         .roof = {ROOF_MEMBRANE, CONCRETE},
         .pitched = {ROOF_SHINGLE},
         .fire_escape = 0,
         .balcony = 0,
         .water_tank = 0,
         .awning = 0,
         .storefront = {CONCRETE_DARK}});
  // --- nordic
  // Painted wooden town houses of an old northern town centre (Roros, Trondheim, Karelian towns):
  // vertical-board cladding in falu red, ochre, white, grey-blue or green with white corner boards
  // and window casings, small cross windows on a narrow bay, steep dark roofs, simple wooden shop
  // windows; no fire escapes, water tanks or awnings to speak of.
  R.add({.id = "nordicWood",
         .walls = {CLAD_FALU, CLAD_FALU, CLAD_OCHRE, CLAD_WHITE, CLAD_WHITE, CLAD_GREYBLUE, CLAD_GREEN},
         .base = {GRANITE, GRANITE_LIGHT, STONE},
         .trim = {FRAME_WHITE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE},
         .window = {.type = "punched", .width = 1.0, .sill = 0.75, .head = 2.25, .bay = 2.5, .lintel = true, .casing = true, .transom = true},
         .cornice = false,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_BLACK_TILE, ROOF_BLACK_METAL, ROOF_TILE_DARK, ROOF_GREEN_DARK, ROOF_SLATE},
         .fire_escape = 0,
         .balcony = 0.08,
         .water_tank = 0,
         .awning = 0.1,
         .storefront = {FRAME_WHITE, FRAME_WOOD},
         .boards = true,
         .corners = true,
         .shop_base = false});
  // Rendered stone merchant houses and civic buildings of the same towns: pale yellow, white, grey
  // or ochre render over a granite plinth, tall punched windows with white casings and lintels,
  // pitched metal or slate roofs.
  R.add({.id = "nordicPlaster",
         .walls = {RENDER_YELLOW, RENDER_WHITE, RENDER_GREY, RENDER_OCHRE, RENDER_YELLOW},
         .base = {GRANITE, GRANITE_LIGHT},
         .trim = {PLASTER_WHITE, TRIM_STONE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE, FRAME_WOOD},
         .window = {.type = "punched", .width = 1.125, .sill = 0.875, .head = 2.5, .bay = 2.75, .lintel = true, .casing = true},
         .cornice = true,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_BLACK_METAL, ROOF_SLATE, ROOF_TILE_DARK, ROOF_GREEN_DARK},
         .fire_escape = 0,
         .balcony = 0.05,
         .water_tank = 0,
         .awning = 0.3,
         .storefront = {FRAME_WOOD, FRAME_DARK, FRAME_WHITE},
         .corners = true});
  // Forest cabin (hytte): tarred logs or falu-red boards, white frames, dark or sod roof.
  R.add({.id = "cabin",
         .walls = {LOG_TARRED, LOG_TARRED, CLAD_FALU, CLAD_FALU, CLAD_GREEN},
         .base = {STONE, GRANITE},
         .trim = {FRAME_WHITE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE},
         .window = {.type = "punched", .width = 1.0, .sill = 0.875, .head = 2.0, .bay = 2.25, .lintel = true, .casing = true, .transom = true},
         .cornice = false,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_BLACK_METAL, ROOF_BLACK_TILE, ROOF_GREEN_DARK, ROOF_SOD},
         .fire_escape = 0,
         .balcony = 0,
         .water_tank = 0,
         .awning = 0,
         .storefront = {FRAME_WHITE},
         .boards = true,
         .corners = true,
         .shop_base = false,
         // the cabin sits on its own stone plinth: walls start at the floor
         .base_h = 2});
  // Wooden church (Norwegian long church): white or falu-red boards, tall cross windows, dark roofs.
  R.add({.id = "nordicChurch",
         .walls = {CLAD_WHITE, CLAD_WHITE, CLAD_WHITE, CLAD_FALU, CLAD_OCHRE},
         .base = {GRANITE, GRANITE_LIGHT},
         .trim = {FRAME_WHITE},
         .glass = {GLASS},
         .frame = {FRAME_WHITE},
         .window = {.type = "punched", .width = 1.125, .sill = 1.5, .head = 4.25, .bay = 3.25, .lintel = true, .casing = true, .transom = true},
         .cornice = false,
         .roof = {ROOF_MEMBRANE},
         .pitched = {ROOF_BLACK_TILE, ROOF_SLATE, ROOF_BLACK_METAL, ROOF_GREEN_DARK},
         .fire_escape = 0,
         .balcony = 0,
         .water_tank = 0,
         .awning = 0,
         .storefront = {FRAME_WHITE},
         .boards = true,
         .corners = true,
         .shop_base = false});
}

const Seam* seam_of(uint16_t wall) {
  using namespace MAT;
  // SEAMS (a Map: wall material -> its seam shade and direction)
  static const Seam kSeams[] = {{CLAD_FALU_SEAM, 'v'}, {CLAD_OCHRE_SEAM, 'v'}, {CLAD_WHITE_SEAM, 'v'}, {CLAD_GREYBLUE_SEAM, 'v'}, {CLAD_GREEN_SEAM, 'v'}, {LOG_TARRED_SEAM, 'h'}};
  static const uint16_t kWalls[] = {CLAD_FALU, CLAD_OCHRE, CLAD_WHITE, CLAD_GREYBLUE, CLAD_GREEN, LOG_TARRED};
  for (size_t k = 0; k < sizeof(kWalls) / sizeof(kWalls[0]); ++k)
    if (kWalls[k] == wall) return &kSeams[k];
  return nullptr;
}

ResolvedStyle resolve_style(const std::string& style_id, Rng& rng) {
  const Style& s = style_registry().get(style_id);
  ResolvedStyle o;
  o.wall = rng.pick(s.walls);
  const Seam* seam = s.boards ? seam_of(o.wall) : nullptr;
  o.id = s.id;
  o.base = rng.pick(s.base);
  o.trim = rng.pick(s.trim);
  o.glass = rng.pick(s.glass);
  o.frame = rng.pick(s.frame);
  o.window = s.window;
  o.cornice = s.cornice;
  o.roof = rng.pick(s.roof);
  o.pitched = rng.pick(s.pitched);
  o.storefront = rng.pick(s.storefront);
  o.fire_escape = rng.chance(s.fire_escape);
  o.balcony = rng.chance(s.balcony);
  o.water_tank = rng.chance(s.water_tank);
  o.awning = s.awning;
  o.shutters = s.window.shutters && js::truthy(*s.window.shutters) ? rng.chance(*s.window.shutters) : false;
  o.accent_strips = s.accent_strips;
  // prefab panels: joints at every panel edge (bay) and above every slab
  o.joint = s.joint.empty() ? -1 : rng.pick(s.joint);
  // nordic extras (no draws, so other styles resolve unchanged)
  o.seam = seam ? seam->m : -1;
  o.seam_dir = seam ? seam->dir : 0;
  o.corners = s.corners;
  o.shop_base = s.shop_base.value_or(true);
  o.base_h = s.base_h.value_or(6);
  return o;
}

}  // namespace svx::city
