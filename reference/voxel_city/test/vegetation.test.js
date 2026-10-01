import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { ChunkBuffer, P, P2 } from "../src/engine/voxel/chunk.js";
import { MAT } from "../src/engine/voxel/materials.js";
import { rasterizeTree, treeBounds, TREE_KINDS } from "../src/engine/nature/trees.js";
import { seasonOf } from "../src/engine/world/season.js";
import { hash32 } from "../src/engine/core/hash.js";
import { parkLayout, pathDist, pondDist } from "../src/engine/city/parks.js";

/**
 * Vegetation: tree models (every kind stays inside its bounds, stands on
 * its foot, reads at coarse LODs), forest stands and understory, parks.
 */

/** Rasterize one tree into all chunks it touches at a LOD; voxel count, and how many lie outside its bounds. */
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

test("tree models: every kind stays inside its bounds, stands on its foot, reads at coarse LODs", () => {
  for (const [kind, spec] of Object.entries(TREE_KINDS)) {
    if (kind === "log" || kind === "berry") continue;
    for (let k = 0; k < 3; k += 1) {
      const f = k / 2;
      const t = { x: 1000 + k * 200, y: 1000, z: 8, h: Math.round((spec.h[0] + (spec.h[1] - spec.h[0]) * f) * 8), r: (spec.r[0] + (spec.r[1] - spec.r[0]) * f) * 8, kind, seed: hash32(k, kind.length, 3) };
      const a = rasterStats(t, 0);
      assert.ok(a.n > 5, `${kind}: voxels at LOD 0`);
      assert.equal(a.outside, 0, `${kind}: ${a.outside} voxels outside its bounds`);
      if (!["fern", "shrub", "shrubDry", "cactus", "acacia", "palm"].includes(kind)) assert.ok(a.foot > 0, `${kind}: stands on its foot`);
      if (spec.h[0] >= 4) assert.ok(rasterStats(t, 2).n > 3, `${kind}: visible at LOD 2`);
    }
  }
});

test("tree models: crowns are clusters of foliage on limbs, not one ball; winter strips them to twigs", () => {
  const t = { x: 2000, y: 2000, z: 8, h: 12 * 8, r: 3.8 * 8, kind: "oak", seed: 12345 };
  const count = (tt) => {
    const out = new Map();
    const bb = treeBounds(tt);
    for (let cz = Math.floor(bb.z0 / 32); cz <= Math.floor(bb.z1 / 32); cz += 1)
      for (let cy = Math.floor(bb.y0 / 32); cy <= Math.floor(bb.y1 / 32); cy += 1)
        for (let cx = Math.floor(bb.x0 / 32); cx <= Math.floor(bb.x1 / 32); cx += 1) {
          const ch = new ChunkBuffer(0, cx, cy, cz);
          rasterizeTree(ch, tt);
          for (let k = 1; k < P - 1; k += 1) for (let j = 1; j < P - 1; j += 1) for (let i = 1; i < P - 1; i += 1) {
            const m = ch.data[i + j * P + k * P2];
            if (m) out.set(m, (out.get(m) ?? 0) + 1);
          }
        }
    return out;
  };
  const summer = count({ ...t });
  const leaves = (summer.get(MAT.LEAVES) ?? 0) + (summer.get(MAT.LEAVES_DARK) ?? 0) + (summer.get(MAT.LEAVES_LIGHT) ?? 0);
  assert.ok(leaves > 2000, `foliage ${leaves}`);
  // three shades: shadowed, body and sunlit
  for (const m of [MAT.LEAVES, MAT.LEAVES_DARK, MAT.LEAVES_LIGHT]) assert.ok((summer.get(m) ?? 0) > 100, `shade ${m}`);
  assert.ok((summer.get(MAT.BARK) ?? 0) > 50, "trunk and limbs");
  const look = seasonOf({ world: { season: "winter" } }).treeLook("oak", t.seed, 0.36);
  const winter = count({ ...t, look });
  assert.equal((winter.get(MAT.LEAVES) ?? 0) + (winter.get(MAT.LEAVES_DARK) ?? 0), 0, "no leaves in winter");
  assert.ok((winter.get(MAT.TWIGS) ?? 0) > 50 && (winter.get(MAT.BARK) ?? 0) > 50, "bare limbs and twigs");
});

