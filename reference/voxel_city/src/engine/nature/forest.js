import { hash32, deriveSeed, Rng } from "../core/hash.js";
import { SimplexNoise } from "../core/noise.js";
import { smoothstep } from "../core/math.js";
import { wrapOf } from "../world/wrap.js";
import { vx } from "../core/units.js";
import { rasterizeTree, treeBounds, TREE_KINDS } from "./trees.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../network/roadSurface.js";
import { LRU } from "../core/lru.js";
import { padDistance } from "../world/sites.js";
import { seasonOf } from "../world/season.js";
import { naturalGroundAt } from "../voxel/compose.js";
import { YAWS, PITCHES, YAW_QUARTER } from "../core/placement.js";

/**
 * Natural vegetation: a jittered tree grid (5 m) over the whole world,
 * thinned by the land-cover forest density and suppressed on roads, lots,
 * urban open spaces and water. Pure function of position => seamless.
 *
 * Forests have structure rather than a uniform scatter:
 *   - stands: every species of the biome follows its own noise (~190 m),
 *     so the woods come in patches (a spruce stand, a birch grove, pines on
 *     the rocks) with mixed borders; birches and rowans favour edges and
 *     open ground, pines outcrops, spruces the dense wood
 *   - clumps and gaps: a ~30 m noise thickens and thins the trees
 *   - growth form: crowded trees grow taller with narrow crowns (and
 *     conifers lose their low branches), open-grown ones stay broad; young
 *     trees fill gaps and edges; now and then a dead snag in old conifer wood
 *   - understory (LOD 0-1, a 2.5 m lattice): ferns, blueberry, shrubs and
 *     junipers, saplings, fallen logs and stumps; shrubs and young trees
 *     line the forest edge
 *
 * In the angled world (world.angles, features.vegetation) the woods grow
 * wild (nature/trees.js): trees stray up to STRAY beyond their lattice cell,
 * lean, fork and lop, snags lean, logs lie along the ground at exact
 * angles or rest windthrown on their root plates. Each keeps within the
 * reach the lattices search (MAX_R, U_R, less the stray): the feature
 * source's budgets are those of the axis-aligned woods.
 */
const CELL = vx(5);
const MAX_R = vx(8.5);
const MAX_H = vx(32);
const UCELL = vx(2.5);
const U_R = vx(3.5);
/**
 * How far round a rect the lattices are searched for plants reaching into
 * it (voxels): past the farthest any reaches from its root (a jungle
 * tree's 71, a hazel's 40, a fallen log's 30; test/nature.test.js), so no
 * chunk misses the tip of one rooted beyond its neighbours. (MAX_R and U_R
 * stay the reach a wild tree keeps within: the budgets S2 was held to.)
 */
export const TREE_SEARCH = vx(10);
export const UNDER_SEARCH = vx(6);
/** How far (voxels) a wild tree may stray beyond its lattice cell. */
const STRAY = vx(1.25);
/** Exact tilts a wild log may lie at along the ground: level, the grades, then the yaw table's first angles (to 25°). */
const LOG_TILTS = [...PITCHES, ...YAWS.slice(1, 9)];

export class Forest {
  constructor(world) {
    this.world = world;
    this.cache = new LRU(20000);
    this.cache2 = new LRU(20000);
    this.ucache = new LRU(40000);
    this.rs = makeRoadSample();
    // a wrapping world: the tree lattices repeat with it
    this.wrap = wrapOf(world.config);
    this.n = this.wrap.on ? this.wrap.sizeV / CELL : 0;
    this.nU = this.wrap.on ? this.wrap.sizeV / UCELL : 0;
    this.nStand = new SimplexNoise(deriveSeed(world.seed, "forest.stand"));
    this.nClump = new SimplexNoise(deriveSeed(world.seed, "forest.clump"));
    this.season = seasonOf(world.config);
    const a = world.config.world.angles;
    this.wild = !!(a?.enabled && a.features?.vegetation !== false);
  }

  /** Trees per cell after clumping: a ~30 m noise thickens and thins the wood. */
  clumped(x, y, density) {
    const c = this.world.landCover.fieldNoise(this.nClump, x, y, 240);
    return Math.min(1, density * (0.55 + 0.9 * smoothstep(-0.45, 0.45, c)));
  }

