import { STYLES } from "../world/registry.js";
import { MAT } from "../voxel/materials.js";

/**
 * Architectural style profiles. A style is orthogonal to the building
 * archetype: any archetype can be dressed in any style. Values are material
 * choice lists (picked per building) and window-system parameters in meters.
 *
 * window.type:
 *   punched  – individual windows in a masonry wall
 *   ribbon   – continuous horizontal bands
 *   curtain  – full glazing with mullion grid
 *   open     – open parking decks: a continuous opening between columns
 */

STYLES.register({
  id: "brick",
  walls: [MAT.BRICK_RED, MAT.BRICK_BROWN, MAT.BRICK_YELLOW, MAT.BRICK_DARK],
  base: [MAT.GRANITE, MAT.LIMESTONE, MAT.CONCRETE_DARK],
  trim: [MAT.TRIM_STONE, MAT.LIMESTONE, MAT.FRAME_WHITE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE, MAT.FRAME_DARK, MAT.FRAME_WOOD],
  window: { type: "punched", width: 1.25, sill: 0.875, head: 2.375, bay: 3.0, lintel: true },
  cornice: true,
  roof: [MAT.ROOF_MEMBRANE, MAT.ROOF_GRAVEL],
  pitched: [MAT.ROOF_TILE_DARK, MAT.ROOF_SHINGLE],
  fireEscape: 0.45,
  balcony: 0.15,
  waterTank: 0.35,
  awning: 0.6,
  storefront: [MAT.FRAME_DARK, MAT.FRAME_WOOD],
});

STYLES.register({
  id: "plaster",
  walls: [MAT.PLASTER_CREAM, MAT.PLASTER_PEACH, MAT.PLASTER_WHITE, MAT.PLASTER_BLUE, MAT.PLASTER_GREEN, MAT.PLASTER_OCHRE, MAT.PLASTER_PINK],
  base: [MAT.CONCRETE_DARK, MAT.GRANITE, MAT.PLASTER_WHITE],
  trim: [MAT.PLASTER_WHITE, MAT.TRIM_STONE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE, MAT.FRAME_DARK],
  window: { type: "punched", width: 1.25, sill: 0.875, head: 2.375, bay: 3.25, lintel: false },
  cornice: true,
  roof: [MAT.ROOF_MEMBRANE, MAT.ROOF_GRAVEL],
  pitched: [MAT.ROOF_TILE_RED, MAT.ROOF_TILE_DARK],
  fireEscape: 0,
  balcony: 0.55,
  waterTank: 0,
  awning: 0.5,
  storefront: [MAT.FRAME_DARK, MAT.FRAME_WHITE],
});

STYLES.register({
  id: "concrete",
  walls: [MAT.CONCRETE, MAT.CONCRETE_LIGHT, MAT.CINDERBLOCK, MAT.LIMESTONE],
  base: [MAT.CONCRETE_DARK, MAT.GRANITE],
  trim: [MAT.CONCRETE_LIGHT, MAT.CONCRETE_DARK],
  glass: [MAT.GLASS, MAT.GLASS_TINT],
  frame: [MAT.FRAME_DARK, MAT.MULLION_SILVER],
  window: { type: "ribbon", width: 2.5, sill: 0.875, head: 2.625, bay: 3.0, lintel: false },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE, MAT.ROOF_GRAVEL],
  pitched: [MAT.ROOF_SHINGLE],
  fireEscape: 0,
  balcony: 0.35,
  waterTank: 0.05,
  awning: 0.25,
  storefront: [MAT.FRAME_DARK, MAT.MULLION_SILVER],
});

STYLES.register({
  id: "glass",
  walls: [MAT.METAL_PANEL, MAT.METAL_PANEL_DARK, MAT.CONCRETE_LIGHT],
  base: [MAT.GRANITE, MAT.FLOOR_MARBLE_DARK, MAT.METAL_PANEL_DARK],
  trim: [MAT.MULLION_SILVER, MAT.MULLION],
  glass: [MAT.GLASS_TINT, MAT.GLASS_GREEN, MAT.GLASS_BRONZE, MAT.GLASS],
  frame: [MAT.MULLION, MAT.MULLION_SILVER],
  window: { type: "curtain", width: 1.5, sill: 0.125, head: 3.0, bay: 1.5, lintel: false },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_SHINGLE],
  fireEscape: 0,
  balcony: 0.05,
  waterTank: 0,
  awning: 0.1,
  storefront: [MAT.MULLION, MAT.MULLION_SILVER],
});

