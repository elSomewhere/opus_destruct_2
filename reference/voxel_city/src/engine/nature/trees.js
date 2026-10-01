import { MAT } from "../voxel/materials.js";
import { hash32 } from "../core/hash.js";
import { P, P2 } from "../voxel/chunk.js";
import { YAWS, YAW_QUARTER } from "../core/placement.js";

/**
 * Procedural trees and small plants, point-sampled so they render at every LOD.
 *
 *   t = { x, y, z (first voxel above ground), h (height), r (crown radius),
 *         kind, seed, look? (world/season.js treeLook) }
 *
 * Every tree is built once from its seed into a model (`treeModel`): a
 * trunk that tapers, flares at the foot and leans a little, limbs forking
 * off it with twigs at their ends, and foliage as a handful of clusters
 * (ellipsoids with a lumpy, gappy surface) at the ends of the limbs, not one
 * ball on a stick. Species differ in structure:
 *
 *   oak        low fork, few long spreading limbs, a broad irregular crown
 *   maple      (beech, ash; alias "autumn") a tall oval crown on steep limbs
 *   street     (lime, plane) a regular oval crown on a clean trunk
 *   birch      slender white stems (often two or three), small airy clusters
 *   rowan      a few thin stems, an open crown, berries late in the year
 *   blossom    (cherry, apple) a low fork, flat spreading crown
 *   willow     a leaning trunk, a dome with hanging curtains of leaves
 *   poplar     a tall narrow column
 *   pine       (Scots pine) a long bare trunk, orange at the top, flat
 *              clumps of needles in the upper third
 *   spruce     a dense narrow cone of drooping whorls, irregular branch lobes
 *   dwarfpine  low sprawling mounds (krummholz); juniper a small dark spire
 *   shrub      a few overlapping clumps; fern, berry, log, stump, snag
 *   hazel      a clump of thin stems fanning out, a leafy dome on top
 *   aspen      a straight pale stem, a small rounded crown high up
 *   alder      a dark egg-shaped crown, often two stems (by the water)
 *   larch      an airy cone of soft needles in tiers of tufts, gold in
 *              autumn and bare in winter (a deciduous conifer)
 *   acacia, palm, cactus, jungle
 *
 * Wild trees (`t.wild`: the organic vegetation of the angled world,
 * ANGLED_WORLD_PLAN.md S2) grow less regularly: trunks lean further and
 * sweep, forks sit higher or lower, crowns are lopsided towards the light
 * (the side they lean to), broadleaves now and then carry twin leaders,
 * conifers lean and grow lopsided cones, snags lean and fallen logs follow
 * the ground or rest on their root plate. Every new direction and tilt is
 * an exact rotation from the yaw table (core/placement.js), never a new
 * sin or cos. `t.amp` (0..1, default 1) scales the lean and lopsidedness;
 * `t.reach` (voxels) is the emitter's search margin, which the tree keeps
 * within (treeModel: less lean, then a smaller crown).
 *
 * Foliage is shaded by exposure (sunlit tops, shadowed undersides and
 * interior). The seasonal look replaces the palette on some or all clusters
 * (autumn trees turn cluster by cluster), strips broadleaves to their limbs
 * and twigs, and lays snow on upper surfaces. Up close the shell is lumpy
 * with gaps (only in the shell: holes deep inside would cost faces for
 * nothing); coarse LODs draw smooth, slightly inflated clusters.
 */

/** Size ranges per kind (meters): height and crown radius. */
export const TREE_KINDS = {
  oak: { h: [8, 15], r: [2.8, 4.8] },
  maple: { h: [10, 17], r: [2.4, 3.8] },
  autumn: { h: [10, 17], r: [2.4, 3.8] },
  street: { h: [7, 12], r: [2.2, 3.4] },
  birch: { h: [8, 16], r: [1.6, 2.8] },
  rowan: { h: [4.5, 8], r: [1.6, 2.6] },
  blossom: { h: [4, 7], r: [2.0, 3.2] },
  willow: { h: [7, 12], r: [3.0, 4.6] },
  poplar: { h: [15, 25], r: [1.4, 2.1] },
  pine: { h: [12, 22], r: [2.2, 3.6] },
  spruce: { h: [12, 26], r: [1.9, 3.2] },
  dwarfpine: { h: [1.6, 3.5], r: [1.2, 2.4] },
  juniper: { h: [1.2, 3.2], r: [0.5, 0.9] },
  acacia: { h: [5, 8], r: [3.0, 4.5] },
  jungle: { h: [15, 26], r: [4.0, 6.0] },
  palm: { h: [7, 12], r: [2.5, 3.5] },
  cactus: { h: [2.0, 4.5], r: [0.8, 1.4] },
  shrub: { h: [0.8, 1.8], r: [0.8, 1.6] },
  hazel: { h: [2.2, 4.5], r: [1.6, 2.6] },
  aspen: { h: [12, 22], r: [1.8, 2.8] },
  alder: { h: [8, 16], r: [2.2, 3.4] },
  larch: { h: [14, 26], r: [2.2, 3.6] },
  shrubDry: { h: [0.5, 1.1], r: [0.5, 1.1] },
  fern: { h: [0.4, 0.9], r: [0.5, 0.9] },
  berry: { h: [0.2, 0.45], r: [0.3, 0.6] },
  log: { h: [0.35, 0.6], r: [2.5, 6] },
  stump: { h: [0.3, 0.7], r: [0.25, 0.45] },
  snag: { h: [5, 12], r: [0.2, 0.4] },
};

/** Summer foliage [shadow, body, sunlit] per kind. */
const PALETTE = {
  oak: [MAT.LEAVES_DARK, MAT.LEAVES, MAT.LEAVES_LIGHT],
  maple: [MAT.LEAVES_DEEP, MAT.LEAVES_DARK, MAT.LEAVES],
  street: [MAT.LEAVES_DARK, MAT.LEAVES, MAT.LEAVES_LIGHT],
  birch: [MAT.LEAVES, MAT.LEAVES_BIRCH, MAT.LEAVES_LIGHT],
  rowan: [MAT.LEAVES_DARK, MAT.LEAVES, MAT.LEAVES_LIGHT],
  blossom: [MAT.LEAVES_DARK, MAT.LEAVES, MAT.LEAVES_LIGHT],
  willow: [MAT.LEAVES, MAT.LEAVES_BIRCH, MAT.LEAVES_WILLOW],
  poplar: [MAT.LEAVES_DEEP, MAT.LEAVES_DARK, MAT.LEAVES],
  pine: [MAT.LEAVES_SPRUCE, MAT.LEAVES_PINE, MAT.LEAVES_PINE_LIGHT],
  spruce: [MAT.LEAVES_DEEP, MAT.LEAVES_SPRUCE, MAT.LEAVES_PINE],
  dwarfpine: [MAT.LEAVES_SPRUCE, MAT.LEAVES_PINE, MAT.LEAVES_PINE_LIGHT],
  juniper: [MAT.LEAVES_DEEP, MAT.LEAVES_SPRUCE, MAT.LEAVES_PINE],
  jungle: [MAT.LEAVES_DEEP, MAT.LEAVES_JUNGLE, MAT.LEAVES],
  shrub: [MAT.LEAVES_DARK, MAT.BUSH, MAT.LEAVES],
  shrubDry: [MAT.SHRUB_DRY, MAT.SHRUB_DRY, MAT.GRASS_STRAW],
  fern: [MAT.LEAVES_DARK, MAT.FERN, MAT.FERN],
  berry: [MAT.BLUEBERRY, MAT.BLUEBERRY, MAT.LEAVES_DARK],
  hazel: [MAT.LEAVES_DARK, MAT.LEAVES, MAT.LEAVES_LIGHT],
  aspen: [MAT.LEAVES, MAT.LEAVES_BIRCH, MAT.LEAVES_LIGHT],
  alder: [MAT.LEAVES_DEEP, MAT.LEAVES_DARK, MAT.LEAVES],
  larch: [MAT.LEAVES, MAT.LEAVES_LIGHT, MAT.LEAVES_SPRING],
};

/** Kinds that keep their needles (the season only lays snow on them). */
export const EVERGREEN = new Set(["pine", "spruce", "dwarfpine", "juniper", "jungle", "palm", "cactus", "acacia"]);

const BLOB = 0;
const LIMB = 1;
const CONE = 2;
const CURTAIN = 3;
const FERN = 4;
const PLATE = 5;

/** Wild trees: how much further broadleaves lean (x their species' lean, at amp 1). */
const WILD_LEAN = 1.5;
/** Largest lean of a wild snag and tilt of a windthrown log: yaw-table indices (16.3°, 14.3°). */
const SNAG_TILT = 5;
const THROW_TILT = 4;