  /**
   * Share (0..1) of conifers among the trees at (x, y) (hM: ground height,
   * m): the stand mix `species` draws from, with cold ground turning
   * broadleaves to conifers as the near trees do. The distant canopy uses it
   * so far forests show the same spruce stands and birch groves.
   */
  coniferShare(x, y, hM) {
    const lc = this.world.landCover;
    const biome = lc.biomeAt(x, y, hM);
    const { t } = lc.climate(x, y, hM);
    let con = 0;
    let all = 0;
    const list = biome.trees;
    for (let i = 0; i < list.length; i += 1) {
      const [kind, w0] = list[i];
      if (kind === "shrub" || kind === "shrubDry" || kind === "cactus") continue;
      const n = lc.fieldNoise(this.nStand, x, y, 1500, i * 7.31, -i * 3.17);
      const w = w0 * (0.1 + 2 * smoothstep(-0.25, 0.45, n));
      all += w;
      const coldConifer = t < 0.38 && !(kind === "birch" || kind === "rowan" || kind === "aspen" || kind === "alder");
      if (kind === "pine" || kind === "spruce" || kind === "larch" || kind === "dwarfpine" || kind === "juniper" || coldConifer) con += w;
    }
    return all > 0 ? con / all : 0;
  }

  /**
   * Species of a tree: the biome's weights, each modulated by its own stand
   * noise (~190 m) and by the site (edges, rock, dense wood).
   */
  species(biome, x, y, r, density, outcrop) {
    const lc = this.world.landCover;
    const list = biome.trees;
    let total = 0;
    const ws = [];
    for (let i = 0; i < list.length; i += 1) {
      const [kind, w0] = list[i];
      const n = lc.fieldNoise(this.nStand, x, y, 1500, i * 7.31, -i * 3.17);
      let f = 0.1 + 2 * smoothstep(-0.25, 0.45, n);
      if (kind === "pine") f *= 1 + 4 * outcrop;
      // larches: the dry continental taiga (Russia, Siberia), hardly on wet Atlantic coasts
      else if (kind === "larch") f *= (1 + 3 * outcrop) * smoothstep(0.62, 0.45, this.world.fields.moisture(x, y));
      else if (kind === "birch" || kind === "rowan") f *= 1 + 1.6 * (1 - density);
      else if (kind === "aspen") f *= 0.6 + 1.2 * (1 - density);
      else if (kind === "spruce") f *= 0.4 + density;
      const w = w0 * f;
      ws.push(w);
      total += w;
    }
    let acc = r * total;
    for (let i = 0; i < list.length; i += 1) {
      acc -= ws[i];
      if (acc < 0) return list[i][0];
    }
    return list[list.length - 1][0];
  }

  treeAt(gx, gy) {
    const key = gx * 100003 + gy;
    let t = this.cache.get(key);
    if (t !== undefined) return t;
    t = this.computeTree(gx, gy);
    this.cache.set(key, t);
    return t;
  }

  /** The second tree of a cell in the densest stands (or null). */
  treeAt2(gx, gy) {
    const key = gx * 100003 + gy;
    let t = this.cache2.get(key);
    if (t !== undefined) return t;
    t = this.computeTree(gx, gy, true);
    this.cache2.set(key, t);
    return t;
  }

