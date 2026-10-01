import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { tileMessage } from "../src/engine/stream/worker.js";
import { partBit, partLocal, partMatrix } from "../src/viewer/partBits.js";
import { partChunk } from "../src/engine/world/partRaster.js";
import { IS_SOLID } from "../src/engine/voxel/materials.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";

/**
 * The viewer in parts mode (the angled world): a column tile brings the
 * parts at home in it, meshed in their own lattices at the tile's LOD, so
 * every part is drawn once at every LOD, with its exact matrix; the walker
 * collides with a part through the exact inverse map of its placement.
 */

const cfg = presetConfig("angledOldHarbourTown");
cfg.world.angles = { ...cfg.world.angles, partsMode: "separate" };
const world = createWorld(cfg);
const parts = [];
for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) parts.push(...world.cellPlan(i, j).parts);

test("viewer parts: every part comes with the tile of its home chunk, meshed in its lattice, at every LOD", () => {
  const road = parts.find((p) => p.kind === "road");
  const building = parts.find((p) => p.kind === "building");
  const wing = parts.find((p) => p.kind === "wing");
  for (const p of [road, building, wing]) {
    for (const lod of [0, 2, 4]) {
      const n = 1 << lod;
      const { message } = tileMessage(world, { id: 1, lod, cx: Math.floor(p.home.cx / n), cy: Math.floor(p.home.cy / n), collision: true });
      const got = message.parts.filter((q) => q.id === p.id);
      assert.equal(got.length, 1, `${p.key}: once in its LOD ${lod} tile`);
      const q = got[0];
      assert.ok(q.chunks.length > 0 && q.chunks.some((c) => c.opaque), `${p.key}: meshed at LOD ${lod}`);
      assert.equal(q.lod, lod);
      assert.deepEqual(q.m, Array.from(p.placement.m));
      if (lod === 0) assert.ok(q.chunks.some((c) => c.solid), `${p.key}: its cells' solid bits`);
      else assert.ok(q.chunks.every((c) => !c.solid));
      // and in no other tile of that LOD
      const { message: other } = tileMessage(world, { id: 2, lod, cx: Math.floor(p.home.cx / n) + 1, cy: Math.floor(p.home.cy / n), collision: false });
      assert.ok(!other.parts.some((r) => r.id === p.id));
    }
  }
  // grid mode: no parts come with tiles (the world grid holds them)
  const grid = createWorld(presetConfig("angledOldHarbourTown"));
  assert.deepEqual(tileMessage(grid, { id: 3, lod: 0, cx: road.home.cx, cy: road.home.cy, collision: false }).message.parts, []);
});

test("viewer parts: the walker's collision finds a part's solid cells through the exact inverse map", () => {
  for (const kind of ["road", "building", "wing"]) {
    const p = parts.find((q) => q.kind === kind);
    const { message } = tileMessage(world, { id: 1, lod: 0, cx: p.home.cx, cy: p.home.cy, collision: true });
    const q = message.parts.find((r) => r.id === p.id);
    const entry = { ...q, bits: { solid: new Map(q.chunks.filter((c) => c.solid).map((c) => [`${c.cx},${c.cy},${c.cz}`, c.solid])) } };
    let solid = 0;
    let n = 0;
    const b = p.aabb;
    const stepZ = Math.max(1, Math.floor((b.z1 - b.z0) / 24));
    for (let z = b.z0; z <= b.z1; z += stepZ)
      for (let y = b.y0; y <= b.y1; y += 3)
        for (let x = b.x0; x <= b.x1; x += 3) {
          const [u, v, w] = p.placement.toLocal(x, y, z);
          assert.deepEqual(partLocal(q, x, y, z), [u, v, w]);
          const e = p.extent;
          const inside = u >= e.u0 && u <= e.u1 && v >= e.v0 && v <= e.v1 && w >= e.w0 && w <= e.w1;
          let want = false;
          if (inside) {
            const c = partChunk(world, p, 0, Math.floor(u / 32), Math.floor(v / 32), Math.floor(w / 32));
            want = !!IS_SOLID[c.data[u - Math.floor(u / 32) * 32 + 1 + (v - Math.floor(v / 32) * 32 + 1) * P + (w - Math.floor(w / 32) * 32 + 1) * P2]];
          }
          assert.equal(partBit(entry, "solid", x, y, z), want, `${p.key} at (${x}, ${y}, ${z})`);
          n += 1;
          if (want) solid += 1;
        }
    assert.ok(n > 500 && solid > 20, `${p.key}: ${solid} of ${n}`);
    assert.equal(partBit(entry, "solid", b.x1 + 5, b.y0, b.z0), null, "outside its box: not its business");
  }
});

test("viewer parts: a part's model matrix puts its lattice where its placement does", () => {
  const p = parts.find((q) => q.kind === "road");
  const M = partMatrix({ m: p.placement.m, d: p.placement.d, origin: p.placement.origin }, 0.125);
  for (const [u, v, w] of [
    [0, 0, 0],
    [10, 3, 2],
    [p.extent.u1, p.extent.v1, 1],
  ]) {
    // (the centre of local cell (u, v, w), in metres)
    const q = [u + 0.5, v + 0.5, w + 0.5];
    const X = [0, 1, 2].map((r) => M[4 * r] * q[0] + M[4 * r + 1] * q[1] + M[4 * r + 2] * q[2] + M[4 * r + 3] / 0.125);
    const cell = p.placement.toWorld(u, v, w);
    for (let a = 0; a < 3; a += 1) assert.equal(Math.floor(X[a]), cell[a]);
  }
});
