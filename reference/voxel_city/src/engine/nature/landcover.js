import { SimplexNoise } from "../core/noise.js";
import { deriveSeed } from "../core/hash.js";
import { MAT } from "../voxel/materials.js";
import { VOXEL_SIZE } from "../core/units.js";
import { smoothstep } from "../core/math.js";
import { classifyBiome } from "./biomes.js";
import { hash32 } from "../core/hash.js";
import { wrapOf } from "../world/wrap.js";
import { Farmland } from "./farmland.js";

/** meters of altitude per unit of the 0..1 temperature scale */
const LAPSE = 10000;
/**
 * local temperature of the tree line and the snow line: a temperate
 * climate (t = 0.5 at sea level) has its tree line near 2,200 m and
 * permanent snow above ~3,200 m; warm regions carry both far higher.
 */
const TREELINE = 0.28;
const SNOWLINE = 0.18;

/** Layered desert rock: bands of sandstone / red earth / pale sand by height. */
function strata(hM) {
  const band = Math.floor(hM / 5);
  const k = ((band * 7919) >>> 0) % 5;
  return k === 0 ? MAT.RED_EARTH : k === 1 ? MAT.SAND_DUNE : k === 2 ? MAT.CLAY : MAT.SANDSTONE;
}

/**
 * Natural land cover for non-urban ground. The biome (nature/biomes.js)
 * decides ground, vegetation, farmland and ponds; elevation overlays
 * (beaches, bare rock on steep slopes, scree, the snow line, which drops in
 * cold climates) are applied on top. `forestDensity` also drives trees.
 */
export { LAPSE, TREELINE, SNOWLINE };

export class LandCover {
  constructor(world) {
    this.world = world;
    this.wrap = wrapOf(world.config);
    this.nForest = new SimplexNoise(deriveSeed(world.seed, "landcover.forest"));
    this.nFarm = new SimplexNoise(deriveSeed(world.seed, "landcover.farm"));
    this.nPatch = new SimplexNoise(deriveSeed(world.seed, "landcover.patch"));
    this.nPool = new SimplexNoise(deriveSeed(world.seed, "landcover.pool"));
    this.torus = world.chart.id.startsWith("torus");
  }

  /**
   * Planar noise at a voxel point with a scale in voxels, sampled on the
   * chart where the world wraps (a torus), so forest patches, fields and
   * biome borders repeat with the world; plain 2D noise elsewhere.
   */
  fieldNoise(noise, x, y, sVox, ox = 0, oy = 0, octaves = 0) {
    if (!this.torus) return octaves ? noise.fbm2(x / sVox + ox, y / sVox + oy, octaves) : noise.n2(x / sVox + ox, y / sVox + oy);
    const [fx, fy, fz, fw] = this.world.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const s = sVox * VOXEL_SIZE;
    return octaves ? noise.fbm4(fx / s + ox, fy / s + oy, fz / s, fw / s, octaves) : noise.n4(fx / s + ox, fy / s + oy, fz / s, fw / s);
  }

  /** fbm at a voxel point with a scale in meters (see fieldNoise). */
  fieldFbmM(noise, x, y, sM, octaves) {
    if (!this.torus) return noise.fbm2((x * VOXEL_SIZE) / sM, (y * VOXEL_SIZE) / sM, octaves);
    return this.fieldNoise(noise, x, y, sM / VOXEL_SIZE, 0, 0, octaves);
  }

  /**
   * Local climate: temperature cooled by altitude (hM meters; about
   * 6.5 °C per km on a 0..1 scale spanning ~55 °C), moisture.
   */
  climate(x, y, hM) {
    const f = this.world.fields;
    // local exposure / microclimate: shifts the tree line and snow line by
    // ~±150 m so altitude zones end in ragged edges instead of contour bands
    const micro = 0.012 * this.fieldNoise(this.nPatch, x, y, 2400, 3.1, 0) + 0.007 * this.fieldNoise(this.nPatch, x, y, 520, 0, -7.7);
    const t = f.temperature(x, y) - Math.max(0, hM) / LAPSE + micro;
    return { t, m: f.moisture(x, y) };
  }

  /** 0 above the tree line, 1 well below it (for vegetation density). */
  treeLine(t) {
    return smoothstep(TREELINE, TREELINE + 0.06, t);
  }

  biomeAt(x, y, hM) {
    const { t, m } = this.climate(x, y, hM);
    const j = 0.035 * this.fieldNoise(this.nPatch, x, y, 2400);
    return classifyBiome(t + j, m - j);
  }

