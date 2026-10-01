import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { buildChunk, groundTile } from "../src/engine/voxel/compose.js";
import { ChunkBuffer, P, P2 } from "../src/engine/voxel/chunk.js";
import { hash32 } from "../src/engine/core/hash.js";
import { rasterizeTree, treeBounds, treeModel, TREE_KINDS } from "../src/engine/nature/trees.js";

/**
 * Organic vegetation (ANGLED_WORLD_PLAN.md S2): the angled world's woods
 * grow wild. Trees stray from their lattice cells, lean, fork and lop to the
 * light; snags lean; logs lie along the ground at exact angles or rest,
 * windthrown, on their root plates; boulders turn and tip. All of it stays
 * in the world grid (no parts, no physics cost) and within the feature
 * sources' budgets: every tree, plant and boulder is still found by every
 * chunk it reaches, so chunks stay pure and seamless.
 */

/** The forest's reach (voxels) for a wild tree (MAX_R - 1 - STRAY) and for the understory (U_R - 1). */
const TREE_REACH = 57;
const UNDER_REACH = 27;
const UNDER = new Set(["fern", "berry", "log", "stump", "shrub", "shrubDry", "hazel", "juniper"]);
/** The dense rugged wood of the angled forest town (and the same spot of its axis-aligned twin). */
const WOOD = { x: -1100 * 8, y: -1100 * 8 };

const worlds = new Map();
/** A forest-size preset world (cached), its world.angles.features patched by `features`. */
function world(preset, features = null) {
  const key = `${preset}/${JSON.stringify(features)}`;
  if (!worlds.has(key)) {
    const cfg = presetConfig(preset, { size: "forest" });
    if (features) cfg.world.angles = { ...cfg.world.angles, features: { ...cfg.world.angles?.features, ...features } };
    worlds.set(key, createWorld(cfg));
  }
  return worlds.get(key);
}

const reachOf = (t, bb = t.bb ?? treeBounds(t)) => Math.max(t.x - bb.x0, bb.x1 - t.x, t.y - bb.y0, bb.y1 - t.y);

/** Rasterize one tree into all chunks it touches at a LOD: voxels, voxels outside its bounds, voxels at its foot. */
function rasterStats(t, lod) {
  const bb = treeBounds(t);
  const s = 32 << lod;
  let n = 0;
  let outside = 0;
  let foot = 0;
  for (let cz = Math.floor((bb.z0 - 4) / s); cz <= Math.floor((bb.z1 + 4) / s); cz += 1)
    for (let cy = Math.floor((bb.y0 - 4) / s); cy <= Math.floor((bb.y1 + 4) / s); cy += 1)
      for (let cx = Math.floor((bb.x0 - 4) / s); cx <= Math.floor((bb.x1 + 4) / s); cx += 1) {
        const ch = new ChunkBuffer(lod, cx, cy, cz);
        rasterizeTree(ch, t);
        for (let k = 1; k < P - 1; k += 1)
          for (let j = 1; j < P - 1; j += 1)
            for (let i = 1; i < P - 1; i += 1) {
              if (!ch.data[i + j * P + k * P2]) continue;
              n += 1;
              const x = ch.wx(i);
              const y = ch.wy(j);
              const z = ch.wz(k);
              if (x < bb.x0 - lod * 2 || x > bb.x1 + lod * 2 || y < bb.y0 - lod * 2 || y > bb.y1 + lod * 2 || z < bb.z0 - lod * 2 || z > bb.z1 + lod * 2) outside += 1;
              if (lod === 0 && z <= t.z + 1 && Math.abs(x - t.x) <= 8 && Math.abs(y - t.y) <= 8) foot += 1;
            }
      }
  return { n, outside, foot };
}

