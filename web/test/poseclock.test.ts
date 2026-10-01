/**
 * When poses are drawn (engine/poseclock.ts): one batch interval behind, the interval measured -
 * an engine slowed down still moves things continuously - and by one rule for everything a batch
 * carries: a car's body (a piece) and the car's own pose (its wheels, the camera on it) agree.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { PieceBodies } from '../src/engine/pieces.ts';
import { PoseClock, TICK_S } from '../src/engine/poseclock.ts';
import { DEBRIS_STRIDE, VEHICLE_STRIDE, type DetachedEvent, type Vec3 } from '../src/engine/protocol.ts';
import { VehicleTracker } from '../src/game/vehicles.ts';

const H = 0.125;

/** A piece's occupancy (the svx::piece_occupancy layout): one shape, a box of voxels, all solid. */
function pieceOccupancy(dim: Vec3): ArrayBuffer {
  const cells = dim[0] * dim[1] * dim[2];
  const buf = new ArrayBuffer(4 + 8 * 8 + 6 * 4 + ((cells + 7) >> 3));
  const v = new DataView(buf);
  let o = 0;
  v.setUint32(o, 1, true);
  o += 4;
  for (const x of [0, 0, 0, 0, 0, 0, 1, H]) {
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
    voxels: 64,
    centroid,
    velocity: [0, 0, 0],
    angular: [0, 0, 0],
    rigid: true,
    mesh: { vertices: new ArrayBuffer(0), vertexCount: 0, indices: new ArrayBuffer(0), indexCount: 0 },
    occupancy: pieceOccupancy([4, 4, 4]),
  };
}

/** A vehicle record at `x` (the frame's origin; its centre of mass 0.6 m up), chassis piece 100 + id. */
function vehicle(id: number, x: number): Float64Array {
  const v = new Float64Array(VEHICLE_STRIDE);
  v[0] = id;
  v[1] = 100 + id;
  v[2] = 1;
  v.set([x, 0, 0.6], 4);
  v.set([0, 0, 0, 1], 7);
  v.set([x, 0, 0], 30);
  return v;
}

test('pose clock: the interval is measured from the batches, once a batch, pauses aside', () => {
  const c = new PoseClock();
  assert.equal(c.interval, TICK_S);
  let t = 10;
  for (let k = 0; k < 60; k++) {
    c.batch(k, t);
    c.batch(k, t + 0.001); // (the same batch's other messages)
    t += 0.05;
  }
  assert.ok(Math.abs(c.interval - 0.05) < 1e-3, `measured ${c.interval}`);
  c.batch(1000, t + 5); // (a pause: not the engine's pace)
  assert.ok(Math.abs(c.interval - 0.05) < 1e-3, `after a pause ${c.interval}`);
  // drawn one interval behind: halfway between samples 0.05 s apart at half an interval past the later one's arrival
  assert.ok(Math.abs(c.weight(1.0, 1.05, 1.075) - 0.5) < 0.02, `weight ${c.weight(1.0, 1.05, 1.075)}`);
  // a sample that is old (a resting piece not sent again) is moved from as if one interval ago
  assert.equal(c.from({ t: 1.0 }, 1.5).t, 1.5 - c.interval);
  assert.equal(c.from({ t: 1.47 }, 1.5).t, 1.47);
});

test('pose clock: an engine at 20 Hz moves a car and its body together and without holds', () => {
  const clock = new PoseClock();
  const pieces = new PieceBodies(clock);
  const tracker = new VehicleTracker(clock);
  const centroid: Vec3 = [0.25, 0.25, 0.25];
  pieces.add(detached(101, centroid));
  // batches every 50 ms (a slow engine): the car (and its body, 0.6 m above its frame) at 10 m/s
  let last = -Infinity;
  for (let k = 0; k < 40; k++) {
    const t = 1 + 0.05 * k;
    const x = 0.5 * k;
    clock.batch(k, t);
    const poses = new Float64Array(DEBRIS_STRIDE);
    poses.set([101, x, 0, 0.6, 0, 0, 0, 1, 1]);
    pieces.applyDebris(poses, t);
    tracker.apply(vehicle(1, x), new Float64Array(0), 1, t);
    if (k < 20) continue; // (the interval measured by now)
    for (let f = 0; f < 3; f++) {
      const now = t + (f * 0.05) / 3;
      pieces.advance(now);
      const body = pieces.get(101)!.pos[0];
      const car = tracker.pose(tracker.vehicles.get(1)!, now).pos[0];
      assert.ok(Math.abs(body - car) < 1e-9, `body ${body} and car ${car} agree`);
      assert.ok(car > last, `moving on at every frame (${last} -> ${car})`);
      last = car;
    }
  }
});

test('pieces: a removed event takes the piece at once, poses or not', () => {
  const pieces = new PieceBodies();
  pieces.add(detached(7, [0.25, 0.25, 0.25]));
  const poses = new Float64Array(DEBRIS_STRIDE);
  poses.set([7, 1, 0, 0.25, 0, 0, 0, 1, 1]);
  pieces.applyDebris(poses, 1);
  assert.equal(pieces.size, 1);
  pieces.remove(7);
  assert.equal(pieces.size, 0);
  pieces.applyDebris(poses, 1 + TICK_S); // (a pose message posted before it went: nothing comes back)
  assert.equal(pieces.size, 0);
});
