import assert from 'node:assert/strict';
import { test } from 'node:test';
import { WorldAccess } from '../src/actors/env.ts';
import { Navigator } from '../src/actors/nav.ts';
import { OccupancyStore } from '../src/game/occupancy.ts';

const H = 0.125;

/** Ground (voxels z < 0) over 3x3 chunks, a wall across x = 2 m with a gap at the far end, and a 1-voxel step. */
function world(): WorldAccess {
  const occ = new OccupancyStore();
  const chunks: { chunk: [number, number, number]; state: 0 | 1 | 2; bits?: ArrayBuffer }[] = [];
  for (let cx = -1; cx <= 1; cx++)
    for (let cy = -1; cy <= 1; cy++) {
      chunks.push({ chunk: [cx, cy, -1], state: 1 });
      const bits = new Uint8Array(4096);
      for (let x = 0; x < 32; x++)
        for (let y = 0; y < 32; y++)
          for (let z = 0; z < 32; z++) {
            const gx = cx * 32 + x, gy = cy * 32 + y, gz = z;
            const wall = gx === 16 && gy > -20 && gy < 20 && gz < 16;
            const step = gx >= -16 && gx < -8 && gy >= -4 && gy < 4 && gz === 0;
            if (wall || step) {
              const v = (x * 32 + y) * 32 + z;
              bits[v >> 3]! |= 1 << (v & 7);
            }
          }
      chunks.push({ chunk: [cx, cy, 0], state: 2, bits: bits.buffer });
    }
  occ.apply({ type: 'occupancy', voxelSize: H, chunks });
  return new WorldAccess(occ, H);
}

test('paths go around a wall and are walkable', () => {
  const w = world();
  const nav = new Navigator(w);
  const from: [number, number, number] = [0, 0, -H / 2];
  const to: [number, number, number] = [3.5, 0, -H / 2];
  assert.equal(nav.walkable(from, to), false, 'the wall blocks the straight line');
  const p = nav.findPath(from, to);
  assert.ok(p && !p.partial, 'a full path');
  // it detours past the end of the wall (|y| >= 2.5 m)
  assert.ok(p!.points.some((q) => Math.abs(q[1]) > 2.4), `detour ${JSON.stringify(p!.points.map((q) => q.map((v) => +v.toFixed(2))))}`);
  let prev = from;
  for (const q of p!.points) {
    assert.ok(nav.walkable(prev, q), `segment ${prev} -> ${q}`);
    prev = q;
  }
});

test('steps up onto a raised floor; random points are standable', () => {
  const w = world();
  const nav = new Navigator(w);
  const p = nav.findPath([-0.5, 0, -H / 2], [-1.5, 0, H / 2]);
  assert.ok(p && !p.partial);
  assert.ok(Math.abs(p!.points[p!.points.length - 1]![2] - H / 2) < 1e-6, 'ends on the step');
  for (let k = 0; k < 20; k++) {
    const r = nav.randomPoint([0, 0, 0], 1, 3.5);
    assert.ok(r && w.fits(r, 0.2, 1.7, 0.3));
  }
});
