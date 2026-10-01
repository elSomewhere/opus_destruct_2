import { SimplexNoise } from "../core/noise.js";
import { deriveSeed, hashFloat, hash32 } from "../core/hash.js";
import { smoothstep, lerp, smin, clamp01 } from "../core/math.js";
import { VOXEL_SIZE } from "../core/units.js";

/**
 * Island mode: the land is one island of `config.world.island.radius` in an
 * endless sea. The plan is a set of pure functions of position (meters)
 * plus a handful of settlements sited once per world:
 *
 *   coast(x, y)     approximate distance (m) to the shore, positive inland:
 *                   a warped ellipse with bays, headlands and points, and
 *                   fjords that wind into the highlands from the sea
 *   highland(x, y)  0..1 mask of the fells / mountains on one side of the
 *                   island (MacroFields.mountainness in island mode)
 *   cliff(x, y)     0..1 coast type: 0 low shores with beaches, 1 cliffs
 *                   (highland coasts always end in cliffs and fjord walls)
 *   skerry(x, y)    metres of rock islets rising from the shelf
 *
 * The island's main town sits at the world origin (the spawn): the plan
 * sites it on a sheltered stretch of lowland coast and shifts the whole
 * island so it lands there. Smaller towns, villages and hamlets follow,
 * spread over the lowlands, some on the shore, some inland in the forest.
 * Everything else (terrain, roads, districts, forests, biomes) is the usual
 * pipeline reading these fields, so the island is fully mapped by the biome
 * system like any other land.
 */
export class IslandPlan {
  constructor(config) {
    this.config = config;
    const cfg = config.world.island;
    this.cfg = cfg;
    const seed = config.seed;
    this.seed = seed;
    const noise = (name) => new SimplexNoise(deriveSeed(seed, `island.${name}`));
    this.nWarpU = noise("warpU");
    this.nWarpV = noise("warpV");
    this.nBays = noise("bays");
    this.nPoints = noise("points");
    this.nHigh = noise("highland");
    this.nFjord = noise("fjord");
    this.nFjordWarp = noise("fjordWarp");
    this.nCliff = noise("cliff");
    this.nSkerry = noise("skerry");
    this.R = cfg.radius;
    this.theta = hashFloat(seed, 901) * Math.PI;
    const e = Math.max(1, cfg.elongation ?? 1.4);
    this.a = this.R * Math.sqrt(e);
    this.b = this.R / Math.sqrt(e);
    this.cos = Math.cos(this.theta);
    this.sin = Math.sin(this.theta);
    // the highlands lie towards one side of the island
    const hd = hashFloat(seed, 902) * Math.PI * 2;
    this.hdx = Math.cos(hd);
    this.hdy = Math.sin(hd);
    // (each count gives two rays: a small island has a handful of fjords, a big one a dozen)
    this.fjordCount = Math.max(2, Math.round((2.5 + 2.5 * hashFloat(seed, 903)) * Math.min(1.5, Math.sqrt(this.R / 8000))));
    this.fjordPhase = hashFloat(seed, 904) * Math.PI * 2;
    // island-local frame: world meters = local - origin (the main town at 0, 0)
    this.ox = 0;
    this.oy = 0;
    this.memo = { x: NaN, y: NaN, c: 0 };
    this.highThreshold = this.calibrateHighlands();
    this.sites = this.siteSettlements();
  }

  /** Island-local coordinates (m) of a world point (m). */
  local(x, y) {
    return [x + this.ox, y + this.oy];
  }

  /** Coast distance of the smooth island shape (no fjords), island-local meters. */
  shape(lx, ly) {
    const R = this.R;
    const rough = this.cfg.roughness ?? 0.5;
    const s = R * 0.9;
    // big headlands and peninsulas: a domain warp of the ellipse
    const wu = R * 0.3 * rough * this.nWarpU.fbm2(lx / s + 3.1, ly / s, 3);
    const wv = R * 0.3 * rough * this.nWarpV.fbm2(lx / s, ly / s - 5.3, 3);
    const u = lx * this.cos + ly * this.sin + wu;
    const v = -lx * this.sin + ly * this.cos + wv;
    const rho = Math.hypot(u / this.a, v / this.b);
    let d = (1 - rho) * R;
    // far offshore the detail does not matter
    if (d < -R * 0.6) return d;
    // bays and points at a few hundred metres to a few kilometres
    d += R * 0.13 * rough * this.nBays.fbm2(lx / (R * 0.32), ly / (R * 0.32), 3);
    d += 170 * rough * this.nPoints.fbm2(lx / 650, ly / 650, 3);
    return d;
  }