  /**
   * The tree of lattice cell (gx, gy), or null. `second`: the cell's second
   * tree, only in the densest stands (a younger, smaller one of the stand's
   * species), so thick woods are thicker than one tree per cell.
   */
  computeTree(gx, gy, second = false) {
    const w = this.world;
    const seed = w.seed;
    const cgx = this.wrap.canon(gx, this.n);
    const cgy = this.wrap.canon(gy, this.n);
    const salt = second ? 20 : 0;
    const h0 = hash32(seed, cgx, cgy, 311 + salt);
    // (wild: anywhere up to STRAY beyond the cell, so neighbours bunch and part)
    const x = this.wild ? gx * CELL - STRAY + (h0 & 1023) * ((CELL + 2 * STRAY) / 1024) : gx * CELL + (h0 & 1023) * (CELL / 1024);
    const y = this.wild ? gy * CELL - STRAY + ((h0 >>> 10) & 1023) * ((CELL + 2 * STRAY) / 1024) : gy * CELL + ((h0 >>> 10) & 1023) * (CELL / 1024);
    const ts = w.terrain.sample(x, y);
    if (ts.u > 0.22) return null;
    const hM = ts.h / 8;
    const biome = w.landCover.biomeAt(x, y, hM);
    const { t: tLocal } = w.landCover.climate(x, y, hM);
    // (farm country: fields, not woods)
    const farm = this.farmAt(x, y);
    const density = farm ? 0 : this.clumped(x, y, w.landCover.forestDensity(x, y, ts.u, biome, hM));
    const meadow = farm ? 0 : biome.meadow * w.landCover.treeLine(tLocal);
    // the banks of rivers, lakes and creeks carry a belt of trees even in
    // open country; hedgerows between some fields hold a tree now and then
    const bank = tLocal > 0.24 ? this.freshBank(x, y, ts) : 0;
    const hedge = w.landCover.hedgeAt(x, y, ts.u, biome, tLocal, farm);
    const roll = ((h0 >>> 20) & 1023) / 1024;
    if (second ? density < 0.7 || roll > (density - 0.7) * 2.6 : roll > Math.max(density, meadow, bank * 0.5 * w.landCover.treeLine(tLocal), hedge * 0.45)) return null;
    if (hM < w.config.world.seaLevel + 1.5) return null;
    if (!this.clearGround(x, y, ts, biome)) return null;
    const h1 = hash32(seed, cgx, cgy, 312 + salt);
    const h2 = hash32(seed, cgx, cgy, 314 + salt);
    // the ground as the tiles shape it, and its slope: no trees on scarps or
    // embankment faces, fewer on steep banks
    const gx0 = Math.round(x);
    const gy0 = Math.round(y);
    const z0 = naturalGroundAt(w, gx0, gy0);
    const slope = Math.hypot(naturalGroundAt(w, gx0 + 6, gy0) - naturalGroundAt(w, gx0 - 6, gy0), naturalGroundAt(w, gx0, gy0 + 6) - naturalGroundAt(w, gx0, gy0 - 6)) / 12;
    if (slope > 1.1 || (slope > 0.75 && ((h2 >>> 9) & 3) !== 0)) return null;
    let kind = this.species(biome, x, y, (h1 & 0xffff) / 0x10000, density, ts.outcrop ?? 0);
    // willows, alders' stand-ins (birches, poplars) along fresh water
    if (bank > 0.25 && ((h2 >>> 12) & 1023) / 1024 < bank * 0.75 && kind !== "cactus" && kind !== "palm" && kind !== "jungle") {
      const q = ((h2 >>> 22) & 1023) / 1024;
      kind = tLocal > 0.4 ? (q < 0.45 ? "willow" : q < 0.7 ? "alder" : q < 0.85 ? "birch" : "poplar") : q < 0.3 ? "willow" : q < 0.65 ? "alder" : "birch";
    }
    // hedgerow trees: oaks, rowans, hawthorn (blossom) and birches; in the north birches and rowans
    if (hedge > 0.3 && density < 0.3) {
      const q = ((h2 >>> 14) & 1023) / 1024;
      kind = tLocal < 0.42 ? (q < 0.6 ? "birch" : "rowan") : q < 0.35 ? "oak" : q < 0.55 ? "blossom" : q < 0.75 ? "rowan" : q < 0.9 ? "birch" : "maple";
    }
    // dry, steep, sunny ground: pines take over from spruces
    if (kind === "spruce" && slope > 0.3 && ((h2 >>> 4) & 31) / 31 < (slope - 0.3) * 2.5) kind = "pine";
    // mountain forests: broadleaves give way to conifers (hardy birches and
    // rowans stay), then to krummholz and stunted mountain birch near the tree line
    if (tLocal < 0.38 && (kind === "oak" || kind === "maple" || kind === "autumn" || kind === "willow" || kind === "jungle" || kind === "palm" || kind === "acacia")) kind = (h1 >>> 3) & 1 ? "spruce" : "pine";
    if (tLocal < 0.3 && (kind === "pine" || kind === "spruce")) kind = "dwarfpine";
    const stunted = tLocal < 0.3 && (kind === "birch" || kind === "rowan" || kind === "aspen") ? 0.55 : 1;
    // growth form: crowded = taller and slimmer, open-grown = broad; young trees in gaps and at edges
    const dense = smoothstep(0.35, 0.85, density);
    let hs = (0.9 + 0.2 * dense) * stunted;
    let rs = (1.25 - 0.2 * dense) * (0.4 + 0.6 * stunted);
    const young = ((h2 & 1023) / 1024) < 0.07 + 0.12 * (1 - dense);
    if (young) {
      hs *= 0.3 + 0.3 * (((h2 >>> 10) & 255) / 255);
      rs *= 0.4 + 0.25 * (((h2 >>> 18) & 255) / 255);
    } else if (second) {
      // (the understorey of the stand: a younger tree under the canopy)
      hs *= 0.45 + 0.35 * (((h2 >>> 10) & 255) / 255);
      rs *= 0.55 + 0.3 * (((h2 >>> 18) & 255) / 255);
    }
    // a dead snag now and then in old conifer wood
    if (!young && !second && dense > 0.5 && (kind === "spruce" || kind === "pine") && ((h2 >>> 26) & 63) === 0) kind = "snag";
    const spec = TREE_KINDS[kind] ?? TREE_KINDS.oak;
    const a = ((h1 >>> 16) & 255) / 255;
    const b = ((h1 >>> 24) & 255) / 255;
    const hgt = vx(spec.h[0] + (spec.h[1] - spec.h[0]) * a) * hs;
    const r = vx(spec.r[0] + (spec.r[1] - spec.r[0]) * b) * (kind === "snag" ? 1 : rs);
    // (on the ground as the tiles shape it: cut and fill beside roads, graded town ground)
    const t = { x: gx0, y: gy0, z: z0 + 1, h: Math.max(3, Math.round(hgt)), r, kind, seed: h1, open: dense < 0.3 };
    // (wild: within the lattice's reach, less the stray)
    if (this.wild) Object.assign(t, { wild: true, reach: MAX_R - 1 - STRAY });
    // the season: leaf colours, bare branches, snow
    t.look = this.season.treeLook(kind, h1, tLocal);
    t.bb = treeBounds(t);
    return t;
  }

