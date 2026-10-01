import { MATERIALS } from "../voxel/materials.js";

/**
 * The city's materials as a structvox world holds them (opus_destruct_2,
 * core/include/svx/material/material.hpp, svx/world/grid.hpp): a voxel is a
 * byte, 1 + a physics material id (up to 127 of them: how it carries,
 * breaks and burns) and bit 7 for anchored (a support, never simulated).
 * The city's 400 looks are far more than a physics engine needs, so each
 * maps to a physics class, and what it looks like travels in a persistent
 * voxel layer (ANGLED_WORLD_PLAN.md §8):
 *
 *   vox     (1 + class id) | 0x80 where anchored
 *   look    its look's index within its class (a solid-bound layer): the
 *           class and the index give back the city material, for colour
 *   flora   decorative voxels (leaves, flowers, grass blades, crops):
 *           neither free structure (they would fragment by the million)
 *           nor anchored (grass would hold up walls: §4.5), so air to the
 *           physics, 1 + their index in FLORA in an air-bound layer, until
 *           the core has a `decorative` flag (then policy "solid")
 *   water   liquids (water, sewage, sludge): air, 255 in the "water" layer
 *
 * A voxel is anchored where the ground holds it: the column tile's fill
 * (terrain, road surfaces, pavements) still standing when every feature is
 * drawn, a bridge's deck, piers and railings aside (svx/source.js); every
 * other voxel is structure, props and furniture isolated voxels (§4.6).
 *
 * Class ids: the core's standard presets keep theirs (MaterialId), the
 * game's materials theirs (svx/game/materials.hpp, mat::, at
 * kStandardMaterials + 0..8), and the city registers its own right after
 * them, at CITY_BASE + k, so saved sessions keep their meaning.
 */

/** Where the city's own physics materials start: kStandardMaterials (12) + mat::kGameMaterials (9). */
export const CITY_BASE = 21;

/**
 * Physics classes: [name, structvox id, properties of the city's own
 * (Material fields, SI units: section strengths of the bonds between
 * fragments, fragment sizes in voxels), null for the core's and the
 * game's, which the engine already has].
 */
const CLASS_DEFS = [
  ["rc", 0],
  ["concrete", 1],
  ["steel", 2],
  ["masonry", 3],
  ["soil", 4],
  ["rock", 5],
  ["bedrock", 6],
  ["wood", 7],
  ["stone", 8],
  ["glass", 9],
  ["steel_section", 11],
  ["sheet", 12],
  ["window", 15],
  ["tyre", 16],
  ["plastic", 17],
  ["asphalt", 18],
  ["paint", 19],
  ["lamp", 20],
  // roof coverings (tiles, shingles, slate, sheet, membrane, sod) on their battens, smeared over a voxel
  ["roofing", CITY_BASE + 0, { E: 5e9, G: 2e9, rho: 700, ft: 0.3e6, fb: 0.6e6, fc: 5e6, cohesion: 0.3e6, friction: 0.6, Gf: 60, frag: [3, 3, 2], frag_noise: 0.3 }],
  // interior partitions and ceilings: plasterboard on studs, smeared
  ["partition", CITY_BASE + 1, { E: 3e9, G: 1.2e9, rho: 600, ft: 0.2e6, fb: 0.4e6, fc: 3e6, cohesion: 0.2e6, friction: 0.6, Gf: 80, frag: [3, 3, 3], frag_noise: 0.35 }],
  // upholstery, bedding, fabric, goods, paper: light, soft, tough to tear
  ["soft", CITY_BASE + 2, { E: 0.02e9, G: 0.008e9, rho: 200, ft: 0.05e6, fb: 0.05e6, fc: 0.2e6, cohesion: 0.05e6, friction: 0.8, Gf: 500, frag: [4, 4, 4], frag_noise: 0.3, crush: 2e4 }],
  // lake and sea ice, brittle, slippery
  ["ice", CITY_BASE + 3, { E: 9e9, G: 3.5e9, rho: 917, ft: 1e6, fb: 1.5e6, fc: 5e6, cohesion: 1e6, friction: 0.1, Gf: 5, frag: [3, 3, 3], frag_noise: 0.5, grip: 0.1 }],
  // packed snow on the ground and on roofs
  ["snow", CITY_BASE + 4, { E: 0.05e9, G: 0.02e9, rho: 350, ft: 0.02e6, fb: 0.02e6, fc: 0.2e6, cohesion: 0.02e6, friction: 0.3, Gf: 5, frag: [2, 2, 2], frag_noise: 0.5, grip: 0.3 }],
  // decorative plants as solid voxels (the "solid" flora policy only)
  ["foliage", CITY_BASE + 5, { E: 0.01e9, G: 0.004e9, rho: 60, ft: 0.02e6, fb: 0.02e6, fc: 0.05e6, cohesion: 0.02e6, friction: 0.6, Gf: 50, frag: [3, 3, 3], frag_noise: 0.5 }],
];

