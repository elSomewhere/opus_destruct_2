import { MAT } from "../voxel/materials.js";
import { hash32 } from "../core/hash.js";

/**
 * Seasons: `config.world.season` is "spring", "summer" (the default),
 * "autumn" or "winter". The season is a lens over the climate: everything
 * it changes depends on the LOCAL temperature t (sea-level temperature
 * cooled by altitude, 0..1, see nature/landcover.js), so one world can be
 * autumn-gold in its valleys, first-snow white on its fells and still green
 * in its warm south. Warm lands (t > ~0.64) hardly change at all.
 *
 *   snow(t)          seasonal snow cover of open ground (0..1): winter
 *                    everywhere cool, lingering snow high up in spring,
 *                    the first snow on the fells in autumn
 *   roofSnow(t)      the same for roofs
 *   freezeT          still water (lakes, ponds, rivers) freezes below it
 *   treeLook(...)    foliage of one tree: leaf materials, bare branches,
 *                    snow on the crown
 *   ground(m, t, n)  seasonal ground colour (fresh spring grass, straw,
 *                    leaf litter, red dwarf shrubs, dead winter grass)
 *   flower(...)      wild flowers on meadows (lupins, buttercups, daisies,
 *                    campion, heather in bloom; wood anemones in spring)
 *   canopy(...)      the crown blanket of distant forests
 *
 * A world that sets `climate.snowCover` / `climate.freeze` explicitly keeps
 * that fixed snow and ice (the pre-season behaviour); the season still
 * colours foliage and grass.
 */
export const SEASONS = [
  { id: "spring", label: "Spring" },
  { id: "summer", label: "Summer" },
  { id: "autumn", label: "Autumn" },
  { id: "winter", label: "Winter" },
];
const IDS = new Set(SEASONS.map((s) => s.id));

/**
 * Default viewer mood per season (layered under a preset's own atmosphere,
 * see config/presets.js presetViewer): sky colour, sun intensity, the sun's
 * highest altitude (sine), fog and desaturation.
 */
export const SEASON_ATMOSPHERE = {
  spring: { sky: 0xb4c8da, sun: 1, sunElevation: 0.8, fogDensity: 1, desaturate: 0.05 },
  summer: {},
  autumn: { sky: 0xadb8c0, sun: 0.8, sunElevation: 0.55, fogDensity: 1.35, desaturate: 0.15 },
  winter: { sky: 0xb9c1c8, sun: 0.65, sunElevation: 0.4, fogDensity: 1.5, desaturate: 0.3 },
};

const WARM = 0.64;

