import { hashFloat } from "../core/hash.js";
import { LRU } from "../core/lru.js";
import { vx } from "../core/units.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";

/**
 * Site links: deep service-road tunnels joining neighbouring sites (research
 * complexes, mountain strongholds, military bases) into one underground
 * network, Black Mesa style.
 *
 * Every site kind that can be linked offers a `port(world, site)`: a point
 * and floor level deep inside its complex (a tram station, the deepest
 * level) where a link tunnel may start. Two sites on neighbouring lattice
 * cells are linked (by chance) with an L-shaped route between their ports.
 * The floor profile runs straight from port to port but never comes closer
 * than COVER to the surface (grade-limited envelope under the terrain), so a
 * link dives under valleys and stays in the rock. Links whose ports are too
 * far apart or too different in height for the grade are skipped.
 *
 * The tunnel rasterizes after the sites (order 4.5): its bore clears a
 * passage through anything it crosses, but its lining only fills solid rock,
 * so a link opens straight into the halls and rooms on its way.
 */

const HALF = vx(4); // carriageway half width
const H = vx(6.5); // clear height
const COVER = vx(30);
const GRADE = 0.06;
const STEP = vx(16);
const MAX_LEN = vx(12000);
const CHANCE = 0.75;

export class SiteLinks {
  constructor(world) {
    this.world = world;
    this.cache = new LRU(256);
  }

  /** Link between lattice cells (a, b) and its neighbour in direction dir (0: +a, 1: +b), or null. */
  link(a, b, dir) {
    const key = `${a},${b},${dir}`;
    let L = this.cache.get(key);
    if (L !== undefined) return L;
    L = this.build(a, b, dir);
    this.cache.set(key, L);
    return L;
  }

  build(a, b, dir) {
    const w = this.world;
    // (a wrapping world hashes the canonical lattice cell)
    const ca = w.sites.wrap.canon(a, w.sites.n);
    const cb = w.sites.wrap.canon(b, w.sites.n);
    if (hashFloat(w.seed, ca, cb, dir, 971) > CHANCE) return null;
    const sa = w.sites.siteAt(a, b);
    const sb = dir === 0 ? w.sites.siteAt(a + 1, b) : w.sites.siteAt(a, b + 1);
    if (!sa || !sb || !sa.def.port || !sb.def.port) return null;
    // cheap rejections before planning either complex
    const dc = Math.abs(sa.center.x - sb.center.x) + Math.abs(sa.center.y - sb.center.y);
    if (dc > MAX_LEN || Math.abs(sa.padZ - sb.padZ) > GRADE * dc + vx(200)) return null;
    const A = sa.def.port(w, sa, sb.center);
    const B = sb.def.port(w, sb, sa.center);
    if (!A || !B) return null;
    // L route: along x then y, or y then x
    const xFirst = hashFloat(w.seed, ca, cb, dir, 972) < 0.5;
    const corner = xFirst ? { x: B.x, y: A.y } : { x: A.x, y: B.y };
    const pts = [A, corner, B];
    const segs = [];
    let len = 0;
    for (let k = 0; k < 2; k += 1) {
      const p = pts[k];
      const q = pts[k + 1];
      const l = Math.abs(q.x - p.x) + Math.abs(q.y - p.y);
      if (l === 0) continue;
      const alongX = q.y === p.y;
      segs.push({ p, q, alongX, s0: len, len: l, dirSign: alongX ? Math.sign(q.x - p.x) : Math.sign(q.y - p.y) });
      len += l;
    }
    if (!segs.length || len > MAX_LEN) return null;
    if (Math.abs(B.z - A.z) > GRADE * len) return null;
    // profile: straight grade between the ports, kept COVER under the terrain
    const n = Math.ceil(len / STEP) + 1;
    const up = new Float64Array(n);
    for (let i = 0; i < n; i += 1) {
      const [x, y] = pointAt(segs, Math.min(len, i * STEP));
      up[i] = w.terrain.sample(x, y).h - COVER;
    }
    // grade-limited lower envelope of the cover limit
    for (let i = 1; i < n; i += 1) up[i] = Math.min(up[i], up[i - 1] + GRADE * STEP);
    for (let i = n - 2; i >= 0; i -= 1) up[i] = Math.min(up[i], up[i + 1] + GRADE * STEP);
    const prof = new Int32Array(n);
    let zlo = Infinity;
    let zhi = -Infinity;
    for (let i = 0; i < n; i += 1) {
      const t = Math.min(1, (i * STEP) / len);
      prof[i] = Math.round(Math.min(A.z + (B.z - A.z) * t, up[i]));
      zlo = Math.min(zlo, prof[i]);
      zhi = Math.max(zhi, prof[i]);
    }
    // the ports themselves must lie under enough rock
    if (prof[0] < A.z - 2 || prof[n - 1] < B.z - 2) return null;
    prof[0] = A.z;
    prof[n - 1] = B.z;
    const pad = HALF + 4;
    const bb = {
      x0: Math.min(A.x, B.x, corner.x) - pad,
      y0: Math.min(A.y, B.y, corner.y) - pad,
      x1: Math.max(A.x, B.x, corner.x) + pad,
      y1: Math.max(A.y, B.y, corner.y) + pad,
    };
    for (const s of segs) s.rect = { x0: Math.min(s.p.x, s.q.x) - pad, y0: Math.min(s.p.y, s.q.y) - pad, x1: Math.max(s.p.x, s.q.x) + pad, y1: Math.max(s.p.y, s.q.y) + pad };
    return { id: `link:${sa.id}>${sb.id}`, a: sa, b: sb, A, B, segs, len, prof, zlo, zhi, bb };
  }

