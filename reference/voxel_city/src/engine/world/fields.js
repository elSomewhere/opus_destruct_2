import { SimplexNoise } from "../core/noise.js";
import { deriveSeed, hashFloat, hash32 } from "../core/hash.js";
import { clamp01, smoothstep, lerp } from "../core/math.js";
import { VOXEL_SIZE } from "../core/units.js";
import { IslandPlan } from "./island.js";
import { wrapOf } from "./wrap.js";

/**
 * Macro fields: continuous, pointwise functions over the whole world. They
 * answer "what kind of place is this?" (urbanization, downtown-ness,
 * moisture, temperature, settlement membership) without any caching or
 * neighborhood state, so they are valid anywhere on an infinite chart.
 *
 * Positions are world VOXEL coordinates; internally converted to meters.
 */
export class MacroFields {
  constructor(config, chart) {
    this.config = config;
    this.chart = chart;
    const seed = config.seed;
    this.nWarp = new SimplexNoise(deriveSeed(seed, "field.warp"));
    this.nUrban = new SimplexNoise(deriveSeed(seed, "field.urban"));
    this.nMoist = new SimplexNoise(deriveSeed(seed, "field.moisture"));
    this.nTemp = new SimplexNoise(deriveSeed(seed, "field.temperature"));
    this.nDistrict = new SimplexNoise(deriveSeed(seed, "field.district"));
    this.nIndustry = new SimplexNoise(deriveSeed(seed, "field.industry"));
    this.nStyle = new SimplexNoise(deriveSeed(seed, "field.style"));
    this.nBelt = new SimplexNoise(deriveSeed(seed, "field.mountainBelt"));
    this.nMassif = new SimplexNoise(deriveSeed(seed, "field.mountainMassif"));
    this.nBeltWarp = new SimplexNoise(deriveSeed(seed, "field.mountainWarp"));
    this.nLobe = new SimplexNoise(deriveSeed(seed, "field.lobes"));
    this.nFringe = new SimplexNoise(deriveSeed(seed, "field.fringe"));
    this.settlementCache = new Map();
    // a wrapping world: settlement and village lattices repeat around it
    this.wrap = wrapOf(config);
    this.nTown = this.wrap.count(config.world.settlementCell);
    this.nVillage = this.wrap.count(config.world.villageCell);
    this.mOff = [0, 0];
    /** island mode: one island in the sea, its settlements sited by the plan (world/island.js) */
    this.island = config.world.mode === "island" ? new IslandPlan(config) : null;
    if (!this.island) this.mOff = this.pickMountainOffset();
  }

  /** Island settlements (towns, villages), sited once. */
  islandSettlements() {
    return this.island.settlements(this);
  }

  /**
   * Distance (m) from a point (voxels) to the shore, positive on land:
   * island mode only (Infinity elsewhere, where the sea is simply terrain
   * below sea level).
   */
  coastDistance(x, y) {
    return this.island ? this.island.coast(x * VOXEL_SIZE, y * VOXEL_SIZE) : Infinity;
  }

  /** Towns whose disk may touch a rect (voxels): the lattice, or the island's list. */
  settlementsIn(rect) {
    if (this.island) return this.islandSettlements().towns.filter((s) => s.x + s.radius * 2.6 >= rect.x0 && s.x - s.radius * 2.6 <= rect.x1 && s.y + s.radius * 2.6 >= rect.y0 && s.y - s.radius * 2.6 <= rect.y1);
    const cell = this.config.world.settlementCell / VOXEL_SIZE;
    const out = [];
    for (let cj = Math.floor(rect.y0 / cell) - 1; cj <= Math.ceil(rect.y1 / cell) + 1; cj += 1)
      for (let ci = Math.floor(rect.x0 / cell) - 1; ci <= Math.ceil(rect.x1 / cell) + 1; ci += 1) {
        const s = this.settlement(ci, cj);
        if (s) out.push(s);
      }
    return out;
  }

  /** Villages and hamlets that may touch a rect (voxels). */
  villagesIn(rect) {
    if (this.island) return this.islandSettlements().villages.filter((v) => v.x + v.radius * 2.2 >= rect.x0 && v.x - v.radius * 2.2 <= rect.x1 && v.y + v.radius * 2.2 >= rect.y0 && v.y - v.radius * 2.2 <= rect.y1);
    const cell = this.config.world.villageCell / VOXEL_SIZE;
    const out = [];
    for (let cj = Math.floor(rect.y0 / cell) - 1; cj <= Math.ceil(rect.y1 / cell) + 1; cj += 1)
      for (let ci = Math.floor(rect.x0 / cell) - 1; ci <= Math.ceil(rect.x1 / cell) + 1; ci += 1) {
        const v = this.village(ci, cj);
        if (v) out.push(v);
      }
    return out;
  }