STYLES.register({
  id: "deco",
  walls: [MAT.LIMESTONE, MAT.BRICK_YELLOW, MAT.TRIM_STONE, MAT.BRICK_BROWN],
  base: [MAT.GRANITE, MAT.FLOOR_MARBLE_DARK],
  trim: [MAT.CORNICE, MAT.TRIM_STONE],
  glass: [MAT.GLASS, MAT.GLASS_TINT],
  frame: [MAT.FRAME_DARK, MAT.MULLION],
  window: { type: "punched", width: 1.25, sill: 0.75, head: 2.5, bay: 2.25, lintel: true },
  cornice: true,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_METAL_GREEN],
  fireEscape: 0.1,
  balcony: 0.05,
  waterTank: 0.4,
  awning: 0.3,
  storefront: [MAT.FRAME_DARK, MAT.MULLION],
});

STYLES.register({
  id: "futurist",
  walls: [MAT.PANEL_WHITE, MAT.PANEL_GRAPHITE],
  base: [MAT.PANEL_GRAPHITE, MAT.FLOOR_MARBLE_DARK],
  trim: [MAT.LIGHT_STRIP, MAT.NEON_CYAN, MAT.NEON_MAGENTA],
  glass: [MAT.GLASS_TINT, MAT.GLASS_GREEN],
  frame: [MAT.PANEL_GRAPHITE, MAT.MULLION],
  window: { type: "ribbon", width: 3.0, sill: 0.5, head: 2.75, bay: 3.0, lintel: false },
  cornice: false,
  roof: [MAT.ROOF_GREEN, MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_SHINGLE],
  fireEscape: 0,
  balcony: 0.25,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.PANEL_GRAPHITE],
  accentStrips: true,
});

STYLES.register({
  id: "siding",
  walls: [MAT.SIDING_WHITE, MAT.SIDING_BLUE, MAT.SIDING_GRAY, MAT.SIDING_YELLOW, MAT.WOOD_SIDING],
  base: [MAT.CONCRETE, MAT.BRICK_RED],
  trim: [MAT.FRAME_WHITE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE],
  window: { type: "punched", width: 1.0, sill: 0.875, head: 2.25, bay: 2.75, lintel: false, shutters: 0.5 },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_SHINGLE, MAT.ROOF_TILE_DARK, MAT.ROOF_TILE_RED, MAT.ROOF_METAL_GREEN],
  fireEscape: 0,
  balcony: 0.1,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.FRAME_WHITE],
});

STYLES.register({
  id: "barn",
  walls: [MAT.BARN_RED, MAT.BARN_RED, MAT.WOOD_SIDING, MAT.WOOD_DARK],
  base: [MAT.STONE, MAT.CONCRETE],
  trim: [MAT.FRAME_WHITE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE],
  window: { type: "punched", width: 1.0, sill: 1.5, head: 2.5, bay: 5.5, lintel: false, shutters: 0 },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_METAL_GREEN, MAT.CORRUGATED_RUST, MAT.ROOF_SHINGLE, MAT.CORRUGATED],
  fireEscape: 0,
  balcony: 0,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.FRAME_WHITE],
});

/** Soviet prefab panel blocks (khrushchyovka / brezhnevka): ribbed panel joints, loggias. */
STYLES.register({
  id: "panel",
  walls: [MAT.PANEL_BEIGE, MAT.PANEL_GRAYBLUE, MAT.CONCRETE_LIGHT, MAT.PANEL_BEIGE, MAT.PLASTER_WHITE],
  joint: [MAT.PANEL_JOINT],
  base: [MAT.CONCRETE, MAT.CONCRETE_DARK],
  trim: [MAT.CONCRETE_DARK],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE, MAT.FRAME_WOOD, MAT.FRAME_DARK],
  window: { type: "punched", width: 1.5, sill: 0.875, head: 2.25, bay: 3.2, lintel: false },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE, MAT.ROOF_GRAVEL],
  pitched: [MAT.ROOF_METAL_GREEN],
  fireEscape: 0,
  balcony: 0.55,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.FRAME_DARK],
});