  /** Is (x, y) in farm country (a rural block of its cell plan)? */
  farmAt(x, y) {
    const c = this.world.cellAt(x, y);
    return this.world.cellPlan(c.i, c.j).farmAt(x, y);
  }

  /**
   * How much (x, y) is a river, lake or creek bank (0..1): full 2-8 m from
   * the water, fading out by 16 m (the sea shore is not: salt, wind).
   */
  freshBank(x, y, ts) {
    const w = this.world;
    let d = Infinity;
    const ri = w.rivers && w.config.rivers.enabled ? w.rivers.at(x, y) : null;
    if (ri) d = Math.min(d, ri.d - ri.half);
    const lk = w.lakes && w.config.lakes.enabled ? w.lakes.at(x, y) : null;
    if (lk) d = Math.min(d, ((lk.k - 1) * lk.lake.r0) / 8);
    if (ts.stream) d = Math.min(d, ts.stream.d - ts.stream.half);
    if (!(d < 16) || d < 2) return 0;
    return d < 8 ? 1 : 1 - (d - 8) / 8;
  }

  understoryAt(gx, gy) {
    const key = gx * 100003 + gy;
    let t = this.ucache.get(key);
    if (t !== undefined) return t;
    t = this.computeUnderstory(gx, gy);
    this.ucache.set(key, t);
    return t;
  }

