import { hashFloat, hash32 } from "../core/hash.js";
import { vx } from "../core/units.js";
import { LRU } from "../core/lru.js";
import { smoothstep } from "../core/math.js";
import { Registry } from "./registry.js";
import { wrapOf } from "./wrap.js";

/**
 * Sites: rare, self-contained special places (military bases, research
 * facilities, later villages, ruins, airports...) anchored on a coarse
 * lattice. A site owns a rectangle inside one arterial cell (clear of the
 * cell's boundary roads), levels the ground to a pad, and provides:
 *
 *   surface(world, site)  -> { envelopes, props }  merged into its cell plan
 *   structure(site)       -> underground / custom geometry (feature source)
 *   ground(site, x, y)    -> surface material override inside the pad
 *   place(world, cand)    -> optional custom placement: null to reject, or
 *                            { padZ, pads: [{ rect, z, margin, round, ramp? | path?, half }],
 *                              footprint } (default: one level pad over the
 *                            rect, rejected on steep ground)
 *
 * New site kinds register in SITES; placement rules (urbanization window,
 * frequency) live on the definition.
 */
export const SITES = new Registry("site");

/**
 * Path pads: a ribbon of half width `half` along a polyline of points
 * { x, y, z } (a road on a slope). Returns the distance beyond the ribbon
 * edge and leaves the interpolated level, the arc length and the distance to
 * the centre line on the pad (p._z, p._s, p._c) for padZ / materials.
 */
function pathDistance(p, x, y) {
  const pts = p.path;
  let best = Infinity;
  let s0 = 0;
  for (let k = 0; k + 1 < pts.length; k += 1) {
    const a = pts[k];
    const b = pts[k + 1];
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const L2 = dx * dx + dy * dy || 1;
    const t = Math.max(0, Math.min(1, ((x - a.x) * dx + (y - a.y) * dy) / L2));
    const d = Math.hypot(x - (a.x + dx * t), y - (a.y + dy * t));
    const L = Math.sqrt(L2);
    if (d < best) {
      best = d;
      p._z = a.z + (b.z - a.z) * t;
      p._s = s0 + L * t;
      p._c = d;
    }
    s0 += L;
  }
  return Math.max(0, best - p.half);
}

/** Distance (voxels) from (x, y) to a pad's footprint (0 inside). */
export function padDistance(p, x, y) {
  if (p.path) return pathDistance(p, x, y);
  const r = p.rect;
  return Math.hypot(Math.max(r.x0 - x, 0, x - r.x1), Math.max(r.y0 - y, 0, y - r.y1));
}

/**
 * Level of a pad at (x, y): flat (`z`) or a ramp rising linearly along
 * `ramp.axis` from `z0` at `a0` to `z1` at `a1` (roads on slopes).
 */
function padZ(p, x, y) {
  if (p.path) return p._z;
  const r = p.ramp;
  if (!r) return p.z;
  const a = r.axis === "x" ? x : y;
  const t = Math.max(0, Math.min(1, (a - r.a0) / (r.a1 - r.a0 || 1)));
  return r.z0 + (r.z1 - r.z0) * t;
}

export class SiteLayer {
  constructor(world) {
    this.world = world;
    this.cache = new LRU(256);
    const cellM = world.config.world.siteCell ?? 5200;
    this.cell = vx(cellM);
    // a wrapping world has n site cells around it
    this.wrap = wrapOf(world.config);
    this.n = this.wrap.count(cellM);
  }

  /** Site anchored at lattice cell (a,b) or null. */
  siteAt(a, b) {
    const key = `${a},${b}`;
    let s = this.cache.get(key);
    if (s !== undefined) return s;
    s = this.build(a, b);
    this.cache.set(key, s);
    return s;
  }

