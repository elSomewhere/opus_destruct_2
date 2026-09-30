/**
 * What the piece store keeps and drops (engine/pieces.ts). A piece the engine never poses would
 * be drawn where it detached and stay there for good - after a world load, a ghost of the
 * structure that came down, with the rubble of the new one crowded out of the renderer's slots
 * behind it. (The other half of that, the client dropping the old world's messages at a load,
 * needs a Worker and is checked in the browser: scripts/reload-wasm.mjs.)
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { PieceBodies } from '../src/engine/pieces.ts';
import { DEBRIS_STRIDE, type DetachedEvent, type Vec3 } from '../src/engine/protocol.ts';

/** A piece's occupancy (the svx::piece_occupancy layout): one shape, a box of voxels, all solid. */
function pieceOccupancy(dim: Vec3): ArrayBuffer {
  const cells = dim[0] * dim[1] * dim[2];
  const buf = new ArrayBuffer(4 + 8 * 8 + 6 * 4 + ((cells + 7) >> 3));
  const v = new DataView(buf);
  let o = 0;
  v.setUint32(o, 1, true);
  o += 4;
  for (const x of [0, 0, 0, 0, 0, 0, 1, 0.125]) {
    v.setFloat64(o, x, true);
    o += 8;
  }
  for (const x of [0, 0, 0, ...dim]) {
    v.setInt32(o, x, true);
    o += 4;
  }
  new Uint8Array(buf, o).fill(0xff);
  return buf;
}

function detached(id: number, centroid: Vec3): DetachedEvent {
  return {
    kind: 'detached',
    id,
    voxels: 8,
    centroid,
    velocity: [0, 0, 0],
    angular: [0, 0, 0],
    rigid: true,
    mesh: { vertices: new ArrayBuffer(0), vertexCount: 0, indices: new ArrayBuffer(0), indexCount: 0 },
    occupancy: pieceOccupancy([2, 2, 2]),
  };
}

/** One piece's pose, at rest where it is. */
function poseOf(id: number, pos: Vec3): Float64Array<ArrayBuffer> {
  const p = new Float64Array(DEBRIS_STRIDE);
  p.set([id, pos[0], pos[1], pos[2], 0, 0, 0, 1, 1]);
  return p;
}

test('pieces: one the engine never poses is not kept', () => {
  const pieces = new PieceBodies();
  // two pieces come, only one of them is ever posed
  pieces.add(detached(1, [0, 0, 0]));
  pieces.add(detached(2, [5, 5, 5]));
  assert.equal(pieces.size, 2);
  for (let batch = 1; batch <= 3; batch++) {
    pieces.applyDebris(poseOf(1, [0, 0, batch]), batch / 60);
    if (batch < 3) assert.equal(pieces.size, 2, `batch ${batch}: the piece without a pose still has its grace`);
  }
  assert.equal(pieces.size, 1, 'the piece without a pose is gone, the posed one stays');
  assert.ok(pieces.get(1));
  assert.ok(!pieces.get(2));
});

test('pieces: one posed and then dropped by the engine goes with the next list', () => {
  const pieces = new PieceBodies();
  pieces.add(detached(1, [0, 0, 0]));
  pieces.applyDebris(poseOf(1, [0, 0, 0]), 1 / 60);
  assert.equal(pieces.size, 1);
  // every list holds every piece the engine has: missing from one is gone
  pieces.applyDebris(new Float64Array(0), 2 / 60);
  assert.equal(pieces.size, 0);
});

test('pieces: a remeshed piece keeps its place while its new pose comes', () => {
  const pieces = new PieceBodies();
  pieces.add(detached(1, [0, 0, 0]));
  pieces.applyDebris(poseOf(1, [0, 0, 0]), 1 / 60);
  // a piece that breaks or is remeshed is sent again, and its poses are relative to the new voxels
  pieces.add(detached(1, [0, 0, 4]));
  pieces.applyDebris(poseOf(1, [0, 0, 4]), 2 / 60);
  assert.equal(pieces.size, 1);
  assert.deepEqual(pieces.get(1)?.centroid, [0, 0, 4]);
});