  /**
   * One understory plant per 2.5 m cell at most: ferns, blueberry, shrubs,
   * junipers and saplings under and at the edge of the wood, fallen logs
   * and stumps (most cells stay empty).
   */
  computeUnderstory(gx, gy) {
    const w = this.world;
    const seed = w.seed;
    const cgx = this.wrap.canon(gx, this.nU);
    const cgy = this.wrap.canon(gy, this.nU);
    const h0 = hash32(seed, cgx, cgy, 411);
    const roll = ((h0 >>> 20) & 1023) / 1024;
    if (roll > 0.66) return null;
    const x = gx * UCELL + (h0 & 1023) * (UCELL / 1024);
    const y = gy * UCELL + ((h0 >>> 10) & 1023) * (UCELL / 1024);
    const ts = w.terrain.sample(x, y);
    if (ts.u > 0.12) return null;
    const hM = ts.h / 8;
    if (hM < w.config.world.seaLevel + 1.2) return null;
    const lc = w.landCover;
    const biome = lc.biomeAt(x, y, hM);
    const { t: tLocal } = lc.climate(x, y, hM);
    if (tLocal < 0.24) return null;
    const farm = this.farmAt(x, y);
    const density = farm ? 0 : this.clumped(x, y, lc.forestDensity(x, y, ts.u, biome, hM));
    // hedgerows: a dense line of shrubs along some field boundaries
    const hedge = density < 0.3 ? lc.hedgeAt(x, y, ts.u, biome, tLocal, farm) : 0;
    if (hedge > 0.25) {
      if (roll > 0.2 + 0.7 * hedge || !this.clearGround(x, y, ts, biome)) return null;
      const h1 = hash32(seed, cgx, cgy, 413);
      const kind = (h1 & 7) === 0 && tLocal > 0.42 ? "blossom" : (h1 & 7) === 1 ? "berry" : "shrub";
      const spec = TREE_KINDS[kind];
      const a = ((h1 >>> 16) & 255) / 255;
      const b = ((h1 >>> 24) & 255) / 255;
      const small = kind === "blossom" ? 0.35 : 1;
      const t = {
        x: Math.round(x),
        y: Math.round(y),
        z: naturalGroundAt(w, Math.round(x), Math.round(y)) + 1,
        h: Math.max(2, Math.round(vx(spec.h[0] + (spec.h[1] - spec.h[0]) * a) * small)),
        r: vx(spec.r[0] + (spec.r[1] - spec.r[0]) * b) * (kind === "blossom" ? 0.5 : 1.1),
        kind,
        seed: h1,
        under: true,
      };
      if (this.wild) Object.assign(t, { wild: true, reach: U_R - 1 });
      t.look = this.season.treeLook(kind, h1, tLocal);
      t.bb = treeBounds(t);
      return t;
    }
    if (density < 0.1 || biome.forest <= 0.2) return null;
    const boreal = tLocal < 0.42;
    const tropical = biome.id === "tropical";
    // (patches: ferns and blueberry carpet parts of the floor)
    const patch = lc.fieldNoise(this.nStand, x, y, 360, 17.3, 5.1);
    let kind = null;
    let q = roll;
    const take = (k, p) => {
      if (kind || q >= p) {
        q -= p;
        return;
      }
      kind = k;
    };
    // thickets: patches of dense young growth (after a windthrow, a clearing, a fire)
    const thicket = smoothstep(0.35, 0.65, lc.fieldNoise(this.nClump, x, y, 520, 31.7, -12.9));
    // rugged ground (boulders, hummocks): more deadwood and junipers, rowans in the gaps
    const R = ts.rugged ?? 0;
    if (density > 0.35) {
      if (tropical) {
        take("shrub", 0.3);
        take("fern", 0.2);
        take("sapling", 0.06);
      } else if (boreal) {
        take("sapling", 0.05 + 0.3 * thicket);
        take("berry", patch > 0 ? 0.14 : 0.03);
        take("fern", patch < -0.1 ? 0.18 : 0.03);
        take("juniper", (ts.outcrop ?? 0) > 0.2 ? 0.08 : 0.02 + 0.03 * R);
        take("log", 0.024 + 0.02 * R);
        take("stump", 0.014 + 0.01 * R);
        take("shrub", 0.02);
      } else {
        take("sapling", 0.06 + 0.28 * thicket);
        take("fern", patch > -0.1 ? 0.22 : 0.05);
        take("hazel", 0.05 + 0.05 * (1 - density));
        take("shrub", 0.06);
        take("log", 0.022);
        take("stump", 0.012);
        take("berry", 0.02);
      }
    } else {
      // the forest edge: a fringe of shrubs, hazels, junipers and young trees
      take("shrub", 0.18);
      if (boreal) take("juniper", 0.06);
      else take("hazel", 0.06);
      take("sapling", 0.07 + 0.2 * thicket);
    }
    if (!kind) return null;
    if (!this.clearGround(x, y, ts, biome)) return null;
    const h1 = hash32(seed, cgx, cgy, 412);
    let k2 = kind;
    let hs = 1;
    let rs = 1;
    if (kind === "sapling") {
      k2 = this.species(biome, x, y, (h1 & 0xffff) / 0x10000, density, ts.outcrop ?? 0);
      if (tLocal < 0.38 && (k2 === "oak" || k2 === "maple" || k2 === "willow" || k2 === "jungle" || k2 === "palm" || k2 === "acacia")) k2 = "spruce";
      if (k2 === "cactus" || k2 === "palm") return null;
      hs = 0.12 + 0.12 * (((h1 >>> 16) & 255) / 255);
      rs = 0.3 + 0.15 * (((h1 >>> 24) & 255) / 255);
    }
    const spec = TREE_KINDS[k2] ?? TREE_KINDS.shrub;
    const a = ((h1 >>> 16) & 255) / 255;
    const b = ((h1 >>> 24) & 255) / 255;
    const t = {
      x: Math.round(x),
      y: Math.round(y),
      z: naturalGroundAt(w, Math.round(x), Math.round(y)) + 1,
      h: Math.max(2, Math.round(vx(spec.h[0] + (spec.h[1] - spec.h[0]) * a) * hs)),
      r: vx(spec.r[0] + (spec.r[1] - spec.r[0]) * b) * rs,
      kind: k2,
      seed: h1,
      under: true,
    };
    if (this.wild) {
      Object.assign(t, { wild: true, reach: U_R - 1 });
      if (k2 === "log") this.layLog(t, cgx, cgy);
    }
    t.look = this.season.treeLook(k2, h1, tLocal);
    t.bb = treeBounds(t);
    return t;
  }

