/**
 * Voxel material palette. Material ids are Uint16 (0 = air), capped at
 * MAX_MATERIALS so lookup tables stay small. Each entry
 * carries a base color plus rendering flags; the renderer adds per-voxel
 * value noise (scaled by `noise`) so large greedy-meshed faces still read as
 * individual voxels.
 *
 * Flags:
 *   transparent – rendered in the blended pass (glass, water)
 *   emissive    – 0..1 self-illumination (lamps, screens, neon)
 *   solid       – collides with the walker (water / leaves-free air are not)
 *   climb       – the walker can climb it (ladders)
 *   glow        – window glass: some panes light up at night (renderer)
 */

export const MAX_MATERIALS = 1024;
export const MAT = Object.create(null);
export const MATERIALS = [];

function def(name, hex, opts = {}) {
  const id = MATERIALS.length;
  if (id >= MAX_MATERIALS) throw new Error("Material palette overflow");
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  MATERIALS.push({
    id,
    name,
    rgb: [r, g, b],
    noise: opts.noise ?? 0.06,
    transparent: opts.transparent ?? false,
    opacity: opts.opacity ?? 1,
    emissive: opts.emissive ?? 0,
    solid: opts.solid ?? true,
    climb: opts.climb ?? false,
    glow: opts.glow ?? false,
  });
  MAT[name] = id;
  return id;
}

def("AIR", "#000000", { solid: false, noise: 0 });

// --- terrain -------------------------------------------------------------
def("STONE", "#7d7b76", { noise: 0.1 });
def("DIRT", "#7a5a3c", { noise: 0.1 });
def("GRASS", "#5f8f3e", { noise: 0.12 });
def("GRASS_DRY", "#8e9a4e", { noise: 0.12 });
def("GRASS_LAWN", "#6a9e45", { noise: 0.08 });
def("FOREST_FLOOR", "#4f5a2c", { noise: 0.14 });
def("SAND", "#d6c28f", { noise: 0.07 });
def("GRAVEL", "#8f8a82", { noise: 0.14 });
def("ROCK", "#6f6d69", { noise: 0.12 });
def("SNOW", "#eef2f5", { noise: 0.03 });
def("CLAY", "#9c6a4a", { noise: 0.08 });
def("MUD", "#5b4631", { noise: 0.08 });
def("BEDROCK", "#4a4845", { noise: 0.1 });
def("SAND_DUNE", "#e2c98f", { noise: 0.05 });
def("SANDSTONE", "#c08a5a", { noise: 0.1 });
def("RED_EARTH", "#a0603a", { noise: 0.1 });
def("SAVANNA_GRASS", "#b3a45a", { noise: 0.14 });
def("MOSS", "#6a7a4a", { noise: 0.14 });
def("TUNDRA", "#8a8f74", { noise: 0.14 });
def("MARSH_GRASS", "#5f7f45", { noise: 0.14 });
def("JUNGLE_FLOOR", "#3f4f26", { noise: 0.14 });
def("WATER", "#3a6f8f", { transparent: true, opacity: 0.72, solid: false, noise: 0.02 });

// --- roads & paving ------------------------------------------------------
def("ASPHALT", "#3d3f42", { noise: 0.07 });
def("ASPHALT_WORN", "#4a4c4f", { noise: 0.08 });
def("ASPHALT_PATCH", "#333538", { noise: 0.05 });
def("LINE_WHITE", "#dcdcd4", { noise: 0.04 });
def("LINE_YELLOW", "#d8b440", { noise: 0.04 });
def("CURB", "#a3a19a", { noise: 0.05 });
def("SIDEWALK", "#aeaaa2", { noise: 0.05 });
def("SIDEWALK_JOINT", "#98948c", { noise: 0.04 });
def("PAVER_RED", "#9a5f4c", { noise: 0.09 });
def("PAVER_GRAY", "#8b8983", { noise: 0.08 });
def("PLAZA_STONE", "#c2b9a6", { noise: 0.06 });
def("PLAZA_STONE_DARK", "#8e8676", { noise: 0.06 });
def("MANHOLE", "#4d4a45", { noise: 0.03 });
def("DRAIN_GRATE", "#2e2e2e", { noise: 0.02 });
def("TACTILE", "#d9b53f", { noise: 0.03 });
def("PARK_PATH", "#b8a585", { noise: 0.1 });
def("TREE_PIT", "#5a4632", { noise: 0.1 });