  /** Highland value before thresholding (0..~1.4): a tilt towards one side plus broad noise. */
  highRaw(lx, ly) {
    const R = this.R;
    const proj = (lx * this.hdx + ly * this.hdy) / R;
    return 0.5 + 0.42 * proj + 0.38 * this.nHigh.fbm2(lx / (R * 0.55), ly / (R * 0.55), 3);
  }

  /** Threshold on highRaw so the highlands cover about `highlands` of the island. */
  calibrateHighlands() {
    const share = clamp01(this.cfg.highlands ?? 0.45);
    const vals = [];
    const R = this.R * 1.4;
    for (let j = -20; j <= 20; j += 1)
      for (let i = -20; i <= 20; i += 1) {
        const lx = (i / 20) * R;
        const ly = (j / 20) * R;
        if (this.shape(lx, ly) > 0) vals.push(this.highRaw(lx, ly));
      }
    if (!vals.length || share <= 0) return Infinity;
    vals.sort((p, q) => p - q);
    return vals[Math.min(vals.length - 1, Math.floor((1 - share) * vals.length))];
  }

  /** 0..1 highland mask (island-local meters). */
  highlandLocal(lx, ly) {
    if (this.highThreshold === Infinity) return 0;
    return smoothstep(this.highThreshold - 0.04, this.highThreshold + 0.2, this.highRaw(lx, ly));
  }

  /**
   * Fjords: warped rays from the island's heart out to the sea, active in
   * the highlands. Returns the coast distance cut by them.
   */
  fjordCut(lx, ly, d) {
    const F = this.cfg.fjords ?? 0;
    if (F <= 0 || d < -200) return d;
    const size = Math.min(1.6, this.R / 8000);
    const reach = (900 + 3800 * F) * size;
    if (d > reach) return d;
    const hi = this.highlandLocal(lx, ly);
    if (hi < 0.2) return d;
    const fn = (px, py) => {
      const w = 1.3 * this.nFjordWarp.fbm2(px / 2200, py / 2200, 3);
      return Math.sin(this.fjordCount * Math.atan2(py, px) + this.fjordPhase + w);
    };
    const v = fn(lx, ly);
    if (Math.abs(v) > 0.7) return d;
    const e = 20;
    const gx = (fn(lx + e, ly) - v) / e;
    const gy = (fn(lx, ly + e) - v) / e;
    const dist = Math.abs(v) / Math.max(Math.hypot(gx, gy), 1 / 2500);
    // the fjord narrows from its mouth to its head; faded in over the highland edge
    const t = clamp01(d / reach);
    const half = (70 + 200 * F) * Math.sqrt(size) * Math.pow(1 - t, 0.6) * smoothstep(0.2, 0.55, hi) * (1 - smoothstep(0.5, 0.7, Math.abs(v)));
    if (half <= 0) return d;
    return smin(d, dist - half, 70);
  }

  /** Coast distance (m, positive inland) at a world point (meters). */
  coast(x, y) {
    const m = this.memo;
    if (x === m.x && y === m.y) return m.c;
    const lx = x + this.ox;
    const ly = y + this.oy;
    const c = this.fjordCut(lx, ly, this.shape(lx, ly));
    m.x = x;
    m.y = y;
    m.c = c;
    return c;
  }

  /** Highland mask at a world point (meters), faded out at the shore. */
  highland(x, y) {
    const lx = x + this.ox;
    const ly = y + this.oy;
    return this.highlandLocal(lx, ly);
  }

  /** Coast type 0 (beaches) .. 1 (cliffs) at a world point (meters). */
  cliff(x, y) {
    const lx = x + this.ox;
    const ly = y + this.oy;
    // about `cliffs` of the shore is rocky (the noise's quantile), the rest low
    const share = this.cfg.cliffs ?? 0.35;
    const n = this.nCliff.fbm2(lx / 1400, ly / 1400, 3) * 0.5 + 0.5;
    const t = 0.5 + (0.5 - share) * 0.55;
    const k = smoothstep(t - 0.07, t + 0.07, n);
    return Math.max(k, smoothstep(0.3, 0.65, this.highlandLocal(lx, ly)));
  }