  /**
   * A wild log's lie: an exact yaw, and either the exact tilt nearest the
   * ground's fall along it (t.tilt), or, on level ground now and then,
   * windthrown on its root plate (t.plate).
   */
  layLog(t, cgx, cgy) {
    const rng = Rng.from(this.world.seed, "forest.log", cgx, cgy);
    t.yaw = rng.int(0, 2 * YAW_QUARTER - 1);
    const Y = YAWS[t.yaw];
    const hl = t.r / 2;
    const ex = Math.round((Y.c / Y.r) * hl);
    const ey = Math.round((Y.s / Y.r) * hl);
    const rise = naturalGroundAt(this.world, t.x + ex, t.y + ey) - naturalGroundAt(this.world, t.x - ex, t.y - ey);
    const run = 2 * Math.sqrt(ex * ex + ey * ey) || 1;
    let best = LOG_TILTS[0];
    let bestCos = -Infinity;
    for (const T of LOG_TILTS) {
      const cos = (T.c * run + T.s * Math.abs(rise)) / T.r;
      if (cos > bestCos) {
        bestCos = cos;
        best = T;
      }
    }
    t.tilt = rise < 0 ? { c: best.c, s: -best.s, r: best.r } : best;
    t.plate = best.s === 0 && rng.chance(0.4);
  }

  understoryIn(rect, out = []) {
    const gx0 = Math.floor((rect.x0 - UNDER_SEARCH) / UCELL);
    const gy0 = Math.floor((rect.y0 - UNDER_SEARCH) / UCELL);
    const gx1 = Math.floor((rect.x1 + UNDER_SEARCH) / UCELL);
    const gy1 = Math.floor((rect.y1 + UNDER_SEARCH) / UCELL);
    for (let gy = gy0; gy <= gy1; gy += 1)
      for (let gx = gx0; gx <= gx1; gx += 1) {
        const t = this.understoryAt(gx, gy);
        if (t && t.bb.x1 >= rect.x0 && t.bb.x0 <= rect.x1 && t.bb.y1 >= rect.y0 && t.bb.y0 <= rect.y1) out.push(t);
      }
    return out;
  }