// --- structure / facades ------------------------------------------------
def("CONCRETE", "#9d9b96", { noise: 0.05 });
def("CONCRETE_DARK", "#6f6e6b", { noise: 0.05 });
def("CONCRETE_LIGHT", "#c3c0b8", { noise: 0.04 });
def("BRICK_RED", "#9a4e3a", { noise: 0.12 });
def("BRICK_BROWN", "#7a5040", { noise: 0.12 });
def("BRICK_YELLOW", "#c9a86e", { noise: 0.1 });
def("BRICK_DARK", "#57393a", { noise: 0.1 });
def("BRICK_WHITE", "#d8d2c6", { noise: 0.08 });
def("LIMESTONE", "#cfc4a8", { noise: 0.06 });
def("GRANITE", "#77726e", { noise: 0.08 });
def("PLASTER_WHITE", "#e6e2d8", { noise: 0.03 });
def("PLASTER_CREAM", "#e3d4ae", { noise: 0.03 });
def("PLASTER_PEACH", "#e0b394", { noise: 0.03 });
def("PLASTER_BLUE", "#9fb6c6", { noise: 0.03 });
def("PLASTER_GREEN", "#a9bd9a", { noise: 0.03 });
def("PLASTER_OCHRE", "#d0a257", { noise: 0.03 });
def("PLASTER_PINK", "#d9a1a0", { noise: 0.03 });
def("METAL_PANEL", "#a6adb3", { noise: 0.03 });
def("METAL_PANEL_DARK", "#4a5058", { noise: 0.03 });
def("CORRUGATED", "#8a939a", { noise: 0.06 });
def("CORRUGATED_RUST", "#8a5b3b", { noise: 0.1 });
def("CORRUGATED_BLUE", "#4f6f8f", { noise: 0.05 });
def("CINDERBLOCK", "#a39f96", { noise: 0.08 });
def("WOOD_SIDING", "#b58a5c", { noise: 0.08 });
def("SIDING_WHITE", "#e9e6de", { noise: 0.03 });
def("SIDING_BLUE", "#7a95ab", { noise: 0.04 });
def("SIDING_GRAY", "#9a9d9c", { noise: 0.04 });
def("SIDING_YELLOW", "#dcc27c", { noise: 0.04 });
def("GLASS", "#a8c8d8", { transparent: true, opacity: 0.28, noise: 0, glow: true });
def("GLASS_TINT", "#4f6f86", { transparent: true, opacity: 0.55, noise: 0, glow: true });
def("GLASS_GREEN", "#6e9a92", { transparent: true, opacity: 0.5, noise: 0, glow: true });
def("GLASS_BRONZE", "#8a7358", { transparent: true, opacity: 0.55, noise: 0, glow: true });
def("MULLION", "#3a3f45", { noise: 0.02 });
def("MULLION_SILVER", "#b4b9bd", { noise: 0.02 });
def("FRAME_WHITE", "#ecebe6", { noise: 0.02 });
def("FRAME_DARK", "#2f3134", { noise: 0.02 });
def("FRAME_WOOD", "#6b4a2e", { noise: 0.04 });
def("TRIM_STONE", "#d4cdbd", { noise: 0.04 });
def("CORNICE", "#bdb6a6", { noise: 0.04 });
def("PANEL_WHITE", "#eef0f2", { noise: 0.015 });
def("PANEL_GRAPHITE", "#2b2f36", { noise: 0.02 });
def("NEON_CYAN", "#4ff0ff", { emissive: 1, noise: 0 });
def("NEON_MAGENTA", "#ff4fd8", { emissive: 1, noise: 0 });
def("LIGHT_STRIP", "#fff3d6", { emissive: 0.9, noise: 0 });

// --- roofs ---------------------------------------------------------------
def("ROOF_MEMBRANE", "#5e6064", { noise: 0.05 });
def("ROOF_GRAVEL", "#8a857c", { noise: 0.12 });
def("ROOF_TILE_RED", "#a64f35", { noise: 0.1 });
def("ROOF_TILE_DARK", "#4b4441", { noise: 0.08 });
def("ROOF_SHINGLE", "#5f6164", { noise: 0.1 });
def("ROOF_METAL_GREEN", "#4f7a60", { noise: 0.04 });
def("ROOF_GREEN", "#6c8a3a", { noise: 0.14 });
def("PARAPET_CAP", "#b0aca3", { noise: 0.03 });