  /** Metres of skerry rock above the shelf at a world point (meters) with coast distance c. */
  skerry(x, y, c) {
    const S = this.cfg.skerries ?? 0;
    if (S <= 0 || c > -30 || c < -1600) return 0;
    const lx = x + this.ox;
    const ly = y + this.oy;
    const zone = smoothstep(-1600, -700, c) * (1 - smoothstep(-90, -30, c));
    const n = this.nSkerry.fbm2(lx / 170, ly / 170, 3) + 0.5 * this.nSkerry.n2(lx / 45 + 7.7, ly / 45);
    const thr = 0.78 - 0.32 * S;
    return n > thr ? (n - thr) * 34 * zone : 0;
  }

  /**
   * Site the settlements (island-local meters), then move the island so the
   * main town lands on the origin. Candidates on a 150 m grid are scored on
   * low ground; the town wants a sheltered stretch of coast with room to
   * grow inland, villages and hamlets keep their distance from each other.
   */
  siteSettlements() {
    const cfg = this.cfg;
    const seed = this.seed;
    const R = this.R;
    const pop = cfg.population ?? 20000;
    // a compact northern town: ~4000 people per km² inside its radius
    // (a small island's town of a few thousand is a few hundred metres across)
    const townR = Math.max(R < 4000 ? 380 : 700, 1300 * Math.sqrt(pop / 20000));
    // smaller places keep proportionally closer on a small island
    const gapK = Math.min(1, R / 4500);
    const step = Math.max(150, R / 60);
    const cands = [];
    for (let ly = -R * 1.7; ly <= R * 1.7; ly += step)
      for (let lx = -R * 1.7; lx <= R * 1.7; lx += step) {
        const jx = lx + (hashFloat(seed, Math.round(lx), Math.round(ly), 911) - 0.5) * step * 0.6;
        const jy = ly + (hashFloat(seed, Math.round(lx), Math.round(ly), 912) - 0.5) * step * 0.6;
        const c = this.fjordCut(jx, jy, this.shape(jx, jy));
        if (c < 120) continue;
        cands.push({ x: jx, y: jy, c, hi: this.highlandLocal(jx, jy), k: hashFloat(seed, Math.round(jx), Math.round(jy), 913) });
      }
    const landShare = (x, y, r) => {
      let land = 0;
      let n = 0;
      for (let a = 0; a < 16; a += 1)
        for (const f of [0.35, 0.7, 1]) {
          const t = (a / 16) * Math.PI * 2;
          const px = x + Math.cos(t) * r * f;
          const py = y + Math.sin(t) * r * f;
          n += 1;
          if (this.fjordCut(px, py, this.shape(px, py)) > 0) land += 1;
        }
      return land / n;
    };
    const highAround = (x, y, r) => {
      let m = this.highlandLocal(x, y);
      for (let a = 0; a < 12; a += 1) {
        const t = (a / 12) * Math.PI * 2;
        m = Math.max(m, this.highlandLocal(x + Math.cos(t) * r, y + Math.sin(t) * r));
      }
      return m;
    };
    // main town: on the coast (its centre 300-900 m inland), mostly land around it, low ground
    let best = null;
    for (const p of cands) {
      if (p.c > townR * 0.75 + 300 || p.c < Math.min(250, townR * 0.5)) continue;
      const hiA = highAround(p.x, p.y, townR * 1.6);
      if (hiA > 0.35) continue;
      const land = landShare(p.x, p.y, townR * 1.1);
      if (land < 0.55) continue;
      // a sheltered bay: some sea close by, but most of the ring is land
      const score = land * 1.2 - hiA - Math.abs(p.c - townR * 0.45) / townR * 0.4 + p.k * 0.15;
      if (!best || score > best.score) best = { ...p, score };
    }
    if (!best) {
      // fallback: the lowest-highland land point nearest the shore
      for (const p of cands) {
        const score = -p.hi - p.c / R;
        if (!best || score > best.score) best = { ...p, score };
      }
    }
    const town = best ?? { x: 0, y: 0, c: 0 };
    this.ox = town.x;
    this.oy = town.y;
    const sites = [{ kind: "town", lx: town.x, ly: town.y, radius: townR, importance: 1 }];
    // smaller places: small towns and villages spread over the lowlands,
    // hamlets anywhere below the fells; alternate shore and forest sites
    const want = [];
    for (let k = 0; k < (cfg.towns ?? 0); k += 1) want.push({ kind: "smallTown", r: [520, 760], gap: 3600 * gapK });
    for (let k = 0; k < (cfg.villages ?? 0); k += 1) want.push({ kind: "village", r: gapK < 1 ? [220, 340] : [280, 520], gap: 2600 * gapK });
    for (let k = 0; k < (cfg.hamlets ?? 0); k += 1) want.push({ kind: "hamlet", r: gapK < 1 ? [120, 200] : [150, 260], gap: 1700 * gapK });
    want.forEach((w, n) => {
      const shore = n % 2 === 0;
      let pick = null;
      for (const p of cands) {
        if (p.hi > (w.kind === "hamlet" ? 0.45 : 0.25)) continue;
        const rr = lerp(w.r[0], w.r[1], p.k);
        if (p.c < rr * 0.5 + 120) continue;
        let near = Infinity;
        for (const s of sites) near = Math.min(near, Math.hypot(p.x - s.lx, p.y - s.ly) - s.radius);
        if (near < w.gap) continue;
        const coastal = shore ? -Math.abs(p.c - rr * 0.6 - 200) / 800 : -Math.abs(p.c - R * 0.35) / R;
        const score = coastal - p.hi * 0.8 - Math.max(0, 1 - near / (w.gap * 2)) * 0.3 + p.k * 0.25;
        if (!pick || score > pick.score) pick = { ...p, score, rr };
      }
      if (pick) sites.push({ kind: w.kind, lx: pick.x, ly: pick.y, radius: pick.rr, importance: w.kind === "smallTown" ? 0.35 : 0.1 });
    });
    return sites;
  }