function smooth(a, b, x) {
  const t = Math.max(0, Math.min(1, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
}

const h01 = (seed, salt) => (hash32(seed, salt, 0x5ea) >>> 8) / 16777216;

/** Kinds that keep their needles or leaves through the year (the season only lays snow on them). */
const EVERGREEN = new Set(["pine", "spruce", "dwarfpine", "juniper", "jungle", "palm", "cactus", "acacia", "shrubDry", "log", "stump", "snag"]);

/**
 * Autumn palettes [shadow, body, sunlit] per kind: birches and poplars go
 * gold, oaks brown and orange, maples fiery, rowans red with berries.
 */
const AUTUMN = {
  birch: [MAT.LEAVES_AUTUMN, MAT.LEAVES_YELLOW, MAT.LEAVES_YELLOW],
  oak: [MAT.LEAVES_BROWN, MAT.LEAVES_AUTUMN, MAT.LEAVES_YELLOW],
  maple: [MAT.LEAVES_RED, MAT.LEAVES_AUTUMN, MAT.LEAVES_YELLOW],
  autumn: [MAT.LEAVES_RED, MAT.LEAVES_AUTUMN, MAT.LEAVES_YELLOW],
  street: [MAT.LEAVES_BROWN, MAT.LEAVES_YELLOW, MAT.LEAVES_YELLOW],
  blossom: [MAT.LEAVES_RED, MAT.LEAVES_RED, MAT.LEAVES_AUTUMN],
  rowan: [MAT.LEAVES_RED, MAT.LEAVES_RED, MAT.LEAVES_AUTUMN],
  willow: [MAT.LEAVES_BIRCH, MAT.LEAVES_YELLOW, MAT.LEAVES_YELLOW],
  poplar: [MAT.LEAVES_AUTUMN, MAT.LEAVES_YELLOW, MAT.LEAVES_YELLOW],
  shrub: [MAT.LEAVES_BROWN, MAT.LEAVES_RED, MAT.LEAVES_AUTUMN],
  fern: [MAT.LEAVES_BROWN, MAT.LEAVES_BROWN, MAT.GRASS_STRAW],
  berry: [MAT.TUNDRA_AUTUMN, MAT.LEAVES_RED, MAT.LEAVES_RED],
  hazel: [MAT.LEAVES_BROWN, MAT.LEAVES_YELLOW, MAT.LEAVES_YELLOW],
  aspen: [MAT.LEAVES_RED, MAT.LEAVES_AUTUMN, MAT.LEAVES_YELLOW],
  alder: [MAT.LEAVES_DARK, MAT.LEAVES_BROWN, MAT.LEAVES_BROWN],
  larch: [MAT.LEAVES_AUTUMN, MAT.LEAVES_YELLOW, MAT.LEAVES_YELLOW],
};
const SPRING = [MAT.LEAVES, MAT.LEAVES_SPRING, MAT.LEAVES_SPRING];
const BLOOM = [MAT.LEAVES_BLOSSOM, MAT.LEAVES_BLOSSOM, MAT.FLOWER_WHITE];

/** Seasonal ground: spring / autumn / winter replacement of a summer ground material. */
function groundTables() {
  const spring = new Map([
    [MAT.GRASS, MAT.GRASS_SPRING],
    [MAT.GRASS_DARK, MAT.GRASS_SPRING],
    [MAT.GRASS_LAWN, MAT.GRASS_SPRING],
    [MAT.GRASS_DRY, MAT.GRASS_STRAW],
    [MAT.MARSH_GRASS, MAT.MARSH_AUTUMN],
  ]);
  const autumn = new Map([
    [MAT.GRASS, MAT.GRASS_AUTUMN],
    [MAT.GRASS_DARK, MAT.LAWN_AUTUMN],
    [MAT.GRASS_LAWN, MAT.LAWN_AUTUMN],
    [MAT.GRASS_DRY, MAT.GRASS_STRAW],
    [MAT.FOREST_FLOOR, MAT.LEAF_LITTER],
    [MAT.TUNDRA, MAT.TUNDRA_AUTUMN],
    [MAT.MOSS, MAT.HEATHER],
    [MAT.MARSH_GRASS, MAT.MARSH_AUTUMN],
    [MAT.SAVANNA_GRASS, MAT.GRASS_STRAW],
  ]);
  const winter = new Map([
    [MAT.GRASS, MAT.GRASS_DEAD],
    [MAT.GRASS_DARK, MAT.LAWN_WINTER],
    [MAT.GRASS_LAWN, MAT.LAWN_WINTER],
    [MAT.GRASS_DRY, MAT.GRASS_STRAW],
    [MAT.FOREST_FLOOR, MAT.LEAF_LITTER],
    [MAT.TUNDRA, MAT.TUNDRA_AUTUMN],
    [MAT.MARSH_GRASS, MAT.MARSH_AUTUMN],
  ]);
  return { spring, autumn, winter };
}
let TABLES = null;

export class Season {
  constructor(config) {
    const w = config.world;
    this.id = IDS.has(w.season) ? w.season : "summer";
    const cl = w.climate ?? null;
    /** explicit (legacy) snow cover / freeze temperature of the climate override */
    this.fixedSnow = cl?.snowCover;
    this.cold = cl ? (cl.temperature ?? 0.5) + (cl.temperatureVar ?? 0.05) < 0.45 : false;
    this.freezeT = cl?.freeze ?? { spring: 0.3, summer: 0.18, autumn: 0.27, winter: 0.47 }[this.id];
    TABLES ??= groundTables();
    this.table = this.id === "summer" ? null : TABLES[this.id];
    /** does the season change anything on the ground at all (summer: flowers only) */
    this.any = this.id !== "summer" || this.fixedSnow > 0;
  }

  /** 0..1: how strongly deciduous vegetation shows the season at local temperature t. */
  strength(t) {
    return smooth(WARM, WARM - 0.1, t);
  }

  /** Seasonal snow cover (0..1) of open ground at local temperature t. */
  snow(t) {
    if (this.fixedSnow !== undefined) return this.fixedSnow > 0 && (this.cold || t < 0.5 + 0.1 * this.fixedSnow) ? this.fixedSnow : 0;
    switch (this.id) {
      case "winter":
        return 0.92 * smooth(0.6, 0.42, t);
      case "spring":
        return 0.8 * smooth(0.33, 0.25, t);
      case "autumn":
        return 0.7 * smooth(0.3, 0.22, t);
      default:
        return 0;
    }
  }

  /** Snow cover of roofs (an explicit climate cover lies on every roof). */
  roofSnow(t) {
    if (this.fixedSnow !== undefined) return this.fixedSnow > 0 ? Math.min(1, this.fixedSnow) : 0;
    return this.snow(t);
  }

  /**
   * Foliage of one tree of `kind` (hash `seed`) at local temperature t:
   * { leaves (palette [shadow, body, sunlit] or null), mix (share of the
   * crown's clusters in that palette: autumn turns a tree cluster by
   * cluster), bare (branches only), snow (0..1 on the crown), holes (extra
   * gaps in the crown shell), accent (berries) }, or null for the plain
   * summer look.
   */
  treeLook(kind, seed, t) {
    const snow = this.snow(t);
    if (EVERGREEN.has(kind)) return snow > 0 ? { leaves: null, mix: 0, bare: false, snow, holes: 0 } : null;
    const k = this.strength(t);
    // (warm lands: the season does not show)
    if (k <= 0 && snow <= 0 && this.fixedSnow === undefined) return null;
    const r = h01(seed, 1);
    switch (this.id) {
      case "winter":
        return { leaves: null, mix: 0, bare: r < k, snow, holes: 0 };
      case "autumn": {
        // colder = later in the autumn: more trees already bare
        const bare = r < 0.5 * smooth(0.42, 0.28, t) * k;
        // (each tree turns at its own pace: some still green, most part-turned, some all gold)
        const mix = k * Math.min(1, h01(seed, 2) * 1.6);
        return { leaves: AUTUMN[kind] ?? AUTUMN.oak, mix, bare, snow, holes: 0.12 + 0.1 * mix, accent: kind === "rowan" ? MAT.BERRY_RED : null };
      }
      case "spring": {
        // late spring in the cold north: many trees still bare or just budding
        const bare = r < 0.7 * smooth(0.37, 0.28, t) * k;
        const bloom = kind === "blossom" || (kind === "street" && h01(seed, 3) < 0.2) || (kind === "rowan" && h01(seed, 3) < 0.5);
        return { leaves: bloom ? BLOOM : SPRING, mix: k * (bloom ? 1 : 0.9), bare, snow, holes: 0.2 };
      }
      default:
        // (an explicit climate snow cover strips summer broadleaves too)
        if (snow > 0) return { leaves: null, mix: 0, bare: snow > 0.4, snow, holes: 0 };
        return null;
    }
  }

  /**
   * Seasonal ground material replacing summer ground `m` at local
   * temperature t; n (0..1) is a smooth patch value so the change is
   * patchy near the warm edge.
   */
  ground(m, t, n) {
    if (!this.table) return m;
    const to = this.table.get(m);
    if (to === undefined) return m;
    const k = this.strength(t);
    if (n > k) return m;
    // autumn heather only in patches of the moss
    if (to === MAT.HEATHER && n > k * 0.3) return m;
    // spring: some old straw left in the fresh grass, fresh shoots in the dry
    if (this.id === "spring" && m === MAT.GRASS && n > k * 0.82) return MAT.GRASS_STRAW;
    if (this.id === "spring" && m === MAT.GRASS_DRY && n < k * 0.5) return MAT.GRASS_SPRING;
    return to;
  }

  /**
   * Wild flowers on open natural ground (top material m) or null. `cl`
   * (0..1) is a smooth cluster value (flowers grow in drifts), `h` a hash
   * per column and `sp` a hash per cluster (its species).
   */
  flower(m, t, cl, h, sp) {
    const k = this.strength(t) + (t > WARM ? 0.4 : 0);
    if (this.id === "summer") {
      if (m === MAT.MOSS || m === MAT.TUNDRA) return cl > 0.62 && h < 0.35 ? MAT.HEATHER_BLOOM : null;
      if (m !== MAT.GRASS && m !== MAT.GRASS_DRY && m !== MAT.MARSH_GRASS) return null;
      if (cl < 0.55 || h > 0.1 + 0.3 * (cl - 0.55)) return null;
      const s = sp * 5;
      return s < 1.4 ? MAT.FLOWER_LUPIN : s < 2.5 ? MAT.FLOWER_YELLOW : s < 3.6 ? MAT.FLOWER_WHITE : s < 4.4 ? MAT.FLOWER_PINK : MAT.FLOWER_PURPLE;
    }
    if (this.id === "spring") {
      if (k <= 0) return null;
      if (m === MAT.FOREST_FLOOR || m === MAT.LEAF_LITTER) return cl > 0.5 && h < 0.18 ? MAT.FLOWER_WHITE : null;
      if (m === MAT.GRASS_SPRING || m === MAT.GRASS) return cl > 0.62 && h < 0.08 ? (sp < 0.6 ? MAT.FLOWER_YELLOW : MAT.FLOWER_WHITE) : null;
    }
    return null;
  }

  /**
   * Distant canopy (crown blanket at coarse LODs): a conifer crown (spruce
   * or pine green) or a broadleaf one in its seasonal colours at local
   * temperature tl; k is a per-crown hash.
   */
  canopy(conifer, tl, k) {
    if (conifer) return k < 0.55 ? MAT.LEAVES_SPRUCE : MAT.LEAVES_PINE;
    const s = this.strength(tl);
    const q = (k * 7.31) % 1;
    switch (this.id) {
      case "autumn":
        if (q < s) return q < s * 0.4 ? MAT.LEAVES_YELLOW : q < s * 0.65 ? MAT.LEAVES_AUTUMN : q < s * 0.82 ? MAT.LEAVES_RED : MAT.LEAVES_BROWN;
        break;
      case "winter":
        if (q < s) return MAT.TWIGS;
        break;
      case "spring":
        if (q < s) return q < s * 0.6 ? MAT.LEAVES_SPRING : MAT.LEAVES_LIGHT;
        break;
      default:
    }
    return k < 0.3 ? MAT.LEAVES_LIGHT : MAT.LEAVES_DARK;
  }
}

const cache = new WeakMap();

/** The Season of a config (cached). */
export function seasonOf(config) {
  let s = cache.get(config);
  if (!s) {
    s = new Season(config);
    cache.set(config, s);
  }
  return s;
}

/**
 * Smooth value noise (0..1) from hashes on a square lattice of `cell`
 * voxels: cheap patchiness for seasonal ground, flower drifts and the like.
 * (x, y) should be canonical coordinates in a wrapping world; its size is
 * a multiple of 1920 voxels, so cells dividing 1920 repeat seamlessly.
 */
export function patchNoise(seed, x, y, cell, salt) {
  const gx = Math.floor(x / cell);
  const gy = Math.floor(y / cell);
  let fx = x / cell - gx;
  let fy = y / cell - gy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const a = hash32(seed, gx, gy, salt) / 4294967296;
  const b = hash32(seed, gx + 1, gy, salt) / 4294967296;
  const c = hash32(seed, gx, gy + 1, salt) / 4294967296;
  const d = hash32(seed, gx + 1, gy + 1, salt) / 4294967296;
  return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