/** Lean and lopsidedness of a wild tree (0: none, 1: full); 0 for every other tree. */
function ampOf(t) {
  return t.wild ? (t.amp ?? 1) : 0;
}

/**
 * A lean of up to `max` voxels in an exact direction of the yaw table:
 * { x, y } (the offset of the top), { ux, uy } (the unit direction).
 */
function leanOf(R, max) {
  const Y = YAWS[R.int(0, YAWS.length - 1)];
  const m = max * R.next();
  return { x: (Y.c / Y.r) * m, y: (Y.s / Y.r) * m, ux: Y.c / Y.r, uy: Y.s / Y.r };
}

function h01(x, y, z, s) {
  return hash32(x, y, z, s) / 4294967296;
}

/** A small seeded generator (mulberry32). */
function prng(seed) {
  let a = seed | 0;
  const next = () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  return { next, range: (lo, hi) => lo + (hi - lo) * next(), int: (lo, hi) => lo + Math.floor(next() * (hi - lo + 1)) };
}

function blob(x, y, z, rx, rz, mix, extra = {}) {
  // (the lumpy surface reaches ~15% past the ellipsoid)
  const pad = 2 + 0.16 * Math.max(rx, rz);
  return { k: BLOB, x, y, z, rx, rz, mix, ...extra, bb: { x0: Math.floor(x - rx - pad), y0: Math.floor(y - rx - pad), z0: Math.floor(z - rz - pad), x1: Math.ceil(x + rx + pad), y1: Math.ceil(y + rx + pad), z1: Math.ceil(z + rz + pad) } };
}

function limb(ax, ay, az, bx, by, bz, r0, r1, mat, extra = {}) {
  const r = Math.max(r0, r1) + 1;
  const dx = bx - ax;
  const dy = by - ay;
  const dz = bz - az;
  const L2 = dx * dx + dy * dy + dz * dz || 1;
  return { k: LIMB, ax, ay, az, dx, dy, dz, L2, r0, r1, mat, ...extra, bb: { x0: Math.floor(Math.min(ax, bx) - r), y0: Math.floor(Math.min(ay, by) - r), z0: Math.floor(Math.min(az, bz) - r), x1: Math.ceil(Math.max(ax, bx) + r), y1: Math.ceil(Math.max(ay, by) + r), z1: Math.ceil(Math.max(az, bz) + r) } };
}

/**
 * Satellite clusters round a cluster (x, y, z, rb): smaller lumps pushed
 * out and up from its surface, so crowns end in an irregular, lumpy
 * outline instead of a ball (and neighbouring clusters grow together).
 */
function satellites(parts, R, c, flat, n, size = [0.42, 0.62], up = 0.35) {
  for (let q = 0; q < n; q += 1) {
    const a = R.range(0, Math.PI * 2);
    const e = R.range(-0.25, 1) * up + (1 - up) * R.range(-0.1, 0.35);
    const d = c.rb * R.range(0.55, 0.85);
    const rs = c.rb * R.range(size[0], size[1]);
    parts.push(blob(c.x + Math.cos(a) * d * Math.cos(e), c.y + Math.sin(a) * d * Math.cos(e), c.z + Math.sin(e) * d * flat, rs, rs * flat, c.mix ?? R.next()));
  }
}

/** Trunk radius (voxels) of a tree `h` voxels tall. */
function trunkR(t, k = 60) {
  return Math.max(0.7, t.h / k);
}

/**
 * A deciduous crown on a forking trunk. `o`: fork height (fraction), limb
 * count, limb elevation (radians), limb length and cluster size (fractions
 * of r), cluster flatness (rz / rx), top cluster size, filler clusters, lean,
 * twin (the chance of a wild tree's twin leaders).
 */
function broadleaf(t, R, parts, o, base = { x: t.x, y: t.y, z: t.z, h: t.h, r: t.r }) {
  const wild = !!t.wild;
  const amp = ampOf(t);
  const h = base.h;
  // (the clusters and their satellites reach ~1.5 of this: t.r stays the crown's visual radius)
  const r = base.r * 0.72;
  const la = R.range(0, Math.PI * 2);
  const cl = Math.cos(la);
  const sl = Math.sin(la);
  const lean = o.lean * (1 + WILD_LEAN * amp) * h * R.next();
  // (wild: the fork anywhere from a little lower to a little higher)
  const fork = wild ? [o.fork[0] - 0.06, o.fork[1] + 0.08] : o.fork;
  const hf = h * R.range(fork[0], fork[1]);
  const tr = o.trunk ?? trunkR(t);
  const F = { x: base.x + cl * lean * (hf / h), y: base.y + sl * lean * (hf / h), z: base.z + hf };
  const bark = o.bark ?? MAT.BARK;
  if (wild) {
    // a trunk that sweeps: upright at the foot, leaning above
    const M = { x: base.x + (F.x - base.x) * 0.3, y: base.y + (F.y - base.y) * 0.3, z: base.z + hf * 0.5 };
    parts.push(limb(base.x, base.y, base.z - 1, M.x, M.y, M.z, tr * 1.15, tr * 0.97, bark, { trunk: true }));
    parts.push(limb(M.x, M.y, M.z, F.x, F.y, F.z, tr * 0.97, tr * 0.8, bark, { trunk: true }));
  } else parts.push(limb(base.x, base.y, base.z - 1, F.x, F.y, F.z, tr * 1.15, tr * 0.8, bark, { trunk: true }));
  if (o.flare !== false) parts.push(limb(base.x, base.y, base.z - 1, base.x, base.y, base.z + 2, tr * 1.7, tr * 1.1, bark, { trunk: true }));
  const n = R.int(o.limbs[0], o.limbs[1] + (wild ? 1 : 0));
  const a0 = R.range(0, Math.PI * 2);
  // wild: a lopsided crown (longer limbs, bigger clusters on the lit side it leans to), flatter or rounder
  const A = wild ? amp * R.range(0.12, 0.36) : 0;
  const flat = wild ? o.flat * R.range(0.88, 1.12) : o.flat;
  const crown = [];
  // the top cluster sits under the tree's height; the limb clusters fill the crown below it
  const rt = r * o.top;
  const T = { x: F.x + cl * lean * 0.4, y: F.y + sl * lean * 0.4, z: base.z + h - rt * flat * 0.9 };
  for (let i = 0; i < n; i += 1) {
    const az = a0 + (i / n) * Math.PI * 2 + R.range(-0.45, 0.45);
    const ca = Math.cos(az);
    const sa = Math.sin(az);
    const e = R.next();
    const lit = A * (ca * cl + sa * sl);
    const L = r * R.range(o.spread[0], o.spread[1]) * (1.1 - 0.25 * e) * (1 + lit);
    const E = { x: F.x + ca * L, y: F.y + sa * L, z: F.z + (T.z - F.z) * (0.28 + 0.42 * e) * R.range(0.9, 1.1) };
    const rb = r * R.range(o.blob[0], o.blob[1]) * (1 + lit * 0.5);
    parts.push(limb(F.x, F.y, F.z, E.x, E.y, E.z, tr * 0.72, Math.max(0.5, tr * 0.32), bark));
    crown.push({ ...E, rb, az });
  }
  if (wild && R.next() < (o.twin ?? 0)) {
    // twin leaders: the crown forks across the lean, one top a little lower
    const d = r * R.range(0.22, 0.4);
    const tops = [
      { x: T.x - sl * d, y: T.y + cl * d, z: T.z },
      { x: T.x + sl * d * 0.8, y: T.y - cl * d * 0.8, z: T.z - h * R.range(0.04, 0.12) },
    ];
    for (const Q of tops) {
      parts.push(limb(F.x, F.y, F.z, Q.x, Q.y, Q.z, tr * 0.6, Math.max(0.5, tr * 0.26), bark));
      crown.push({ ...Q, rb: rt * 0.8, az: la });
    }
  } else {
    // the leader and the top cluster
    parts.push(limb(F.x, F.y, F.z, T.x, T.y, T.z, tr * 0.7, Math.max(0.5, tr * 0.3), bark));
    crown.push({ ...T, rb: rt, az: la });
  }
  // filler clusters between neighbouring limbs, higher up and further in (no hollows)
  for (let k = 0; k < (o.fill ?? 0) && n > 1; k += 1) {
    const a = crown[k % n];
    const b = crown[(k + 1) % n];
    const mx = (a.x + b.x) / 2;
    const my = (a.y + b.y) / 2;
    const mz = (a.z + b.z) / 2;
    const f = R.range(0.35, 0.65);
    crown.push({ x: F.x + (mx - F.x) * 0.65, y: F.y + (my - F.y) * 0.65, z: mz + (T.z - mz) * f, rb: (a.rb + b.rb) * 0.4, az: (a.az + b.az) / 2 });
  }
  for (const c of crown) {
    c.mix = R.next();
    parts.push(blob(c.x, c.y, c.z, c.rb, c.rb * flat, c.mix));
    satellites(parts, R, c, flat, o.sat ?? 3);
    // twigs from the cluster centre outwards (the bare crown in winter)
    for (let q = 0; q < 3; q += 1) {
      const ta = c.az + R.range(-1.4, 1.4);
      const tl = c.rb * R.range(0.6, 0.95);
      parts.push(limb(c.x, c.y, c.z, c.x + Math.cos(ta) * tl, c.y + Math.sin(ta) * tl, c.z + R.range(-0.2, 0.6) * c.rb * flat, 0.55, 0.4, bark, { twig: true }));
    }
  }
  return F;
}