// --- interior walls ------------------------------------------------------
def("PAINT_WHITE", "#ecebe5", { noise: 0.015 });
def("PAINT_CREAM", "#ece0c4", { noise: 0.015 });
def("PAINT_GRAY", "#c9cac8", { noise: 0.015 });
def("PAINT_SAGE", "#b9c6ae", { noise: 0.015 });
def("PAINT_BLUE", "#b3c6d6", { noise: 0.015 });
def("PAINT_PEACH", "#ecc9ae", { noise: 0.015 });
def("PAINT_TERRACOTTA", "#c98c6e", { noise: 0.015 });
def("PAINT_MINT", "#c3e2d2", { noise: 0.015 });
def("PAINT_LAVENDER", "#cfc4dd", { noise: 0.015 });
def("PAINT_DARK", "#4e5359", { noise: 0.015 });
def("WALL_TILE_WHITE", "#f2f2ef", { noise: 0.02 });
def("WALL_TILE_BLUE", "#8fb2c9", { noise: 0.02 });
def("WALL_TILE_GREEN", "#9cc2a5", { noise: 0.02 });
def("WOOD_PANEL", "#8a6440", { noise: 0.06 });
def("BASEBOARD", "#f4f3ef", { noise: 0.01 });

// --- floors ---------------------------------------------------------------
def("FLOOR_OAK", "#b98c5a", { noise: 0.07 });
def("FLOOR_WALNUT", "#6d4a31", { noise: 0.07 });
def("FLOOR_PARQUET", "#a3764a", { noise: 0.09 });
def("FLOOR_TILE_WHITE", "#dcdad4", { noise: 0.03 });
def("FLOOR_TILE_GRAY", "#9a9a97", { noise: 0.03 });
def("FLOOR_TILE_DARK", "#4e4f52", { noise: 0.03 });
def("FLOOR_TILE_TERRA", "#b56f4f", { noise: 0.05 });
def("FLOOR_CARPET_GRAY", "#7d7f84", { noise: 0.04 });
def("FLOOR_CARPET_BLUE", "#4f5f7d", { noise: 0.04 });
def("FLOOR_CARPET_RED", "#7e3e3a", { noise: 0.04 });
def("FLOOR_CARPET_BEIGE", "#b8a88c", { noise: 0.04 });
def("FLOOR_CONCRETE", "#8d8b86", { noise: 0.05 });
def("FLOOR_EPOXY", "#7b8a86", { noise: 0.02 });
def("FLOOR_MARBLE", "#e1ddd3", { noise: 0.04 });
def("FLOOR_MARBLE_DARK", "#3b3c40", { noise: 0.04 });
def("FLOOR_LINOLEUM", "#a8b19b", { noise: 0.03 });
def("FLOOR_TERRAZZO", "#c9c2b5", { noise: 0.045 });
def("CEILING", "#f0efea", { noise: 0.01 });
def("CEILING_TILE", "#e4e3dd", { noise: 0.02 });
def("CEILING_LIGHT", "#fff8e8", { emissive: 1, noise: 0 });

// --- doors / stairs --------------------------------------------------------
def("DOOR_WOOD", "#7a5334", { noise: 0.04 });
def("DOOR_WHITE", "#f1f0ea", { noise: 0.01 });
def("DOOR_METAL", "#6c737a", { noise: 0.02 });
def("DOOR_RED", "#8e2f2a", { noise: 0.02 });
def("DOOR_GREEN", "#2f5e45", { noise: 0.02 });
def("DOOR_GLASS", "#a8c8d8", { transparent: true, opacity: 0.3, noise: 0 });
def("DOOR_FRAME", "#e9e7e0", { noise: 0.01 });
def("ROLLUP_DOOR", "#9ea4a8", { noise: 0.05 });
def("STAIR_CONCRETE", "#a9a69f", { noise: 0.04 });
def("STAIR_WOOD", "#9c7248", { noise: 0.06 });
def("STAIR_NOSING", "#5a5854", { noise: 0.02 });
def("RAILING", "#3c3f43", { noise: 0.02 });
def("HANDRAIL_WOOD", "#7a5334", { noise: 0.03 });
def("ELEVATOR_DOOR", "#b9bec2", { noise: 0.01 });

