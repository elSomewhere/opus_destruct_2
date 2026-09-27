import assert from 'node:assert/strict';
import { test } from 'node:test';
import { H, makeSoldier, Pose, qidentity, qz, vlerp, WorldPose, type V3, type VoxelModel } from '../src/index.ts';
import { carveModel, detachSubtree, partIntegrity, raycastModel, severDisconnected } from '../src/voxel/damage.ts';

function posed(model: VoxelModel, pos: V3 = [0, 0, 0], yaw = 0): { skin: Float32Array; world: WorldPose } {
  const sk = model.skeleton;
  const world = new WorldPose(sk).compute(new Pose(sk), pos, yaw === 0 ? qidentity() : qz(yaw));
  const skin = new Float32Array(sk.count * 16);
  world.writeSkin(skin);
  return { skin, world };
}

test('a ray from the front at chest height hits the chest; a ray pointing away misses', () => {
  const model = makeSoldier(1).model.clone();
  const { skin } = posed(model);
  const hit = raycastModel(model, skin, [0, 2, 1.3], [0, -1, 0], 10);
  assert.ok(hit, 'hit');
  assert.equal(hit.bone, H.chest);
  assert.ok(hit.distance > 1.7 && hit.distance < 1.95, `distance ${hit.distance}`);
  assert.ok(hit.normal[1] > 0.99, `normal ${hit.normal}`);
  for (let a = 0; a < 3; a++) assert.ok(Math.abs(hit.point[a]! - hit.restPoint[a]!) < 1e-5, 'rest pose: rest point = world point');
  assert.equal(raycastModel(model, skin, [0, 2, 1.3], [0, 1, 0], 10), null);
  assert.equal(raycastModel(model, skin, [0, 2, 1.3], [0, -1, 0], 1.5), null, 'out of range');
});

test('carving removes voxels, bumps versions and counts', () => {
  const model = makeSoldier(1).model.clone();
  const { skin } = posed(model);
  const hit = raycastModel(model, skin, [0, 2, 1.3], [0, -1, 0], 10)!;
  const part = model.parts[hit.part]!;
  const before = part.count;
  const v0 = part.version;
  const t0 = performance.now();
  const removed = carveModel(model, hit.restPoint, 0.05);
  const ms = performance.now() - t0;
  assert.ok(removed.length > 5, `${removed.length} removed`);
  assert.ok(part.count < before && part.version > v0);
  assert.ok(partIntegrity(part) < 1 && partIntegrity(part) > 0.5);
  assert.ok(ms < 5, `carve took ${ms} ms`);
  // the same ray now goes deeper
  const again = raycastModel(model, skin, [0, 2, 1.3], [0, -1, 0], 10)!;
  assert.ok(again.distance > hit.distance + 0.02, `${again.distance} vs ${hit.distance}`);
  // removed voxels are unique and inside the sphere
  const keys = new Set(removed.map((r) => r.rest.map((v) => v.toFixed(4)).join(',')));
  assert.equal(keys.size, removed.length);
  for (const r of removed) assert.ok(Math.hypot(r.rest[0] - hit.restPoint[0], r.rest[1] - hit.restPoint[1], r.rest[2] - hit.restPoint[2]) <= 0.05 + 1e-9);
});

test('cutting through the forearm severs the lower forearm; detaching takes the hand', () => {
  const model = makeSoldier(1).model.clone();
  const sk = model.skeleton;
  const fa = model.partOfBone[H.forearmL]!;
  const part = model.parts[fa]!;
  const before = part.count;
  const cut = vlerp(sk.restHead[H.forearmL]!, sk.restTail[H.forearmL]!, 0.45);
  carveModel(model, cut, 0.06);
  const pieces = severDisconnected(model, fa, 0.05);
  assert.ok(pieces.length >= 1, 'something came off');
  const off = pieces.reduce((n, p) => n + p.count, 0);
  assert.ok(off > 10 && part.count > 10 && part.count + off < before, `off ${off}, left ${part.count} of ${before}`);
  for (const p of pieces) {
    assert.equal(p.bone, H.forearmL);
    assert.equal(p.version, 0);
    assert.equal(p.initialCount, p.count);
  }
  // the pieces are below the cut, the rest above it
  const s = model.voxelSize;
  const big = pieces[0]!;
  assert.ok((big.origin[2] + big.dims[2]) * s <= cut[2] + 0.02, 'severed piece lies below the cut');
  // the pelvis never severs
  assert.deepEqual(severDisconnected(model, model.partOfBone[H.pelvis]!, 0.05), []);

  const m2 = makeSoldier(1).model.clone();
  const parts = detachSubtree(m2, H.forearmL, true);
  const bones = parts.map((p) => p.bone);
  assert.ok(bones.includes(H.forearmL) && bones.includes(H.handL), `bones ${bones}`);
  assert.equal(m2.parts[m2.partOfBone[H.handL]!]!.count, 0);
  assert.ok(m2.parts[m2.partOfBone[H.upperarmL]!]!.count > 0);
  // cut at the joint: no anchor left -> the whole part comes off
  const m3 = makeSoldier(1).model.clone();
  const hand = m3.partOfBone[H.handL]!;
  carveModel(m3, m3.skeleton.restHead[H.handL]!, 0.05);
  const whole = severDisconnected(m3, hand, 0.03);
  assert.equal(whole.length, 1);
  assert.equal(m3.parts[hand]!.count, 0);
});

test('rays hit a rotated, translated character', () => {
  const model = makeSoldier(1).model.clone();
  const { skin, world } = posed(model, [5, 3, 0.5], Math.PI / 2);
  const hr = model.skeleton.restHead[H.head]!;
  const head = world.pointOf(H.head, [hr[0], hr[1] + 0.012, hr[2] + 0.1]);
  // the model's right (+x) is world +y after a quarter turn: shoot the head from that side
  const hit = raycastModel(model, skin, [head[0], head[1] + 2, head[2]], [0, -1, 0], 10);
  assert.ok(hit, 'head hit');
  assert.equal(hit.bone, H.head);
  assert.ok(hit.normal[1] > 0.99, `normal ${hit.normal}`);
  assert.ok(hit.restPoint[0] > 0.05, `hit on the right side in rest space: ${hit.restPoint}`);
  // performance: many rays against the posed model
  const t0 = performance.now();
  let n = 0;
  for (let i = 0; i < 2000; i++) {
    const z = 0.1 + (i % 100) * 0.018;
    if (raycastModel(model, skin, [head[0] + ((i * 7) % 11) * 0.02 - 0.1, head[1] + 3, z], [0, -1, 0], 10)) n++;
  }
  const ms = performance.now() - t0;
  console.log(`2000 rays: ${ms.toFixed(1)} ms, ${n} hits`);
  assert.ok(n > 100);
});
