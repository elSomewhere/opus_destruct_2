import { BIOMES } from "../world/registry.js";
import { MAT } from "../voxel/materials.js";
import { smoothstep } from "../core/math.js";

/**
 * Biomes: the natural counterpart of urban districts. A biome is chosen per
 * column from climate (temperature cooled by altitude, moisture) as the
 * nearest climate centroid, with a little noise so borders interleave
 * naturally. Each biome says how its ground looks, how dense and which
 * vegetation grows, whether fields are farmed and whether shallow pools
 * form. Elevation overlays (beach, rock slopes, snow line) are applied by
 * LandCover on top, so a mountain in the desert still gets bare rock.
 *
 * Adding a biome = registering an entry here; LandCover, Forest and the
 * overview map pick it up.
 *
 *   climate   [temperature, moisture] centroid (both 0..1)
 *   forest    multiplier on the forest-patch noise (0 = never forest)
 *   meadow    background tree chance outside forest patches
 *   farmland  whether farm parcels appear on flat open land
 *   trees     weighted tree kinds (see nature/trees.js)
 *   ground(c) -> [top, sub] for open (non-forest) ground; c = {patch (~11 m
 *             noise), p2 (~5 m noise), h (0..1 hash of the column: mix
 *             materials by comparing it with a smooth ratio), x, y}
 *   floor     ground under forest canopy
 *   pools     chance-ish threshold for shallow ponds (wetlands)
 */

BIOMES.register({
  id: "temperate",
  label: "Temperate forest",
  color: "#4f8a3e",
  climate: [0.5, 0.62],
  forest: 1.0,
  meadow: 0.035,
  farmland: true,
  trees: [["oak", 4], ["maple", 3], ["birch", 2], ["pine", 0.6], ["spruce", 0.8], ["rowan", 0.4], ["aspen", 0.45], ["alder", 0.35]],
  floor: MAT.FOREST_FLOOR,
  ground: (c) => [c.h < smoothstep(0.38, 0.62, c.patch + 0.25 * c.p2) ? MAT.GRASS_DRY : c.h < smoothstep(-0.45, -0.7, c.patch - 0.3 * c.p2) ? MAT.GRASS_DARK : MAT.GRASS, MAT.DIRT],
});

BIOMES.register({
  id: "grassland",
  label: "Grassland",
  color: "#8fb35a",
  climate: [0.52, 0.4],
  forest: 0.45,
  meadow: 0.02,
  farmland: true,
  trees: [["oak", 5], ["maple", 1], ["birch", 1], ["poplar", 0.4], ["aspen", 0.4], ["shrub", 3]],
  floor: MAT.FOREST_FLOOR,
  ground: (c) => [c.h < smoothstep(0.2, 0.45, c.patch + 0.25 * c.p2) ? MAT.GRASS_DRY : c.h < smoothstep(-0.5, -0.75, c.patch) ? MAT.GRASS_DARK : MAT.GRASS, MAT.DIRT],
});

BIOMES.register({
  id: "boreal",
  label: "Boreal forest",
  color: "#3d6a4a",
  climate: [0.3, 0.58],
  forest: 1.3,
  meadow: 0.05,
  farmland: false,
  // dark spruce stands and pine on the drier ground, birches, aspens and rowans at the
  // edges, larches on the dry and rocky ground, alders along the water
  trees: [["spruce", 5], ["pine", 4], ["birch", 2.5], ["rowan", 0.4], ["aspen", 0.45], ["larch", 0.5], ["alder", 0.25]],
  floor: MAT.MOSS,
  ground: (c) => [c.h < smoothstep(0.3, 0.55, c.patch + 0.3 * c.p2) ? MAT.MOSS : c.h < smoothstep(-0.4, -0.65, c.patch) ? MAT.GRASS : MAT.GRASS_DRY, MAT.DIRT],
});

BIOMES.register({
  id: "tundra",
  label: "Tundra",
  color: "#a9ae98",
  climate: [0.14, 0.45],
  forest: 0.15,
  meadow: 0.012,
  farmland: false,
  trees: [["dwarfpine", 4], ["shrub", 2]],
  floor: MAT.MOSS,
  ground: (c) => (c.patch > 0.35 ? [MAT.SNOW, MAT.TUNDRA] : c.patch < -0.3 ? [MAT.GRAVEL, MAT.STONE] : [MAT.TUNDRA, MAT.DIRT]),
});