// --- furniture ------------------------------------------------------------
def("FABRIC_GRAY", "#6f7275", { noise: 0.04 });
def("FABRIC_BLUE", "#3f5a7d", { noise: 0.04 });
def("FABRIC_RED", "#8e3a3a", { noise: 0.04 });
def("FABRIC_GREEN", "#4c6b4c", { noise: 0.04 });
def("FABRIC_BEIGE", "#c7b595", { noise: 0.04 });
def("FABRIC_MUSTARD", "#c19a3a", { noise: 0.04 });
def("LEATHER", "#5e3d27", { noise: 0.04 });
def("WOOD_LIGHT", "#c9a67a", { noise: 0.05 });
def("WOOD_MED", "#94683f", { noise: 0.05 });
def("WOOD_DARK", "#553724", { noise: 0.05 });
def("LAMINATE_WHITE", "#f0efeb", { noise: 0.01 });
def("LAMINATE_GRAY", "#9ea2a6", { noise: 0.01 });
def("METAL_CHROME", "#c8ccd0", { noise: 0.02 });
def("METAL_BLACK", "#26282b", { noise: 0.02 });
def("PLASTIC_WHITE", "#f3f3f0", { noise: 0.01 });
def("PLASTIC_BLACK", "#1f2023", { noise: 0.01 });
def("CERAMIC", "#f7f7f5", { noise: 0.01 });
def("MATTRESS", "#f1eee6", { noise: 0.02 });
def("BEDSHEET_BLUE", "#6f8fb3", { noise: 0.03 });
def("BEDSHEET_GREEN", "#8aa586", { noise: 0.03 });
def("BEDSHEET_WHITE", "#e8e6df", { noise: 0.03 });
def("PILLOW", "#f7f4ea", { noise: 0.02 });
def("SCREEN", "#1d3b57", { emissive: 0.55, noise: 0 });
def("SCREEN_OFF", "#141619", { noise: 0 });
def("COUNTERTOP", "#d7d3cb", { noise: 0.03 });
def("COUNTERTOP_DARK", "#353536", { noise: 0.03 });
def("APPLIANCE_STEEL", "#b7bcc0", { noise: 0.02 });
def("BOOKS", "#8d5a3b", { noise: 0.35 });
def("PLANT", "#3f7a3a", { noise: 0.2 });
def("PLANT_POT", "#9a5a3e", { noise: 0.04 });
def("RUG_RED", "#8b3e36", { noise: 0.08 });
def("RUG_BLUE", "#3e557a", { noise: 0.08 });
def("RUG_BEIGE", "#cbb892", { noise: 0.08 });
def("CARDBOARD", "#b48a5a", { noise: 0.08 });
def("PALLET", "#a88a5e", { noise: 0.08 });
def("SHELF_METAL", "#5d7f9c", { noise: 0.03 });
def("SHELF_ORANGE", "#d0762f", { noise: 0.03 });
def("GOODS", "#b07a54", { noise: 0.4 });
def("MIRROR", "#cfdde6", { noise: 0 });
def("LAMP_SHADE", "#f3e3bd", { emissive: 0.6, noise: 0 });

// --- street props / vehicles ---------------------------------------------
def("POLE_METAL", "#3b4045", { noise: 0.02 });
def("POLE_GREEN", "#2f4a3c", { noise: 0.02 });
def("LAMP_LIGHT", "#fff1c9", { emissive: 1, noise: 0 });
def("SIGNAL_BOX", "#2a2c2e", { noise: 0.02 });
def("SIGNAL_RED", "#ff3a2f", { emissive: 1, noise: 0 });
def("SIGNAL_GREEN", "#3aff7a", { emissive: 1, noise: 0 });
def("SIGNAL_AMBER", "#ffb22f", { emissive: 0.8, noise: 0 });
def("HYDRANT", "#b3322b", { noise: 0.03 });
def("BIN_GREEN", "#2f5b3f", { noise: 0.03 });
def("SIGN_GREEN", "#2a6e45", { noise: 0.02 });
def("SIGN_WHITE", "#f0f0ea", { noise: 0.01 });
def("SIGN_BLUE", "#2d5aa0", { noise: 0.02 });
def("SIGN_RED", "#c0302a", { noise: 0.02 });
def("BOLLARD", "#55595d", { noise: 0.03 });
def("CAR_RED", "#a8282a", { noise: 0.02 });
def("CAR_BLUE", "#2a4f8e", { noise: 0.02 });
def("CAR_WHITE", "#e9e9e6", { noise: 0.02 });
def("CAR_BLACK", "#1d1f22", { noise: 0.02 });
def("CAR_SILVER", "#a9aeb3", { noise: 0.02 });
def("CAR_YELLOW", "#e2b52c", { noise: 0.02 });
def("CAR_GREEN", "#35604a", { noise: 0.02 });
def("TIRE", "#151515", { noise: 0.02 });
def("CAR_GLASS", "#2c3e4c", { transparent: true, opacity: 0.7, noise: 0 });
def("HEADLIGHT", "#fdf6dc", { emissive: 0.4, noise: 0 });
def("TAILLIGHT", "#b3261e", { emissive: 0.3, noise: 0 });

