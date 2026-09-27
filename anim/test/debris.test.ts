import assert from 'node:assert/strict';
import { test } from 'node:test';
import { FlatGround, H, makeSoldier, qaxis, qidentity, transformPoint, vnorm, VoxelCollision, type V3, type VoxelPart } from '../src/index.ts';
import { GibSystem, type Gib } from '../src/physics/debris.ts';

const soldier = makeSoldier(1);
const model = soldier.model;
const s = model.voxelSize;
const partOf = (bone: number): VoxelPart => model.parts[model.partOfBone[bone]!]!;

/** World centres of every solid cell of a gib. */
function cellCentres(sys: GibSystem, g: Gib): V3[] {
  const p = g.part;
  const out: V3[] = [];
  for (let z = 0; z < p.dims[2]; z++)
    for (let y = 0; y < p.dims[1]; y++)
      for (let x = 0; x < p.dims[0]; x++) {
        if (p.cells[x + p.dims[0] * (y + p.dims[1] * z)] === 0) continue;
        out.push(sys.worldPoint(g, [(p.origin[0] + x + 0.5) * s, (p.origin[1] + y + 0.5) * s, (p.origin[2] + z + 0.5) * s]));
      }
  return out;
}

test('a gib dropped with spin comes to rest on the ground and sleeps', () => {
  const sys = new GibSystem(new FlatGround(0));
  const bone = H.forearmL;
  const g = sys.spawn(partOf(bone), s, [0.3, -0.2, 2], qaxis(vnorm([0.3, 1, 0.2]), 1.1), model.skeleton.restHead[bone]!, [0.5, 0, 0], [3, -2, 5]);
  let slept = -1;
  for (let i = 0; i < 240; i++) {
    sys.update(1 / 60);
    if (g.asleep && slept < 0) slept = (i + 1) / 60;
  }
  assert.ok(slept > 0 && slept <= 4, `asleep after ${slept} s`);
  const lowest = Math.min(...cellCentres(sys, g).map((p) => p[2] - s / 2));
  assert.ok(lowest >= -s, `lowest cell bottom at ${lowest}`);
  assert.ok(lowest < 0.05, `resting on the ground, lowest at ${lowest}`);
  assert.ok(Math.hypot(g.pos[0], g.pos[1]) < 3, 'stayed near where it fell');
});

test('a gib thrown at 15 m/s into a raised block does not end inside solid voxels', () => {
  const h = 0.125;
  const solid = (i: number, j: number, k: number): boolean => k < 0 || (i >= 8 && i < 12 && j >= -24 && j < 24 && k < 8);
  const world = new VoxelCollision(h, solid);
  const sys = new GibSystem(world);
  const idx = (v: number): number => Math.floor(v / h + 0.5);
  for (const bone of [H.head, H.thighR, H.chest]) {
    const g = sys.spawn(partOf(bone), s, [0, 0, 0.35], qidentity(), model.skeleton.restHead[bone]!, [15, 0.4, 0.5], [0, 4, 1]);
    // (the chunk starts with its pivot at bonePos + (pivot - head); lift it clear of the ground)
    const low = Math.min(...cellCentres(sys, g).map((p) => p[2] - s / 2));
    if (low < 0.05) g.pos[2] += 0.05 - low;
    // a cell is 'deep' in solid when its centre and the six points half a voxel around it all
    // are (a cell between collision samples may rest up to half a voxel into a surface)
    const deep = (p: V3): boolean => {
      for (const [dx, dy, dz] of [[0, 0, 0], [1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]] as const) {
        if (!solid(idx(p[0] + (dx * s) / 2), idx(p[1] + (dy * s) / 2), idx(p[2] + (dz * s) / 2))) return false;
      }
      return true;
    };
    for (let i = 0; i < 180; i++) {
      sys.update(1 / 60);
      for (const p of cellCentres(sys, g)) assert.ok(!deep(p), `bone ${bone}: cell deep in solid at ${p.map((v) => v.toFixed(3))} (t ${((i + 1) / 60).toFixed(2)})`);
    }
    // and at the end nothing is below the ground surface by more than half a voxel
    const lowest = Math.min(...cellCentres(sys, g).map((p) => p[2]));
    assert.ok(lowest > -h / 2 - s / 2, `bone ${bone}: lowest cell centre ${lowest.toFixed(4)}`);
    assert.ok(g.pos[0] < 0.9375, `bone ${bone}: stopped by the wall, at ${g.pos.map((v) => v.toFixed(3))}`);
  }
});

test('blood spray leaves stains on the ground with upward normals', () => {
  const sys = new GibSystem(new FlatGround(0), { seed: 3 });
  sys.spray([0, 0, 1.2], [1, 0, 0.3], 60, 4, 0.5);
  assert.equal(sys.drops.length, 60);
  for (let i = 0; i < 180; i++) sys.update(1 / 60);
  assert.equal(sys.drops.length, 0, 'every drop landed or expired');
  assert.ok(sys.stains.length > 5, `${sys.stains.length} stains`);
  sys.forEachStain((p, n, size) => {
    assert.ok(Math.abs(p[2]) < 1e-6, `stain height ${p[2]}`);
    assert.ok(n[2] > 0.999, `stain normal ${n}`);
    assert.ok(size > 0 && size < 0.4);
  });
});

test('writeSkin maps the pivot to the gib position; impulses wake sleeping gibs', () => {
  const sys = new GibSystem(new FlatGround(0));
  const bone = H.head;
  const g = sys.spawn(partOf(bone), s, [1, 2, 0.5], qaxis([0, 0, 1], 0.7), model.skeleton.restHead[bone]!, [0, 0, 0], [0, 0, 0]);
  const m = new Float32Array(32);
  sys.writeSkin(g, m, 16);
  const p = transformPoint(m, 16, g.pivot);
  for (let a = 0; a < 3; a++) assert.ok(Math.abs(p[a]! - g.pos[a]!) < 1e-5);
  for (let i = 0; i < 300; i++) sys.update(1 / 60);
  assert.ok(g.asleep, 'head came to rest');
  sys.impulse([g.pos[0] - 0.5, g.pos[1], 0], 3, 8);
  assert.ok(!g.asleep && g.vel[0] > 0, 'blast woke and pushed it');
  sys.update(1 / 60);
  assert.ok(g.pos[2] > 0);
});

test('blood hitting a voxel wall stains it with the wall normal', () => {
  const h = 0.125;
  const world = new VoxelCollision(h, (i, _j, k) => k < 0 || i >= 8);
  const sys = new GibSystem(world, { seed: 9 });
  sys.spray([0, 0, 1], [1, 0, 0], 40, 9, 0.08);
  for (let i = 0; i < 120; i++) sys.update(1 / 60);
  const wall = sys.stains.filter((st) => st.normal[0] < -0.99);
  assert.ok(wall.length > 3, `${wall.length} wall stains of ${sys.stains.length}`);
  for (const st of wall) assert.ok(Math.abs(st.pos[0] - 7.5 * h) < 0.01, `wall stain at x ${st.pos[0]}`);
});