  /**
   * Settlement records in the shape MacroFields hands out (voxel coords):
   * towns (the main town is `S0_0` at the origin) and villages / hamlets.
   * `fields` fills in the regional climate.
   */
  settlements(fields) {
    if (this.towns) return { towns: this.towns, villages: this.villages };
    const towns = [];
    const villages = [];
    this.sites.forEach((s, n) => {
      const x = (s.lx - this.ox) / VOXEL_SIZE;
      const y = (s.ly - this.oy) / VOXEL_SIZE;
      const style = hash32(this.seed, n, 71);
      if (s.kind === "town" || s.kind === "smallTown") {
        towns.push({
          id: n === 0 ? "S0_0" : `S${n}_1000`,
          i: n,
          j: n === 0 ? 0 : 1000,
          x,
          y,
          radius: s.radius / VOXEL_SIZE,
          importance: s.importance,
          style,
          flavor: this.cfg.flavor ?? null,
          island: true,
        });
      } else {
        const hamlet = s.kind === "hamlet";
        villages.push({
          id: `V${n}_1000`,
          i: `v${n}`,
          j: 1000,
          village: true,
          hamlet,
          x,
          y,
          radius: s.radius / VOXEL_SIZE,
          peak: hamlet ? 0.3 : 0.42 + 0.12 * hashFloat(this.seed, n, 72),
          importance: 0.1,
          style,
          flavor: this.cfg.flavor ?? null,
          island: true,
        });
      }
    });
    for (const s of [...towns, ...villages]) {
      s.t = fields.temperature(s.x, s.y);
      s.m = fields.moisture(s.x, s.y);
    }
    this.towns = towns;
    this.villages = villages;
    return { towns, villages };
  }

  /**
   * The main town's harbour: the shore point nearest its centre (world
   * meters) and the direction from the town out to the water, or null.
   */
  harbour() {
    if (this._harbour !== undefined) return this._harbour;
    let best = null;
    for (let a = 0; a < 72; a += 1) {
      const t = (a / 72) * Math.PI * 2;
      const dx = Math.cos(t);
      const dy = Math.sin(t);
      for (let r = 40; r < 4000 && (!best || r < best.r); r += 20) {
        if (this.coast(dx * r, dy * r) >= 0) continue;
        best = { r, x: dx * r, y: dy * r, dx, dy };
        break;
      }
    }
    this._harbour = best;
    return best;
  }

