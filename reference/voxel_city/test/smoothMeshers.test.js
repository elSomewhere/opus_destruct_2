import test from "node:test";
import assert from "node:assert/strict";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { MAT } from "../src/engine/voxel/materials.js";
import { MESHERS, meshChunkWith } from "../src/engine/voxel/meshers.js";

const volume = () => new Uint16Array(P * P * P);
const at = (x, y, z) => x + y * P + z * P2;

for (const mesher of MESHERS.slice(1)) {
  test(`${mesher}: empty and padded-full chunks have no surface`, () => {
    assert.equal(meshChunkWith(volume(), { mesher }).opaque, null);
    const full = volume();
    full.fill(MAT.STONE);
    assert.equal(meshChunkWith(full, { mesher }).opaque, null);
  });

  test(`${mesher}: output is deterministic, finite and bounded`, () => {
    const data = volume();
    for (let z = 1; z <= 16; z += 1) for (let y = 1; y <= 32; y += 1) for (let x = 1; x <= 32; x += 1) data[at(x, y, z)] = MAT.STONE;
    const a = meshChunkWith(data, { mesher }).opaque;
    const b = meshChunkWith(data, { mesher }).opaque;
    assert.ok(a.index.length > 0);
    assert.deepEqual(a, b);
    for (const p of a.position) assert.ok(Number.isFinite(p) && p >= -0.5 && p <= 32.5);
  });

  test(`${mesher}: neighbouring ownership reaches the common border without a gap`, () => {
    const left = volume();
    for (let z = 1; z <= 16; z += 1) for (let y = 1; y <= 32; y += 1) for (let x = 1; x <= 33; x += 1) left[at(x, y, z)] = MAT.STONE;
    const mesh = meshChunkWith(left, { mesher }).opaque;
    assert.ok(Math.max(...mesh.position) >= 32);
  });
}