  /**
   * Shift the mountain fields (a seed-derived domain offset) so the spawn
   * city lies in open country with a range in view: mountainness ~0 at the
   * origin and a high range within `spawnMountains` km. Settlements are kept
   * off mountains anyway; the spawn city is the one that cannot move.
   */
  pickMountainOffset() {
    const [near, far] = this.config.terrain.spawnMountains ?? [10, 35];
    const bs = this.config.terrain.mountainBeltScale;
    const seed = this.config.seed;
    let fallback = null;
    for (let k = 0; k < 96; k += 1) {
      this.mOff = [(hashFloat(seed, k, 41) - 0.5) * 4 * bs, (hashFloat(seed, k, 42) - 0.5) * 4 * bs];
      if (this.mountainness(0, 0) > 0.02) continue;
      // distance (km) to the nearest high ground along 12 rays
      let d = Infinity;
      for (let a = 0; a < 12; a += 1) {
        const ang = (a / 12) * Math.PI * 2;
        for (let r = 4; r <= far; r += 2) {
          if (this.mountainness((Math.cos(ang) * r * 1000) / VOXEL_SIZE, (Math.sin(ang) * r * 1000) / VOXEL_SIZE) > 0.6) {
            d = Math.min(d, r);
            break;
          }
        }
      }
      if (d >= near && d <= far) return this.mOff;
      if (!fallback) fallback = this.mOff;
    }
    return fallback ?? [0, 0];
  }

  /** Settlement anchored in macro cell (i, j) or null. Deterministic, cached. */
  settlement(i, j) {
    if (this.island) return this.islandSettlements().towns.find((t) => t.i === i && t.j === j) ?? null;
    const key = `${i},${j}`;
    let s = this.settlementCache.get(key);
    if (s !== undefined) return s;
    // a wrapping world: the canonical town, moved by whole laps
    const W = this.wrap;
    const ci = W.canon(i, this.nTown);
    const cj = W.canon(j, this.nTown);
    if (ci !== i || cj !== j) {
      const c = this.settlement(ci, cj);
      s = c ? lapCopy(c, W.lap(i, this.nTown) * W.sizeV, W.lap(j, this.nTown) * W.sizeV) : null;
      this.settlementCache.set(key, s);
      return s;
    }
    const w = this.config.world;
    const seed = this.config.seed;
    const cell = w.settlementCell;
    const spawn = i === 0 && j === 0;
    // on a planet face, settlements (and with them roads, highways and
    // sites) keep a wilderness band along the face edges, so neighbouring
    // faces only have to agree on terrain, climate, biomes and rivers
    const edge = this.chart.edgeDistance(i * cell, j * cell);
    const jx = spawn ? 0 : (hashFloat(seed, i, j, 12) - 0.5) * 0.4 * cell;
    const jy = spawn ? 0 : (hashFloat(seed, i, j, 13) - 0.5) * 0.4 * cell;
    const [r0, r1] = w.cityRadius;
    const radius = spawn ? w.spawnCityRadius : lerp(r0, r1, hashFloat(seed, i, j, 14));
    if (!spawn && (hashFloat(seed, i, j, 11) > w.settlementChance || edge < w.faceMargin + w.cityRadius[1] || this.mountainsAround(i * cell + jx, j * cell + jy, radius) > 0.12)) {
      s = null;
    } else {
      s = {
        id: `S${i}_${j}`,
        i,
        j,
        x: (i * cell + jx) / VOXEL_SIZE,
        y: (j * cell + jy) / VOXEL_SIZE,
        radius: radius / VOXEL_SIZE,
        importance: spawn ? 1 : 0.55 + 0.45 * hashFloat(seed, i, j, 15),
        style: hash32(seed, i, j, 16),
      };
      // canonical position (wrapping worlds hash it; equal to x, y elsewhere)
      s.cx = s.x;
      s.cy = s.y;
      // regional climate at the center, for climate-bound city flavors
      s.t = this.temperature(s.x, s.y);
      s.m = this.moisture(s.x, s.y);
    }
    this.settlementCache.set(key, s);
    return s;
  }

