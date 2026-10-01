import { MAT } from "../voxel/materials.js";
import { vx } from "../core/units.js";
import { hash32, hashString } from "../core/hash.js";

/**
 * Landscape parks: a layout per park space, built once from its id and
 * size, shared by the ground surface (landscape.js) and the dressing
 * (dressing.js):
 *
 *   entrances  on the street sides, 2-5 of them, not at the corners
 *   hub        a small round plaza off-centre where the main paths meet
 *   paths      curved (quadratic) walks from every entrance to the hub,
 *              sampled every 2 m into segments on a lookup grid; one or
 *              two cross links between entrances
 *   pond       in bigger parks: an irregular shore (a few harmonics on the
 *              radius), off-centre, with a walk round part of it
 *   groves     tree clumps where a coarse noise is high, open lawn and
 *              meadow (long grass, left unmown) elsewhere
 *
 * Coordinates are relative to the space rect (voxels).
 */

const cache = new WeakMap();

function h01(seed, a, b = 0) {
  return hash32(seed, a, b, 0x9a4c) / 4294967296;
}

/** Smooth value noise (0..1) on cells of `c` voxels, relative coordinates. */
function vnoise(seed, u, v, c, salt) {
  const gx = Math.floor(u / c);
  const gy = Math.floor(v / c);
  let fx = u / c - gx;
  let fy = v / c - gy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const a = h01(seed, gx * 7 + salt, gy);
  const b = h01(seed, (gx + 1) * 7 + salt, gy);
  const d = h01(seed, gx * 7 + salt, gy + 1);
  const e = h01(seed, (gx + 1) * 7 + salt, gy + 1);
  return (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
}

export function parkLayout(space) {
  let L = cache.get(space);
  if (L) return L;
  const r = space.rect;
  const W = r.x1 - r.x0;
  const H = r.y1 - r.y0;
  const seed = hashString(space.id);
  const R = (k) => h01(seed, k);
  const m = vx(5);
  // the hub: off-centre, on a path crossing
  const hub = { u: W * (0.35 + 0.3 * R(1)), v: H * (0.35 + 0.3 * R(2)), r: Math.min(vx(7), Math.min(W, H) * 0.1) };
  // entrances on each side (a side with a street), 1-2 per side on bigger parks
  const ents = [];
  const sides = ["N", "S", "W", "E"];
  sides.forEach((s, k) => {
    const len = s === "N" || s === "S" ? W : H;
    const n = len > vx(90) ? 2 : len > vx(35) ? 1 : 0;
    for (let q = 0; q < n; q += 1) {
      if (R(10 + k * 3 + q) < 0.25) continue;
      const t = (q + 0.25 + 0.5 * R(20 + k * 3 + q)) / n;
      const a = m * 2 + (len - m * 4) * t;
      ents.push(s === "N" ? { u: a, v: 0 } : s === "S" ? { u: a, v: H } : s === "W" ? { u: 0, v: a } : { u: W, v: a });
    }
  });
  if (ents.length < 2) ents.push({ u: W / 2, v: 0 }, { u: W / 2, v: H });
  // pond: in bigger parks, off-centre, away from the hub
  let pond = null;
  const big = Math.min(W, H) > vx(60);
  if (big && R(3) < 0.75) {
    const pr = Math.min(W, H) * (0.13 + 0.08 * R(4));
    let best = null;
    for (let k = 0; k < 8; k += 1) {
      const pu = pr + vx(8) + (W - 2 * pr - vx(16)) * R(40 + k);
      const pv = pr + vx(8) + (H - 2 * pr - vx(16)) * R(50 + k);
      const d = Math.hypot(pu - hub.u, pv - hub.v);
      if (d > pr + hub.r + vx(8) && (!best || d < best.d)) best = { u: pu, v: pv, d };
    }
    if (best) pond = { u: best.u, v: best.v, r: pr, a: [0.12 + 0.1 * R(5), 0.08 * R(6), 0.05 * R(7)], p: [R(8) * 6.28, R(9) * 6.28, R(11) * 6.28], stretch: 1 + 0.5 * R(12), rot: R(13) * 3.14 };
  }
  // paths: quadratic curves from each entrance to the hub (bowed sideways), plus a link or two
  const segs = [];
  const addCurve = (a, b, bow) => {
    const mx = (a.u + b.u) / 2;
    const my = (a.v + b.v) / 2;
    const dx = b.u - a.u;
    const dy = b.v - a.v;
    const len = Math.hypot(dx, dy) || 1;
    const c = { u: mx - (dy / len) * bow * len, v: my + (dx / len) * bow * len };
    const n = Math.max(2, Math.ceil(len / vx(2)));
    let prev = a;
    for (let k = 1; k <= n; k += 1) {
      const t = k / n;
      const p = { u: (1 - t) * (1 - t) * a.u + 2 * (1 - t) * t * c.u + t * t * b.u, v: (1 - t) * (1 - t) * a.v + 2 * (1 - t) * t * c.v + t * t * b.v };
      segs.push([prev.u, prev.v, p.u, p.v]);
      prev = p;
    }
  };
  ents.forEach((e, k) => addCurve(e, hub, (R(60 + k) - 0.5) * 0.5));
  for (let k = 0; k < (ents.length > 3 ? 2 : 1); k += 1) {
    const a = ents[Math.floor(R(70 + k) * ents.length)];
    const b = ents[Math.floor(R(75 + k) * ents.length)];
    if (a !== b) addCurve(a, b, (R(80 + k) - 0.5) * 0.7);
  }
  // a walk round the pond (most of the way)
  if (pond) {
    const n = 36;
    const gap = Math.floor(R(14) * n);
    let prev = null;
    for (let k = 0; k <= n; k += 1) {
      const t = (k / n) * Math.PI * 2;
      const rr = pondRadius(pond, t) + vx(3.5);
      const p = { u: pond.u + Math.cos(t) * rr * pond.stretch, v: pond.v + Math.sin(t) * rr };
      const q = rot(pond, p);
      if (prev && Math.abs(k - gap) > 4) segs.push([prev.u, prev.v, q.u, q.v]);
      prev = q;
    }
  }
  // drop path pieces that would run through the pond
  const keep = pond ? segs.filter((s) => pondDist(pond, (s[0] + s[2]) / 2, (s[1] + s[3]) / 2) > vx(1.5)) : segs;
  // lookup grid of segments (8 m buckets)
  const G = vx(8);
  const grid = new Map();
  for (const s of keep) {
    const i0 = Math.floor((Math.min(s[0], s[2]) - vx(2)) / G);
    const i1 = Math.floor((Math.max(s[0], s[2]) + vx(2)) / G);
    const j0 = Math.floor((Math.min(s[1], s[3]) - vx(2)) / G);
    const j1 = Math.floor((Math.max(s[1], s[3]) + vx(2)) / G);
    for (let j = j0; j <= j1; j += 1)
      for (let i = i0; i <= i1; i += 1) {
        const k = i * 4096 + j;
        if (!grid.has(k)) grid.set(k, []);
        grid.get(k).push(s);
      }
  }
  L = { W, H, seed, hub, ents, pond, segs: keep, grid, G, pathW: vx(1.2) + (big ? vx(0.4) : 0) };
  cache.set(space, L);
  return L;
}

function rot(pond, p) {
  const c = Math.cos(pond.rot);
  const s = Math.sin(pond.rot);
  const du = p.u - pond.u;
  const dv = p.v - pond.v;
  return { u: pond.u + du * c - dv * s, v: pond.v + du * s + dv * c };
}

function pondRadius(pond, t) {
  const [a1, a2, a3] = pond.a;
  const [p1, p2, p3] = pond.p;
  return pond.r * (1 + a1 * Math.sin(t + p1) + a2 * Math.sin(2 * t + p2) + a3 * Math.sin(3 * t + p3));
}

/** Signed distance (voxels, approx.) to the pond's shore, negative in the water. */
export function pondDist(pond, u, v) {
  const c = Math.cos(-pond.rot);
  const s = Math.sin(-pond.rot);
  const du = u - pond.u;
  const dv = v - pond.v;
  const x = (du * c - dv * s) / pond.stretch;
  const y = du * s + dv * c;
  const t = Math.atan2(y, x);
  return Math.hypot(x, y) - pondRadius(pond, t);
}

/** Distance (voxels) from (u, v) to the nearest path centre line. */
export function pathDist(L, u, v) {
  const list = L.grid.get(Math.floor(u / L.G) * 4096 + Math.floor(v / L.G));
  let best = Infinity;
  if (!list) return best;
  for (const s of list) {
    const dx = s[2] - s[0];
    const dy = s[3] - s[1];
    const l2 = dx * dx + dy * dy || 1;
    let t = ((u - s[0]) * dx + (v - s[1]) * dy) / l2;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const d = Math.hypot(u - s[0] - dx * t, v - s[1] - dy * t);
    if (d < best) best = d;
  }
  return best;
}

/** 0..1 grove value: trees stand in clumps where it is high. */
export function groveAt(L, u, v) {
  return 0.65 * vnoise(L.seed, u, v, vx(22), 1) + 0.35 * vnoise(L.seed, u, v, vx(9), 2);
}

/** Park ground: lawn, unmown meadow, paths, the hub, a pond with reeds, flower beds round the hub. */
export function parkSurface(space, u, v, out, hx = u, hy = v) {
  const L = parkLayout(space);
  if (L.pond) {
    const pd = pondDist(L.pond, u, v);
    if (pd < 0) {
      out.water = true;
      out.mat = MAT.WATER;
      out.dz = -Math.min(8, Math.floor(-pd / 5) + 2);
      return out;
    }
    if (pd < vx(1.4)) {
      out.mat = pd < vx(0.6) && vnoise(L.seed, u, v, vx(4), 5) < 0.6 ? MAT.REED : (hash32(hx, hy, 0x57) & 3) === 0 ? MAT.GRAVEL : MAT.MUD;
      return out;
    }
  }
  const hd = Math.hypot(u - L.hub.u, v - L.hub.v);
  if (hd < L.hub.r) {
    out.mat = hd < L.hub.r - vx(0.8) ? ((Math.floor(hd / 6) & 1) === 0 ? MAT.PLAZA_STONE : MAT.PLAZA_STONE_DARK) : MAT.CURB;
    return out;
  }
  if (hd < L.hub.r + vx(1.6) && (Math.floor(Math.atan2(v - L.hub.v, u - L.hub.u) * 5) & 1) === 0) {
    out.mat = (hash32(hx >> 1, hy >> 1, 0x58) & 3) === 0 ? MAT.FLOWER_YELLOW : MAT.FLOWER_RED;
    return out;
  }
  const pd = pathDist(L, u, v);
  if (pd < L.pathW) {
    out.mat = pd > L.pathW - 1 ? MAT.GRAVEL : MAT.PARK_PATH;
    return out;
  }
  // unmown meadow in the quiet parts, mown lawn near the paths
  const meadow = vnoise(L.seed, u, v, vx(28), 3);
  if (meadow > 0.62 && pd > vx(4)) {
    out.mat = (hash32(hx, hy, 0x59) & 7) < 2 ? MAT.GRASS_DRY : MAT.GRASS;
    return out;
  }
  out.mat = (hash32(hx >> 2, hy >> 2, 0x5a) & 15) === 0 ? MAT.GRASS : MAT.GRASS_LAWN;
  return out;
}