  /**
   * Is (x, y) open natural ground (not a road, lot, urban space, site pad,
   * water, stream, pool or highway corridor)? Trees and boulders ask.
   */
  clearGround(x, y, ts, biome) {
    const w = this.world;
    const seed = w.seed;
    // not on roads / lots / urban spaces
    const c = w.cellAt(x, y);
    const view = w.roadView(c.i, c.j);
    const cands = view.near({ x0: x - 2, y0: y - 2, x1: x + 2, y1: y + 2 });
    sampleRoadSurface(cands, x + 0.5, y + 0.5, this.rs, seed);
    if (this.rs.kind !== KIND.NONE || this.rs.sdfR < vx(2)) return false;
    const plan = w.cellPlan(c.i, c.j);
    if (plan.lotAt(x, y) || plan.spaceAt(x, y)) return false;
    if (w.sites) {
      for (const s of w.sites.sitesNear({ x0: x - 40, y0: y - 40, x1: x + 40, y1: y + 40 })) {
        // areas the site keeps clear of trees (default: its pad and driveway)
        const clear = s.plan.clear ?? [s.blend, s.plan.drive];
        for (const d of clear) if (d && x >= d.x0 - 24 && x <= d.x1 + 24 && y >= d.y0 - 24 && y <= d.y1 + 24) return false;
        // winding roads clear a strip along their path only
        for (const p of s.pads ?? []) {
          if (!p.path) continue;
          const r = p.rect;
          const m = p.margin + 24;
          if (x < r.x0 - m || x > r.x1 + m || y < r.y0 - m || y > r.y1 + m) continue;
          if (padDistance(p, x, y) < p.margin * 0.6 + 16) return false;
        }
      }
    }
    if (w.landCover.poolDepth(x, y, biome, 0, ts.u)) return false;
    if (w.landmarks && w.landmarks.blocks(x, y)) return false;
    // bare rock: only a stunted pine here and there in the cracks
    if (ts.outcrop > 0.55 && (hash32(seed, Math.round(x), Math.round(y), 313) & 7) !== 0) return false;
    if (w.isWet && w.isWet(x, y, 3)) return false;
    // not in a creek or canyon stream
    if (ts.stream && ts.stream.d < ts.stream.half + 1.5) return false;
    if (w.highways) {
      const hw = w.highways;
      const reach = hw.hw + vx(9);
      if (hw.nearest(x, y, hw.edgesNear({ x0: x - reach, y0: y - reach, x1: x + reach, y1: y + reach }), reach)) return false;
    }
    return true;
  }

  treesIn(rect, out = []) {
    const gx0 = Math.floor((rect.x0 - TREE_SEARCH) / CELL);
    const gy0 = Math.floor((rect.y0 - TREE_SEARCH) / CELL);
    const gx1 = Math.floor((rect.x1 + TREE_SEARCH) / CELL);
    const gy1 = Math.floor((rect.y1 + TREE_SEARCH) / CELL);
    for (let gy = gy0; gy <= gy1; gy += 1) {
      for (let gx = gx0; gx <= gx1; gx += 1) {
        const t = this.treeAt(gx, gy);
        if (t && t.bb.x1 >= rect.x0 && t.bb.x0 <= rect.x1 && t.bb.y1 >= rect.y0 && t.bb.y0 <= rect.y1) out.push(t);
        const t2 = this.treeAt2(gx, gy);
        if (t2 && t2.bb.x1 >= rect.x0 && t2.bb.x0 <= rect.x1 && t2.bb.y1 >= rect.y0 && t2.bb.y0 <= rect.y1) out.push(t2);
      }
    }
    return out;
  }
}

export const forestSource = {
  id: "forest",
  order: 7,
  // from streaming.canopyLod on, the ground tile lays a canopy heightfield instead (compose)
  maxLod: 5,
  zRange(world, rect, lod) {
    if (lod >= (world.config.streaming.canopyLod ?? 4)) return null;
    // quick reject in urban areas
    const cx = (rect.x0 + rect.x1) / 2;
    const cy = (rect.y0 + rect.y1) / 2;
    if (world.fields.urban(cx, cy).u > 0.45 && world.fields.urban(rect.x0, rect.y0).u > 0.45) return null;
    let lo = Infinity;
    let hi = -Infinity;
    const items = world.forest.treesIn(rect);
    if (lod <= 1) world.forest.understoryIn(rect, items);
    for (const t of items) {
      if (t.z < lo) lo = t.z;
      if (t.bb.z1 > hi) hi = t.bb.z1;
    }
    return lo === Infinity ? null : [lo - 2, Math.min(hi, lo + MAX_H + 8)];
  },
  rasterize(world, chunk) {
    if (chunk.lod >= (world.config.streaming.canopyLod ?? 4)) return;
    const box = chunk.worldBox;
    const cx = (box.x0 + box.x1) / 2;
    const cy = (box.y0 + box.y1) / 2;
    if (world.fields.urban(cx, cy).u > 0.45 && world.fields.urban(box.x0, box.y0).u > 0.45 && world.fields.urban(box.x1, box.y1).u > 0.45) return;
    // (each tree carries its seasonal look; the understory up close only)
    for (const t of world.forest.treesIn(box)) rasterizeTree(chunk, t);
    if (chunk.lod <= 1) for (const t of world.forest.understoryIn(box)) rasterizeTree(chunk, t);
  },
};