  /**
   * Highest mountainness over a town's foothill buffer (its centre and a
   * ring just beyond 2.6 radii; meters). Towns keep off the mountains
   * entirely: fading a high range down to a town would raise cliff walls.
   */
  mountainsAround(mx, my, radius) {
    let m = this.mountainness(mx / VOXEL_SIZE, my / VOXEL_SIZE);
    const R = radius * 2.6 + 1000;
    for (let k = 0; k < 10 && m <= 0.12; k += 1) {
      const a = (k / 10) * Math.PI * 2;
      m = Math.max(m, this.mountainness((mx + Math.cos(a) * R) / VOXEL_SIZE, (my + Math.sin(a) * R) / VOXEL_SIZE));
    }
    return m;
  }

  /**
   * Village or hamlet anchored in village-lattice cell (i, j) or null: a
   * small place (180-760 m) whose urbanization peaks low (a mixed-use
   * main street at most, suburban and farm edges), kept clear of towns and
   * mountains. Villages count as settlements everywhere below (terrain
   * grading, flavors) but never get a downtown, highways or a subway.
   */
  village(i, j) {
    if (this.island) return this.islandSettlements().villages.find((v) => v.i === `v${i}` && v.j === j) ?? null;
    const key = `v${i},${j}`;
    let v = this.settlementCache.get(key);
    if (v !== undefined) return v;
    const W = this.wrap;
    const ci = W.canon(i, this.nVillage);
    const cj = W.canon(j, this.nVillage);
    if (ci !== i || cj !== j) {
      const c = this.village(ci, cj);
      v = c ? lapCopy(c, W.lap(i, this.nVillage) * W.sizeV, W.lap(j, this.nVillage) * W.sizeV) : null;
      this.settlementCache.set(key, v);
      return v;
    }
    v = null;
    const w = this.config.world;
    const seed = this.config.seed;
    const cell = w.villageCell;
    if (hashFloat(seed, i, j, 51) < w.villageChance) {
      const mx = (i + 0.5 + (hashFloat(seed, i, j, 52) - 0.5) * 0.7) * cell;
      const my = (j + 0.5 + (hashFloat(seed, i, j, 53) - 0.5) * 0.7) * cell;
      const k = hashFloat(seed, i, j, 54);
      const radius = lerp(w.villageRadius[0], w.villageRadius[1], k * k);
      const x = mx / VOXEL_SIZE;
      const y = my / VOXEL_SIZE;
      let ok = this.chart.edgeDistance(mx, my) > w.faceMargin + radius && this.mountainsAround(mx, my, radius) <= 0.15;
      // keep clear of the towns (and their suburbs)
      if (ok)
        for (const s of this.nearestSettlements(x, y))
          if (Math.hypot(x - s.x, y - s.y) < s.radius * 1.7 + (radius * 2) / VOXEL_SIZE) {
            ok = false;
            break;
          }
      if (ok) {
        v = {
          id: `V${i}_${j}`,
          i: `v${i}`,
          j,
          village: true,
          hamlet: radius < 330,
          x,
          y,
          radius: radius / VOXEL_SIZE,
          peak: 0.3 + 0.32 * k,
          importance: 0.1,
          style: hash32(seed, i, j, 55),
        };
        v.cx = x;
        v.cy = y;
        v.t = this.temperature(x, y);
        v.m = this.moisture(x, y);
      }
    }
    this.settlementCache.set(key, v);
    return v;
  }

  nearestVillages(x, y) {
    if (this.island) return this.islandSettlements().villages;
    const cellV = this.config.world.villageCell / VOXEL_SIZE;
    const ci = Math.floor(x / cellV);
    const cj = Math.floor(y / cellV);
    const out = [];
    for (let dj = -1; dj <= 1; dj += 1)
      for (let di = -1; di <= 1; di += 1) {
        const v = this.village(ci + di, cj + dj);
        if (v) out.push(v);
      }
    return out;
  }

  nearestSettlements(x, y) {
    if (this.island) return this.islandSettlements().towns;
    const cellV = this.config.world.settlementCell / VOXEL_SIZE;
    const ci = Math.round(x / cellV);
    const cj = Math.round(y / cellV);
    const out = [];
    for (let dj = -1; dj <= 1; dj += 1) {
      for (let di = -1; di <= 1; di += 1) {
        const s = this.settlement(ci + di, cj + dj);
        if (s) out.push(s);
      }
    }
    return out;
  }

  /** Domain-warp factor of settlement distances at a point (shared by all settlements). */
  settlementWarp(x, y) {
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    return 1 + 0.28 * this.nWarp.fbmP(fx / 1800, fy / 1800, fz / 1800, fw / 1800, 3);
  }