const BUILD = {
  oak(t, R, parts) {
    broadleaf(t, R, parts, { fork: [0.26, 0.38], limbs: [3, 5], elev: [0.35, 0.85], spread: [0.45, 0.68], blob: [0.42, 0.56], flat: 0.75, top: 0.5, fill: 2, lean: 0.06, twin: 0.3, trunk: trunkR(t, 45) });
  },
  maple(t, R, parts) {
    broadleaf(t, R, parts, { fork: [0.32, 0.45], limbs: [4, 5], elev: [0.8, 1.2], spread: [0.35, 0.55], blob: [0.4, 0.52], flat: 0.95, top: 0.55, fill: 2, lean: 0.03, twin: 0.22, bark: R.next() < 0.5 ? MAT.BARK_GREY : MAT.BARK });
  },
  street(t, R, parts) {
    broadleaf(t, R, parts, { fork: [0.4, 0.5], limbs: [4, 5], elev: [0.75, 1.15], spread: [0.35, 0.5], blob: [0.45, 0.55], flat: 0.9, top: 0.55, fill: 1, lean: 0.01 });
  },
  blossom(t, R, parts) {
    broadleaf(t, R, parts, { fork: [0.22, 0.32], limbs: [4, 6], elev: [0.3, 0.65], spread: [0.55, 0.85], blob: [0.36, 0.48], flat: 0.62, top: 0.4, fill: 2, lean: 0.07, twin: 0.3, bark: MAT.BARK });
  },
  jungle(t, R, parts) {
    broadleaf(t, R, parts, { fork: [0.6, 0.72], limbs: [4, 6], elev: [0.2, 0.55], spread: [0.6, 0.9], blob: [0.35, 0.5], flat: 0.55, top: 0.45, fill: 3, lean: 0.04, twin: 0.2, trunk: trunkR(t, 40) });
  },
  willow(t, R, parts) {
    broadleaf(t, R, parts, { fork: [0.28, 0.38], limbs: [4, 6], elev: [0.6, 1.0], spread: [0.5, 0.75], blob: [0.4, 0.52], flat: 0.7, top: 0.45, fill: 2, lean: 0.12, twin: 0.25, trunk: trunkR(t, 38) });
    // hanging curtains of leaves round every cluster
    for (const p of parts.filter((q) => q.k === BLOB && q.rx > t.r * 0.3))
      parts.push({ k: CURTAIN, x: p.x, y: p.y, z: p.z, rx: p.rx, rz: p.rz, drop: Math.max(4, p.z - p.rz - t.z - t.h * 0.08), mix: p.mix, bb: { x0: Math.floor(p.x - p.rx - 2), y0: Math.floor(p.y - p.rx - 2), z0: Math.floor(t.z + t.h * 0.06), x1: Math.ceil(p.x + p.rx + 2), y1: Math.ceil(p.y + p.rx + 2), z1: Math.ceil(p.z) } });
  },
  rowan(t, R, parts) {
    // two or three thin stems from the foot, each with a small open crown
    const n = R.int(1, 3);
    for (let s = 0; s < n; s += 1) {
      const a = R.range(0, Math.PI * 2);
      const off = n > 1 ? R.range(0.5, 1.2) : 0;
      const hh = t.h * R.range(0.8, 1);
      broadleaf(t, R, parts, { fork: [0.45, 0.6], limbs: [2, 3], elev: [0.9, 1.3], spread: [0.3, 0.5], blob: [0.38, 0.52], flat: 0.9, top: 0.45, fill: 0, sat: 2, lean: n > 1 ? 0.14 : 0.05, trunk: trunkR(t, 70), flare: false }, { x: t.x + Math.cos(a) * off, y: t.y + Math.sin(a) * off, z: t.z, h: hh, r: t.r * (n > 1 ? 0.8 : 1) });
    }
  },
  poplar(t, R, parts) {
    const tr = trunkR(t, 70);
    // (wild: the column leans a little)
    const L = t.wild ? leanOf(R, ampOf(t) * 0.03 * t.h) : null;
    parts.push(limb(t.x, t.y, t.z - 1, L ? t.x + L.x * 0.9 : t.x, L ? t.y + L.y * 0.9 : t.y, t.z + t.h * 0.9, tr * 1.1, tr * 0.5, MAT.BARK_GREY, { trunk: true }));
    const n = R.int(4, 6);
    for (let k = 0; k < n; k += 1) {
      const f = 0.2 + (0.8 * (k + 0.5)) / n;
      const rb = t.r * (1 - Math.abs(f - 0.45) * 0.9) * R.range(0.85, 1.05);
      const x = t.x + R.range(-0.6, 0.6);
      const y = t.y + R.range(-0.6, 0.6);
      parts.push(blob(L ? x + L.x * f : x, L ? y + L.y * f : y, t.z + t.h * f, Math.max(1.2, rb), t.h * 0.13, R.next()));
    }
  },
  birch(t, R, parts) {
    // one slender stem, or two or three from the foot leaning apart
    const amp = ampOf(t);
    const n = R.next() < 0.3 ? R.int(2, 3) : 1;
    // (wild: stems lean further and bow, upright at the foot)
    const bow = t.wild ? (f) => (f <= 0.5 ? 0.6 * f : 0.3 + 1.4 * (f - 0.5)) : null;
    for (let s = 0; s < n; s += 1) {
      const a = R.range(0, Math.PI * 2);
      const lean = (n > 1 ? R.range(0.06, 0.13 + 0.06 * amp) : R.range(0, 0.05 + 0.05 * amp)) * t.h;
      const hh = t.h * (s === 0 ? 1 : R.range(0.75, 0.95));
      const top = { x: t.x + Math.cos(a) * lean, y: t.y + Math.sin(a) * lean, z: t.z + hh };
      const tr = trunkR(t, 75);
      if (bow) {
        const M = { x: t.x + (top.x - t.x) * 0.3, y: t.y + (top.y - t.y) * 0.3, z: t.z + hh * 0.5 };
        parts.push(limb(t.x, t.y, t.z - 1, M.x, M.y, M.z, tr, tr * 0.72, MAT.BARK_BIRCH, { trunk: true, birch: true }));
        parts.push(limb(M.x, M.y, M.z, top.x, top.y, top.z - t.r * 0.4, tr * 0.72, tr * 0.45, MAT.BARK_BIRCH, { trunk: true, birch: true, foot: t.z - 1 }));
      } else parts.push(limb(t.x, t.y, t.z - 1, top.x, top.y, top.z - t.r * 0.4, tr, tr * 0.45, MAT.BARK_BIRCH, { trunk: true, birch: true }));
      const m = R.int(5, 8);
      for (let k = 0; k < m; k += 1) {
        const f = R.range(0.42, 0.95);
        const g = bow ? bow(f) : f;
        const cx = t.x + (top.x - t.x) * g;
        const cy = t.y + (top.y - t.y) * g;
        const cz = t.z + hh * f;
        const ba = R.range(0, Math.PI * 2);
        const off = t.r * R.range(0.2, 0.55) * (1.15 - f * 0.5);
        const rb = t.r * R.range(0.3, 0.44);
        const bx = cx + Math.cos(ba) * off;
        const by = cy + Math.sin(ba) * off;
        const bz = cz - rb * 0.25;
        parts.push(limb(cx, cy, cz - rb * 0.3, bx, by, bz, 0.6, 0.45, MAT.BARK_BIRCH));
        const mix = R.next();
        parts.push(blob(bx, by, bz, rb, rb * 1.2, mix));
        satellites(parts, R, { x: bx, y: by, z: bz, rb, mix }, 1.2, 2, [0.4, 0.6], 0.1);
      }
      parts.push(blob(top.x, top.y, top.z - t.r * 0.35, t.r * 0.32, t.r * 0.45, R.next()));
    }
  },
  pine(t, R, parts) {
    // a long bare trunk (grey below, orange above) with a gentle S, flat clumps of needles on top
    const amp = ampOf(t);
    t = { ...t, r: t.r * 0.78 };
    const tr = trunkR(t, 55);
    const a = R.range(0, Math.PI * 2);
    const bend = R.range(0.01, 0.04 + 0.03 * amp) * t.h;
    // wild: the whole pine leans (the rock and the edge of the wood), its clumps crowding to the lit side
    const L = t.wild ? leanOf(R, amp * 0.05 * t.h) : null;
    const A = L ? amp * R.range(0.2, 0.5) : 0;
    const M = { x: t.x + Math.cos(a) * bend, y: t.y + Math.sin(a) * bend, z: t.z + t.h * 0.5 };
    const T = { x: t.x - Math.cos(a) * bend * 0.3, y: t.y - Math.sin(a) * bend * 0.3, z: t.z + t.h * 0.97 };
    if (L) {
      M.x += L.x * 0.35;
      M.y += L.y * 0.35;
      T.x += L.x;
      T.y += L.y;
    }
    parts.push(limb(t.x, t.y, t.z - 1, M.x, M.y, M.z, tr * 1.1, tr * 0.8, MAT.BARK, { trunk: true }));
    parts.push(limb(t.x, t.y, t.z - 1, t.x, t.y, t.z + 2, tr * 1.5, tr, MAT.BARK, { trunk: true }));
    parts.push(limb(M.x, M.y, M.z, T.x, T.y, T.z, tr * 0.8, tr * 0.35, MAT.BARK_PINE, { trunk: true }));
    // (a young or crowded pine keeps its needles lower down, an old one only near the top)
    const c0 = t.open ? R.range(0.48, 0.58) : R.range(0.6, 0.7);
    const n = R.int(6, 9);
    for (let k = 0; k < n; k += 1) {
      // more clumps higher up; the lower ones reach further out on stout limbs
      const q = Math.sqrt((k + R.range(0.1, 0.9)) / n);
      const f = c0 + (0.95 - c0) * q;
      const cx = t.x + (T.x - t.x) * f;
      const cy = t.y + (T.y - t.y) * f;
      const cz = t.z + t.h * f;
      const ba = R.range(0, Math.PI * 2);
      const cb = Math.cos(ba);
      const sb = Math.sin(ba);
      const off = t.r * R.range(0.2, 0.5) * (1.3 - q * 0.7) * (L ? 1 + A * (cb * L.ux + sb * L.uy) : 1);
      const rb = t.r * R.range(0.38, 0.55) * (1.1 - q * 0.3);
      const bx = cx + cb * off;
      const by = cy + sb * off;
      const bz = cz + rb * 0.25;
      parts.push(limb(cx, cy, cz - 1, bx, by, bz, tr * 0.5, 0.6, MAT.BARK_PINE));
      const mix = R.next();
      parts.push(blob(bx, by, bz, rb, rb * 0.55, mix));
      // (a clump of needles is a ragged tuft, not a plate)
      satellites(parts, R, { x: bx, y: by, z: bz, rb, mix }, 0.6, 3, [0.4, 0.6], 0.3);
    }
    // the crown's rounded top
    parts.push(blob(T.x, T.y, T.z - t.r * 0.1, t.r * 0.5, t.r * 0.32, R.next()));
  },
  spruce(t, R, parts) {
    const tr = trunkR(t, 70);
    // (wild: a slight lean and a lopsided cone)
    const L = t.wild ? leanOf(R, ampOf(t) * 0.035 * t.h) : null;
    const A = L ? ampOf(t) * R.range(0.06, 0.22) : 0;
    parts.push(limb(t.x, t.y, t.z - 1, L ? t.x + L.x : t.x, L ? t.y + L.y : t.y, t.z + t.h, tr * 1.1, 0.5, MAT.BARK, { trunk: true }));
    // crowded spruces lose their lowest whorls
    const z0 = t.z + t.h * (t.open ? R.range(0.02, 0.06) : R.range(0.1, 0.22));
    const cone = { k: CONE, x: t.x, y: t.y, z0, z1: t.z + t.h, zLive: z0, r: t.r, tier: R.int(4, 6), lobes: R.int(5, 7), phase: R.range(0, 6.3), bb: { x0: Math.floor(t.x - t.r - 3), y0: Math.floor(t.y - t.r - 3), z0: Math.floor(z0 - 1), x1: Math.ceil(t.x + t.r + 3), y1: Math.ceil(t.y + t.r + 3), z1: Math.ceil(t.z + t.h + 2) } };
    parts.push(L ? wildCone(cone, t, L, A) : cone);
  },
  juniper(t, R, parts) {
    const L = t.wild ? leanOf(R, ampOf(t) * 0.05 * t.h) : null;
    const A = L ? ampOf(t) * R.range(0.08, 0.25) : 0;
    const cone = { k: CONE, x: t.x, y: t.y, z0: t.z, z1: t.z + t.h, zLive: t.z, r: t.r, tier: 3, lobes: 4, phase: R.range(0, 6.3), bb: { x0: Math.floor(t.x - t.r - 3), y0: Math.floor(t.y - t.r - 3), z0: t.z - 1, x1: Math.ceil(t.x + t.r + 3), y1: Math.ceil(t.y + t.r + 3), z1: Math.ceil(t.z + t.h + 2) } };
    parts.push(L ? wildCone(cone, t, L, A) : cone);
  },
  dwarfpine(t, R, parts) {
    // krummholz: low sprawling mounds on crooked stems
    const n = R.int(2, 4);
    for (let k = 0; k < n; k += 1) {
      const a = R.range(0, Math.PI * 2);
      const off = t.r * R.range(0.1, 0.6);
      const rb = t.r * R.range(0.45, 0.75);
      const x = t.x + Math.cos(a) * off;
      const y = t.y + Math.sin(a) * off;
      parts.push(limb(t.x, t.y, t.z - 1, x, y, t.z + t.h * 0.4, 0.8, 0.5, MAT.BARK));
      parts.push(blob(x, y, t.z + t.h * 0.45, rb, Math.max(1.2, t.h * R.range(0.35, 0.55)), R.next()));
    }
  },
  shrub(t, R, parts) {
    t = { ...t, r: t.r * 0.75 };
    const n = R.int(2, 4);
    for (let k = 0; k < n; k += 1) {
      const a = R.range(0, Math.PI * 2);
      const off = t.r * R.range(0, 0.45);
      const rb = t.r * R.range(0.5, 0.8);
      const rz = Math.max(0.8, t.h * R.range(0.45, 0.6));
      const mix = R.next();
      parts.push(blob(t.x + Math.cos(a) * off, t.y + Math.sin(a) * off, t.z + rz * 0.55, rb, rz, mix));
      satellites(parts, R, { x: t.x + Math.cos(a) * off, y: t.y + Math.sin(a) * off, z: t.z + rz * 0.55, rb, mix }, rz / rb, 1, [0.45, 0.65], 0.2);
    }
  },
  hazel(t, R, parts) {
    // a clump of thin stems fanning out from the foot, a leafy dome on top
    const n = R.int(4, 7);
    for (let k = 0; k < n; k += 1) {
      const a = (k / n) * Math.PI * 2 + R.range(-0.4, 0.4);
      const out = t.r * R.range(0.35, 0.75);
      const top = { x: t.x + Math.cos(a) * out, y: t.y + Math.sin(a) * out, z: t.z + t.h * R.range(0.65, 0.95) };
      parts.push(limb(t.x + Math.cos(a) * 0.6, t.y + Math.sin(a) * 0.6, t.z - 1, top.x, top.y, top.z, 0.7, 0.5, MAT.BARK_GREY));
      const rb = t.r * R.range(0.4, 0.55);
      const mix = R.next();
      parts.push(blob(top.x, top.y, top.z - rb * 0.2, rb, rb * 0.8, mix));
      satellites(parts, R, { x: top.x, y: top.y, z: top.z - rb * 0.2, rb, mix }, 0.8, 1, [0.45, 0.6], 0.2);
    }
  },
  aspen(t, R, parts) {
    // a straight pale grey-green stem, a small rounded crown high up
    const tr = trunkR(t, 70);
    const lean = R.range(0, 0.03 + 0.03 * ampOf(t)) * t.h;
    const a = R.range(0, Math.PI * 2);
    const top = { x: t.x + Math.cos(a) * lean, y: t.y + Math.sin(a) * lean, z: t.z + t.h };
    parts.push(limb(t.x, t.y, t.z - 1, top.x, top.y, top.z - t.r * 0.3, tr, tr * 0.4, MAT.BARK_GREY, { trunk: true }));
    const m = R.int(8, 12);
    for (let k = 0; k < m; k += 1) {
      const f = R.range(0.4, 0.96);
      const cx = t.x + (top.x - t.x) * f;
      const cy = t.y + (top.y - t.y) * f;
      const cz = t.z + t.h * f;
      const ba = R.range(0, Math.PI * 2);
      const off = t.r * R.range(0.2, 0.6) * (1.25 - f * 0.6);
      const rb = t.r * R.range(0.36, 0.5);
      const bx = cx + Math.cos(ba) * off;
      const by = cy + Math.sin(ba) * off;
      parts.push(limb(cx, cy, cz - rb * 0.4, bx, by, cz, 0.6, 0.45, MAT.BARK_GREY));
      const mix = R.next();
      parts.push(blob(bx, by, cz, rb, rb * 0.95, mix));
      satellites(parts, R, { x: bx, y: by, z: cz, rb, mix }, 0.95, 2, [0.4, 0.55], 0.25);
    }
  },
  alder(t, R, parts) {
    // a dark egg-shaped crown on one to three stems (by the water)
    const n = R.next() < 0.35 ? 2 : 1;
    for (let k = 0; k < n; k += 1) {
      const a = R.range(0, Math.PI * 2);
      const off = n > 1 ? R.range(0.6, 1.2) : 0;
      broadleaf(t, R, parts, { fork: [0.35, 0.5], limbs: [3, 4], elev: [0.9, 1.3], spread: [0.3, 0.45], blob: [0.42, 0.55], flat: 1.05, top: 0.6, fill: 1, lean: n > 1 ? 0.1 : 0.04, twin: n > 1 ? 0 : 0.12, bark: MAT.BARK_GREY, trunk: trunkR(t, 60) }, { x: t.x + Math.cos(a) * off, y: t.y + Math.sin(a) * off, z: t.z, h: t.h * (k ? R.range(0.75, 0.9) : 1), r: t.r * (n > 1 ? 0.8 : 1) });
    }
  },
  larch(t, R, parts) {
    // an airy cone of soft needles in tiers of tufts on slightly drooping limbs (gold in autumn, bare in winter)
    const tr = trunkR(t, 60);
    // (wild: it leans, its tiers reaching further on the lit side)
    const W = t.wild ? leanOf(R, ampOf(t) * 0.04 * t.h) : null;
    const A = W ? ampOf(t) * R.range(0.15, 0.4) : 0;
    parts.push(limb(t.x, t.y, t.z - 1, W ? t.x + W.x : t.x, W ? t.y + W.y : t.y, t.z + t.h, tr * 1.1, 0.5, MAT.BARK, { trunk: true }));
    parts.push(limb(t.x, t.y, t.z - 1, t.x, t.y, t.z + 2, tr * 1.5, tr, MAT.BARK, { trunk: true }));
    const z0 = t.h * (t.open ? R.range(0.08, 0.15) : R.range(0.25, 0.35));
    const tiers = R.int(7, 10);
    for (let k = 0; k < tiers; k += 1) {
      const f = (z0 + ((t.h * 0.95 - z0) * (k + R.range(0, 0.6))) / tiers) / t.h;
      const rk = t.r * Math.pow(1 - f, 0.85) * R.range(0.85, 1.1) + 1;
      const n = R.int(3, 5);
      const a0 = R.range(0, Math.PI * 2);
      for (let q = 0; q < n; q += 1) {
        const a = a0 + (q / n) * Math.PI * 2 + R.range(-0.3, 0.3);
        const ca = Math.cos(a);
        const sa = Math.sin(a);
        const cz = t.z + t.h * f;
        // (the trunk at this height)
        const ax = W ? t.x + W.x * f : t.x;
        const ay = W ? t.y + W.y * f : t.y;
        const L = rk * R.range(0.6, 0.95) * (W ? 1 + A * (ca * W.ux + sa * W.uy) : 1);
        const E = { x: ax + ca * L, y: ay + sa * L, z: cz - L * 0.15 };
        parts.push(limb(ax, ay, cz, E.x, E.y, E.z, 0.7, 0.45, MAT.BARK));
        const rb = Math.max(1.2, rk * R.range(0.35, 0.5));
        parts.push(blob(E.x - ca * rb * 0.4, E.y - sa * rb * 0.4, E.z, rb, rb * 0.55, R.next()));
      }
    }
    parts.push(blob(W ? t.x + W.x : t.x, W ? t.y + W.y : t.y, t.z + t.h - 1.5, 1.4, 2.2, R.next()));
  },
  berry(t, R, parts) {
    parts.push(blob(t.x, t.y, t.z, Math.max(1, t.r), Math.max(0.8, t.h), R.next()));
  },
  fern(t, R, parts) {
    parts.push({ k: FERN, x: t.x, y: t.y, z: t.z, r: Math.max(3, t.r), h: Math.max(2, t.h), n: R.int(5, 8), phase: R.range(0, 6.3), bb: { x0: Math.floor(t.x - t.r - 2), y0: Math.floor(t.y - t.r - 2), z0: t.z, x1: Math.ceil(t.x + t.r + 2), y1: Math.ceil(t.y + t.r + 2), z1: t.z + Math.ceil(t.h) + 2 } });
  },
  log(t, R, parts) {
    // a fallen trunk with moss on its back; t.r is its length, t.h its thickness
    if (t.wild) {
      wildLog(t, R, parts);
      return;
    }
    const a = R.range(0, Math.PI);
    const L = t.r;
    const rr = Math.max(1.5, t.h);
    const x0 = t.x - (Math.cos(a) * L) / 2;
    const y0 = t.y - (Math.sin(a) * L) / 2;
    parts.push(limb(x0, y0, t.z + rr - 1, x0 + Math.cos(a) * L, y0 + Math.sin(a) * L, t.z + rr - 1.5, rr, rr * 0.8, MAT.DEADWOOD, { moss: true }));
  },
  stump(t, R, parts) {
    const rr = Math.max(1.5, t.r);
    parts.push(limb(t.x, t.y, t.z - 1, t.x, t.y, t.z + Math.max(2, t.h), rr * 1.2, rr, MAT.BARK, { stump: true }));
  },
  snag(t, R, parts) {
    // a dead standing trunk, a few broken limbs
    const tr = Math.max(1, t.r);
    if (t.wild) {
      wildSnag(t, R, parts, tr);
      return;
    }
    parts.push(limb(t.x, t.y, t.z - 1, t.x + R.range(-1, 1), t.y + R.range(-1, 1), t.z + t.h, tr * 1.2, tr * 0.5, MAT.DEADWOOD, { trunk: true }));
    for (let k = 0; k < R.int(1, 3); k += 1) {
      const z = t.z + t.h * R.range(0.4, 0.85);
      const a = R.range(0, Math.PI * 2);
      const L = R.range(3, 8);
      parts.push(limb(t.x, t.y, z, t.x + Math.cos(a) * L, t.y + Math.sin(a) * L, z + L * 0.5, 0.8, 0.5, MAT.DEADWOOD));
    }
  },
};
BUILD.autumn = BUILD.maple;
BUILD.shrubDry = BUILD.shrub;

