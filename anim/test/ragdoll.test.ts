import assert from 'node:assert/strict';
import { test } from 'node:test';
import { FlatGround, H, HumanoidAnimator, HumanoidRagdoll, humanoidSkeleton, VoxelCollision } from '../src/index.ts';

function dieWhileWalking(collision: FlatGround | VoxelCollision, push: [number, number, number]) {
  const sk = humanoidSkeleton();
  const an = new HumanoidAnimator(sk, collision, 3);
  an.place([0, 0, 0], 0);
  let x = 0;
  for (let i = 0; i < 90; i++) {
    x += 1.4 * Math.min(1, (i + 1) / 24) / 60;
    an.setRoot([x, 0, 0], 0);
    an.update(1 / 60);
  }
  const rd = new HumanoidRagdoll(sk, collision, an.world, an.prevWorld, 1 / 60);
  rd.hit(an.world.p[H.chest]!, push);
  let t = 0;
  for (; t < 8 && !rd.asleep; t += 1 / 60) rd.update(1 / 60);
  return { rd, t };
}

test('a ragdoll falls, lands on the ground and sleeps', () => {
  const { rd, t } = dieWhileWalking(new FlatGround(0), [0, 0, 0]);
  assert.ok(rd.asleep, `asleep after ${t.toFixed(2)} s`);
  for (const p of rd.body.particles) {
    assert.ok(Number.isFinite(p.p[0] + p.p[1] + p.p[2]));
    assert.ok(p.p[2] > p.r * 0.5 - 0.02, `particle below ground: ${p.p[2]}`);
  }
  // lying down: the head is low
  assert.ok(rd.world.p[H.head]![2] < 0.5, `head at ${rd.world.p[H.head]![2]}`);
  // the bones stay connected to their particles
  const d = Math.hypot(...([0, 1, 2].map((a) => rd.world.p[H.shinL]![a]! - rd.body.particles[13]!.p[a]!) as [number, number, number]));
  assert.ok(d < 0.03, `knee joint ${d} from its particle`);
});

test('a shot in the back throws it forwards; it stays out of voxel walls', () => {
  const h = 0.125;
  // floor below z = 0 and a wall at x in [1.5, 2]
  const vox = new VoxelCollision(h, (i, _j, k) => k < 0 || (i * h >= 1.5 && i * h <= 2 && k * h < 2));
  const { rd } = dieWhileWalking(vox, [6, 0, 1]);
  for (const p of rd.body.particles) {
    const i = Math.floor(p.p[0] / h + 0.5), k = Math.floor(p.p[2] / h + 0.5);
    assert.ok(!vox.solid(i, 0, k), `particle inside a voxel at ${p.p.map((v) => v.toFixed(2))}`);
  }
});