test("wild tree models: every kind stays inside its bounds and its emitter's reach, stands on its foot, reads at coarse LODs", () => {
  for (const [kind, spec] of Object.entries(TREE_KINDS)) {
    if (kind === "berry") continue;
    const under = UNDER.has(kind);
    for (let k = 0; k < 6; k += 1) {
      const f = (k % 3) / 2;
      // (the forest's largest: open-grown, a quarter broader)
      const t = { x: 1000 + k * 200, y: 1000, z: 8, h: Math.round((spec.h[0] + (spec.h[1] - spec.h[0]) * f) * 8 * (under ? 1 : 1.1)), r: (spec.r[0] + (spec.r[1] - spec.r[0]) * f) * 8 * (under ? 1 : 1.25), kind, seed: hash32(k, kind.length, 5), open: k > 2, wild: true, reach: under ? UNDER_REACH : TREE_REACH };
      if (kind === "log") t.plate = k % 2 === 0;
      const a = rasterStats(t, 0);
      assert.ok(a.n > 5, `${kind}: voxels at LOD 0`);
      assert.equal(a.outside, 0, `${kind}: ${a.outside} voxels outside its bounds`);
      assert.ok(reachOf(t) <= t.reach, `${kind}: reaches ${reachOf(t)} > ${t.reach}`);
      if (!["fern", "shrub", "shrubDry", "cactus", "acacia", "palm", "log"].includes(kind)) assert.ok(a.foot > 0, `${kind}: stands on its foot`);
      if (spec.h[0] >= 4) assert.ok(rasterStats(t, 2).n > 3, `${kind}: visible at LOD 2`);
    }
  }
  // a windthrow: the log rises to its root plate, which reaches down into the ground
  const log = { x: 0, y: 0, z: 8, h: 4, r: 40, kind: "log", seed: 77, wild: true, plate: true, reach: UNDER_REACH };
  const parts = treeModel(log).parts;
  const plate = parts.find((p) => p.nx !== undefined);
  const trunk = parts.find((p) => p.moss);
  assert.ok(plate && trunk && trunk.dz > 3, "a lifted trunk on its root plate");
  assert.ok(plate.z - plate.r * Math.sqrt(1 - plate.nz * plate.nz) <= log.z - 1, "the plate stands in the ground");
});

test("wild woods: trees stray and lean, and every tree, plant and boulder is found by every chunk it reaches", () => {
  const w = world("angledNordicTown");
  const f = w.forest;
  let n = 0;
  let stray = 0;
  for (const [ox, oy] of [[0, 0], [128, 96], [-200, 300]]) {
    const rect = { x0: WOOD.x + ox, y0: WOOD.y + oy, x1: WOOD.x + ox + 127, y1: WOOD.y + oy + 127 };
    const hit = (bb) => bb.x1 >= rect.x0 && bb.x0 <= rect.x1 && bb.y1 >= rect.y0 && bb.y0 <= rect.y1;
    // (brute force: every lattice cell far round the rect)
    const trees = new Set(f.treesIn(rect));
    for (let gy = Math.floor(rect.y0 / 40) - 4; gy <= Math.floor(rect.y1 / 40) + 4; gy += 1)
      for (let gx = Math.floor(rect.x0 / 40) - 4; gx <= Math.floor(rect.x1 / 40) + 4; gx += 1)
        for (const t of [f.treeAt(gx, gy), f.treeAt2(gx, gy)]) {
          if (!t) continue;
          assert.ok(t.wild && reachOf(t) <= TREE_REACH, `tree ${gx},${gy}: reach ${reachOf(t)}`);
          if (hit(t.bb)) assert.ok(trees.has(t), `tree ${gx},${gy} missed`);
          n += 1;
          if (t.x < gx * 40 || t.x >= gx * 40 + 40 || t.y < gy * 40 || t.y >= gy * 40 + 40) stray += 1;
        }
    const under = new Set(f.understoryIn(rect));
    for (let gy = Math.floor(rect.y0 / 20) - 3; gy <= Math.floor(rect.y1 / 20) + 3; gy += 1)
      for (let gx = Math.floor(rect.x0 / 20) - 3; gx <= Math.floor(rect.x1 / 20) + 3; gx += 1) {
        const t = f.understoryAt(gx, gy);
        if (!t) continue;
        assert.ok(reachOf(t) <= UNDER_REACH, `${t.kind} ${gx},${gy}: reach ${reachOf(t)}`);
        if (hit(t.bb)) assert.ok(under.has(t), `${t.kind} ${gx},${gy} missed`);
      }
    const rocks = new Set(w.boulders.near(rect));
    for (let gy = Math.floor(rect.y0 / 48) - 2; gy <= Math.floor(rect.y1 / 48) + 2; gy += 1)
      for (let gx = Math.floor(rect.x0 / 48) - 2; gx <= Math.floor(rect.x1 / 48) + 2; gx += 1) {
        const b = w.boulders.at(gx, gy);
        if (!b) continue;
        assert.ok(b.rot && Math.max(b.x - b.bb.x0, b.bb.x1 - b.x, b.y - b.bb.y0, b.bb.y1 - b.y) <= 24, `boulder ${gx},${gy}`);
        if (hit(b.bb)) assert.ok(rocks.has(b), `boulder ${gx},${gy} missed`);
      }
  }
  // (up to 1.25 m beyond a 5 m cell: over half of them leave it)
  assert.ok(n > 100 && stray > n * 0.35, `${stray} of ${n} trees stray`);
});