/** Stalinist blocks on the avenues of Soviet towns: ochre plaster, cornices, tall windows. */
STYLES.register({
  id: "stalinist",
  walls: [MAT.PLASTER_OCHRE, MAT.PLASTER_CREAM, MAT.PLASTER_PEACH, MAT.BRICK_BROWN],
  base: [MAT.GRANITE, MAT.TRIM_STONE],
  trim: [MAT.TRIM_STONE, MAT.PLASTER_WHITE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE, MAT.FRAME_WOOD],
  window: { type: "punched", width: 1.25, sill: 0.875, head: 2.75, bay: 2.9, lintel: true },
  cornice: true,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_METAL_GREEN, MAT.ROOF_TILE_DARK],
  fireEscape: 0,
  balcony: 0.3,
  waterTank: 0,
  awning: 0.2,
  storefront: [MAT.FRAME_WOOD, MAT.FRAME_DARK],
});

/** Housing projects: dark brick slabs and towers, small windows, no ornament. */
STYLES.register({
  id: "projects",
  walls: [MAT.BRICK_DARK, MAT.BRICK_BROWN, MAT.BRICK_RED, MAT.CONCRETE_DARK],
  base: [MAT.CONCRETE_DARK],
  trim: [MAT.CONCRETE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_DARK, MAT.FRAME_WHITE],
  window: { type: "punched", width: 1.1, sill: 0.875, head: 2.125, bay: 2.9, lintel: false },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE, MAT.ROOF_GRAVEL],
  pitched: [MAT.ROOF_SHINGLE],
  fireEscape: 0,
  balcony: 0.1,
  waterTank: 0.2,
  awning: 0,
  storefront: [MAT.FRAME_DARK],
});

STYLES.register({
  id: "suburbanBrick",
  walls: [MAT.BRICK_RED, MAT.BRICK_BROWN, MAT.BRICK_WHITE],
  base: [MAT.CONCRETE],
  trim: [MAT.FRAME_WHITE, MAT.TRIM_STONE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE, MAT.FRAME_DARK],
  window: { type: "punched", width: 1.0, sill: 0.875, head: 2.25, bay: 2.75, lintel: true, shutters: 0.3 },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_SHINGLE, MAT.ROOF_TILE_DARK],
  fireEscape: 0,
  balcony: 0,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.FRAME_WHITE],
});

STYLES.register({
  id: "industrial",
  walls: [MAT.CORRUGATED, MAT.CORRUGATED_BLUE, MAT.CORRUGATED_RUST, MAT.CINDERBLOCK, MAT.METAL_PANEL],
  base: [MAT.CONCRETE, MAT.CINDERBLOCK],
  trim: [MAT.CONCRETE_DARK, MAT.HAZARD_YELLOW],
  glass: [MAT.GLASS, MAT.GLASS_TINT],
  frame: [MAT.FRAME_DARK],
  window: { type: "ribbon", width: 2.0, sill: 2.0, head: 3.0, bay: 6.0, lintel: false },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE, MAT.CORRUGATED],
  pitched: [MAT.CORRUGATED, MAT.CORRUGATED_RUST],
  fireEscape: 0,
  balcony: 0,
  waterTank: 0.1,
  awning: 0,
  storefront: [MAT.FRAME_DARK],
});

STYLES.register({
  id: "adobe",
  walls: [MAT.SANDSTONE, MAT.PLASTER_OCHRE, MAT.PLASTER_CREAM, MAT.CLAY, MAT.PLASTER_WHITE],
  base: [MAT.SANDSTONE, MAT.CLAY],
  trim: [MAT.WOOD_DARK, MAT.PLASTER_WHITE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WOOD, MAT.WOOD_DARK],
  window: { type: "punched", width: 0.875, sill: 1.0, head: 2.125, bay: 2.75, lintel: true },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE, MAT.CLAY],
  pitched: [MAT.ROOF_TILE_RED],
  fireEscape: 0,
  balcony: 0.25,
  waterTank: 0.3,
  awning: 0.55,
  storefront: [MAT.FRAME_WOOD, MAT.WOOD_DARK],
});