  build(a, b) {
    const w = this.world;
    const seed = w.seed;
    const defs = SITES.all();
    if (!defs.length) return null;
    // a wrapping world: canonical seeds, the anchor moved by whole laps
    // (the site itself is planned again in place, so its geometry is local)
    const W = this.wrap;
    const ca = W.canon(a, this.n);
    const cb = W.canon(b, this.n);
    const lapX = W.lap(a, this.n) * W.sizeV;
    const lapY = W.lap(b, this.n) * W.sizeV;
    const r = hashFloat(seed, ca, cb, 881);
    let acc = 0;
    let def = null;
    for (const d of defs) {
      acc += d.frequency;
      if (r < acc) {
        def = d;
        break;
      }
    }
    if (!def) return null;
    // anchor: jittered point -> its arterial cell -> centered rect inside it
    const px = (ca + 0.5 + (hashFloat(seed, ca, cb, 882) - 0.5) * 0.6) * this.cell + lapX;
    const py = (cb + 0.5 + (hashFloat(seed, ca, cb, 883) - 0.5) * 0.6) * this.cell + lapY;
    if (w.chart.edgeDistance(px / 8, py / 8) < w.config.world.faceMargin) return null;
    const c = w.cellAt(px, py);
    const cr = w.arterials.cellRect(c.i, c.j);
    const ur = w.fields.urban((cr.x0 + cr.x1) / 2, (cr.y0 + cr.y1) / 2);
    if (ur.u < def.minU || ur.u > def.maxU) return null;
    const [sw, sh] = def.size.map(vx);
    const margin = vx(def.margin ?? 60);
    if (cr.x1 - cr.x0 < sw + 2 * margin + vx(40) || cr.y1 - cr.y0 < sh + 2 * margin + vx(40)) return null;
    const x0 = Math.round((cr.x0 + cr.x1 - sw) / 2);
    const y0 = Math.round((cr.y0 + cr.y1 - sh) / 2);
    const rect = { x0, y0, x1: x0 + sw - 1, y1: y0 + sh - 1 };
    const cx = (rect.x0 + rect.x1) / 2;
    const cy = (rect.y0 + rect.y1) / 2;
    // on an island: the whole site well inside the shore
    if (w.fields.island) {
      for (const [qx, qy] of [[cx, cy], [rect.x0, rect.y0], [rect.x1, rect.y0], [rect.x0, rect.y1], [rect.x1, rect.y1]])
        if (w.fields.coastDistance(qx, qy) < 150) return null;
    }
    // placement: custom (def.place) or a level pad over the whole rect
    let placed;
    if (def.place) {
      placed = def.place(w, { rect, cellRect: cr, margin, seed: hash32(seed, ca, cb, 885) });
      if (!placed) return null;
    } else {
      let hSum = 0;
      let hMin = Infinity;
      let hMax = -Infinity;
      for (const [fx, fy] of [[0.5, 0.5], [0.1, 0.1], [0.9, 0.1], [0.1, 0.9], [0.9, 0.9]]) {
        const h = w.terrain.sample(rect.x0 + (rect.x1 - rect.x0) * fx, rect.y0 + (rect.y1 - rect.y0) * fy).h;
        hSum += h;
        hMin = Math.min(hMin, h);
        hMax = Math.max(hMax, h);
      }
      if (hMax - hMin > vx(def.maxRelief ?? 25)) return null;
      const padZ = Math.max(Math.round(hSum / 5), Math.round(w.config.world.seaLevel * 8) + 16);
      placed = { padZ, pads: [{ rect, z: padZ, margin }], footprint: rect };
    }
    const fp = placed.footprint;
    const fpm = { x0: fp.x0 - margin, y0: fp.y0 - margin, x1: fp.x1 + margin, y1: fp.y1 + margin };
    if (w.highways && w.highways.corridorsNear(fpm).some((c) => c.hitsRect(fpm))) return null;
    if (w.waterHitsRect && w.waterHitsRect(fpm, 10)) return null;
    const padZ = placed.padZ;
    const site = {
      id: `site:${def.id}:${ca}_${cb}`,
      type: def.id,
      def,
      a,
      b,
      cell: c,
      cellRect: cr,
      rect,
      blend: { x0: rect.x0 - margin, y0: rect.y0 - margin, x1: rect.x1 + margin, y1: rect.y1 + margin },
      margin,
      padZ,
      pads: placed.pads,
      placed,
      seed: hash32(seed, ca, cb, 884),
      center: { x: cx, y: cy },
    };
    site.plan = def.plan(w, site);
    return site;
  }

  /** Sites whose blend rect overlaps a world rect. */
  sitesNear(rect) {
    const out = [];
    const a0 = Math.floor(rect.x0 / this.cell) - 1;
    const a1 = Math.floor(rect.x1 / this.cell) + 1;
    const b0 = Math.floor(rect.y0 / this.cell) - 1;
    const b1 = Math.floor(rect.y1 / this.cell) + 1;
    for (let b = b0; b <= b1; b += 1)
      for (let a = a0; a <= a1; a += 1) {
        const s = this.siteAt(a, b);
        if (!s) continue;
        const r = s.plan.bounds ?? s.blend;
        if (r.x0 <= rect.x1 && rect.x0 <= r.x1 && r.y0 <= rect.y1 && rect.y0 <= r.y1) out.push(s);
      }
    return out;
  }