  /** Links whose route passes near a rect. */
  near(rect) {
    const cell = this.world.sites.cell;
    // a link lies within the bounding box of its two ports, i.e. within the
    // two neighbouring lattice cells (plus site overhang): one cell of reach
    const reach = 1;
    const out = [];
    const a0 = Math.floor(rect.x0 / cell) - reach;
    const a1 = Math.floor(rect.x1 / cell) + reach;
    const b0 = Math.floor(rect.y0 / cell) - reach;
    const b1 = Math.floor(rect.y1 / cell) + reach;
    for (let b = b0; b <= b1; b += 1)
      for (let a = a0; a <= a1; a += 1)
        for (let dir = 0; dir < 2; dir += 1) {
          const L = this.link(a, b, dir);
          if (!L) continue;
          const r = L.bb;
          if (r.x0 <= rect.x1 && rect.x0 <= r.x1 && r.y0 <= rect.y1 && rect.y0 <= r.y1) out.push(L);
        }
    return out;
  }

  /** Floor z of a link at arc length s. */
  static zAt(L, s) {
    const f = Math.max(0, Math.min(L.prof.length - 1.0001, s / STEP));
    const i = Math.floor(f);
    const t = f - i;
    return Math.round(L.prof[i] * (1 - t) + L.prof[Math.min(i + 1, L.prof.length - 1)] * t);
  }

  mapData(rect) {
    return this.near(rect).map((L) => ({ id: L.id, pts: [L.A, ...L.segs.map((s) => s.q)].map((p) => ({ x: p.x, y: p.y })) }));
  }
}

function pointAt(segs, s) {
  for (const g of segs) {
    if (s <= g.s0 + g.len || g === segs[segs.length - 1]) {
      const t = Math.max(0, Math.min(g.len, s - g.s0));
      return g.alongX ? [g.p.x + g.dirSign * t, g.p.y] : [g.p.x, g.p.y + g.dirSign * t];
    }
  }
  return [segs[0].p.x, segs[0].p.y];
}

/** Along / across coordinates of a column in a segment (or null when outside its band). */
function local(g, x, y, band) {
  const along = g.alongX ? (x - g.p.x) * g.dirSign : (y - g.p.y) * g.dirSign;
  const across = g.alongX ? y - g.p.y : x - g.p.x;
  if (along < -band || along > g.len + band || Math.abs(across) > band) return null;
  return { along, across };
}