STYLES.register({
  id: "parkingDeck",
  walls: [MAT.CONCRETE, MAT.CONCRETE_LIGHT, MAT.CONCRETE_DARK],
  base: [MAT.CONCRETE_DARK],
  trim: [MAT.CONCRETE_LIGHT, MAT.PAINT_WHITE],
  // open decks: the "glazing" of an open parking level is air
  glass: [MAT.AIR],
  frame: [MAT.CONCRETE, MAT.CONCRETE_LIGHT],
  window: { type: "open", width: 7.0, sill: 1.0, head: 2.5, bay: 7.5, lintel: false },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE, MAT.CONCRETE],
  pitched: [MAT.ROOF_SHINGLE],
  fireEscape: 0,
  balcony: 0,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.CONCRETE_DARK],
});

// --- nordic --------------------------------------------------------------------
//
// Optional style keys used by the nordic styles (absent = off, so every other
// style resolves exactly as before):
//   boards        wall materials get a darker seam every 3 voxels (SEAMS):
//                 vertical board joints, or horizontal grooves for logs
//   corners       corner boards / quoins in the trim material
//   shopBase      false: storefront ground floors keep the wall above the plinth
//   baseH         height (voxels) of the base material at the foot of the
//                 ground floor facade (default 6)
//   window.casing a 1-voxel trim surround beside every punched window
//   window.transom a horizontal glazing bar (with the mullion: a window cross)

/** Seam shade + direction of each board / log wall material. */
export const SEAMS = new Map([
  [MAT.CLAD_FALU, { m: MAT.CLAD_FALU_SEAM, dir: "v" }],
  [MAT.CLAD_OCHRE, { m: MAT.CLAD_OCHRE_SEAM, dir: "v" }],
  [MAT.CLAD_WHITE, { m: MAT.CLAD_WHITE_SEAM, dir: "v" }],
  [MAT.CLAD_GREYBLUE, { m: MAT.CLAD_GREYBLUE_SEAM, dir: "v" }],
  [MAT.CLAD_GREEN, { m: MAT.CLAD_GREEN_SEAM, dir: "v" }],
  [MAT.LOG_TARRED, { m: MAT.LOG_TARRED_SEAM, dir: "h" }],
]);

/**
 * Painted wooden town houses of an old northern town centre (Røros,
 * Trondheim, Karelian towns): vertical-board cladding in falu red, ochre,
 * white, grey-blue or green with white corner boards and window casings,
 * small cross windows on a narrow bay, steep dark roofs, simple wooden shop
 * windows; no fire escapes, water tanks or awnings to speak of.
 */
STYLES.register({
  id: "nordicWood",
  walls: [MAT.CLAD_FALU, MAT.CLAD_FALU, MAT.CLAD_OCHRE, MAT.CLAD_WHITE, MAT.CLAD_WHITE, MAT.CLAD_GREYBLUE, MAT.CLAD_GREEN],
  base: [MAT.GRANITE, MAT.GRANITE_LIGHT, MAT.STONE],
  trim: [MAT.FRAME_WHITE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE],
  window: { type: "punched", width: 1.0, sill: 0.75, head: 2.25, bay: 2.5, lintel: true, casing: true, transom: true },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_BLACK_TILE, MAT.ROOF_BLACK_METAL, MAT.ROOF_TILE_DARK, MAT.ROOF_GREEN_DARK, MAT.ROOF_SLATE],
  fireEscape: 0,
  balcony: 0.08,
  waterTank: 0,
  awning: 0.1,
  storefront: [MAT.FRAME_WHITE, MAT.FRAME_WOOD],
  boards: true,
  corners: true,
  shopBase: false,
});

/**
 * Rendered stone merchant houses and civic buildings of the same towns: pale
 * yellow, white, grey or ochre render over a granite plinth, tall punched
 * windows with white casings and lintels, pitched metal or slate roofs.
 */