/**
 * A wild cone (spruce, juniper): its axis leans with the trunk (sheared
 * from the foot up to the top offset L) and it is lopsided, fuller by up to
 * A on the lit side it leans to.
 */
function wildCone(cone, t, L, A) {
  const r = t.r * (1 + A) + 3;
  const bb = { ...cone.bb, x0: Math.floor(Math.min(t.x, t.x + L.x) - r), y0: Math.floor(Math.min(t.y, t.y + L.y) - r), x1: Math.ceil(Math.max(t.x, t.x + L.x) + r), y1: Math.ceil(Math.max(t.y, t.y + L.y) + r) };
  return { ...cone, lean: true, lx: L.x, ly: L.y, zf: t.z, hh: t.h, ax: L.ux, ay: L.uy, asym: A, bb };
}

/** A unit direction of the yaw table: { x, y }. */
function dirOf(R) {
  const Y = YAWS[R.int(0, YAWS.length - 1)];
  return { x: Y.c / Y.r, y: Y.s / Y.r };
}

/** A wild snag: the dead trunk leans at an exact tilt of the yaw table, its broken limbs along it. */
function wildSnag(t, R, parts, tr) {
  const Y = YAWS[R.int(0, YAWS.length - 1)];
  const T = YAWS[Math.round(ampOf(t) * R.int(0, SNAG_TILT))];
  const ux = (Y.c / Y.r) * (T.s / T.r);
  const uy = (Y.s / Y.r) * (T.s / T.r);
  const uz = T.c / T.r;
  parts.push(limb(t.x, t.y, t.z - 1, t.x + ux * t.h, t.y + uy * t.h, t.z + uz * t.h, tr * 1.2, tr * 0.5, MAT.DEADWOOD, { trunk: true }));
  for (let k = 0; k < R.int(1, 3); k += 1) {
    const f = R.range(0.4, 0.85);
    const x = t.x + ux * t.h * f;
    const y = t.y + uy * t.h * f;
    const z = t.z + uz * t.h * f;
    const D = dirOf(R);
    const L = R.range(3, 8);
    parts.push(limb(x, y, z, x + D.x * L, y + D.y * L, z + L * 0.5, 0.8, 0.5, MAT.DEADWOOD));
  }
}

