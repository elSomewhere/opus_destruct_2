/**
 * The client's collision against oriented grids and pieces (game/occupancy.ts,
 * engine/gridframes.ts, engine/pieces.ts): a turned grid is felt as the turned cubes it is, and a
 * moving piece (a lift's car, a turntable) carries what stands on it.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { GridFrames } from '../src/engine/gridframes.ts';
import { PieceBodies } from '../src/engine/pieces.ts';
import { DEBRIS_STRIDE, GRID_STRIDE, type ChunkOccupancy, type DetachedEvent, type Vec3 } from '../src/engine/protocol.ts';
import { OccupancyStore } from '../src/game/occupancy.ts';

const H = 0.125;

/** Occupancy bits of one chunk: the voxels (local coordinates) for which `solid` holds. */
function chunkBits(solid: (x: number, y: number, z: number) => boolean): ArrayBuffer {
  const bits = new Uint8Array(4096);
  for (let x = 0; x < 32; x++)
    for (let y = 0; y < 32; y++)
      for (let z = 0; z < 32; z++) {
        if (!solid(x, y, z)) continue;
        const v = (x * 32 + y) * 32 + z;
        bits[v >> 3]! |= 1 << (v & 7);
      }
  return bits.buffer;
}

function frame(id: number, origin: Vec3, yawDeg: number): Float64Array {
  const f = new Float64Array(GRID_STRIDE);
  const t = (0.5 * yawDeg * Math.PI) / 180;
  f.set([id, origin[0], origin[1], origin[2], 0, 0, Math.sin(t), Math.cos(t), H]);
  return f;
}

/** A piece's occupancy (the svx::piece_occupancy layout): one shape, a box of voxels, all solid. */
function pieceOccupancy(origin: Vec3, lo: Vec3, dim: Vec3): ArrayBuffer {
  const cells = dim[0] * dim[1] * dim[2];
  const buf = new ArrayBuffer(4 + 8 * 8 + 6 * 4 + ((cells + 7) >> 3));
  const v = new DataView(buf);
  let o = 0;
  v.setUint32(o, 1, true);
  o += 4;
  for (const x of [origin[0], origin[1], origin[2], 0, 0, 0, 1, H]) {
    v.setFloat64(o, x, true);
    o += 8;
  }
  for (const x of [...lo, ...dim]) {
    v.setInt32(o, x, true);
    o += 4;
  }
  new Uint8Array(buf, o).fill(0xff);
  return buf;
}

test('occupancy: a wall turned 45 degrees stops a box at its face, not before', () => {
  const store = new OccupancyStore();
  const frames = new GridFrames();
  store.setFrames(frames);
  // a wall of grid 7: lattice x in [0, 32) (4 m), y in [0, 2) (0.25 m), z in [0, 24), turned 45 degrees about z at the origin
  const occ: ChunkOccupancy = { chunk: [0, 0, 0], state: 2, bits: chunkBits((_x, y, z) => y < 2 && z < 24), grid: 7 };
  store.apply({ type: 'occupancy', voxelSize: H, chunks: [occ] });
  frames.apply(frame(7, [0, 0, 0], 45), []);
  // a box (0.4 m square) moving along +y towards the wall's face (the lattice plane y' = -h/2;
  // lattice y' = (y - x) / sqrt(2)): its corner (cx - half, front) meets it first
  const half = 0.2;
  const cx = 2.0;
  const mn: Vec3 = [cx - half, -0.4, 0.5];
  const mx: Vec3 = [cx + half, 0.0, 1.5];
  const r = store.collide(mn, mx, [0, 2.0, 0]);
  const front = mx[1] + r.move[1];
  const cornerY = (front - (cx - half)) / Math.SQRT2;
  assert.ok(Math.abs(cornerY - -0.5 * H) < 1e-3, `stopped with its corner at lattice y ${cornerY} (the face: ${-0.5 * H})`);
  assert.ok(r.move[1] > 1.5 && r.move[1] < 2.0, `it moved up to the wall (${r.move[1]})`);
  // away from the wall (x beyond its end), nothing stops it
  const far = store.collide([10 - half, -1.5 - half, 0.5], [10 + half, -1.5 + half, 1.5], [0, 2.0, 0]);
  assert.equal(far.move[1], 2.0);
  // a box in it overlaps it; one beside it does not
  assert.ok(store.overlaps([1.4, 1.4, 0.5], [1.6, 1.6, 1.0]));
  assert.ok(!store.overlaps([1.4, 0.6, 0.5], [1.6, 0.8, 1.0]));
});

test('occupancy: standing on a moving piece reports it, and its velocity there', () => {
  const store = new OccupancyStore();
  const pieces = new PieceBodies();
  store.setFrames(new GridFrames());
  store.setPieces(pieces);
  // a 4 m square deck (32 x 32 x 2 voxels, lattice at the origin, its centre at (1.9375, 1.9375,
  // 0.0625)), detached as piece 5, then rising at 0.5 m/s and turning at 0.2 rad/s about z
  const centroid: Vec3 = [15.5 * H, 15.5 * H, 0.5 * H];
  const ev: DetachedEvent = {
    kind: 'detached',
    id: 5,
    voxels: 2048,
    centroid,
    velocity: [0, 0, 0],
    angular: [0, 0, 0],
    rigid: true,
    mesh: { vertices: new ArrayBuffer(0), vertexCount: 0, indices: new ArrayBuffer(0), indexCount: 0 },
    occupancy: pieceOccupancy([0, 0, 0], [0, 0, 0], [32, 32, 2]),
  };
  pieces.add(ev);
  const up = 1.0;
  const poses = new Float64Array(DEBRIS_STRIDE);
  poses.set([5, centroid[0], centroid[1], centroid[2] + up, 0, 0, 0, 1, 1, 0, 0, 0.5, 0, 0, 0.2]);
  pieces.applyDebris(poses, 10.0);
  pieces.advance(10.0 + 1 / 60); // (as drawn: its last pose)
  // a box just above its top (1 m up: lattice z 1.5 h), falling onto it at x = 3, y = 1
  const top = up + 1.5 * H;
  const r = store.collide([2.8, 0.8, top + 0.05], [3.2, 1.2, top + 1.8], [0, 0, -0.2]);
  assert.ok(r.onGround);
  assert.equal(r.groundPiece, 5);
  assert.ok(Math.abs(r.move[2] - -0.05) < 1e-3, `landed on its top (moved ${r.move[2]})`);
  const v = r.groundVelocity!;
  // v + w x (X - c): (0, 0, 0.5) + 0.2 z x (3 - 1.9375, 1 - 1.9375, .) = (0.1875, 0.2125, 0.5)
  assert.ok(Math.abs(v[0] - 0.1875) < 1e-9 && Math.abs(v[1] - 0.2125) < 1e-9 && Math.abs(v[2] - 0.5) < 1e-9, `its velocity there ${v}`);
  assert.equal(r.groundAngular![2], 0.2);
  // a box sunk into it is lifted out; one beside it is free
  assert.ok(store.overlaps([2.8, 0.8, top - 0.1], [3.2, 1.2, top + 1.7]));
  assert.ok(!store.overlaps([4.8, 0.8, top - 0.1], [5.2, 1.2, top + 1.7]));
  // gone from the poses: gone
  pieces.applyDebris(new Float64Array(0), 10.1);
  assert.equal(pieces.size, 0);
  assert.ok(!store.collide([2.8, 0.8, top + 0.05], [3.2, 1.2, top + 1.8], [0, 0, -0.2]).onGround);
});
