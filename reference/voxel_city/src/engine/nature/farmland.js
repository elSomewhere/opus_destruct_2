import { hash32 } from "../core/hash.js";
import { MAT } from "../voxel/materials.js";
import { wrapOf } from "../world/wrap.js";
import { seasonOf } from "../world/season.js";

/**
 * Farmland: the fields of open country round towns, villages and farms.
 *
 * The land is cut into farm blocks of BLOCK (300 m); each block is split
 * into 2-6 strips of varying width across its own direction, and a strip
 * now and then once more across, so fields come in long parcels of
 * different sizes rather than a checkerboard. Every field has a crop by
 * climate (wheat, rapeseed, maize and potatoes where it is mild; hay
 * meadows, pasture and potatoes in the north) and shows the season (young
 * green shoots in spring, rapeseed in flower, golden wheat in summer,
 * stubble and ploughed soil in autumn). Fields are separated by grass
 * margins; some boundaries carry a hedgerow (shrubs and trees:
 * nature/forest.js) or, in the north, a low wall of cleared field stones.
 *
 * Pure functions of position (canonical in a wrapping world).
 */

const BLOCK = 2400; // 300 m
const MARGIN = 6; // grass strip along every field edge (0.75 m)

/** Crops by climate: [kind, weight]. */
const MILD = [
  ["wheat", 5],
  ["barley", 3],
  ["rapeseed", 2],
  ["potato", 1.5],
  ["maize", 1],
  ["hay", 1.5],
  ["pasture", 2],
  ["fallow", 0.6],
];
const HOT = [
  ["maize", 4],
  ["pasture", 3],
  ["potato", 1],
  ["fallow", 1],
  ["hay", 0.5],
];
const COOL = [
  ["barley", 2],
  ["hay", 4],
  ["pasture", 3],
  ["potato", 1.5],
  ["fallow", 0.5],
];

function pick(list, r) {
  let total = 0;
  for (const [, w] of list) total += w;
  let acc = r * total;
  for (const [k, w] of list) {
    acc -= w;
    if (acc < 0) return k;
  }
  return list[list.length - 1][0];
}

const h01 = (...a) => (hash32(...a) >>> 0) / 4294967296;

export class Farmland {
  constructor(world) {
    this.world = world;
    this.seed = world.seed;
    this.wrap = wrapOf(world.config);
    this.season = seasonOf(world.config).id;
  }

  /**
   * The field at a world column: { key (hash), alongX (rows run along x),
   * a, b (position across / along the strip, voxels), edge (distance to the
   * nearest field boundary, voxels), edgeKey (hash of that boundary) }.
   */
  fieldAt(x0, y0) {
    const x = this.wrap.vi(x0);
    const y = this.wrap.vi(y0);
    const bi = Math.floor(x / BLOCK);
    const bj = Math.floor(y / BLOCK);
    const lx = x - bi * BLOCK;
    const ly = y - bj * BLOCK;
    const seed = this.seed;
    const hb = hash32(seed, bi, bj, 0x6a1);
    const alongX = (hb & 1) === 1;
    const across = alongX ? ly : lx;
    const along = alongX ? lx : ly;
    // strips of varying width across the block
    const n = 2 + ((hb >>> 1) % 5);
    let total = 0;
    const w = [];
    for (let k = 0; k < n; k += 1) {
      const v = 0.45 + h01(seed, bi, bj, k, 0x6a2);
      w.push(v);
      total += v;
    }
    let s0 = 0;
    let k = 0;
    for (; k < n - 1; k += 1) {
      const s1 = s0 + (w[k] / total) * BLOCK;
      if (across < s1) break;
      s0 += (w[k] / total) * BLOCK;
    }
    const s1 = k === n - 1 ? BLOCK : s0 + (w[k] / total) * BLOCK;
    // now and then a strip is cut once across
    const hs = hash32(seed, bi, bj, k, 0x6a3);
    const cut = (hs & 3) === 0 ? Math.round(BLOCK * (0.3 + 0.4 * (((hs >>> 2) & 1023) / 1023))) : -1;
    const part = cut >= 0 && along >= cut ? 1 : 0;
    const t0 = part ? cut : 0;
    const t1 = cut >= 0 && !part ? cut : BLOCK;
    const dA = Math.min(across - s0, s1 - across);
    const dB = Math.min(along - t0, t1 - along);
    // (a block's own edges are keyed by the line, so both blocks agree on a hedge or a wall)
    const line = (vertical, i, j) => hash32(seed, i, j, vertical ? 0x6b1 : 0x6b2);
    let edge;
    let edgeKey;
    if (dA <= dB) {
      edge = dA;
      const side = across - s0 < s1 - across ? 0 : 1;
      if (side === 0 && k === 0) edgeKey = alongX ? line(false, bi, bj) : line(true, bi, bj);
      else if (side === 1 && k === n - 1) edgeKey = alongX ? line(false, bi, bj + 1) : line(true, bi + 1, bj);
      else edgeKey = hash32(seed, bi, bj, k + side, 0x6a5);
    } else {
      edge = dB;
      const side = along - t0 < t1 - along ? 0 : 1;
      if (side === 0 && t0 === 0) edgeKey = alongX ? line(true, bi, bj) : line(false, bi, bj);
      else if (side === 1 && t1 === BLOCK) edgeKey = alongX ? line(true, bi + 1, bj) : line(false, bi, bj + 1);
      else edgeKey = hash32(seed, bi, bj, k, 0x6a8);
    }
    return { key: hash32(seed, bi, bj, k, part, 0x6a9), alongX, a: across - s0, b: along - t0, edge, edgeKey };
  }

