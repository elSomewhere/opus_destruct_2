/**
 * Vehicles on the client (game/vehicles.ts, render/wheel-mesh.ts; docs/VEHICLES.md): the
 * `vehicles` message parsed and interpolated one tick behind, the car to take found by its box,
 * and the wheel's mesh wound the way it faces.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { VEHICLE_STRIDE, WHEEL_STRIDE, type Vec3 } from '../src/engine/protocol.ts';
import { MeshBuilder } from '../src/engine/vertex.ts';
import { VehicleTracker } from '../src/game/vehicles.ts';
import { buildWheel } from '../src/render/wheel-mesh.ts';

/** One vehicle's record: at `origin`, turned `yawDeg` about z, its centre of mass 0.6 m up. */
function vehicle(id: number, origin: Vec3, yawDeg: number, speed = 0): Float64Array {
  const v = new Float64Array(VEHICLE_STRIDE);
  const t = (0.5 * yawDeg * Math.PI) / 180;
  const c = Math.cos((yawDeg * Math.PI) / 180);
  const s = Math.sin((yawDeg * Math.PI) / 180);
  v[0] = id;
  v[1] = 100 + id; // chassis
  v[2] = 1; // sedan
  v[3] = 4; // red
  v.set([origin[0], origin[1], origin[2] + 0.6], 4);
  v.set([0, 0, Math.sin(t), Math.cos(t)], 7);
  v.set([c * speed, s * speed, 0], 11);
  v[14] = speed;
  // the seat at (-0.15, 0.37, 0.9) in its frame
  v.set([origin[0] - 0.15 * c - 0.37 * s, origin[1] - 0.15 * s + 0.37 * c, origin[2] + 0.9], 22);
  v.set([2.35, 0.95, 1.5], 25);
  v[28] = 4;
  v.set(origin, 30);
  v[33] = 6500;
  v[34] = 7; // parts on (one of its eight come off)
  v[35] = 8;
  return v;
}

function wheel(vehicleId: number, id: number, centre: Vec3): Float64Array {
  const w = new Float64Array(WHEEL_STRIDE);
  w[0] = vehicleId;
  w[1] = id;
  w.set(centre, 2);
  w.set([0, 0, 0, 1], 5);
  w[9] = 0.32;
  w[10] = 0.22;
  w[11] = 1;
  w[13] = 18;
  return w;
}

test('vehicles: poses come one tick behind, interpolated; the seat is kept in the car frame', () => {
  const t = new VehicleTracker();
  t.apply(vehicle(1, [0, 0, 0], 0, 10), wheel(1, 7, [1.4, 0.8, 0.32]), 1, 10);
  t.apply(vehicle(1, [1, 0, 0], 0, 10), wheel(1, 7, [2.4, 0.8, 0.32]), 1, 10 + 1 / 60);
  const v = t.vehicles.get(1)!;
  assert.equal(t.player, 1);
  assert.equal(v.chassis, 101);
  assert.equal(v.parts, 7);
  assert.equal(v.partsBuilt, 8);
  assert.ok(Math.abs(v.seat[0] + 0.15) < 1e-9 && Math.abs(v.seat[1] - 0.37) < 1e-9 && Math.abs(v.seat[2] - 0.9) < 1e-9);
  // halfway between the samples, a tick behind
  const p = t.pose(v, 10 + 1 / 60 + 0.5 / 60);
  assert.ok(Math.abs(p.origin[0] - 0.5) < 1e-6, `origin x ${p.origin[0]}`);
  const w = t.wheels.get(7)!;
  assert.equal(w.side, 1); // (a left wheel: its rim faces +y)
  const wp = t.wheelPose(w, 10 + 1 / 60 + 0.5 / 60);
  assert.ok(Math.abs(wp.centre[0] - 1.9) < 1e-6);
  // gone from the next message: gone
  t.apply(new Float64Array(0), new Float64Array(0), 0, 11);
  assert.equal(t.size, 0);
  assert.equal(t.wheels.size, 0);
});

test('vehicles: the car to take is the one whose box is within reach, turned as it stands', () => {
  const t = new VehicleTracker();
  const both = new Float64Array(2 * VEHICLE_STRIDE);
  both.set(vehicle(1, [0, 0, 0], 90), 0); // along y: its box 0.95 m wide in x
  both.set(vehicle(2, [10, 0, 0], 0), VEHICLE_STRIDE);
  t.apply(both, new Float64Array(0), 0, 5);
  // 2 m to the side of the first (x = 2): 1.05 m from its box
  assert.equal(t.nearest([2, 0, 0.9], 2.2, 5)?.id, 1);
  // 2 m beyond its nose (y = 4.35): within reach; 3 m: not
  assert.equal(t.nearest([0, 4.35, 0.9], 2.2, 5)?.id, 1);
  assert.equal(t.nearest([0, 5.4, 0.9], 2.2, 5), null);
  // between the two, nearer the second's tail (x = 10 - 2.35)
  assert.equal(t.nearest([6.5, 0, 0.9], 2.2, 5)?.id, 2);
});

test('wheel mesh: every triangle faces the way its normal says, within the tyre', () => {
  for (const side of [1, -1])
    for (const blur of [false, true]) {
      const b = new MeshBuilder(256);
      buildWheel(b, 0.32, 0.22, side, blur);
      const m = b.finish();
      assert.ok(m.indexCount > 300, `${m.indexCount} indices`);
      const f = new Float32Array(m.vertices);
      const i8 = new Int8Array(m.vertices);
      const idx = new Uint32Array(m.indices);
      const P = (i: number): Vec3 => [f[i * 7]!, f[i * 7 + 1]!, f[i * 7 + 2]!];
      for (let k = 0; k < idx.length; k += 3) {
        const a = P(idx[k]!);
        const b2 = P(idx[k + 1]!);
        const c = P(idx[k + 2]!);
        const e1 = [b2[0] - a[0], b2[1] - a[1], b2[2] - a[2]];
        const e2 = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
        const cr = [e1[1]! * e2[2]! - e1[2]! * e2[1]!, e1[2]! * e2[0]! - e1[0]! * e2[2]!, e1[0]! * e2[1]! - e1[1]! * e2[0]!];
        const o = idx[k]! * 28 + 12;
        const n = [i8[o]! / 127, i8[o + 1]! / 127, i8[o + 2]! / 127];
        const area = Math.hypot(cr[0]!, cr[1]!, cr[2]!);
        if (area < 1e-9) continue;
        assert.ok(cr[0]! * n[0]! + cr[1]! * n[1]! + cr[2]! * n[2]! > 0, `triangle ${k / 3} wound against its normal`);
        for (const p of [a, b2, c]) {
          assert.ok(Math.hypot(p[0], p[2]) <= 0.32 + 1e-5 && Math.abs(p[1]) <= 0.11 + 0.013, `vertex ${p} outside the tyre`);
        }
      }
    }
});