/**
 * A wild fallen log (t.r its length, t.h its thickness), turned to an exact
 * yaw (t.yaw, a yaw-table index, or one of its own): lying along the ground
 * (t.tilt { c, s, r }, s > 0 where its +u end lies higher), or windthrown
 * (t.plate): lifted at its root end on the root plate it tore out of the
 * ground, at an exact tilt of the yaw table.
 */
function wildLog(t, R, parts) {
  const Y = YAWS[t.yaw ?? R.int(0, 2 * YAW_QUARTER - 1)];
  const ux = Y.c / Y.r;
  const uy = Y.s / Y.r;
  const rr = Math.max(1.5, t.h);
  const plate = t.plate ?? R.next() < 0.35;
  const T = plate ? YAWS[Math.max(1, Math.round(ampOf(t) * R.int(1, THROW_TILT)))] : (t.tilt ?? YAWS[0]);
  const c = T.c / T.r;
  const s = T.s / T.r;
  const hl = (t.r / 2) * c;
  // (from the broken crown end to the root end; a windthrow rests its crown end on the ground)
  const z0 = plate ? t.z + rr * 0.8 - 1 : t.z + rr - 1.25 - (t.r / 2) * s;
  const A = { x: t.x - ux * hl, y: t.y - uy * hl, z: z0 };
  const B = { x: t.x + ux * hl, y: t.y + uy * hl, z: z0 + t.r * s };
  parts.push(limb(A.x, A.y, A.z, B.x, B.y, B.z, rr * 0.8, rr, MAT.DEADWOOD, { moss: true }));
  if (!plate) return;
  // the root plate: a ragged disc of soil and roots square to the trunk, down into the ground
  const n = { x: ux * c, y: uy * c, z: s };
  const th = Math.max(1.5, rr * 0.45);
  const C = { x: B.x + n.x * th, y: B.y + n.y * th, z: B.z + n.z * th };
  const rp = Math.max(rr * 1.8, (C.z - t.z + 2) / c);
  // (its rim reaches out to 1.12 rp in lobes)
  const e = (k) => rp * 1.12 * Math.sqrt(Math.max(0, 1 - k * k)) + th * Math.abs(k) + 2;
  parts.push({ k: PLATE, x: C.x, y: C.y, z: C.z, nx: n.x, ny: n.y, nz: n.z, r: rp, th, bb: { x0: Math.floor(C.x - e(n.x)), y0: Math.floor(C.y - e(n.y)), z0: Math.floor(C.z - e(n.z)), x1: Math.ceil(C.x + e(n.x)), y1: Math.ceil(C.y + e(n.y)), z1: Math.ceil(C.z + e(n.z)) } });
}