  /**
   * 0..1 open meadow in forest country (glades, old hay meadows, pasture
   * round the farms): where it is high the woods give way to grass.
   */
  clearing(x, y) {
    return smoothstep(0.32, 0.58, this.fieldFbmM(this.nPatch, x + 91000, y - 37000, 260, 2));
  }

  forestDensity(x, y, u, biome = null, hMeters = null) {
    const m = this.world.fields.moisture(x, y);
    const n = this.fieldFbmM(this.nForest, x, y, 700, 3);
    let base = smoothstep(0.1, 0.55, n * 0.6 + (m - 0.45) * 1.2) * (1 - 0.9 * this.clearing(x, y));
    // mountain flanks below the tree line are wooded
    const mtn = this.world.fields.mountainness(x, y);
    if (mtn > 0.02) base = Math.max(base, 0.9 * smoothstep(0.02, 0.25, mtn) * smoothstep(0.25, 0.4, m + 0.1));
    const hM = hMeters ?? this.world.terrain.sample(x, y).h * VOXEL_SIZE;
    const b = biome ?? this.biomeAt(x, y, hM);
    const { t } = this.climate(x, y, hM);
    // (farmland round the towns and farms takes the place of the woods)
    return Math.min(1, base * Math.max(b.forest, mtn > 0.1 ? 0.9 : 0)) * (1 - smoothstep(0.08, 0.2, u)) * this.treeLine(t) * (1 - 0.85 * this.farmMask(x, y, u, b));
  }

  /** The fields of open country (nature/farmland.js), made on first use. */
  get farmland() {
    return (this._farm ??= new Farmland(this.world));
  }

  /**
   * How much (x, y) is farmland (0..1): wide stretches of fields in mild
   * farming country, thickest round the towns and villages; in the north
   * only small fields close round them.
   */
  farmMask(x, y, u, biome) {
    if (u >= 0.2) return 0;
    const near = smoothstep(0.002, 0.04, u) * (1 - smoothstep(0.14, 0.2, u));
    if (biome.farmland) return smoothstep(0.22, 0.36, this.fieldFbmM(this.nFarm, x, y, 1500, 2) + 0.22 * near);
    if (biome.id !== "boreal" && biome.id !== "temperate" && biome.id !== "grassland") return 0;
    return smoothstep(0.35, 0.5, this.fieldFbmM(this.nFarm, x, y, 900, 2) * 0.5 + 0.7 * near);
  }

  isFarmland(x, y, u, biome = null) {
    return this.farmMask(x, y, u, biome ?? this.biomeAt(x, y, this.world.terrain.sample(x, y).h * VOXEL_SIZE)) > 0.5;
  }

  /** Hedgerow strength at (x, y) (0..1: shrubs and trees along some field boundaries), t the local climate. */
  hedgeAt(x, y, u, biome, t, farm = false) {
    if (!farm && this.farmMask(x, y, u, biome) <= 0.5) return 0;
    return this.farmland.hedge(this.farmland.fieldAt(x, y), t);
  }

  /** Pond depth (voxels) of a wetland column, 0 = dry. */
  poolDepth(x, y, biome, slope, u) {
    if (!biome.pools || slope > 0.12 || u > 0.1) return 0;
    const n = this.fieldNoise(this.nPool, x, y, 520, 0, 0, 2);
    const e = n - (1 - biome.pools * 2.2);
    return e > 0 ? Math.min(4, 1 + Math.floor(e * 18)) : 0;
  }

