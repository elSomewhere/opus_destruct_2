/**
 * The client's collision against oriented grids (game/occupancy.ts, engine/gridframes.ts): a
 * turned grid is felt as the turned cubes it is, and a moving grid carries what stands on it.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { GridFrames } from '../src/engine/gridframes.ts';
import { GRID_STRIDE, type ChunkOccupancy, type Vec3 } from '../src/engine/protocol.ts';
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

function frame(id: number, origin: Vec3, yawDeg: number, vel: Vec3 = [0, 0, 0], ang: Vec3 = [0, 0, 0]): Float64Array {
  const f = new Float64Array(GRID_STRIDE);
  const t = (0.5 * yawDeg * Math.PI) / 180;
  f.set([id, origin[0], origin[1], origin[2], 0, 0, Math.sin(t), Math.cos(t), H, vel.some((v) => v !== 0) || ang.some((v) => v !== 0) ? 1 : 0, ...vel, ...ang, ...origin]);
  return f;
}

test('occupancy: a wall turned 45 degrees stops a box at its face, not before', () => {
  const store = new OccupancyStore();
  const frames = new GridFrames();
  store.setFrames(frames);
  // a wall of grid 7: lattice x in [0, 32) (4 m), y in [0, 2) (0.25 m), z in [0, 24), turned 45 degrees about z at the origin
  const occ: ChunkOccupancy = { chunk: [0, 0, 0], state: 2, bits: chunkBits((_x, y, z) => y < 2 && z < 24), grid: 7 };
  store.apply({ type: 'occupancy', voxelSize: H, chunks: [occ] });
  frames.apply(frame(7, [0, 0, 0], 45), [], 0);
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

test('occupancy: standing on a moving grid reports its velocity there', () => {
  const store = new OccupancyStore();
  const frames = new GridFrames();
  store.setFrames(frames);
  // a 4 m platform of grid 3, 2 voxels thick, rising at 0.5 m/s and turning at 0.2 rad/s about z
  store.apply({ type: 'occupancy', voxelSize: H, chunks: [{ chunk: [0, 0, 0], state: 2, bits: chunkBits((_x, _y, z) => z < 2), grid: 3 }] });
  frames.apply(frame(3, [0, 0, 1], 0, [0, 0, 0.5], [0, 0, 0.2]), [], 0);
  // a box just above its top (lattice z = 1.5 h), falling onto it at x = 2, y = 1
  const top = 1 + 1.5 * H;
  const r = store.collide([1.8, 0.8, top + 0.05], [2.2, 1.2, top + 1.8], [0, 0, -0.2]);
  assert.ok(r.onGround);
  assert.equal(r.ground, 3);
  assert.ok(Math.abs(r.move[2] - -0.05) < 1e-3, `landed on its top (moved ${r.move[2]})`);
  const v = r.groundVelocity!;
  // v + w x (X - c): (0, 0, 0.5) + 0.2 z x (2, 1, .) = (-0.2, 0.4, 0.5)
  assert.ok(Math.abs(v[0] - -0.2) < 1e-9 && Math.abs(v[1] - 0.4) < 1e-9 && Math.abs(v[2] - 0.5) < 1e-9, `its velocity there ${v}`);
  assert.equal(r.groundAngular![2], 0.2);
});