/** A model: the tree's parts and bounds. */
function buildModel(t) {
  const parts = [];
  const build = BUILD[t.kind];
  if (build) build(t, prng(t.seed ^ 0x7ee5), parts);
  let bb = null;
  for (const p of parts) {
    const b = p.bb;
    bb = bb ? { x0: Math.min(bb.x0, b.x0), y0: Math.min(bb.y0, b.y0), z0: Math.min(bb.z0, b.z0), x1: Math.max(bb.x1, b.x1), y1: Math.max(bb.y1, b.y1), z1: Math.max(bb.z1, b.z1) } : { ...b };
  }
  // (foliage first: limbs only show where the leaves leave a gap)
  parts.sort((a, b) => (a.k === LIMB) - (b.k === LIMB));
  return { parts, bb, pal: PALETTE[t.kind] ?? PALETTE[t.kind === "autumn" ? "maple" : "oak"] };
}

/** How far (voxels) a model reaches from the tree's foot, horizontally. */
function reachOf(bb, t) {
  return bb ? Math.max(t.x - bb.x0, bb.x1 - t.x, t.y - bb.y0, bb.y1 - t.y) : 0;
}

/**
 * The tree's model (parts, bounds), built once from its seed and cached on
 * it. A wild tree keeps within the reach its emitter allows (t.reach): if
 * it would not, it leans and lops less, then its crown (a log: its length)
 * shrinks, a pure function of the tree still.
 */
export function treeModel(t) {
  if (t.model) return t.model;
  let m = buildModel(t);
  if (t.wild && t.reach !== undefined)
    for (let k = 1; k <= 7 && reachOf(m.bb, t) > t.reach; k += 1) m = buildModel(k <= 2 ? { ...t, amp: 1 - k / 2 } : { ...t, amp: 0, r: t.r * 0.9 ** (k - 2) });
  t.model = m;
  return t.model;
}

export function treeBounds(t) {
  if (SHAPES[t.kind]) {
    const r = Math.ceil(t.r) + (t.kind === "palm" ? 8 : 2);
    return { x0: t.x - r, y0: t.y - r, z0: t.z - 1, x1: t.x + r, y1: t.y + r, z1: t.z + t.h + 2 };
  }
  const bb = treeModel(t).bb;
  return bb ? { ...bb } : { x0: t.x, y0: t.y, z0: t.z, x1: t.x, y1: t.y, z1: t.z };
}

/**
 * Rasterize a tree into a chunk. Its seasonal look `t.look` (world/season.js
 * treeLook) chooses the palette, bare branches and snow; without one,
 * `snow` (0..1) is the snow cover (a snowy crown sheds its leaves).
 */
export function rasterizeTree(chunk, t, snow = 0) {
  const special = SHAPES[t.kind];
  if (special) {
    const bb = treeBounds(t);
    if (chunk.touches(bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1)) special(chunk, t, bb);
    return;
  }
  const m = treeModel(t);
  const bb = m.bb;
  if (!bb || !chunk.touches(bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1)) return;
  const look = t.look;
  const ctx = {
    snow: look ? look.snow : snow,
    bare: look ? look.bare : snow > 0.4 && !EVERGREEN.has(t.kind),
    pal: m.pal,
    season: look?.leaves ?? null,
    mix: look?.mix ?? 1,
    accent: look?.accent ?? null,
    holes: (t.kind === "birch" || t.kind === "pine" || t.kind === "rowan" || t.kind === "shrubDry" ? 0.26 : 0.14) + (look?.holes ?? 0),
    seed: t.seed,
  };
  for (const p of m.parts) {
    const b = p.bb;
    if (!chunk.touches(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1)) continue;
    if (p.k === BLOB) rasterBlob(chunk, p, ctx);
    else if (p.k === LIMB) rasterLimb(chunk, p, ctx);
    else if (p.k === CONE) rasterCone(chunk, p, ctx);
    else if (p.k === CURTAIN) rasterCurtain(chunk, p, ctx);
    else if (p.k === FERN) rasterFern(chunk, p, ctx);
    else if (p.k === PLATE) rasterPlate(chunk, p, ctx);
  }
}

/** The palette of one cluster: the season's on `mix` of them, else summer's. */
function paletteOf(p, ctx) {
  return ctx.season && p.mix < ctx.mix ? ctx.season : ctx.pal;
}

