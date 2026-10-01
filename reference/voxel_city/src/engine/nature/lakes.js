import { SimplexNoise } from "../core/noise.js";
import { deriveSeed, hashFloat } from "../core/hash.js";
import { LRU } from "../core/lru.js";
import { smoothstep } from "../core/math.js";
import { wrapOf } from "../world/wrap.js";

/**
 * Lakes: natural basins on a jittered lattice (one candidate per ~3.5 km
 * cell); a few are big lakes several kilometres across (towns on their
 * shores get a port). A lake has an irregular shore (noise on the radius), a bowl-shaped
 * bed and a water level just below the lowest point of its rim, so water
 * never stands above the surrounding land. Lakes form in lowlands and in
 * the mountains alike (alpine lakes) but keep out of towns.
 *
 * Like rivers they are pure functions of position that only the ground tile
 * carves; planning keeps lots and sites off them with `hitsRect`.
 */

export class Lakes {
  constructor(world) {
    this.world = world;
    this.cfg = world.config.lakes;
    /** lattice cell (voxels); a wrapping world has n cells around it */
    this.cell = (this.cfg.cell ?? 3500) * 8;
    this.wrap = wrapOf(world.config);
    this.n = this.wrap.count(this.cfg.cell ?? 3500);
    this.shore = new SimplexNoise(deriveSeed(world.seed, "lake.shore"));
    this.cache = new LRU(512);
    this.out = { level: 0, bed: 0, k: 0, lake: null };
  }

  /** Lake of lattice cell (a, b), or null. */
  lake(a, b) {
    const key = `${a},${b}`;
    let L = this.cache.get(key);
    if (L !== undefined) return L;
    L = this.build(a, b);
    this.cache.set(key, L);
    return L;
  }

  build(a, b) {
    const w = this.world;
    const seed = w.seed;
    // a wrapping world: the canonical lake, moved by whole laps
    const W = this.wrap;
    const ca = W.canon(a, this.n);
    const cb = W.canon(b, this.n);
    if (ca !== a || cb !== b) {
      const L = this.lake(ca, cb);
      if (!L) return null;
      const dx = W.lap(a, this.n) * W.sizeV;
      const dy = W.lap(b, this.n) * W.sizeV;
      return { ...L, x: L.x + dx, y: L.y + dy, port: L.port };
    }
    const CELL = this.cell;
    if (!this.cfg.enabled || hashFloat(seed, a, b, 601) > this.cfg.chance) return null;
    let x = Math.round((a + 0.2 + 0.6 * hashFloat(seed, a, b, 602)) * CELL);
    let y = Math.round((b + 0.2 + 0.6 * hashFloat(seed, a, b, 603)) * CELL);
    // raw terrain: harbour grading depends on the lakes, not the other way round
    let ts = w.terrain.sample(x, y, null, true);
    // now and then a big lake (kilometres across) in the lowlands
    const big = ts.mountain < 0.2 && hashFloat(seed, a, b, 606) < this.cfg.bigChance;
    // (lakes.scale shrinks the ordinary lakes: tarns on a small island)
    const r0 = (big ? 1400 + 2600 * hashFloat(seed, a, b, 604) : (ts.mountain > 0.3 ? 90 + 260 * hashFloat(seed, a, b, 604) : 150 + 650 * hashFloat(seed, a, b, 604)) * (this.cfg.scale ?? 1)) * 8;
    // half the big lakes lie right against a town: its shore becomes a harbour front
    let port = null;
    if (big && hashFloat(seed, a, b, 607) < 0.5) {
      let best = null;
      for (const s of w.fields.nearestSettlements(x, y)) {
        const d = Math.hypot(s.x - x, s.y - y);
        if (!best || d < best.d) best = { s, d };
      }
      if (best && best.d < r0 + best.s.radius + 12000 * 8) {
        const s = best.s;
        const dx = (x - s.x) / (best.d || 1);
        const dy = (y - s.y) / (best.d || 1);
        const dist = s.radius * 0.92 + r0;
        x = Math.round(s.x + dx * dist);
        y = Math.round(s.y + dy * dist);
        ts = w.terrain.sample(x, y, null, true);
        port = s.id;
      }
    }
    if (ts.u > 0.03 || (!port && w.fields.settlementProximity(x, y) > (this.cfg.townProximity ?? 0.2))) return null;
    // island: lakes lie well inside the shore
    if (w.fields.island && w.fields.coastDistance(x, y) < (r0 / 8) * 1.7 + (this.cfg.scale ?? 1) * 120) return null;
    const mountain = ts.mountain;
    if (big && mountain > 0.2) return null;
    // water level: below the lowest point of the rim
    let lo = ts.h;
    const nr = big ? 40 : 16;
    for (let k = 0; k < nr; k += 1) {
      const t = (k / nr) * Math.PI * 2;
      lo = Math.min(lo, w.terrain.sample(x + Math.cos(t) * r0 * 1.2, y + Math.sin(t) * r0 * 1.2, null, true).h);
    }
    const level = Math.floor(lo - (r0 < 800 ? 8 : 12));
    if (level < w.config.world.seaLevel * 8 + 8) return null;
    const depth = Math.round((Math.min(3, 1 + r0 / 400) + 12 * Math.min(1, r0 / (700 * 8)) + (big ? 25 * Math.min(1, r0 / (4000 * 8)) : 0)) * 8);
    // cx, cy: canonical centre (the shore noise follows it, so every lap has the same shore)
    return { id: `L${a}_${b}`, x, y, cx: x, cy: y, r0, level, depth, big, port, reach: r0 * 1.55 };
  }