/** The classes: [{ name, id, own (properties to register, or null) }] by name too. */
export const CLASSES = CLASS_DEFS.map(([name, id, own]) => ({ name, id, own: own ?? null }));
export const CLASS = Object.fromEntries(CLASSES.map((c) => [c.name, c]));

/**
 * Rules, first match wins: [pattern on the material name, what it is].
 *   s  its class as structure (off the ground), g its class as ground
 *      (anchored; default s), f decorative (flora), w liquid (water layer)
 */
const RULES = [
  [/^AIR$/, {}],
  [/^(WATER|SEWER_WATER|SLUDGE|NUKAGE)$/, { w: true }],
  // --- decorative plants: flora off the ground (on the ground as its cover: soil)
  [/^(LEAVES|LEAF_|HEDGE|BUSH|FLOWER|REED|CACTUS|PALM_FROND|SHRUB|TWIGS|BERRY|FERN|BLUEBERRY|LINGON|GRASS_TALL|MOSS_BRIGHT|MUSHROOM|CROP_|PLANT$|HEATHER|LICHEN)/, { f: true, g: "soil" }],
  [/^NEEDLE_LITTER$/, { f: true, g: "soil" }],
  // --- ground: soil, rock, bedrock (off the ground: a stone is stone, soil soil)
  [/^BEDROCK$/, { s: "rock", g: "bedrock" }],
  [/^(STONE|ROCK|ROCK_DARK|ROCK_LIGHT|SANDSTONE|GRANITE_PINK|CAVE_FLOOR|CRYSTAL_)/, { s: "stone", g: "rock" }],
  [/^(DIRT|GRASS|FOREST_FLOOR|SAND|GRAVEL|CLAY|MUD|RED_EARTH|SAVANNA_GRASS|MOSS|TUNDRA|MARSH|JUNGLE_FLOOR|CAVE_MOSS|LAWN_|SOIL_BED|BALLAST|PARK_PATH|TREE_PIT|SAND_BOX|SAND_DUNE)/, { s: "soil" }],
  [/^(SNOW|SNOW_WIND)$/, { s: "snow" }],
  [/^ICE$/, { s: "ice" }],
  // --- roads and paving
  [/^(ASPHALT|SPORT_COURT|COURT_ORANGE)/, { s: "asphalt" }],
  [/^(LINE_|TACTILE|HAZARD_)/, { s: "paint" }],
  [/^(CURB|SIDEWALK|PAVER_|PLATFORM_EDGE|SEWER_CURB|SEWER_FLOOR)/, { s: "concrete" }],
  [/^(PLAZA_STONE|COBBLE|FLAGSTONE)/, { s: "stone" }],
  [/^(MANHOLE|DRAIN_GRATE|GRATE_STEEL|MANHOLE_RIM)/, { s: "steel" }],
  [/^RUBBER_MAT$/, { s: "tyre" }],
  // --- structure and facades
  [/^(HW_CONCRETE|HW_BARRIER|TUNNEL_WALL|CONCRETE|CONCRETE_DARK|CONCRETE_LIGHT)$/, { s: "rc" }],
  [/^(CINDERBLOCK|BRICK_|SEWER_BRICK|PLASTER_|RENDER_|CERAMIC|BONE)/, { s: "masonry" }],
  [/^(LIMESTONE|GRANITE|GRANITE_LIGHT|TRIM_STONE|CORNICE|COUNTERTOP|PLANT_POT)/, { s: "stone" }],
  [/^(PANEL_WHITE|PANEL_GRAPHITE|PANEL_BEIGE|PANEL_GRAYBLUE|PANEL_JOINT|PARAPET_CAP|TUNNEL_TILE|STAIR_CONCRETE|RAIL_TIE|FLOOR_(TILE|CARPET|CONCRETE|EPOXY|MARBLE|LINOLEUM|TERRAZZO))/, { s: "concrete" }],
  [/^(METAL_PANEL|CORRUGATED|CONTAINER_|SIGN_)/, { s: "sheet" }],
  [/^(ROLLUP_DOOR|ELEVATOR_DOOR|TANK_WHITE|SILO_STEEL|AC_UNIT|VENT|SIGNAL_BOX|CAR_(RED|BLUE|WHITE|BLACK|SILVER|YELLOW|GREEN)|FIRE_RED|POLICE_BLUE|GOLD|DOME_GREEN|DOME_BLUE)$/, { s: "sheet" }],
  [/^(WOOD_|SIDING_|CLAD_|LOG_TARRED|BARN_RED|PINE_PANEL|FLOOR_(OAK|WALNUT|PARQUET)|DOOR_(WOOD|WHITE|RED|GREEN|FRAME)|STAIR_WOOD|HANDRAIL_WOOD|FRAME_WOOD|BASEBOARD|LAMINATE_|PALLET|SHELF_ORANGE|CHALKBOARD|FENCE_WOOD|FENCE_WHITE|WATER_TANK_WOOD|STAGE_BLACK|BARK|DEADWOOD)/, { s: "wood" }],
  [/^(GLASS|DOOR_GLASS|NEON_|LIGHT_STRIP|SCREEN|MIRROR|LAMP_SHADE|LAMP_LIGHT|SOLAR_PANEL|STAGE_LIGHT|PROJECTION|CEILING_LIGHT|BOTTLES)/, { s: "glass" }],
  [/^(STEEL_BEAM|STEEL_RUST|RAIL_STEEL|HYDRANT|BOLLARD|VALVE_RED|APPLIANCE_STEEL)$/, { s: "steel" }],
  [/^(MULLION|FRAME_WHITE|FRAME_DARK|DOOR_METAL|STAIR_NOSING|RAILING|POLE_|CHAINLINK|PIPE|LADDER|LAMP_CAGE|METAL_CHROME|METAL_BLACK|SHELF_METAL)/, { s: "steel_section" }],
  // --- roofs, interiors, furnishings
  [/^(ROOF_|DOME_SHINGLE)/, { s: "roofing" }],
  [/^(PAINT_|WALL_TILE_|CEILING|CEILING_TILE)/, { s: "partition" }],
  [/^(FABRIC_|LEATHER|MATTRESS|BEDSHEET_|PILLOW|RUG_|BOOKS|CARDBOARD|GOODS|BREAD|PRODUCE_|CLOTHES|MEAT|FISH|CURTAIN_|VELVET_RED|MEDICAL_GREEN|ART_|AWNING_|HAY)/, { s: "soft" }],
  [/^(PLASTIC_|BIN_GREEN|PLAY_|CONE_|SIGNAGE|EMERGENCY_RED)/, { s: "plastic" }],
  [/^(TIRE)$/, { s: "tyre" }],
  [/^(CAR_GLASS)$/, { s: "window" }],
  [/^(HEADLIGHT|TAILLIGHT|SIGNAL_RED|SIGNAL_GREEN|SIGNAL_AMBER)$/, { s: "lamp" }],
];