function rasterBlob(chunk, p, ctx) {
  const b = p.bb;
  const [i0, i1] = chunk.rangeX(b.x0, b.x1);
  const [j0, j1] = chunk.rangeY(b.y0, b.y1);
  const [k0, k1] = chunk.rangeZ(b.z0, b.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const d = chunk.data;
  const s = chunk.s;
  const fine = s === 1;
  const infl = fine ? 0 : s * 0.5;
  const irx = 1 / (p.rx + infl) ** 2;
  const irz = 1 / (p.rz + infl) ** 2;
  const pal = paletteOf(p, ctx);
  const seed = ctx.seed;
  const { snow, bare, holes } = ctx;
  for (let k = k0; k <= k1; k += 1) {
    const z = chunk.wz(k);
    const dz = z + 0.5 - p.z;
    const qz = dz * dz * irz;
    if (qz > 1.3) continue;
    for (let j = j0; j <= j1; j += 1) {
      const y = chunk.wy(j);
      const dy = y + 0.5 - p.y;
      for (let i = i0; i <= i1; i += 1) {
        const idx = i + j * P + k * P2;
        if (d[idx] !== 0) continue;
        const x = chunk.wx(i);
        const dx = x + 0.5 - p.x;
        const q = (dx * dx + dy * dy) * irx + qz;
        // a lumpy surface of leaf tufts (2-voxel noise) up close
        const n = fine ? (h01(x >> 1, y >> 1, z >> 1, seed) - 0.5) * 0.6 : 0;
        if (q >= 1 + n) continue;
        const shell = q > 0.68 + n;
        if (bare) {
          // leafless: a few twig voxels in the shell (the limbs and twigs are drawn anyway)
          if (!shell || h01(x, y, z, seed + 5) > (fine ? 0.07 : 0.16)) continue;
          d[idx] = snow > 0 && dz > 0 && h01(x, y, z, seed + 11) < snow * 0.4 ? MAT.SNOW : MAT.TWIGS;
          continue;
        }
        if (fine && shell && h01(x, y, z, seed) < holes) continue;
        if (snow > 0 && dz > 0) {
          // snow where nothing of the cluster lies above
          const dz2 = dz + s;
          if ((dx * dx + dy * dy) * irx + dz2 * dz2 * irz >= 1 + n && h01(x, y, z, seed + 11) < snow * 0.85) {
            d[idx] = MAT.SNOW;
            continue;
          }
        }
        if (ctx.accent && shell && h01(x, y, z, seed + 13) < 0.06) {
          d[idx] = ctx.accent;
          continue;
        }
        // sunlit tops, shaded undersides and interior
        const v = 0.5 + 0.42 * dz * Math.sqrt(irz) + (h01(x >> 1, y >> 1, z >> 1, seed + 9) - 0.5) * 0.45 - (shell ? 0 : 0.2);
        d[idx] = v < 0.34 ? pal[0] : v < 0.74 ? pal[1] : pal[2];
      }
    }
  }
}

function rasterLimb(chunk, p, ctx) {
  const s = chunk.s;
  // twigs vanish at a distance; thin limbs thicken a little so trunks still read
  if (p.twig && (s > 1 || !ctx.bare)) return;
  const grow = s > 1 ? s * 0.35 : 0;
  if (!p.trunk && s > 2 && p.r0 < 1.2) return;
  const b = p.bb;
  const [i0, i1] = chunk.rangeX(b.x0, b.x1);
  const [j0, j1] = chunk.rangeY(b.y0, b.y1);
  const [k0, k1] = chunk.rangeZ(b.z0, b.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const d = chunk.data;
  for (let k = k0; k <= k1; k += 1) {
    const z = chunk.wz(k) + 0.5;
    for (let j = j0; j <= j1; j += 1) {
      const y = chunk.wy(j) + 0.5;
      for (let i = i0; i <= i1; i += 1) {
        const idx = i + j * P + k * P2;
        if (d[idx] !== 0) continue;
        const x = chunk.wx(i) + 0.5;
        const vx = x - p.ax;
        const vy = y - p.ay;
        const vz = z - p.az;
        let u = (vx * p.dx + vy * p.dy + vz * p.dz) / p.L2;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        const ex = vx - p.dx * u;
        const ey = vy - p.dy * u;
        const ez = vz - p.dz * u;
        const r = p.r0 + (p.r1 - p.r0) * u + grow;
        if (ex * ex + ey * ey + ez * ez > r * r) continue;
        let m = p.mat;
        const wz = z - 0.5;
        // birch: short dark marks on the white bark, a dark rough foot
        if (p.birch && ((s === 1 && h01(wz, (x - 0.5) >> 1, (y - 0.5) >> 1, ctx.seed) < 0.13) || wz - (p.foot ?? p.az) < 3 + (h01(x, y, 0, ctx.seed) < 0.5 ? 2 : 0))) m = MAT.BARK_BIRCH_MARK;
        else if (p.moss && ez > 0.3 * r && ez * ez > ex * ex + ey * ey) m = MAT.MOSS;
        else if (p.stump && wz >= p.az + p.dz - 1) m = MAT.WOOD_LIGHT;
        else if (ctx.snow > 0.2 && !p.trunk && ez > 0 && ez * ez > (ex * ex + ey * ey) * 0.5 && h01(x, y, z, ctx.seed + 17) < ctx.snow * 0.6) m = MAT.SNOW;
        d[idx] = m;
      }
    }
  }
}

/**
 * Conifer cone (spruce, juniper): whorls every few voxels that droop (each
 * widest at its foot), their branches in irregular lobes round the stem;
 * dead lower whorls in a crowded stand; snow on every whorl.
 */
function rasterCone(chunk, p, ctx) {
  const b = p.bb;
  const [i0, i1] = chunk.rangeX(b.x0, b.x1);
  const [j0, j1] = chunk.rangeY(b.y0, b.y1);
  const [k0, k1] = chunk.rangeZ(b.z0, b.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const d = chunk.data;
  const s = chunk.s;
  const fine = s === 1;
  const H = Math.max(1, p.z1 - p.z0);
  const pal = ctx.pal;
  const seed = ctx.seed;
  const radius = (zz, a, ti0) => {
    const rel = (zz - p.z0) / H;
    if (rel < 0 || rel > 1) return -1;
    const env = p.r * Math.pow(1 - rel, 1.05);
    if (!fine) return env * 0.9 + 0.8 + s * 0.5;
    const fz = (zz - p.z0) / p.tier;
    const ti = ti0 ?? Math.floor(fz);
    const pt = fz - Math.floor(fz);
    const lob = 0.8 + 0.2 * Math.sin(p.lobes * a + p.phase + ti * 2.3) + (h01(ti, Math.floor((a + 3.2) * 1.6), 3, seed) - 0.5) * 0.3;
    return env * (1 - 0.4 * pt) * lob + 0.7;
  };
  for (let k = k0; k <= k1; k += 1) {
    const z = chunk.wz(k);
    const zz = z + 0.5;
    if (zz < p.z0 || zz > p.z1 + 1) continue;
    // (a wild cone leans with its trunk: the axis at this height)
    const ox = p.lean ? p.x + p.lx * ((zz - p.zf) / p.hh) : p.x;
    const oy = p.lean ? p.y + p.ly * ((zz - p.zf) / p.hh) : p.y;
    for (let j = j0; j <= j1; j += 1) {
      const y = chunk.wy(j);
      const dy = y + 0.5 - oy;
      for (let i = i0; i <= i1; i += 1) {
        const idx = i + j * P + k * P2;
        if (d[idx] !== 0) continue;
        const x = chunk.wx(i);
        const dx = x + 0.5 - ox;
        const hr = Math.sqrt(dx * dx + dy * dy);
        const a = Math.atan2(dy, dx);
        // (and it is lopsided: fuller on the lit side)
        const lop = p.lean ? 1 + (p.asym * (dx * p.ax + dy * p.ay)) / Math.max(hr, 1e-9) : 1;
        const R = p.lean ? radius(zz, a) * lop : radius(zz, a);
        // the leader: a thin spike over the top whorl
        if (R < 0) continue;
        const edge = fine ? R + (h01(x >> 1, y >> 1, z >> 1, seed) - 0.5) * 1.1 : R;
        if (hr > edge) continue;
        const shell = hr > edge - 1.5;
        if (fine && shell && h01(x, y, z, seed) < 0.12) continue;
        if (ctx.snow > 0) {
          const Ra = p.lean ? radius(zz + s, a) * lop : radius(zz + s, a);
          // (snow on the upper side of every whorl, the dark needles still showing beneath)
          if (hr > Ra - 0.6 && h01(x, y, z, seed + 11) < ctx.snow * 0.7) {
            d[idx] = MAT.SNOW;
            continue;
          }
        }
        // outer needles lighter, the inside and the underside of a whorl dark
        const pt = fine ? ((zz - p.z0) / p.tier) % 1 : 0.5;
        const v = 0.25 + 0.55 * (hr / Math.max(1, R)) + 0.25 * pt + (h01(x >> 1, y >> 1, z >> 1, seed + 9) - 0.5) * 0.35;
        d[idx] = v < 0.42 ? pal[0] : v < 0.82 ? pal[1] : pal[2];
      }
    }
  }
}

/**
 * Willow curtains: thin strands of leaves hanging from the underside of a
 * cluster, longer towards its rim, swaying in small groups.
 */
function rasterCurtain(chunk, p, ctx) {
  if (ctx.bare && chunk.s === 1) return;
  const b = p.bb;
  const [i0, i1] = chunk.rangeX(b.x0, b.x1);
  const [j0, j1] = chunk.rangeY(b.y0, b.y1);
  const [k0, k1] = chunk.rangeZ(b.z0, b.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const d = chunk.data;
  const fine = chunk.s === 1;
  const pal = ctx.bare ? [MAT.TWIGS, MAT.TWIGS, MAT.TWIGS] : paletteOf(p, ctx);
  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j);
    const dy = y + 0.5 - p.y;
    for (let i = i0; i <= i1; i += 1) {
      const x = chunk.wx(i);
      const dx = x + 0.5 - p.x;
      const rr = Math.sqrt(dx * dx + dy * dy) / p.rx;
      if (rr > 1.02 || rr < 0.5) continue;
      const strand = h01(x, y, 0, ctx.seed + 21);
      if (fine && strand > 0.45) continue;
      // hanging from the cluster's underside, longest at its rim
      const top = p.z - p.rz * Math.sqrt(Math.max(0, 1 - rr * rr)) + 1;
      const len = p.drop * (0.45 + 0.55 * h01(x >> 1, y >> 1, 1, ctx.seed + 22)) * (0.3 + 0.7 * rr);
      for (let k = k0; k <= k1; k += 1) {
        const z = chunk.wz(k);
        if (z > top || z < top - len) continue;
        const idx = i + j * P + k * P2;
        if (d[idx] === 0) d[idx] = strand < 0.1 ? pal[0] : strand < 0.3 ? pal[1] : pal[2];
      }
    }
  }
}

/** A fern: fronds arching out from the crown in every direction. */
function rasterFern(chunk, p, ctx) {
  // (ferns die back in winter)
  if (chunk.s > 1 || ctx.bare) return;
  const b = p.bb;
  const [i0, i1] = chunk.rangeX(b.x0, b.x1);
  const [j0, j1] = chunk.rangeY(b.y0, b.y1);
  const [k0, k1] = chunk.rangeZ(b.z0, b.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const d = chunk.data;
  const pal = ctx.season ?? ctx.pal;
  for (let j = j0; j <= j1; j += 1) {
    const dy = chunk.wy(j) + 0.5 - p.y;
    for (let i = i0; i <= i1; i += 1) {
      const dx = chunk.wx(i) + 0.5 - p.x;
      const hr = Math.sqrt(dx * dx + dy * dy);
      if (hr > p.r) continue;
      // near a frond's axis?
      const a = Math.atan2(dy, dx) - p.phase;
      const sector = (Math.PI * 2) / p.n;
      const off = Math.abs(((a % sector) + sector * 1.5) % sector - sector / 2) * hr;
      if (off > 0.75 + hr * 0.12) continue;
      const zf = p.z + Math.round(p.h * Math.sin(Math.min(1, hr / p.r) * Math.PI * 0.85));
      for (let k = k0; k <= k1; k += 1) {
        const z = chunk.wz(k);
        if (z < zf - (hr < 1.5 ? p.h : 0) || z > zf) continue;
        const idx = i + j * P + k * P2;
        if (d[idx] === 0) d[idx] = ctx.snow > 0.3 ? MAT.SNOW : off < 0.4 ? pal[0] : pal[1];
      }
    }
  }
}

/**
 * A windthrow's root plate: a ragged disc (normal n, radius r, half
 * thickness th) of soil threaded with roots, its face towards the trunk the
 * mossy forest floor it lifted. Up close only.
 */
function rasterPlate(chunk, p, ctx) {
  if (chunk.s > 2) return;
  const b = p.bb;
  const [i0, i1] = chunk.rangeX(b.x0, b.x1);
  const [j0, j1] = chunk.rangeY(b.y0, b.y1);
  const [k0, k1] = chunk.rangeZ(b.z0, b.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const d = chunk.data;
  const seed = ctx.seed;
  for (let k = k0; k <= k1; k += 1) {
    const z = chunk.wz(k);
    const dz = z + 0.5 - p.z;
    for (let j = j0; j <= j1; j += 1) {
      const y = chunk.wy(j);
      const dy = y + 0.5 - p.y;
      for (let i = i0; i <= i1; i += 1) {
        const idx = i + j * P + k * P2;
        if (d[idx] !== 0) continue;
        const x = chunk.wx(i);
        const dx = x + 0.5 - p.x;
        const a = dx * p.nx + dy * p.ny + dz * p.nz;
        if (a > p.th || a < -p.th) continue;
        // (a lobed, ragged rim: soil falls away between the roots)
        const rim = p.r * (0.82 + 0.3 * h01(x >> 2, y >> 2, z >> 2, seed + 31));
        const q2 = dx * dx + dy * dy + dz * dz - a * a;
        if (q2 > rim * rim || (q2 > rim * rim * 0.56 && h01(x, y, z, seed + 35) < 0.35)) continue;
        const q = h01(x, y, z, seed + 33);
        d[idx] = a < -p.th * 0.35 ? (q < 0.12 ? MAT.LICHEN : MAT.MOSS) : q < 0.2 ? MAT.BARK : q < 0.3 ? MAT.DEADWOOD : MAT.DIRT;
      }
    }
  }
}

/** Visit every representative voxel of a bounding box (fills only air). */
function eachVoxel(chunk, bb, fn) {
  const [i0, i1] = chunk.rangeX(bb.x0, bb.x1);
  const [j0, j1] = chunk.rangeY(bb.y0, bb.y1);
  const [k0, k1] = chunk.rangeZ(bb.z0, bb.z1);
  const d = chunk.data;
  for (let k = k0; k <= k1; k += 1) {
    const z = chunk.wz(k);
    for (let j = j0; j <= j1; j += 1) {
      const y = chunk.wy(j);
      for (let i = i0; i <= i1; i += 1) {
        const idx = i + j * P + k * P2;
        if (d[idx] !== 0) continue;
        const m = fn(chunk.wx(i), y, z);
        if (m) d[idx] = m;
      }
    }
  }
}

/** Trees with shapes of their own (no clusters): acacia, cactus, palm. */
const SHAPES = {
  /** umbrella crown on a leaning, forking trunk */
  acacia(chunk, t, bb) {
    const top = t.z + t.h;
    const crown0 = top - 3;
    const lean = ((t.seed >>> 4) & 3) - 1.5;
    eachVoxel(chunk, bb, (x, y, z) => {
      const dx = x - t.x;
      const dy = y - t.y;
      const hr = Math.hypot(dx, dy);
      if (z >= t.z && z < crown0) {
        const k = (z - t.z) / Math.max(1, crown0 - t.z);
        const ax = lean * k * 3;
        if (Math.hypot(dx - ax, dy) <= 1.1) return MAT.BARK;
        if (k > 0.6 && Math.hypot(dx - ax + (k - 0.6) * 8, dy) <= 0.8) return MAT.BARK;
        return 0;
      }
      if (z >= crown0 && z <= top) {
        const rr = t.r * (z === top ? 0.75 : 1) + (h01(x >> 1, y >> 1, z, t.seed) - 0.5) * 1.5;
        return hr <= rr && h01(x, y, z, t.seed + 5) > 0.1 ? MAT.LEAVES_ACACIA : 0;
      }
      return 0;
    });
  },
  /** saguaro: a column with one or two upturned arms */
  cactus(chunk, t, bb) {
    const arms = [];
    const n = 1 + ((t.seed >>> 3) & 1);
    for (let a = 0; a < n; a += 1) {
      const ang = ((t.seed >>> (6 + a * 3)) & 7) * (Math.PI / 4) + a * Math.PI;
      const z0 = t.z + Math.round(t.h * (0.35 + 0.15 * a));
      arms.push({ ox: Math.round(Math.cos(ang) * 4), oy: Math.round(Math.sin(ang) * 4), z0 });
    }
    eachVoxel(chunk, bb, (x, y, z) => {
      const dx = x - t.x;
      const dy = y - t.y;
      if (z >= t.z && z <= t.z + t.h && dx * dx + dy * dy <= 2) return MAT.CACTUS;
      for (const a of arms) {
        // horizontal elbow then an upright arm
        if (z === a.z0 || z === a.z0 + 1) {
          const tt = (dx * a.ox + dy * a.oy) / 16;
          if (tt >= 0 && tt <= 1 && Math.hypot(dx - a.ox * tt, dy - a.oy * tt) <= 1) return MAT.CACTUS;
        }
        if (z >= a.z0 && z <= a.z0 + Math.round(t.h * 0.35) && Math.hypot(dx - a.ox, dy - a.oy) <= 1) return MAT.CACTUS;
      }
      return 0;
    });
  },
  /** straight thin trunk with drooping fronds */
  palm(chunk, t, bb) {
    const top = t.z + t.h;
    const leanX = (((t.seed >>> 2) & 7) - 3.5) * 0.35;
    const leanY = (((t.seed >>> 5) & 7) - 3.5) * 0.35;
    const nF = 7;
    eachVoxel(chunk, bb, (x, y, z) => {
      const k = (z - t.z) / t.h;
      const cx = t.x + leanX * k * k * 6;
      const cy = t.y + leanY * k * k * 6;
      if (z >= t.z && z <= top && Math.hypot(x - cx, y - cy) <= 0.9) return MAT.BARK_PALM;
      const dx = x - (t.x + leanX * 6);
      const dy = y - (t.y + leanY * 6);
      const dr = Math.hypot(dx, dy);
      if (dr > t.r || z > top + 1 || z < top - 4) return 0;
      const want = Math.round(top + 1 - (dr / t.r) ** 2 * 4);
      if (z !== want && z !== want - 1) return 0;
      const ang = Math.atan2(dy, dx) + (t.seed & 7) * 0.3;
      const f = ((ang / (2 * Math.PI)) * nF + nF) % 1;
      return Math.abs(f - 0.5) < 0.16 + 0.25 / Math.max(1, dr) ? MAT.PALM_FROND : 0;
    });
  },
};