// --- nature ---------------------------------------------------------------
def("BARK", "#5b4331", { noise: 0.1 });
def("BARK_BIRCH", "#d8d4c9", { noise: 0.12 });
def("LEAVES", "#3e7534", { noise: 0.16 });
def("LEAVES_LIGHT", "#6c9a3c", { noise: 0.16 });
def("LEAVES_DARK", "#2c5a2c", { noise: 0.14 });
def("LEAVES_PINE", "#284a33", { noise: 0.12 });
def("LEAVES_AUTUMN", "#c0702c", { noise: 0.2 });
def("LEAVES_BLOSSOM", "#e3a3b8", { noise: 0.14 });
def("HEDGE", "#3a6a31", { noise: 0.14 });
def("BUSH", "#4d7a36", { noise: 0.16 });
def("FLOWER_RED", "#c43a3a", { noise: 0.1 });
def("FLOWER_YELLOW", "#e6c43a", { noise: 0.1 });
def("FLOWER_PURPLE", "#8a4fb0", { noise: 0.1 });
def("FLOWER_WHITE", "#f1efe8", { noise: 0.05 });
def("REED", "#8a9a4a", { noise: 0.14 });
def("CACTUS", "#4f7a3a", { noise: 0.08 });
def("LEAVES_ACACIA", "#6b7d34", { noise: 0.16 });
def("LEAVES_JUNGLE", "#2f6a2a", { noise: 0.16 });
def("PALM_FROND", "#5a8a36", { noise: 0.12 });
def("BARK_PALM", "#8a7358", { noise: 0.1 });
def("SHRUB_DRY", "#8a7f4a", { noise: 0.16 });

// --- infrastructure --------------------------------------------------------
def("HW_CONCRETE", "#a7a49c", { noise: 0.05 });
def("HW_BARRIER", "#c5c2ba", { noise: 0.03 });
def("STEEL_BEAM", "#4e5963", { noise: 0.03 });
def("STEEL_RUST", "#7a4f36", { noise: 0.08 });
def("TUNNEL_WALL", "#8f8b83", { noise: 0.06 });
def("TUNNEL_TILE", "#e8e4d8", { noise: 0.03 });
def("TUNNEL_TILE_ACCENT", "#3d6f9e", { noise: 0.02 });
def("RAIL_STEEL", "#8a8d90", { noise: 0.02 });
def("RAIL_TIE", "#5e5145", { noise: 0.06 });
def("BALLAST", "#6e6a64", { noise: 0.16 });
def("PLATFORM_EDGE", "#e0c03a", { noise: 0.03 });
def("CHAINLINK", "#9aa1a6", { noise: 0.03 });
def("FENCE_WOOD", "#9a7650", { noise: 0.06 });
def("FENCE_WHITE", "#efeee8", { noise: 0.02 });
def("CONTAINER_RED", "#a3392b", { noise: 0.05 });
def("CONTAINER_BLUE", "#2f5d8a", { noise: 0.05 });
def("CONTAINER_GREEN", "#3f6f45", { noise: 0.05 });
def("CONTAINER_ORANGE", "#c9702a", { noise: 0.05 });
def("HAZARD_YELLOW", "#e3b92c", { noise: 0.03 });
def("HAZARD_BLACK", "#1f1f1f", { noise: 0.02 });
def("TANK_WHITE", "#dcdad3", { noise: 0.03 });
def("PIPE", "#79858d", { noise: 0.03 });
def("AC_UNIT", "#b7bab8", { noise: 0.03 });
def("VENT", "#6f7479", { noise: 0.03 });
def("SOLAR_PANEL", "#1f3350", { noise: 0.02 });
def("WATER_TANK_WOOD", "#7d5a3b", { noise: 0.07 });
def("AWNING_RED", "#a8302f", { noise: 0.03 });
def("AWNING_GREEN", "#2f6b48", { noise: 0.03 });
def("AWNING_BLUE", "#2f4f8a", { noise: 0.03 });
def("AWNING_STRIPE", "#efe9da", { noise: 0.02 });
def("AWNING_BLACK", "#26282b", { noise: 0.02 });
def("SIGNAGE", "#f4d35e", { emissive: 0.7, noise: 0 });
def("SIGNAGE_RED", "#ff5a4a", { emissive: 0.7, noise: 0 });
def("SIGNAGE_BLUE", "#5ab0ff", { emissive: 0.7, noise: 0 });
def("SAND_BOX", "#e2cf9a", { noise: 0.06 });
def("PLAY_RED", "#d0453b", { noise: 0.02 });
def("PLAY_YELLOW", "#e8c23a", { noise: 0.02 });
def("PLAY_BLUE", "#3b74c9", { noise: 0.02 });
def("RUBBER_MAT", "#7f3b36", { noise: 0.05 });
def("SPORT_COURT", "#4f7d9a", { noise: 0.02 });
def("SPORT_COURT_GREEN", "#4a8a56", { noise: 0.02 });

