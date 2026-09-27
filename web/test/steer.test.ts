import assert from 'node:assert/strict';
import { test } from 'node:test';
import { pursue, steer, steerOptions, turn, turnRateAt, wrap } from '../src/actors/steer.ts';

const DT = 1 / 60;

test('steer: a walker speeds up over about a second and stops within a metre', () => {
  let v: [number, number] = [0, 0];
  let t = 0;
  while (Math.hypot(v[0], v[1]) < 1.35 && t < 5) {
    v = steer(v, [1, 0], 1.4, DT, steerOptions(1.4));
    t += DT;
  }
  assert.ok(t > 0.55 && t < 1.2, `reaches walking speed after ${t.toFixed(2)} s`);
  let x = 0;
  while (Math.hypot(v[0], v[1]) > 0.01) {
    v = steer(v, [1, 0], 0, DT, steerOptions(1.4));
    x += v[0] * DT;
  }
  assert.ok(x > 0.15 && x < 0.6, `brakes to a stop in ${x.toFixed(2)} m`);
});

test('steer: the heading turns at a limited rate, wider when running, and sharp turns are slower', () => {
  for (const speed of [1.4, 4.5]) {
    let v: [number, number] = [speed, 0];
    const h0 = 0;
    for (let i = 0; i < 6; i++) v = steer(v, [-1, 0.001], speed, DT, steerOptions(speed));
    const h = Math.atan2(v[1], v[0]);
    const rate = Math.abs(wrap(h - h0)) / (6 * DT);
    assert.ok(Math.abs(rate - turnRateAt(speed)) < 0.2, `${speed} m/s: turns at ${rate.toFixed(2)} rad/s`);
    assert.ok(Math.hypot(v[0], v[1]) < speed, 'slowing for the U-turn');
  }
  assert.ok(turnRateAt(4.5) < turnRateAt(1.4));
});

test('turn: a facing speeds up and brakes into the new direction without overshooting', () => {
  let yaw = 0, rate = 0, t = 0, peak = 0;
  const want = Math.PI * 0.9;
  while (Math.abs(wrap(want - yaw)) > 1e-4 && t < 5) {
    const r = turn(yaw, rate, want, DT, 2.6, 9);
    yaw = r.yaw;
    rate = r.rate;
    peak = Math.max(peak, rate);
    assert.ok(wrap(want - yaw) > -1e-6, 'no overshoot');
    t += DT;
  }
  assert.ok(peak <= 2.6 + 1e-9 && peak > 2.3, `peak rate ${peak.toFixed(2)}`);
  assert.ok(t > 1.1 && t < 1.8, `a 160 degree turn takes ${t.toFixed(2)} s`);
});

test('pursue: the point ahead cuts the corner of a path, and the length left is the path', () => {
  const r = pursue([0, 0, 0], [[2, 0, 0], [2, 2, 0]], [2, 2, 0], 2.5);
  assert.deepEqual(r.point.map((v) => +v.toFixed(3)), [2, 0.5, 0]);
  assert.ok(Math.abs(r.remaining - 4) < 1e-9);
  const end = pursue([1.8, 1.9, 0], [[2, 2, 0]], [2, 2, 0], 1);
  assert.deepEqual(end.point, [2, 2, 0]);
});