  /**
   * Surface + subsurface materials for a natural column, plus an optional
   * pond depth, a bump (a field wall's height, voxels) and whether it is a
   * cultivated field: [top, sub, waterDepth, bump, field].
   */
  surface(x, y, hVox, slope, u, farm = false) {
    const hM = hVox * VOXEL_SIZE;
    const { t } = this.climate(x, y, hM);
    const biome = this.biomeAt(x, y, hM);
    const patch = this.fieldNoise(this.nPatch, x, y, 800);
    const cold = t < 0.22;
    if (hM < this.world.config.world.seaLevel + 1.2) {
      // northern shores are shingle with sand in the coves
      if (cold || (this.world.fields.island && t < 0.42 && patch > -0.2)) return [MAT.GRAVEL, MAT.STONE];
      return [MAT.SAND, MAT.SAND];
    }
    const desert = biome.id === "desert" || biome.id === "savanna";
    // cliffs: bare rock; dry country shows layered sandstone strata
    // outcrops break up steep ground (a noise lowers the threshold in bands)
    const crag = 0.25 * this.fieldNoise(this.nPatch, x, y, 60);
    // vegetation clings to steep ground below the tree line; above it rock takes over sooner
    const cliff = t < TREELINE ? 1.05 : 1.45 + 0.8 * Math.min(1, (t - TREELINE) * 4);
    if (slope > cliff + crag) return desert ? [strata(hM), MAT.SANDSTONE] : [MAT.ROCK, MAT.STONE];
    // glaciers and permanent snow above the snow line (steep faces stay rock);
    // wind-scoured crust in streaks, rock ribs through the steeper snow
    if (t < SNOWLINE + 0.025 * patch && slope < 1.0) {
      // rock outcrops break through where the snow lies steeper
      const out = this.fieldNoise(this.nPatch, x, y, 150, 11, 0) + 0.45 * this.fieldNoise(this.nPatch, x, y, 38, 0, -4);
      if (slope > 0.5 && out > 1.25 - slope) return [MAT.ROCK_DARK, MAT.ROCK];
      const wind = (this.torus ? this.fieldNoise(this.nPatch, x, y, 36, -7, 3) : this.nPatch.n2(x / 60 - 7, y / 22 + 3)) + 0.4 * this.fieldNoise(this.nPatch, x, y, 9);
      return [wind > 0.45 ? MAT.SNOW_WIND : MAT.SNOW, MAT.ROCK];
    }
    if (desert && slope > 0.7) return [strata(hM), MAT.SANDSTONE];
    // scree aprons under the cliffs: rubble of the rock above
    if (desert && slope > 0.38) {
      const hr = hash32(this.wrap.vi(x), this.wrap.vi(y), 0x9a7) & 7;
      return [hr < 3 ? MAT.GRAVEL : hr < 5 ? MAT.RED_EARTH : strata(hM), MAT.SANDSTONE];
    }
    if (slope > 0.8 + crag && t < TREELINE + 0.02) return [MAT.GRAVEL, MAT.ROCK];
    // alpine zone above the tree line: meadows, scree and snow patches
    if (t < TREELINE) {
      const p = this.fieldNoise(this.nPatch, x, y, 140);
      if (p > 0.45 && t < TREELINE - 0.03) return [MAT.SNOW, MAT.GRAVEL];
      if (p < -0.3 || slope > 0.45) return [MAT.GRAVEL, MAT.ROCK];
      return [p > 0.1 ? MAT.TUNDRA : MAT.GRASS_DRY, MAT.DIRT];
    }
    const pool = this.poolDepth(x, y, biome, slope, u);
    if (pool) return [MAT.MUD, MAT.MUD, pool];
    const fd = farm ? 0 : this.forestDensity(x, y, u, biome, hM);
    // per column: a hash and a fine patch noise, so materials mix in dithered
    // drifts instead of flat blobs with hard outlines
    const h = hash32(this.wrap.vi(x), this.wrap.vi(y), 0x9a5) / 4294967296;
    const p2 = this.fieldNoise(this.nPatch, x, y, 40, 3.3, 7.7);
    if (fd > 0.35) return [this.forestFloor(biome, t, h, p2, patch), MAT.DIRT];
    if (slope < 0.3 && (farm || this.farmMask(x, y, u, biome) > 0.5)) {
      // fields: crops by climate and season, grass margins, hedges and field walls
      const [top, sub, bump] = this.farmland.ground(this.farmland.fieldAt(x, y), t, x, y);
      return [top, sub, 0, bump, true];
    }
    // meadows in the glades of wooded country: lush grass (flowers in summer)
    if (biome.forest >= 0.8 && this.clearing(x, y) > 0.35) return [h < smoothstep(0.35, 0.6, patch + 0.3 * p2) ? MAT.GRASS_DRY : h < smoothstep(-0.4, -0.65, patch) ? MAT.GRASS_DARK : MAT.GRASS, MAT.DIRT];
    return biome.ground({ patch: this.fieldNoise(this.nPatch, x, y, 90), x, y, h, p2 });
  }

  /**
   * Forest floor: the biome's floor dithered with moss, leaf litter or
   * brown needles (under the conifers of cool woods) and lichen.
   */
  forestFloor(biome, t, h, p2, patch) {
    const f = biome.floor;
    if (f === MAT.MOSS || t < 0.42) {
      // boreal: moss carpets, needle litter under the spruce, grey lichen on the dry
      if (h < smoothstep(0.05, 0.4, p2) * 0.8) return MAT.NEEDLE_LITTER;
      if (h > 0.94 && patch > 0.2) return MAT.LICHEN;
      return f === MAT.MOSS ? MAT.MOSS : f;
    }
    if (f === MAT.FOREST_FLOOR) {
      if (h < smoothstep(0.1, 0.45, p2) * 0.6) return MAT.MOSS;
      if (h > 0.9) return MAT.LEAF_LITTER;
      return MAT.FOREST_FLOOR;
    }
    return h > 0.85 ? MAT.FOREST_FLOOR : f;
  }
}