  /**
   * Domain-warped normalized distance to a settlement center (0 center, 1
   * edge). Besides the shared regional warp every place has its own lobes
   * (a noise at about its own size), so towns spread out in arms and bays
   * instead of sitting in a round disk.
   */
  settlementDistance(s, x, y, warp = this.settlementWarp(x, y)) {
    const d = Math.hypot(x - s.x, y - s.y) / s.radius;
    // (the lobes fade out far from the place: no noise to evaluate out there)
    const far = smoothstep(2.2, 3.2, d);
    if (far >= 1) return d * warp;
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const sc = Math.max(220, s.radius * VOXEL_SIZE * 0.6);
    const o = ((s.style ?? 0) % 997) * 0.37;
    const lobes = 1 + 0.3 * this.nLobe.fbmP(fx / sc + o, fy / sc - o, fz / sc, fw / sc, 2) * Math.min(1, d * 2) * (1 - far);
    return d * warp * lobes;
  }

  /**
   * Mid-scale noise (-1..1, ~250 m) that frays the edges of towns: blocks
   * and houses at the fringe are built where it is high, fields and woods
   * reach in where it is low.
   */
  fringeNoise(x, y) {
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    return this.nFringe.fbmP(fx / 240, fy / 240, fz / 240, fw / 240, 2);
  }

  /**
   * Urbanization in [0, 1] plus the dominant settlement.
   * Returns { u, core, settlement }.
   */
  urban(x, y) {
    const mode = this.config.world.mode;
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const wobble = 0.12 * this.nUrban.fbmP(fx / 900, fy / 900, fz / 900, fw / 900, 3);
    let best = null;
    let u = 0;
    let core = 0;
    let prox = 0;
    const parts = [];
    const near = this.nearestSettlements(x, y);
    const warp = near.length ? this.settlementWarp(x, y) : 1;
    for (const s of near) {
      const d = this.settlementDistance(s, x, y, warp);
      prox = Math.max(prox, 1 - smoothstep(0.95, 2.6, d));
      // (a wide rim: the town thins out gradually into its outskirts)
      const su = 1 - smoothstep(0.34, 1.06, d + wobble);
      const sc = (1 - smoothstep(0.0, 0.34, d)) * s.importance;
      if (su > 0) parts.push([s, su]);
      if (su > u) {
        u = su;
        best = s;
      }
      if (sc > core) core = sc;
    }
    // villages and hamlets: low urbanization peaks, no core
    if (mode !== "infiniteCity" && u < 0.5) {
      for (const v of this.nearestVillages(x, y)) {
        const d = this.settlementDistance(v, x, y, warp);
        if (d > 2.6) continue;
        prox = Math.max(prox, 1 - smoothstep(0.95, 2.2, d));
        const vu = v.peak * (1 - smoothstep(0.3, 1.02, d + wobble * 0.6));
        if (vu > 0) parts.push([v, vu]);
        if (vu > u) {
          u = vu;
          best = v;
        }
      }
    }
    if (mode === "infiniteCity") {
      u = Math.max(u, 0.72 + 0.28 * this.nUrban.fbmP(fx / 2500, fy / 2500, fz / 2500, fw / 2500, 2));
    }
    return { u: clamp01(u), core: clamp01(core), settlement: best, parts, prox };
  }

  /** 1 inside towns fading to 0 at about twice their radius: foothills around every town. */
  settlementProximity(x, y) {
    const near = this.nearestSettlements(x, y);
    const vil = this.config.world.mode === "infiniteCity" ? [] : this.nearestVillages(x, y);
    if (!near.length && !vil.length) return 0;
    const warp = this.settlementWarp(x, y);
    let p = 0;
    for (const s of near) p = Math.max(p, 1 - smoothstep(0.95, 2.6, this.settlementDistance(s, x, y, warp)));
    for (const v of vil) p = Math.max(p, 1 - smoothstep(0.95, 2.2, this.settlementDistance(v, x, y, warp)));
    return p;
  }

