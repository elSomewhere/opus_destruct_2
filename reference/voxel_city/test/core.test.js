import { test } from "node:test";
import assert from "node:assert/strict";
import { Rng, hash32, deriveSeed } from "../src/engine/core/hash.js";
import { rSubtract, rArea, rSharedWall } from "../src/engine/core/rect.js";
import { Frame } from "../src/engine/buildings/frame.js";
import { ChunkBuffer } from "../src/engine/voxel/chunk.js";
import { meshChunk } from "../src/engine/voxel/mesher.js";
import { MAT } from "../src/engine/voxel/materials.js";

test("hashing and rng are deterministic and order-independent", () => {
  assert.equal(hash32(1, 2, 3), hash32(1, 2, 3));
  assert.notEqual(hash32(1, 2, 3), hash32(1, 2, 4));
  assert.equal(deriveSeed(7, "building", "C0_0/b1"), deriveSeed(7, "building", "C0_0/b1"));
  const a = new Rng(99);
  const b = new Rng(99);
  for (let k = 0; k < 100; k += 1) assert.equal(a.next(), b.next());
  // forks do not disturb the parent stream
  const c = new Rng(5);
  const d = new Rng(5);
  c.fork("x").next();
  assert.equal(c.next(), d.next());
});

test("rect subtraction conserves area", () => {
  const a = { x0: 0, y0: 0, x1: 99, y1: 49 };
  const b = { x0: 20, y0: 10, x1: 39, y1: 29 };
  const parts = rSubtract(a, b);
  assert.equal(parts.reduce((s, r) => s + rArea(r), 0), rArea(a) - rArea(b));
  assert.deepEqual(rSharedWall({ x0: 0, y0: 0, x1: 9, y1: 9 }, { x0: 11, y0: 3, x1: 20, y1: 20 }, 1).sideOfA, "E");
});

test("building frames are proper rotations that round-trip", () => {
  const R = { x0: 100, y0: 200, x1: 139, y1: 219 };
  for (const front of ["N", "S", "E", "W"]) {
    const f = new Frame(R, front);
    for (const [x, y] of [
      [100, 200],
      [139, 219],
      [117, 205],
    ]) {
      const [u, v] = f.fromWorld(x, y);
      assert.deepEqual(f.toWorld(u, v), [x, y]);
    }
    // v = 0 lies on the street side
    const [fx, fy] = f.toWorld(3, 0);
    const onFront = { N: fy === R.y0, S: fy === R.y1, W: fx === R.x0, E: fx === R.x1 }[front];
    assert.ok(onFront, `front ${front}`);
  }
});

test("mesher emits six faces for a lone voxel and none for buried ones", () => {
  const c = new ChunkBuffer(0, 0, 0, 0);
  c.set(10, 10, 10, MAT.CONCRETE);
  const m = meshChunk(c.data);
  assert.equal(m.opaque.quads, 6);
  const full = new ChunkBuffer(0, 0, 0, 0);
  full.data.fill(MAT.STONE);
  assert.equal(meshChunk(full.data).opaque, null);
});