function ruleFor(name) {
  for (const [re, spec] of RULES) if (re.test(name)) return spec;
  return null;
}

/**
 * Per city material id: { air, liquid, flora, s (structure class id), g
 * (ground class id), sLook / gLook (look indices in those classes), floraIdx }.
 * Throws on a material no rule covers (test/svx.test.js keeps it total).
 */
export const CLASSIFY = [];
/** Per class id: the city materials of that class in look order (look index -> city material id). */
export const LOOKS = new Map();
/** Flora layer value - 1 -> city material id. */
export const FLORA = [];

function lookOf(classId, matId) {
  let list = LOOKS.get(classId);
  if (!list) LOOKS.set(classId, (list = []));
  let k = list.indexOf(matId);
  if (k < 0) {
    k = list.length;
    list.push(matId);
  }
  if (k > 255) throw new Error(`svx: class ${classId} has more than 256 looks`);
  return k;
}

const unclassified = MATERIALS.filter((m) => !ruleFor(m.name)).map((m) => m.name);
if (unclassified.length) throw new Error(`svx: no physics class for ${unclassified.join(" ")}`);
for (const m of MATERIALS) {
  const spec = ruleFor(m.name);
  if (m.id === 0) {
    CLASSIFY.push({ air: true });
    continue;
  }
  if (spec.w) {
    CLASSIFY.push({ liquid: true });
    continue;
  }
  const s = CLASS[spec.s ?? (spec.f ? "foliage" : null)] ?? null;
  const g = CLASS[spec.g ?? spec.s ?? "soil"];
  const e = { s: s.id, g: g.id, sLook: lookOf(s.id, m.id), gLook: lookOf(g.id, m.id) };
  if (spec.f) {
    e.flora = true;
    e.floraIdx = FLORA.length;
    FLORA.push(m.id);
  }
  CLASSIFY.push(e);
}

/** The voxel byte of a physics class (1 + id, bit 7 anchored). */
export const vox = (classId, anchored) => (1 + classId) | (anchored ? 0x80 : 0);

/**
 * What a structvox host registers and draws with: the city's own physics
 * materials (at their ids, after the core's and the game's), and for
 * colour the city materials behind every (class, look) and flora value.
 */
export function svxMaterials() {
  return {
    cityBase: CITY_BASE,
    register: CLASSES.filter((c) => c.own).map((c) => ({ id: c.id, name: `city_${c.name}`, ...c.own })),
    classes: CLASSES.map((c) => ({ id: c.id, name: c.name, own: !!c.own })),
    looks: Object.fromEntries([...LOOKS].map(([id, list]) => [id, list.map((m) => MATERIALS[m].name)])),
    flora: FLORA.map((m) => MATERIALS[m].name),
    palette: Object.fromEntries(MATERIALS.map((m) => [m.name, { rgb: m.rgb, transparent: m.transparent, opacity: m.opacity, emissive: m.emissive, glow: m.glow, noise: m.noise }])),
  };
}