// --- underground utilities ---------------------------------------------------
def("SEWER_BRICK", "#6b4a3c", { noise: 0.14 });
def("SEWER_BRICK_DARK", "#4d3a33", { noise: 0.12 });
def("SEWER_FLOOR", "#5f5c55", { noise: 0.1 });
def("SEWER_CURB", "#7c7870", { noise: 0.06 });
def("SEWER_WATER", "#5f6f48", { transparent: true, opacity: 0.85, solid: false, noise: 0.05 });
def("SLUDGE", "#5a5640", { noise: 0.1 });
def("LADDER", "#8a8f93", { noise: 0.02, climb: true });
def("MANHOLE_RIM", "#2b2a28", { noise: 0.03 });
def("CONE_ORANGE", "#e8641e", { noise: 0.02 });
def("CONE_WHITE", "#eeeeea", { noise: 0.01 });
def("LAMP_CAGE", "#ffd98a", { emissive: 0.85, noise: 0 });
def("VALVE_RED", "#a3322a", { noise: 0.03 });
def("GRATE_STEEL", "#4a4f52", { noise: 0.05 });
def("NUKAGE", "#62d23a", { transparent: true, opacity: 0.85, emissive: 0.7, solid: false, noise: 0.06 });
def("EMERGENCY_RED", "#ff3a2a", { emissive: 0.9, noise: 0 });
def("CHALKBOARD", "#2f4a3a", { noise: 0.04 });
def("COURT_ORANGE", "#c8643a", { noise: 0.02 });
def("CAVE_FLOOR", "#5d5850", { noise: 0.12 });
def("CAVE_MOSS", "#4a5a3a", { noise: 0.14 });
def("CRYSTAL_CYAN", "#6fe8ff", { emissive: 0.8, noise: 0.05 });
def("CRYSTAL_VIOLET", "#b58cff", { emissive: 0.75, noise: 0.05 });
def("ICE", "#cfe6f2", { noise: 0.04 });
def("BARN_RED", "#8c3325", { noise: 0.1 });
def("SNOW_WIND", "#dbe3ec", { noise: 0.05 });
def("ROCK_DARK", "#5f5c58", { noise: 0.12 });
def("ROCK_LIGHT", "#9d968c", { noise: 0.1 });
def("HAY", "#d4b45c", { noise: 0.16 });
def("SILO_STEEL", "#b8bcbf", { noise: 0.05 });
def("PANEL_BEIGE", "#bdb4a2", { noise: 0.05 });
def("PANEL_GRAYBLUE", "#9aa4ab", { noise: 0.05 });
def("PANEL_JOINT", "#6d6a64", { noise: 0.03 });

// --- nordic architecture ---------------------------------------------------
// Painted vertical-board cladding of Norwegian / Swedish / Karelian wooden
// towns, slightly desaturated for the low northern light; every colour has a
// darker *_SEAM shade for the board joints (buildings/styles.js SEAMS).
def("CLAD_FALU", "#7c352b", { noise: 0.07 });
def("CLAD_FALU_SEAM", "#62291f", { noise: 0.06 });
def("CLAD_OCHRE", "#c29a55", { noise: 0.06 });
def("CLAD_OCHRE_SEAM", "#9f7d44", { noise: 0.05 });
def("CLAD_WHITE", "#dddbd3", { noise: 0.04 });
def("CLAD_WHITE_SEAM", "#bdbcb4", { noise: 0.04 });
def("CLAD_GREYBLUE", "#8a9aa3", { noise: 0.05 });
def("CLAD_GREYBLUE_SEAM", "#6f7e87", { noise: 0.05 });
def("CLAD_GREEN", "#44574a", { noise: 0.06 });
def("CLAD_GREEN_SEAM", "#33433a", { noise: 0.05 });
// dark tarred log walls (cabins), the seam is the shadowed groove between logs
def("LOG_TARRED", "#43352b", { noise: 0.1 });
def("LOG_TARRED_SEAM", "#2c231d", { noise: 0.08 });
// roofs: black sheet metal, black glazed tile, slate, weathered green metal, sod
def("ROOF_BLACK_METAL", "#2c2e31", { noise: 0.04 });
def("ROOF_BLACK_TILE", "#35312f", { noise: 0.08 });
def("ROOF_SLATE", "#595e62", { noise: 0.09 });
def("ROOF_GREEN_DARK", "#435c4f", { noise: 0.05 });
def("ROOF_SOD", "#5d6440", { noise: 0.16 });
// rendered stone of northern merchant / civic buildings, granite plinths
def("RENDER_YELLOW", "#d6c794", { noise: 0.03 });
def("RENDER_WHITE", "#dedcd3", { noise: 0.03 });
def("RENDER_GREY", "#b5b6b1", { noise: 0.03 });
def("RENDER_OCHRE", "#c29d64", { noise: 0.03 });
def("GRANITE_LIGHT", "#8d8b86", { noise: 0.09 });
// unpainted weathered boards (porches, steps), interior pine panelling
def("WOOD_WEATHERED", "#7d7466", { noise: 0.08 });
def("PINE_PANEL", "#c6a172", { noise: 0.05 });