  /** The crop of a field (key: fieldAt().key) at local temperature t. */
  crop(key, t) {
    return pick(t < 0.42 ? COOL : t > 0.66 ? HOT : MILD, (key >>> 0) / 4294967296);
  }

  /**
   * Ground of a field column: [top, sub, bump] (bump: voxels of a field
   * wall above the ground), for climate t at world column (x, y).
   */
  ground(f, t, x, y) {
    const cold = t < 0.42;
    const ek = (f.edgeKey >>> 0) / 4294967296;
    if (f.edge < MARGIN) {
      // a stone wall along some boundaries in the north, grass margins elsewhere
      if (cold && ek < 0.3 && f.edge < 4 && (hash32(this.seed, this.wrap.vi(x) >> 5, this.wrap.vi(y) >> 5, 0x6aa) & 15) !== 0) {
        const hj = hash32(this.seed, this.wrap.vi(x), this.wrap.vi(y), 0x6ab) & 7;
        return [hj < 2 ? MAT.LICHEN : hj < 4 ? MAT.GRANITE : MAT.STONE, MAT.STONE, f.edge < 2 ? 5 : 3];
      }
      return [f.edge < 2 ? MAT.GRASS_TALL : MAT.GRASS, MAT.DIRT, 0];
    }
    const kind = this.crop(f.key, t);
    const row = Math.floor(f.a / 6) & 1;
    const s = this.season;
    const h = hash32(this.seed, this.wrap.vi(x), this.wrap.vi(y), 0x6ac) & 15;
    switch (kind) {
      case "wheat":
      case "barley":
        if (s === "spring") return [row ? MAT.SOIL_BED : MAT.CROP_GREEN, MAT.DIRT, 0];
        if (s === "autumn") return [(f.key >>> 5) & 1 ? (row ? MAT.SOIL_BED : MAT.DIRT) : row ? MAT.GRASS_STRAW : MAT.DIRT, MAT.DIRT, 0];
        return [kind === "wheat" ? (row && h < 12 ? MAT.CROP_WHEAT : MAT.HAY) : row ? MAT.GRASS_STRAW : MAT.HAY, MAT.DIRT, 0];
      case "rapeseed":
        if (s === "spring") return [h < 13 ? MAT.CROP_RAPE : MAT.CROP_GREEN, MAT.DIRT, 0];
        if (s === "autumn") return [row ? MAT.SOIL_BED : MAT.DIRT, MAT.DIRT, 0];
        return [row ? MAT.GRASS_DRY : MAT.CROP_GREEN, MAT.DIRT, 0];
      case "maize":
        if (s === "summer" || s === "autumn") return [row ? (s === "autumn" ? MAT.GRASS_STRAW : MAT.GRASS_TALL) : MAT.SOIL_BED, MAT.DIRT, 0];
        return [row ? MAT.SOIL_BED : MAT.DIRT, MAT.DIRT, 0];
      case "potato":
        if (s === "summer") return [row ? MAT.GRASS_DARK : MAT.SOIL_BED, MAT.DIRT, 0];
        return [row ? MAT.SOIL_BED : MAT.DIRT, MAT.DIRT, 0];
      case "hay": {
        // mown in wide swaths in summer
        const swath = Math.floor(f.a / 24) & 1;
        if (s === "summer") return [swath ? MAT.GRASS_STRAW : MAT.GRASS, MAT.DIRT, 0];
        return [h < 3 ? MAT.GRASS_DRY : MAT.GRASS, MAT.DIRT, 0];
      }
      case "fallow":
        return [h < 5 ? MAT.GRASS_DRY : h < 6 ? MAT.FLOWER_YELLOW : MAT.GRASS, MAT.DIRT, 0];
      default:
        // pasture: tufted grass
        return [h < 3 ? MAT.GRASS_DARK : MAT.GRASS, MAT.DIRT, 0];
    }
  }

  /**
   * Hedgerow at (x, y): 0..1, high within a metre of a field boundary that
   * carries a hedge (a third of them where it is mild, fewer in the north).
   */
  hedge(f, t) {
    if (f.edge > 12) return 0;
    const ek = (f.edgeKey >>> 0) / 4294967296;
    const share = t < 0.42 ? 0.12 : 0.38;
    // (in the north the walls take the edges the hedges leave)
    if (t < 0.42 ? ek < 0.3 || ek > 0.3 + share : ek > share) return 0;
    return 1 - f.edge / 12;
  }
}