/** Lean (trunk top off its foot per height), spacing and the lie of logs and boulders, over one wood. */
function woodStats(w) {
  const rect = { x0: WOOD.x - 480, y0: WOOD.y - 480, x1: WOOD.x + 480, y1: WOOD.y + 480 };
  const trees = w.forest.treesIn(rect).filter((t) => t.x >= rect.x0 && t.x <= rect.x1 && t.y >= rect.y0 && t.y <= rect.y1);
  let lean = 0;
  let n = 0;
  let leaners = 0;
  for (const t of trees) {
    let top = null;
    for (const p of treeModel(t).parts) if (p.trunk && (!top || p.az + p.dz > top.z)) top = { x: p.ax + p.dx, y: p.ay + p.dy, z: p.az + p.dz };
    if (!top) continue;
    const off = Math.hypot(top.x - t.x, top.y - t.y) / Math.max(1, top.z - t.z);
    lean += off;
    n += 1;
    if (off > 0.05) leaners += 1;
  }
  const nn = trees.map((t) => {
    let d = Infinity;
    for (const u of trees) if (u !== t) d = Math.min(d, Math.hypot(u.x - t.x, u.y - t.y));
    return d;
  });
  const mean = nn.reduce((a, b) => a + b, 0) / nn.length;
  const cv = Math.sqrt(nn.reduce((a, b) => a + (b - mean) ** 2, 0) / nn.length) / mean;
  const logs = w.forest.understoryIn(rect).filter((t) => t.kind === "log");
  const rocks = w.boulders.near({ x0: rect.x0 - 2000, y0: rect.y0 - 2000, x1: rect.x1 + 2000, y1: rect.y1 + 2000 });
  return { trees: trees.length, lean: lean / n, leaners: leaners / n, cv, logs, rocks };
}

test("wild woods read less regular: trees lean and part, logs follow the ground, boulders turn every way", () => {
  const a = woodStats(world("nordicTown"));
  const b = woodStats(world("angledNordicTown"));
  assert.ok(a.trees > 400 && Math.abs(b.trees - a.trees) < a.trees * 0.1, `${a.trees} / ${b.trees} trees`);
  // twice the lean, and twice the trees leaning noticeably
  assert.ok(b.lean > 2 * a.lean && b.leaners > 2 * a.leaners, `lean ${a.lean.toFixed(3)} -> ${b.lean.toFixed(3)}, leaners ${a.leaners.toFixed(2)} -> ${b.leaners.toFixed(2)}`);
  // spacing: from a little more regular than random to a little clumped
  assert.ok(b.cv > a.cv + 0.04, `nearest-neighbour spread ${a.cv.toFixed(3)} -> ${b.cv.toFixed(3)}`);
  // logs: level in the axis-aligned wood; here most lie along the slope, at exact tilts, some windthrown
  assert.ok(a.logs.every((t) => t.tilt === undefined && !t.plate));
  const tilted = b.logs.filter((t) => t.tilt.s !== 0);
  assert.ok(tilted.length > b.logs.length * 0.4 && b.logs.some((t) => t.plate), `${tilted.length} of ${b.logs.length} logs tilted`);
  for (const t of b.logs) assert.equal(t.tilt.c * t.tilt.c + t.tilt.s * t.tilt.s, t.tilt.r * t.tilt.r, "an exact tilt");
  // boulders: every one turned and tipped by an exact rotation (MᵀM = d²I in integers), long axes every way
  assert.ok(a.rocks.every((r) => !r.rot) && b.rocks.length > 100);
  const ways = new Set();
  let tipped = 0;
  for (const r of b.rocks) {
    const { m, d } = r.rot;
    for (let i = 0; i < 3; i += 1)
      for (let j = 0; j < 3; j += 1) assert.equal(m[i] * m[j] + m[3 + i] * m[3 + j] + m[6 + i] * m[6 + j], i === j ? d * d : 0);
    ways.add(`${m[0]},${m[3]}`);
    if (m[8] !== d) tipped += 1;
  }
  assert.ok(ways.size > 30 && tipped > b.rocks.length * 0.7, `${ways.size} ways, ${tipped} of ${b.rocks.length} tipped`);
});

/** World voxels where a chunk's east and south padding disagree with its neighbours' own voxels. */
function seams(w, mx, my, z) {
  const A = buildChunk(w, 0, mx, my, z);
  const E = buildChunk(w, 0, mx + 1, my, z);
  const S = buildChunk(w, 0, mx, my + 1, z);
  const out = new Set();
  let solid = 0;
  for (let k = 1; k <= 32; k += 1)
    for (let q = 1; q <= 32; q += 1) {
      if (A.data[33 + q * P + k * P2] !== E.data[1 + q * P + k * P2]) out.add(`${A.wx(33)},${A.wy(q)},${A.wz(k)}`);
      if (A.data[q + 33 * P + k * P2] !== S.data[q + P + k * P2]) out.add(`${A.wx(q)},${A.wy(33)},${A.wz(k)}`);
      if (E.data[1 + q * P + k * P2]) solid += 1;
    }
  return { out, solid };
}