// --- seasons (world/season.js) ----------------------------------------------
// ground: fresh spring grass, autumn and winter grass, leaf litter, heather,
// dwarf shrubs turning red on the fells in autumn
def("GRASS_SPRING", "#7aa845", { noise: 0.12 });
def("GRASS_AUTUMN", "#958f4a", { noise: 0.13 });
def("GRASS_STRAW", "#ad9c68", { noise: 0.12 });
def("GRASS_DEAD", "#7b7657", { noise: 0.12 });
def("LAWN_AUTUMN", "#7a9044", { noise: 0.09 });
def("LAWN_WINTER", "#6c7a50", { noise: 0.08 });
def("LEAF_LITTER", "#86592f", { noise: 0.16 });
def("HEATHER", "#6e4b5e", { noise: 0.14 });
def("HEATHER_BLOOM", "#93588a", { noise: 0.14 });
def("TUNDRA_AUTUMN", "#8f5638", { noise: 0.16 });
def("MARSH_AUTUMN", "#a0824c", { noise: 0.14 });
// foliage: spring green, autumn yellows, oranges, reds and browns, bare twigs
// (distant canopy), dark spruce needles, rowan berries
def("LEAVES_SPRING", "#8dbd4c", { noise: 0.16 });
def("LEAVES_YELLOW", "#d4ae36", { noise: 0.18 });
def("LEAVES_RED", "#a3352b", { noise: 0.18 });
def("LEAVES_BROWN", "#88603a", { noise: 0.16 });
def("TWIGS", "#6a5d53", { noise: 0.14 });
def("LEAVES_SPRUCE", "#1f3b2b", { noise: 0.12 });
def("BERRY_RED", "#b72e26", { noise: 0.1 });
// wild flowers: lupins, pink campion / clover
def("FLOWER_LUPIN", "#7a60b6", { noise: 0.12 });
def("FLOWER_PINK", "#d4829f", { noise: 0.1 });

// --- old towns ---------------------------------------------------------------
// cobbled lanes and squares, flagstone pavements, a darker stone for gutters
def("COBBLE", "#76716a", { noise: 0.16 });
def("COBBLE_DARK", "#5d5953", { noise: 0.14 });
def("COBBLE_LIGHT", "#928c82", { noise: 0.14 });
def("FLAGSTONE", "#a19b90", { noise: 0.07 });
// granite of outcrops and coastal rock slabs (svaberg), grey lichen on it
def("GRANITE_PINK", "#8f7f78", { noise: 0.1 });
def("LICHEN", "#9da08a", { noise: 0.14 });
// vegetable beds of allotments, tarred boathouse boards, red-white lighthouse paint
def("SOIL_BED", "#5e4430", { noise: 0.12 });
def("PAINT_RED", "#b33a2e", { noise: 0.03 });

// --- vegetation (nature/trees.js, nature/groundcover.js) ------------------------
// foliage shades: deep shadowed green, fresh birch green, silvery willow, pale pine
def("LEAVES_DEEP", "#24462a", { noise: 0.14 });
def("LEAVES_BIRCH", "#7ea246", { noise: 0.16 });
def("LEAVES_WILLOW", "#8ea86c", { noise: 0.14 });
def("LEAVES_PINE_LIGHT", "#3b5e3d", { noise: 0.12 });
// bark: the orange upper trunk of a Scots pine, smooth grey beech, dark marks on birch, grey deadwood
def("BARK_PINE", "#9b5d3b", { noise: 0.12 });
def("BARK_GREY", "#7b7870", { noise: 0.08 });
def("BARK_BIRCH_MARK", "#34302c", { noise: 0.1 });
def("DEADWOOD", "#8d867a", { noise: 0.12 });
// forest ground: brown needle litter under conifers, a darker lush grass, meadow grass blades, ferns, blueberry
def("NEEDLE_LITTER", "#6b5237", { noise: 0.14 });
def("GRASS_DARK", "#4f7d35", { noise: 0.12 });
def("GRASS_TALL", "#6f9a42", { noise: 0.16 });
def("FERN", "#4f8237", { noise: 0.16 });
def("BLUEBERRY", "#3b5b3a", { noise: 0.14 });