export const siteLinkSource = {
  id: "siteLinks",
  // after the sites: the bore always clears a passage through any room it
  // crosses, while its lining only fills solid rock (existing spaces stay open)
  order: 4.5,
  maxLod: 2,
  zRange(world, rect) {
    if (!world.siteLinks) return null;
    let lo = Infinity;
    let hi = -Infinity;
    for (const L of world.siteLinks.near(rect)) {
      for (const g of L.segs) {
        const r = g.rect;
        if (r.x0 > rect.x1 || r.x1 < rect.x0 || r.y0 > rect.y1 || r.y1 < rect.y0) continue;
        // arc-length window of the rect on this segment
        const a0 = g.alongX ? (g.dirSign > 0 ? rect.x0 - g.p.x : g.p.x - rect.x1) : g.dirSign > 0 ? rect.y0 - g.p.y : g.p.y - rect.y1;
        const a1 = a0 + (g.alongX ? rect.x1 - rect.x0 : rect.y1 - rect.y0);
        for (let s = Math.max(0, a0) - STEP; s <= Math.min(g.len, a1) + STEP; s += STEP / 2) {
          const z = SiteLinks.zAt(L, g.s0 + Math.max(0, Math.min(g.len, s)));
          lo = Math.min(lo, z - 3);
          hi = Math.max(hi, z + H + 4);
        }
      }
    }
    return lo === Infinity ? null : [lo, hi];
  },
  rasterize(world, chunk) {
    if (!world.siteLinks) return;
    const box = chunk.worldBox;
    const links = world.siteLinks.near(box);
    if (!links.length) return;
    const data = chunk.data;
    const band = HALF + 2;
    for (const L of links)
      for (const g of L.segs) {
        const r = g.rect;
        if (r.x0 > box.x1 || r.x1 < box.x0 || r.y0 > box.y1 || r.y1 < box.y0) continue;
        for (let j = 0; j < P; j += 1)
          for (let i = 0; i < P; i += 1) {
            const x = chunk.wx(i);
            const y = chunk.wy(j);
            const lc = local(g, x, y, band);
            if (!lc) continue;
            const { along, across } = lc;
            // the other segment owns the corner beyond this segment's end
            if (along > g.len + 1 && g !== L.segs[L.segs.length - 1]) continue;
            if (along < -1 && g !== L.segs[0]) continue;
            const z = SiteLinks.zAt(L, g.s0 + Math.max(0, Math.min(g.len, along)));
            if (z + H + 2 < box.z0 || z - 2 > box.z1) continue;
            const aa = Math.abs(across);
            const col = i + j * P;
            const wall = aa > HALF;
            const chamfer = aa > HALF - 6 ? aa - (HALF - 6) : 0;
            const walk = aa > HALF - 8 && aa <= HALF;
            const seg = ((Math.round(along) % 64) + 64) % 64;
            for (let k = 0; k < P; k += 1) {
              const zz = chunk.wz(k);
              const dz = zz - z;
              if (dz < -1 || dz > H + 2) continue;
              const idx = col + k * P2;
              let m;
              const lining = dz === -1 || wall || dz > H - chamfer;
              if (lining && data[idx] === 0) continue;
              if (dz === -1) m = MAT.CONCRETE;
              else if (wall || dz > H - chamfer) m = dz >= H + 1 || wall ? MAT.TUNNEL_WALL : MAT.CONCRETE;
              else if (dz === 0) m = walk ? MAT.SIDEWALK : aa < 1 && seg < 24 ? MAT.LINE_YELLOW : MAT.ASPHALT;
              else if (walk && dz <= 2) m = MAT.SIDEWALK;
              else if (dz === H - chamfer && aa < 2 && seg < 8) m = MAT.LIGHT_STRIP;
              else if (wall === false && aa === HALF && dz === vx(2.5) && seg < 2) m = MAT.EMERGENCY_RED;
              else m = 0;
              data[idx] = m;
            }
          }
      }
  },
};