  /** Sites anchored in arterial cell (i,j). */
  sitesInCell(i, j) {
    const cr = this.world.arterials.cellRect(i, j);
    return this.sitesNear(cr).filter((s) => s.cell.i === i && s.cell.j === j);
  }

  /**
   * Ground override: flattened pad + blend ramp. Returns null outside.
   * out: { z, mat, sub }
   */
  ground(x, y, naturalZ, out) {
    for (const s of this.sitesNear({ x0: x, y0: y, x1: x, y1: y })) {
      // the nearest pad wins where blend zones overlap (a road's switchbacks)
      let best = null;
      let bestK = Infinity;
      let bestD = 0;
      for (const p of s.pads) {
        const r = p.rect;
        const m = p.margin;
        if (x < r.x0 - m || x > r.x1 + m || y < r.y0 - m || y > r.y1 + m) continue;
        const d = p.path ? pathDistance(p, x, y) : Math.hypot(Math.max(r.x0 - x, 0, x - r.x1), Math.max(r.y0 - y, 0, y - r.y1));
        if (p.round && d >= m) continue;
        const k = d / m;
        if (k < bestK) {
          bestK = k;
          best = p;
          bestD = d;
        }
      }
      if (!best) continue;
      const p = best;
      const t = smoothstep(0, p.margin, bestD);
      out.z = Math.round(padZ(p, x, y) * (1 - t) + naturalZ * t);
      out.mat = 0;
      out.sub = 0;
      out.site = s;
      out.pad = p;
      out.natural = naturalZ;
      out.inside = bestD === 0;
      if (s.def.ground) s.def.ground(s, x, y, out);
      return out;
    }
    return null;
  }

  mapData(rect) {
    return this.sitesNear(rect).map((s) => ({ id: s.id, type: s.type, rect: s.rect, center: s.center }));
  }

  /** Nearest sites to a point (for the viewer's points of interest). */
  nearest(x, y, n = 4, radiusCells = 3) {
    const a = Math.floor(x / this.cell);
    const b = Math.floor(y / this.cell);
    const out = [];
    for (let db = -radiusCells; db <= radiusCells; db += 1)
      for (let da = -radiusCells; da <= radiusCells; da += 1) {
        const s = this.siteAt(a + da, b + db);
        if (s) out.push({ id: s.id, type: s.type, x: s.center.x, y: s.center.y, d: Math.hypot(s.center.x - x, s.center.y - y), z: s.padZ });
      }
    return out.sort((p, q) => p.d - q.d).slice(0, n);
  }
}

/**
 * Feature source for every site: rasterizes the boxes of the site's
 * structure (built by its definition) through the structure's spatial grid.
 * The z-range covers only the boxes over the requested rect, so tiles away
 * from the deep parts of a complex do not build empty chunks.
 */
export const siteSource = {
  id: "sites",
  order: 4,
  maxLod: 7,
  zRange(world, rect) {
    let lo = Infinity;
    let hi = -Infinity;
    const hit = (q) => !(q.x1 < rect.x0 || q.x0 > rect.x1 || q.y1 < rect.y0 || q.y0 > rect.y1);
    for (const s of world.sites.sitesNear(rect)) {
      const st = s.def.structure(world, s);
      if (!st.bb || !hit(st.bb)) continue;
      for (const q of st.grid.query(rect)) {
        if (!hit(q)) continue;
        if (q.z0 < lo) lo = q.z0;
        if (q.z1 > hi) hi = q.z1;
      }
      for (const c of st.custom ?? []) {
        if (!hit(c.bb)) continue;
        lo = Math.min(lo, c.bb.z0);
        hi = Math.max(hi, c.bb.z1);
      }
    }
    return lo === Infinity ? null : [lo, hi];
  },
  rasterize(world, chunk, tile) {
    const box = chunk.worldBox;
    for (const s of world.sites.sitesNear(box)) {
      const st = s.def.structure(world, s);
      for (const c of st.custom ?? []) {
        const b = c.bb;
        if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1 || b.z1 < box.z0 || b.z0 > box.z1) continue;
        c.rasterize(chunk);
      }
      const qs = st.grid.query(box);
      qs.sort((a, b) => a.i - b.i);
      // coarse LODs: rooms and tunnels sealed in the rock below the ground are invisible
      const deep = chunk.lod >= 3 && tile ? tile.zMin - 16 : -Infinity;
      for (const q of qs) {
        if (q.z1 < box.z0 || q.z0 > box.z1 || q.z1 < deep) continue;
        chunk.fillBox(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode ?? 0);
      }
    }
  },
};