  /** The big lake a town was built against (its port), or null. Cached on the settlement. */
  portLakeOf(s) {
    if (s.portLake !== undefined) return s.portLake;
    const R = s.radius + 12000 * 8 + 4000 * 8;
    s.portLake = this.near({ x0: s.x - R, y0: s.y - R, x1: s.x + R, y1: s.y + R }).find((L) => L.port === s.id) ?? null;
    return s.portLake;
  }

  /** Lakes whose reach overlaps a rect. */
  near(rect) {
    const out = [];
    const CELL = this.cell;
    for (let b = Math.floor(rect.y0 / CELL) - 2; b <= Math.floor(rect.y1 / CELL) + 2; b += 1)
      for (let a = Math.floor(rect.x0 / CELL) - 2; a <= Math.floor(rect.x1 / CELL) + 2; a += 1) {
        const L = this.lake(a, b);
        if (!L) continue;
        if (L.x + L.reach < rect.x0 || L.x - L.reach > rect.x1 || L.y + L.reach < rect.y0 || L.y - L.reach > rect.y1) continue;
        out.push(L);
      }
    return out;
  }

  /** Normalized shore distance k (< 1 inside the water) of a point for lake L. */
  shoreK(L, x, y, smooth = false) {
    const dx = x - L.x;
    const dy = y - L.y;
    const ang = Math.atan2(dy, dx);
    const n = this.shore.fbm2(Math.cos(ang) * 1.3 + L.cx * 1e-5, Math.sin(ang) * 1.3 + L.cy * 1e-5, 3);
    // (shore wiggles in the lake's canonical frame)
    const lx = x - L.x + L.cx;
    const ly = y - L.y + L.cy;
    // small bays and points: at most ~40 m on any lake; big lakes also get
    // bays that scale with their size (a fixed-scale wiggle proportional to
    // the radius would make kilometre-sized lake shores jagged)
    const n2 = smooth ? 0 : this.shore.n2(lx / 700, ly / 700);
    const bays = L.r0 > 8000 ? 0.08 * L.r0 * this.shore.n2(lx / (L.r0 * 0.7) + 3.3, ly / (L.r0 * 0.7)) : 0;
    return Math.hypot(dx, dy) / (L.r0 * (1 + 0.35 * n) + Math.min(0.12 * L.r0, 320) * n2 + bays);
  }

  /**
   * Lake info at (x, y) or null: { level, bed (voxel z of the bottom), k
   * (normalized shore distance, < 1 in the water) }. Shared object.
   */
  at(x, y) {
    if (!this.cfg.enabled) return null;
    const a = Math.floor(x / this.cell);
    const b = Math.floor(y / this.cell);
    for (let j = b - 2; j <= b + 2; j += 1)
      for (let i = a - 2; i <= a + 2; i += 1) {
        const L = this.lake(i, j);
        if (!L || Math.abs(x - L.x) > L.reach || Math.abs(y - L.y) > L.reach) continue;
        const k = this.shoreK(L, x, y);
        if (k > 1.35) continue;
        const o = this.out;
        o.lake = L;
        o.k = k;
        o.level = L.level;
        o.bed = k < 1 ? Math.round(L.level - L.depth * (1 - k * k) - 4) : L.level + Math.round((k - 1) * 90);
        return o;
      }
    return null;
  }

  /**
   * Big-lake shore near a land point: { lake, dist (voxels to the water's
   * edge), nx, ny (unit vector from the lake centre) } or null. Towns build
   * their port where it is close.
   */
  shoreNear(x, y, maxDist) {
    if (!this.cfg.enabled) return null;
    let best = null;
    for (const L of this.near({ x0: x - maxDist, y0: y - maxDist, x1: x + maxDist, y1: y + maxDist })) {
      if (!L.big) continue;
      const k = this.shoreK(L, x, y);
      const dist = (k - 1) * L.r0;
      if (dist < -maxDist || dist > maxDist) continue;
      if (!best || Math.abs(dist) < Math.abs(best.dist)) {
        const d = Math.hypot(x - L.x, y - L.y) || 1;
        best = { lake: L, dist, nx: (x - L.x) / d, ny: (y - L.y) / d };
      }
    }
    return best;
  }

  /** Carved ground (voxels) for a column whose surface is h. */
  groundAt(info, h) {
    if (info.k < 1) return Math.min(h, info.bed);
    // shore: the land eases down to the water
    const t = smoothstep(1, 1.35, info.k);
    return Math.min(h, Math.round(info.level + 2 + t * (h - info.level - 2)));
  }

  hitsRect(r, marginM = 6) {
    if (!this.cfg.enabled) return false;
    const pad = marginM * 8;
    for (const L of this.near({ x0: r.x0 - pad, y0: r.y0 - pad, x1: r.x1 + pad, y1: r.y1 + pad })) {
      const cx = Math.max(r.x0, Math.min(r.x1, L.x));
      const cy = Math.max(r.y0, Math.min(r.y1, L.y));
      if (Math.hypot(cx - L.x, cy - L.y) < L.r0 * 1.5 + pad) {
        // sample the rect edges and interior for a real overlap
        const step = 64;
        for (let y = r.y0; y <= r.y1; y += step)
          for (let x = r.x0; x <= r.x1; x += step) if (this.shoreK(L, x, y) < 1.1 + pad / L.r0) return true;
      }
    }
    return false;
  }
}