BIOMES.register({
  id: "desert",
  label: "Desert",
  color: "#dcc58c",
  climate: [0.78, 0.18],
  forest: 0,
  meadow: 0.02,
  farmland: false,
  trees: [["cactus", 5], ["shrubDry", 4]],
  floor: MAT.SAND,
  ground: (c) => (c.patch > 0.62 ? [MAT.SANDSTONE, MAT.SANDSTONE] : c.patch > 0.05 ? [MAT.SAND_DUNE, MAT.SAND] : [MAT.SAND, MAT.SAND]),
  dunes: 1,
});

BIOMES.register({
  id: "savanna",
  label: "Savanna",
  color: "#c2b060",
  climate: [0.74, 0.38],
  forest: 0.12,
  meadow: 0.018,
  farmland: false,
  trees: [["acacia", 5], ["shrubDry", 3]],
  floor: MAT.RED_EARTH,
  ground: (c) => (c.patch > 0.5 ? [MAT.RED_EARTH, MAT.RED_EARTH] : [MAT.SAVANNA_GRASS, MAT.RED_EARTH]),
});

BIOMES.register({
  id: "tropical",
  label: "Rainforest",
  color: "#2f6a36",
  climate: [0.8, 0.78],
  forest: 1.5,
  meadow: 0.08,
  farmland: false,
  trees: [["jungle", 6], ["palm", 2], ["shrub", 2]],
  floor: MAT.JUNGLE_FLOOR,
  ground: (c) => (c.patch > 0.3 ? [MAT.JUNGLE_FLOOR, MAT.DIRT] : [MAT.GRASS, MAT.DIRT]),
});

BIOMES.register({
  id: "wetland",
  label: "Wetland",
  color: "#5f8a6a",
  climate: [0.48, 0.86],
  forest: 0.35,
  meadow: 0.02,
  farmland: false,
  trees: [["willow", 3], ["birch", 3], ["oak", 1], ["alder", 3], ["shrub", 3]],
  floor: MAT.MARSH_GRASS,
  ground: (c) => [c.h < smoothstep(0.1, 0.35, c.patch + 0.3 * c.p2) ? MAT.MARSH_GRASS : c.h < smoothstep(-0.4, -0.65, c.patch) ? MAT.GRASS_DARK : MAT.GRASS, MAT.MUD],
  pools: 0.28,
});

/** Northern bog (myr): sphagnum and sedge, black pools, stunted pines. */
BIOMES.register({
  id: "bog",
  label: "Bog",
  color: "#7d8a5e",
  climate: [0.33, 0.84],
  forest: 0.22,
  meadow: 0.02,
  farmland: false,
  trees: [["dwarfpine", 5], ["pine", 2], ["birch", 0.8], ["shrub", 2]],
  floor: MAT.MOSS,
  ground: (c) => (c.patch > 0.25 ? [MAT.MARSH_GRASS, MAT.MUD] : c.patch < -0.35 ? [MAT.TUNDRA, MAT.MUD] : [MAT.MOSS, MAT.MUD]),
  pools: 0.3,
});

const LIST = () => BIOMES.all();

/**
 * Nearest climate centroid. `t` is the local (altitude-cooled) temperature
 * and `m` moisture; callers jitter both slightly so borders interleave.
 */
export function classifyBiome(t, m) {
  let best = null;
  let bd = Infinity;
  for (const b of LIST()) {
    const dt = t - b.climate[0];
    const dm = m - b.climate[1];
    const d = dt * dt * 1.3 + dm * dm;
    if (d < bd) {
      bd = d;
      best = b;
    }
  }
  return best;
}

/** How desert-like a climate is (0..1): drives dunes in the terrain. */
export function desertness(t, m) {
  const d = BIOMES.get("desert").climate;
  const dist = Math.hypot((t - d[0]) * 1.14, m - d[1]);
  return Math.max(0, Math.min(1, 1 - dist / 0.27));
}

export { BIOMES };
