import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  B,
  FlatGround,
  H,
  HumanoidBody,
  Joint,
  Pose,
  RigidBody,
  RigidSystem,
  WorldPose,
  humanoidSkeleton,
  qaxis,
  qrotate,
  qz,
  vdist,
  type V3,
} from '../src/index.ts';

const DT = 1 / 60;

/** Two unit boxes joined at a point: a pendulum hanging from a fixed body. */
function pendulum(kind: 'ball' | 'hinge', hinge?: [number, number]) {
  const sys = new RigidSystem(null);
  const I: V3 = [0.1, 0.1, 0.1];
  const top = sys.add(new RigidBody(0, [0, 0, 0], [0, 0, 1], [0, 0, 0, 1]));
  const bob = sys.add(new RigidBody(2, I, [0, 0, 0.5], [0, 0, 0, 1]));
  const j = sys.addJoint(new Joint(top, bob, { kind, anchorA: [0, 0, 0], anchorB: [0, 0, 0.5], frameA: [0, 0, 0, 1], frameB: [0, 0, 0, 1], ...(hinge ? { hinge } : {}) }));
  return { sys, top, bob, j };
}

test('rigid: a joint holds its anchors together while the body swings, and energy does not grow', () => {
  const { sys, bob } = pendulum('ball');
  bob.v[0] = 3;
  let worst = 0, maxZ = -Infinity;
  for (let i = 0; i < 600; i++) {
    sys.step(DT);
    const a = bob.point([0, 0, 0.5]);
    worst = Math.max(worst, vdist(a, [0, 0, 1]));
    if (i > 60) maxZ = Math.max(maxZ, bob.x[2]);
  }
  assert.ok(worst < 0.002, `anchor drift ${worst}`);
  // it started at the bottom with 3 m/s: it can never swing higher than v^2 / 2g above it
  assert.ok(maxZ < 0.5 + 9 / (2 * 9.81) + 0.02, `swung up to ${maxZ}`);
});

test('rigid: a hinge bends one way within its range; a drive holds a target against gravity', () => {
  const { sys, bob, j } = pendulum('hinge', [-0.3, 1.2]);
  bob.v[1] = -4; // push it round the wrong way (about +x, backwards)
  let low = 0;
  for (let i = 0; i < 240; i++) {
    sys.step(DT);
    const d: V3 = [bob.x[0], bob.x[1], bob.x[2] - 1];
    // angle of the bob about +x from hanging straight down
    low = Math.min(low, Math.atan2(d[1], -d[2]));
  }
  assert.ok(low > -0.36, `past the hinge's limit: ${low}`);
  // a drive: hold the bob 60 degrees forward
  const q = qaxis([1, 0, 0], 1.05);
  j.target[0] = q[0];
  j.target[1] = q[1];
  j.target[2] = q[2];
  j.target[3] = q[3];
  j.stiffness = 400;
  j.damping = 20;
  for (let i = 0; i < 240; i++) sys.step(DT);
  const d: V3 = [bob.x[0], bob.x[1], bob.x[2] - 1];
  const angle = Math.atan2(d[1], -d[2]);
  assert.ok(Math.abs(angle - 1.05) < 0.08, `held at ${angle.toFixed(3)} rad`);
});

function humanoid() {
  const sk = humanoidSkeleton();
  const pose = new Pose(sk);
  const world = new WorldPose(sk);
  world.compute(pose, [0, 0, 0], qz(0));
  const body = new HumanoidBody(sk, new FlatGround(0));
  return { sk, pose, world, body };
}

test('body: segments weigh what bodies weigh, and a pose goes into the bodies and back unchanged', () => {
  const { sk, world, body } = humanoid();
  assert.ok(Math.abs(body.totalMass - 75) < 0.5, `mass ${body.totalMass}`);
  const legs = [B.thighL, B.shinL, B.footL].reduce((m, i) => m + body.parts[i]!.mass, 0);
  assert.ok(legs > 10 && legs < 14, `a leg weighs ${legs} kg`);
  // the centre of mass of a standing body is just above the hips
  body.setFromPose(world, null, 0);
  const c = body.com();
  assert.ok(c[2] > 0.9 && c[2] < 1.1, `centre of mass at ${c[2]}`);
  const out = new WorldPose(sk);
  body.writePose(out);
  for (let b = 0; b < sk.count; b++) {
    if (b === H.weapon) continue;
    assert.ok(vdist(out.p[b]!, world.p[b]!) < 1e-6, `bone ${sk.names[b]} moved ${vdist(out.p[b]!, world.p[b]!)}`);
  }
});

test('body: without muscle it collapses onto the ground and sleeps; it never sinks in', () => {
  const { world, body } = humanoid();
  body.setFromPose(world, null, 0);
  body.tone.fill(0);
  body.applyTone();
  let t = 0;
  for (; t < 8 && !body.system.trySleep(0.5); t += DT) body.system.step(DT);
  assert.ok(body.system.asleep, `asleep after ${t.toFixed(2)} s`);
  for (const p of body.parts) for (const s of p.spheres) assert.ok(p.point(s.c)[2] > s.r - 0.03, 'a sphere sunk into the ground');
  assert.ok(body.parts[B.head]!.x[2] < 0.4, 'down');
});

test('body: muscles and assists hold a standing pose; the arms keep their shape under gravity', () => {
  const { world, pose, body } = humanoid();
  body.setFromPose(world, null, 0);
  body.track(world, pose);
  body.applyTone();
  const m = body.totalMass;
  Object.assign(body.support, { enabled: true, stiffness: m * 900, maxForce: m * 25, damping: m * 55 });
  body.support.target[2] = world.p[H.pelvis]![2] - 0.02;
  Object.assign(body.steer, { enabled: true, stiffness: m * 700, maxForce: m * 10, damping: m * 50 });
  Object.assign(body.upright, { enabled: true, stiffness: 5000, maxTorque: 900, damping: 450 });
  for (let i = 0; i < 2; i++) {
    const a = body.feet[i]!;
    a.enabled = true;
    const p = world.p[i === 0 ? H.footL : H.footR]!;
    a.target[0] = p[0];
    a.target[1] = p[1];
    a.target[2] = p[2];
    body.feetTurn[i]!.enabled = true;
  }
  for (let i = 0; i < 180; i++) {
    body.compensateGravity();
    body.system.step(DT);
  }
  const out = new WorldPose(body.skeleton);
  body.writePose(out);
  assert.ok(vdist(out.p[H.head]!, world.p[H.head]!) < 0.06, `head off by ${vdist(out.p[H.head]!, world.p[H.head]!)}`);
  // the hands hang where the pose has them (gravity compensation)
  for (const h of [H.handL, H.handR]) assert.ok(vdist(out.p[h]!, world.p[h]!) < 0.06, `hand off by ${vdist(out.p[h]!, world.p[h]!)}`);
  const up = qrotate(out.q[H.chest]!, [0, 0, 1]);
  assert.ok(up[2] > 0.98, 'upright');
});