// --- civic buildings and shops (buildings/civic.js) -------------------------------
// gilding (frames, onion domes), painted onion domes, bone (museum skeleton)
def("GOLD", "#c9a13b", { noise: 0.05 });
def("DOME_GREEN", "#3f7a5c", { noise: 0.05 });
def("DOME_BLUE", "#3b5f9c", { noise: 0.05 });
def("DOME_SHINGLE", "#8d8a80", { noise: 0.1 });
def("BONE", "#e3dac2", { noise: 0.08 });
// artworks: canvases in a few strong colours
def("ART_BLUE", "#2e5f9e", { noise: 0.25 });
def("ART_OCHRE", "#c8902f", { noise: 0.25 });
def("ART_CRIMSON", "#a32838", { noise: 0.25 });
def("ART_TEAL", "#2f8a86", { noise: 0.25 });
// stage and auditorium: black stage floor, velvet seats and curtains, stage light
def("STAGE_BLACK", "#1d1c1f", { noise: 0.03 });
def("VELVET_RED", "#7a1f28", { noise: 0.06 });
def("STAGE_LIGHT", "#fff4d8", { emissive: 1, noise: 0 });
def("PROJECTION", "#e8ecf0", { emissive: 0.35, noise: 0 });
// hospital: pale green scrubs / curtains, medical steel
def("MEDICAL_GREEN", "#8fc2ae", { noise: 0.03 });
def("CURTAIN_BLUE", "#7fa6c8", { noise: 0.05 });
// fire engine red, police blue, petrol-station canopy white, neon green (pharmacy)
def("FIRE_RED", "#b8231d", { noise: 0.03 });
def("POLICE_BLUE", "#233f7a", { noise: 0.03 });
def("NEON_GREEN", "#4fe07a", { emissive: 1, noise: 0 });
def("NEON_RED", "#ff4040", { emissive: 1, noise: 0 });
// shop goods: bread, produce, bottles, clothes
def("BREAD", "#c08a4a", { noise: 0.2 });
def("PRODUCE_GREEN", "#5c9a3a", { noise: 0.3 });
def("PRODUCE_RED", "#c0402c", { noise: 0.3 });
def("PRODUCE_YELLOW", "#e0c040", { noise: 0.3 });
def("BOTTLES", "#4f7a58", { noise: 0.35 });
def("CLOTHES", "#6a5a8a", { noise: 0.4 });
def("MEAT", "#a8483f", { noise: 0.2 });
def("FISH", "#a8b4ba", { noise: 0.25 });
// field crops (nature/farmland.js)
def("CROP_WHEAT", "#c9a847", { noise: 0.14 });
def("CROP_RAPE", "#e2cd2c", { noise: 0.12 });
def("CROP_GREEN", "#78a83f", { noise: 0.12 });
// forest floor plants (nature/groundcover.js)
def("BERRY_BLUE", "#34406e", { noise: 0.1 });
def("MOSS_BRIGHT", "#7e9a45", { noise: 0.14 });
def("MUSHROOM_RED", "#b53a2c", { noise: 0.08 });
def("MUSHROOM_BROWN", "#8a5e38", { noise: 0.1 });
def("MUSHROOM_STEM", "#e6ddc8", { noise: 0.05 });
def("LINGON", "#2e4a2c", { noise: 0.12 });

export const MATERIAL_COUNT = MATERIALS.length;

/** Pack material table for GPU / worker transfer. */
export function materialTable() {
  return MATERIALS.map((m) => ({
    id: m.id,
    name: m.name,
    rgb: m.rgb,
    noise: m.noise,
    transparent: m.transparent,
    opacity: m.opacity,
    emissive: m.emissive,
    solid: m.solid,
    climb: m.climb,
  }));
}

/** Lookup arrays for hot loops (mesher, collision). */
export const IS_TRANSPARENT = new Uint8Array(MAX_MATERIALS);
export const IS_SOLID = new Uint8Array(MAX_MATERIALS);
export const IS_CLIMB = new Uint8Array(MAX_MATERIALS);
for (const m of MATERIALS) {
  IS_TRANSPARENT[m.id] = m.transparent ? 1 : 0;
  IS_SOLID[m.id] = m.solid ? 1 : 0;
  IS_CLIMB[m.id] = m.climb ? 1 : 0;
}
