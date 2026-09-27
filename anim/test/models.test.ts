import assert from 'node:assert/strict';
import { test } from 'node:test';
import { H, makeCivilian, makeRifle, makeSoldier, meshPart, ModelMesher, Pose, WorldPose, qidentity } from '../src/index.ts';

test('soldier and civilian models build with a part per voxel-bearing bone', () => {
  const t0 = performance.now();
  const s = makeSoldier(1);
  const t1 = performance.now();
  const c = makeCivilian(2);
  const t2 = performance.now();
  console.log(`soldier ${s.model.voxelCount} voxels ${(t1 - t0).toFixed(0)} ms, civilian ${c.model.voxelCount} voxels ${(t2 - t1).toFixed(0)} ms`);
  for (const v of [s, c]) {
    assert.ok(v.model.voxelCount > 1500, `${v.spec.name}: ${v.model.voxelCount} voxels`);
    for (const b of [H.pelvis, H.chest, H.head, H.handL, H.footR, H.shinL]) assert.ok(v.model.partOfBone[b]! >= 0, `bone ${b} has voxels`);
    assert.equal(v.model.partOfBone[H.weapon], -1);
  }
  const mesher = new ModelMesher();
  const m = mesher.mesh(s.model);
  console.log(`soldier mesh: ${m.vertexCount} vertices, ${m.indexCount / 3} triangles`);
  assert.ok(m.indexCount > 0 && m.indexCount % 3 === 0);
  const rifle = makeRifle();
  assert.ok(rifle.model.voxelCount > 100);
  assert.ok(meshPart(rifle.model.parts[0]!, rifle.model.voxelSize).vertexCount > 0);
});

test('the rest pose skins every vertex to its rest position', () => {
  const s = makeSoldier(3);
  const pose = new Pose(s.model.skeleton);
  const wp = new WorldPose(s.model.skeleton).compute(pose, [0, 0, 0], qidentity());
  for (let i = 0; i < s.model.skeleton.count; i++) {
    const h = s.model.skeleton.restHead[i]!;
    for (let a = 0; a < 3; a++) assert.ok(Math.abs(wp.p[i]![a]! - h[a]!) < 1e-9);
  }
});