STYLES.register({
  id: "nordicPlaster",
  walls: [MAT.RENDER_YELLOW, MAT.RENDER_WHITE, MAT.RENDER_GREY, MAT.RENDER_OCHRE, MAT.RENDER_YELLOW],
  base: [MAT.GRANITE, MAT.GRANITE_LIGHT],
  trim: [MAT.PLASTER_WHITE, MAT.TRIM_STONE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE, MAT.FRAME_WOOD],
  window: { type: "punched", width: 1.125, sill: 0.875, head: 2.5, bay: 2.75, lintel: true, casing: true },
  cornice: true,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_BLACK_METAL, MAT.ROOF_SLATE, MAT.ROOF_TILE_DARK, MAT.ROOF_GREEN_DARK],
  fireEscape: 0,
  balcony: 0.05,
  waterTank: 0,
  awning: 0.3,
  storefront: [MAT.FRAME_WOOD, MAT.FRAME_DARK, MAT.FRAME_WHITE],
  corners: true,
});

/** Forest cabin (hytte): tarred logs or falu-red boards, white frames, dark or sod roof. */
STYLES.register({
  id: "cabin",
  walls: [MAT.LOG_TARRED, MAT.LOG_TARRED, MAT.CLAD_FALU, MAT.CLAD_FALU, MAT.CLAD_GREEN],
  base: [MAT.STONE, MAT.GRANITE],
  trim: [MAT.FRAME_WHITE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE],
  window: { type: "punched", width: 1.0, sill: 0.875, head: 2.0, bay: 2.25, lintel: true, casing: true, transom: true },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_BLACK_METAL, MAT.ROOF_BLACK_TILE, MAT.ROOF_GREEN_DARK, MAT.ROOF_SOD],
  fireEscape: 0,
  balcony: 0,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.FRAME_WHITE],
  boards: true,
  corners: true,
  shopBase: false,
  // the cabin sits on its own stone plinth: walls start at the floor
  baseH: 2,
});

/** Wooden church (Norwegian long church): white or falu-red boards, tall cross windows, dark roofs. */
STYLES.register({
  id: "nordicChurch",
  walls: [MAT.CLAD_WHITE, MAT.CLAD_WHITE, MAT.CLAD_WHITE, MAT.CLAD_FALU, MAT.CLAD_OCHRE],
  base: [MAT.GRANITE, MAT.GRANITE_LIGHT],
  trim: [MAT.FRAME_WHITE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE],
  window: { type: "punched", width: 1.125, sill: 1.5, head: 4.25, bay: 3.25, lintel: true, casing: true, transom: true },
  cornice: false,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_BLACK_TILE, MAT.ROOF_SLATE, MAT.ROOF_BLACK_METAL, MAT.ROOF_GREEN_DARK],
  fireEscape: 0,
  balcony: 0,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.FRAME_WHITE],
  boards: true,
  corners: true,
  shopBase: false,
});

/** Resolve a style into concrete materials for one building. */
export function resolveStyle(styleId, rng) {
  const s = STYLES.get(styleId);
  const wall = rng.pick(s.walls);
  const seam = s.boards ? SEAMS.get(wall) ?? null : null;
  return {
    id: s.id,
    wall,
    base: rng.pick(s.base),
    trim: rng.pick(s.trim),
    glass: rng.pick(s.glass),
    frame: rng.pick(s.frame),
    window: s.window,
    cornice: s.cornice,
    roof: rng.pick(s.roof),
    pitched: rng.pick(s.pitched),
    storefront: rng.pick(s.storefront),
    fireEscape: rng.chance(s.fireEscape),
    balcony: rng.chance(s.balcony),
    waterTank: rng.chance(s.waterTank),
    awning: s.awning,
    shutters: s.window.shutters ? rng.chance(s.window.shutters) : false,
    accentStrips: !!s.accentStrips,
    // prefab panels: joints at every panel edge (bay) and above every slab
    joint: s.joint ? rng.pick(s.joint) : null,
    // nordic extras (no rng draws, so other styles resolve unchanged)
    seam: seam ? seam.m : null,
    seamDir: seam ? seam.dir : null,
    corners: !!s.corners,
    shopBase: s.shopBase ?? true,
    baseH: s.baseH ?? 6,
  };
}