test("forests: species grow in stands, crowded trees grow slim, an understory at LOD 0", () => {
  const w = createWorld({ seed: 1337 });
  // a wooded stretch of temperate country near the spawn (searched for, so
  // farmland and terrain tuning cannot quietly move the test out of the woods)
  let trees = [];
  let spot = null;
  for (let gy = -12000; gy <= 12000; gy += 1000)
    for (let gx = -12000; gx <= 12000; gx += 1000) {
      const x = gx * 8;
      const y = gy * 8;
      const ts = w.terrain.sample(x, y);
      const b = w.landCover.biomeAt(x, y, ts.h / 8);
      if (b.id !== "temperate" || w.landCover.forestDensity(x, y, ts.u, b) < 0.5) continue;
      const here = w.forest.treesIn({ x0: x - 1600, y0: y - 1600, x1: x + 1600, y1: y + 1600 });
      if (here.length > trees.length) [trees, spot] = [here, { x, y }];
    }
  assert.ok(trees.length > 800, `${trees.length} trees`);
  const kinds = new Map();
  for (const t of trees) kinds.set(t.kind, (kinds.get(t.kind) ?? 0) + 1);
  assert.ok(kinds.size >= 4, `species ${[...kinds.keys()]}`);
  // stands: in 50 m squares the commonest species is more dominant than overall
  const overall = Math.max(...kinds.values()) / trees.length;
  const squares = new Map();
  for (const t of trees) {
    const k = `${Math.floor(t.x / 400)},${Math.floor(t.y / 400)}`;
    const m = squares.get(k) ?? new Map();
    m.set(t.kind, (m.get(t.kind) ?? 0) + 1);
    squares.set(k, m);
  }
  let dom = 0;
  let n = 0;
  for (const m of squares.values()) {
    const total = [...m.values()].reduce((a, b) => a + b, 0);
    if (total < 20) continue;
    dom += Math.max(...m.values()) / total;
    n += 1;
  }
  assert.ok(n > 4 && dom / n > overall + 0.05, `local dominance ${(dom / n).toFixed(2)} vs overall ${overall.toFixed(2)}`);
  // understory: ferns, shrubs, saplings, deadwood
  const under = w.forest.understoryIn({ x0: spot.x - 400, y0: spot.y - 400, x1: spot.x + 400, y1: spot.y + 400 });
  const uk = new Set(under.map((t) => t.kind));
  assert.ok(under.length > 50, `${under.length} understory plants`);
  assert.ok(uk.size >= 3, `understory kinds ${[...uk]}`);
});

test("parks: winding paths join every entrance to the hub, the pond keeps off the paths", () => {
  const w = createWorld({ seed: 1337 });
  let parks = 0;
  let ponds = 0;
  for (let j = -2; j <= 2; j += 1)
    for (let i = -2; i <= 2; i += 1)
      for (const sp of w.cellPlan(i, j).spaces) {
        if (sp.kind !== "park") continue;
        const L = parkLayout(sp);
        parks += 1;
        for (const e of L.ents) assert.ok(pathDist(L, Math.min(L.W - 1, Math.max(1, e.u)), Math.min(L.H - 1, Math.max(1, e.v))) < L.pathW + 16, `${sp.id}: entrance on a path`);
        if (L.pond) {
          ponds += 1;
          for (const s of L.segs) assert.ok(pondDist(L.pond, (s[0] + s[2]) / 2, (s[1] + s[3]) / 2) > 8, `${sp.id}: path through the pond`);
        }
      }
  assert.ok(parks > 5 && ponds > 0, `${parks} parks, ${ponds} ponds`);
});

test("rugged northern woods: boulders, hummocky ground, a thick undergrowth and a mixed stand", async () => {
  const { presetConfig } = await import("../src/engine/config/presets.js");
  const w = createWorld(presetConfig("nordicTown", { size: "forest", seed: 1337 }));
  // the densest rugged wood on the island (searched for)
  let spot = null;
  for (let gy = -1500; gy <= 1500; gy += 100)
    for (let gx = -1500; gx <= 1500; gx += 100) {
      const x = gx * 8;
      const y = gy * 8;
      const ts = w.terrain.sample(x, y);
      if (ts.u > 0.02 || ts.coast < 60) continue;
      const b = w.landCover.biomeAt(x, y, ts.h / 8);
      const fd = w.landCover.forestDensity(x, y, ts.u, b);
      if (fd > 0.6 && ts.rugged > 0.4 && (!spot || fd > spot.fd)) spot = { x, y, fd };
    }
  assert.ok(spot, "a rugged wood");
  const r = { x0: spot.x - 480, y0: spot.y - 480, x1: spot.x + 480, y1: spot.y + 480 };
  const trees = w.forest.treesIn(r);
  const kinds = new Set(trees.map((t) => t.kind));
  assert.ok(trees.length > 150 && kinds.size >= 4, `${trees.length} trees of ${[...kinds]}`);
  assert.ok(kinds.has("spruce") || kinds.has("pine"), "conifers");
  const under = w.forest.understoryIn(r);
  assert.ok(under.length > 400, `${under.length} understory plants`);
  // glacial erratics, mossy on top
  const rocks = w.boulders.near({ x0: spot.x - 1600, y0: spot.y - 1600, x1: spot.x + 1600, y1: spot.y + 1600 });
  assert.ok(rocks.length >= 3 && rocks.some((b) => b.moss > 0), `${rocks.length} boulders`);
  // hummocky ground: heights along a 100 m line stray from a straight fit
  const hs = [];
  for (let k = 0; k <= 50; k += 1) hs.push(w.terrain.sample(spot.x - 400 + k * 16, spot.y).h / 8);
  const n = hs.length;
  const mx = (n - 1) / 2;
  const my = hs.reduce((a, b) => a + b, 0) / n;
  let sxy = 0;
  let sxx = 0;
  hs.forEach((h, i) => {
    sxy += (i - mx) * (h - my);
    sxx += (i - mx) ** 2;
  });
  const b1 = sxy / sxx;
  const sd = Math.sqrt(hs.reduce((a, h, i) => a + (h - my - b1 * (i - mx)) ** 2, 0) / n);
  assert.ok(sd > 0.3, `relief ${sd.toFixed(2)} m off a straight line`);
});