test("wild woods: chunks are pure (any order, any worker) and agree across their borders", () => {
  const a = world("angledNordicTown");
  const b = createWorld(presetConfig("angledNordicTown", { size: "forest" }));
  const mx = Math.floor(WOOD.x / 32);
  const my = Math.floor(WOOD.y / 32);
  const cz = Math.floor(groundTile(a, 0, mx, my).z[16 + 16 * P] / 32);
  const keys = [[0, mx, my, cz], [0, mx + 1, my, cz], [0, mx, my + 1, cz + 1], [0, mx + 1, my + 1, cz + 2], [2, mx >> 2, my >> 2, cz >> 2]];
  const hash = (c) => {
    let h = 0;
    for (let i = 0; i < c.data.length; i += 1) h = hash32(h, c.data[i], i);
    return h;
  };
  const ha = keys.map(([l, x, y, z]) => hash(buildChunk(a, l, x, y, z)));
  const hb = keys.slice().reverse().map(([l, x, y, z]) => hash(buildChunk(b, l, x, y, z))).reverse();
  assert.deepEqual(ha, hb);
  // no seam the axis-aligned wood does not have (its ground cover leaves a few moss voxels out of the padding)
  const base = world("nordicTown");
  let canopy = 0;
  for (const [x, y] of [[mx, my], [mx + 2, my + 1]])
    for (let z = cz; z <= cz + 3; z += 1) {
      const s = seams(a, x, y, z);
      const ref = seams(base, x, y, z).out;
      const extra = [...s.out].filter((v) => !ref.has(v));
      assert.equal(extra.length, 0, `chunk ${x},${y},${z}: new seams at ${extra.slice(0, 4)}`);
      if (z > cz) canopy += s.solid;
    }
  assert.ok(canopy > 500, `crowns across the borders (${canopy} voxels)`);
});

test("wild woods: the new geometry takes its angles from the exact tables (no sin, cos or atan2)", () => {
  const body = (file, name) => {
    const src = readFileSync(new URL(`../src/engine/nature/${file}`, import.meta.url), "utf8");
    const at = src.search(new RegExp(`(^|\\n)\\s*(function )?${name}\\(`));
    assert.ok(at >= 0, `${file}: ${name}`);
    let depth = 0;
    let i = src.indexOf("{", at);
    const start = i;
    for (; i < src.length; i += 1) {
      if (src[i] === "{") depth += 1;
      else if (src[i] === "}" && --depth === 0) break;
    }
    return src.slice(start, i + 1).replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/.*$/gm, "");
  };
  const fns = { "trees.js": ["ampOf", "leanOf", "dirOf", "wildCone", "wildSnag", "wildLog", "rasterPlate"], "boulders.js": ["tip", "rasterTipped"], "forest.js": ["layLog"] };
  for (const [file, names] of Object.entries(fns)) for (const name of names) assert.doesNotMatch(body(file, name), /Math\.(sin|cos|tan|atan2?|asin|acos)\b/, `${file}: ${name}`);
});

test("wild woods are the angled world's only, and switch off with features.vegetation", () => {
  const off = world("angledNordicTown", { vegetation: false });
  assert.equal(off.config.world.angles.enabled, true);
  const rect = { x0: WOOD.x - 200, y0: WOOD.y - 200, x1: WOOD.x + 200, y1: WOOD.y + 200 };
  const trees = off.forest.treesIn(rect);
  assert.ok(trees.length > 50 && trees.every((t) => !t.wild));
  assert.ok(off.forest.understoryIn(rect).every((t) => !t.wild));
  assert.ok(off.boulders.near({ x0: rect.x0 - 2000, y0: rect.y0 - 2000, x1: rect.x1 + 2000, y1: rect.y1 + 2000 }).every((b) => !b.rot));
  // (the axis-aligned woods, tree for tree, wherever the angled streets leave them be)
  const base = world("nordicTown");
  const key = (t) => `${t.x},${t.y},${t.z},${t.kind},${t.h},${t.seed}`;
  const same = new Set(base.forest.treesIn(rect).map(key));
  assert.ok(trees.filter((t) => same.has(key(t))).length > trees.length * 0.95);
});