  /**
   * Country trunk roads joining every place on the island (world, lazy):
   * settlements are linked into a tree (each to its nearest already linked
   * one) and each link follows the arterial grid by A*, avoiding the sea
   * and steep ground. Returns a Set of edge keys `${axis}:${line}:${span}`
   * (see city/cellNetwork.js edgeInfo) that always carry a road.
   */
  trunkEdges(world) {
    if (this._trunk) return this._trunk;
    const edges = new Set();
    this._trunk = edges;
    const { towns, villages } = this.settlements(world.fields);
    const places = [...towns, ...villages];
    if (places.length < 2) return edges;
    const A = world.arterials;
    const nodeCache = new Map();
    const node = (i, j) => {
      const k = `${i},${j}`;
      let n = nodeCache.get(k);
      if (!n) {
        const x = A.line(0, i);
        const y = A.line(1, j);
        const c = this.coast(x / 8, y / 8);
        n = { i, j, x, y, c, h: c > 0 ? world.terrain.sample(x, y).h / 8 : 0 };
        nodeCache.set(k, n);
      }
      return n;
    };
    const edgeCost = (a, b) => {
      if (a.c < 25 || b.c < 25) return Infinity;
      // the midpoint and quarter points must be on land too
      for (const t of [0.25, 0.5, 0.75]) if (this.coast((a.x + (b.x - a.x) * t) / 8, (a.y + (b.y - a.y) * t) / 8) < 25) return Infinity;
      const len = Math.hypot(b.x - a.x, b.y - a.y) / 8;
      const grade = Math.abs(b.h - a.h) / len;
      if (grade > 0.16) return Infinity;
      return len * (1 + 40 * grade * grade + 2.5 * this.highland(a.x / 8, a.y / 8));
    };
    const route = (s, t) => {
      const a = A.cellAt(s.x, s.y);
      const b = A.cellAt(t.x, t.y);
      const key = (n) => `${n.i},${n.j}`;
      const start = node(a.i, a.j);
      const goal = node(b.i, b.j);
      const g = new Map([[key(start), 0]]);
      const prev = new Map();
      const open = [{ n: start, f: 0 }];
      const hEst = (n) => Math.hypot(n.x - goal.x, n.y - goal.y) / 8;
      let iter = 0;
      while (open.length && iter < 6000) {
        iter += 1;
        let bi = 0;
        for (let k = 1; k < open.length; k += 1) if (open[k].f < open[bi].f) bi = k;
        const { n } = open[bi];
        open.splice(bi, 1);
        if (n === goal) break;
        const gn = g.get(key(n));
        for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const m = node(n.i + di, n.j + dj);
          const cost = edgeCost(n, m);
          if (cost === Infinity) continue;
          const gm = gn + cost;
          if (gm < (g.get(key(m)) ?? Infinity)) {
            g.set(key(m), gm);
            prev.set(key(m), n);
            open.push({ n: m, f: gm + hEst(m) });
          }
        }
      }
      if (!prev.has(key(goal))) return;
      for (let n = goal; n !== start; ) {
        const p = prev.get(key(n));
        if (p.i === n.i) edges.add(`0:${n.i}:${Math.min(p.j, n.j)}`);
        else edges.add(`1:${n.j}:${Math.min(p.i, n.i)}`);
        n = p;
      }
    };
    // a tree over the places: each joins the nearest one already linked
    const linked = [places[0]];
    const rest = places.slice(1);
    while (rest.length) {
      let best = null;
      for (const p of rest)
        for (const q of linked) {
          const d = Math.hypot(p.x - q.x, p.y - q.y);
          if (!best || d < best.d) best = { p, q, d };
        }
      route(best.q, best.p);
      linked.push(best.p);
      rest.splice(rest.indexOf(best.p), 1);
    }
    return edges;
  }

  /** Axis-aligned bounds (world meters) that contain the whole island and its skerries. */
  bounds() {
    const r = this.R * 2.1 + 2000;
    return { x0: -this.ox - r, y0: -this.oy - r, x1: -this.ox + r, y1: -this.oy + r };
  }
}

/**
 * Terrain parameters of an island: highland relief scaled to the island's
 * peak height and size (a 1 km fell country, not 6 km ranges).
 */
export function islandTerrain(config) {
  const cfg = config.world.island;
  const peak = cfg.peak ?? 950;
  const k = peak / 950;
  const s = Math.min(1.6, Math.max(0.55, cfg.radius / 8000));
  // a small island (a few km) gets finer relief: hills and knolls at its own scale
  const small = cfg.radius < 4000;
  return {
    lowlandBase: small ? 18 : 30,
    continentAmplitude: 0,
    hillAmplitude: small ? 16 : 22,
    hillScale: small ? 900 : 1800,
    detailAmplitude: small ? 5 : 6,
    detailScale: small ? 260 : 700,
    cityRelief: small ? 6 : 4,
    cityReliefScale: small ? 700 : 2600,
    mountainBase: 70 * k,
    mountainUplift: 260 * k,
    mountainHeight: 720 * k,
    mountainScale: 5200 * s,
    mountainDetail: 110 * k,
    mountainDetailScale: 900,
    valleyScale: 3000 * s,
    valleyDepth: 230 * k,
    plateauHeight: 0,
    canyonDepth: 0,
    ravineDepth: 22,
    // glaciated island country: knolls, hollows, boulders and bare rock
    rugged: 0.15,
  };
}