  /**
   * Mountainness 0..1: long mountain belts (ridges of a very low-frequency
   * noise, like fold belts along plate edges) plus isolated massifs. Terrain
   * raises peaks of up to ~6 km where it is 1; settlements avoid it.
   */
  mountainness(x, y) {
    if (this.island) {
      const xm = x * VOXEL_SIZE;
      const ym = y * VOXEL_SIZE;
      const h = this.island.highland(xm, ym);
      return h > 0 ? h * smoothstep(-200, 0, this.island.coast(xm, ym)) : 0;
    }
    const [px, py, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const fx = px + this.mOff[0];
    const fy = py + this.mOff[1];
    const t = this.config.terrain;
    const bs = t.mountainBeltScale;
    const wx = t.mountainBeltWarp * bs * this.nBeltWarp.fbmP(fx / (bs * 0.8), fy / (bs * 0.8), fz / (bs * 0.8), fw / (bs * 0.8), 2);
    const belt = 1 - Math.abs(this.nBelt.fbmP((fx + wx) / bs, (fy - wx) / bs, fz / bs, fw / bs, 2));
    const [b0, b1] = t.mountainBelt;
    const m1 = smoothstep(b0, b1, belt);
    const ms = t.mountainMassifScale;
    const [q0, q1] = t.mountainMassif;
    const m2 = 0.85 * smoothstep(q0, q1, this.nMassif.fbmP(fx / ms, fy / ms, fz / ms, fw / ms, 3));
    return Math.max(m1, m2);
  }

  /**
   * Moisture 0..1: broad wet / dry regions (tens of km) with some regional
   * variation, so biomes form coherent zones rather than speckle.
   */
  moisture(x, y) {
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const c = this.config.world.climateScale;
    const cl = this.config.world.climate;
    const n = this.nMoist.fbmP(fx / (c * 0.55), fy / (c * 0.55), fz / (c * 0.55), fw / (c * 0.55), 3);
    if (cl?.moisture !== undefined) return cl.moisture + (cl.moistureVar ?? 0.08) * n;
    return 0.5 + 0.62 * n;
  }

  /**
   * Temperature 0..1 at sea level: a very broad noise field plus the
   * chart's latitude (poles cold) when the chart has one (planet faces).
   * LandCover cools it with altitude.
   */
  temperature(x, y) {
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const c = this.config.world.climateScale;
    const cl = this.config.world.climate;
    if (cl?.temperature !== undefined) return cl.temperature + (cl.temperatureVar ?? 0.05) * this.nTemp.fbmP(fx / c, fy / c, fz / c, fw / c, 3);
    let t = 0.5 + 0.6 * this.nTemp.fbmP(fx / c, fy / c, fz / c, fw / c, 3);
    const lat = this.chart.latitude ? this.chart.latitude(fx, fy, fz, fw) : null;
    if (lat !== null) t = 0.25 * t + 0.95 - 0.9 * Math.abs(lat);
    return t;
  }

  /** Low-frequency noise used to vary district mix within a city (-1..1). */
  districtNoise(x, y) {
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    return this.nDistrict.fbmP(fx / 1100, fy / 1100, fz / 1100, fw / 1100, 2);
  }

  /**
   * Industry: scattered industrial patches (noise) plus each town's
   * industrial quarter, a wide sector of its outer ring on a side picked per
   * town, whose far part turns into heavy industry.
   */
  industryNoise(x, y) {
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    let q = 0;
    for (const s of this.nearestSettlements(x, y)) {
      const d = Math.hypot(x - s.x, y - s.y) / s.radius;
      if (d < 0.3 || d > 1.2) continue;
      const th = hashFloat(this.config.seed, s.i, s.j, 17) * Math.PI * 2;
      const c = Math.cos(Math.atan2(y - s.y, x - s.x) - th);
      const ang = smoothstep(0.3, 0.65, c);
      const rad = smoothstep(0.34, 0.5, d) * (1 - smoothstep(0.98, 1.15, d));
      q = Math.max(q, ang * rad * (0.45 + 0.35 * smoothstep(0.55, 0.85, d)));
    }
    // (an island town keeps its industry to its quarter: a few plants, not a belt)
    return this.nIndustry.fbmP(fx / 1600, fy / 1600, fz / 1600, fw / 1600, 2) * (this.island ? 0.35 : 1) + q;
  }

  styleNoise(x, y) {
    const [fx, fy, fz, fw] = this.chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    return this.nStyle.fbmP(fx / 900, fy / 900, fz / 900, fw / 900, 2);
  }
}

/**
 * A settlement or village of a wrapping world seen `dx`, `dy` voxels (whole
 * laps) away from its canonical cell: same record, moved; lazily cached
 * per-copy fields (graded base height, port lake) start over.
 */
function lapCopy(c, dx, dy) {
  const copy = { ...c, x: c.x + dx, y: c.y + dy };
  delete copy.baseH;
  delete copy.portLake;
  delete copy.plan;
  return copy;
}
