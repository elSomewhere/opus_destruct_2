import assert from 'node:assert/strict';
import { test } from 'node:test';
import { makeRifle, makeSoldier, meshPart, ModelMesher, Pose, WorldPose, type VoxelModel } from '../src/index.ts';
import { bakePose } from '../src/retro/bake.ts';
import { bakeRetroSet, RETRO_STATES, RetroPlayer, retroStateOf, snapYaw8, TICS_PER_SECOND } from '../src/retro/sequences.ts';

function zSpan(m: VoxelModel): [number, number] {
  const p = m.parts[0]!;
  const [nx, ny, nz] = p.dims;
  let lo = Infinity, hi = -Infinity;
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++)
      for (let x = 0; x < nx; x++)
        if (p.cells[x + nx * (y + ny * z)] !== 0) {
          lo = Math.min(lo, (p.origin[2] + z) * m.voxelSize);
          hi = Math.max(hi, (p.origin[2] + z + 1) * m.voxelSize);
        }
  return [lo, hi];
}

test('baking the rest pose reproduces the model', () => {
  const s = makeSoldier(1);
  const world = new WorldPose(s.model.skeleton).compute(new Pose(s.model.skeleton), [0, 0, 0], [0, 0, 0, 1]);
  const t0 = performance.now();
  const baked = bakePose(s.model, world);
  const t1 = performance.now();
  const coarse = bakePose(s.model, world, { voxelSize: 1 / 16 });
  const t2 = performance.now();
  console.log(`rest bake 1/32: ${(t1 - t0).toFixed(1)} ms, ${baked.voxelCount} voxels (model ${s.model.voxelCount} incl. joint copies); 1/16: ${(t2 - t1).toFixed(1)} ms, ${coarse.voxelCount}`);
  assert.equal(baked.parts.length, 1);
  assert.equal(baked.skeleton.count, 1);
  // joint balls are copied into both parts of a joint: the unique cells are fewer
  const unique = new Set<string>();
  for (const p of s.model.parts) {
    const [nx, ny, nz] = p.dims;
    for (let z = 0; z < nz; z++)
      for (let y = 0; y < ny; y++)
        for (let x = 0; x < nx; x++) if (p.cells[x + nx * (y + ny * z)] !== 0) unique.add(`${p.origin[0] + x},${p.origin[1] + y},${p.origin[2] + z}`);
  }
  assert.ok(Math.abs(baked.voxelCount - unique.size) <= unique.size * 0.05, `baked ${baked.voxelCount} vs unique ${unique.size}`);
  assert.ok(baked.voxelCount <= s.model.voxelCount);
  const ratio = coarse.voxelCount / baked.voxelCount;
  assert.ok(ratio > 0.09 && ratio < 0.18, `1/16 m keeps ${ratio.toFixed(3)} of the voxels`);
  const mesh = new ModelMesher().mesh(baked);
  assert.ok(mesh.indexCount > 0 && mesh.indexCount % 3 === 0);
  assert.ok(meshPart(coarse.parts[0]!, coarse.voxelSize).vertexCount > 0);
});

test('a retro set bakes every state with its frames, in time', () => {
  const s = makeSoldier(2);
  const t0 = performance.now();
  const set = bakeRetroSet(s.model, { weapon: makeRifle() });
  const t1 = performance.now();
  const coarse = bakeRetroSet(s.model, { weapon: makeRifle(), voxelSize: 1 / 16 });
  const t2 = performance.now();
  let frames = 0;
  for (const seq of set.sequences.values()) frames += seq.frames.length;
  console.log(`retro set 1/32: ${(t1 - t0).toFixed(0)} ms (${frames} frames); 1/16: ${(t2 - t1).toFixed(0)} ms`);
  const counts: Record<string, number> = { idle: 2, walk: 4, run: 4, crouch: 1, crouchWalk: 4, aim: 1, fire: 2, pain: 1, panic: 4, cower: 1, surrender: 2 };
  for (const st of RETRO_STATES) {
    const seq = set.sequences.get(st);
    assert.ok(seq, `state ${st}`);
    assert.equal(seq.frames.length, counts[st], `${st} frames`);
    assert.ok(seq.tics >= 1);
    for (const f of seq.frames) assert.ok(f.voxelCount > 1000, `${st}: ${f.voxelCount} voxels`);
  }
  // a walk frame stands on the ground and is about a person tall
  const walk = set.sequences.get('walk')!;
  for (const f of walk.frames) {
    const [lo, hi] = zSpan(f);
    assert.ok(lo < 0.06 && lo > -0.1, `walk frame bottom ${lo}`);
    assert.ok(hi > 1.6 && hi < 1.95, `walk frame top ${hi}`); // (builds are 0.97..1.05 of 1.78 m, walking sits lower)
  }
  // the four walk frames differ (it is a cycle, not one pose)
  const sizes = new Set(walk.frames.map((f) => f.parts[0]!.cells.length + ':' + f.voxelCount));
  assert.ok(sizes.size >= 3, 'walk frames differ');
  // crouching is lower
  assert.ok(zSpan(set.sequences.get('crouch')!.frames[0]!)[1] < 1.5);
  assert.equal(coarse.voxelSize, 1 / 16);
  assert.ok(coarse.sequences.get('walk')!.frames[0]!.voxelCount < walk.frames[0]!.voxelCount / 4);
});

test('the retro player steps in tics, loops and restarts on a state change', () => {
  const s = makeSoldier(3);
  const set = bakeRetroSet(s.model, { states: ['idle', 'walk', 'pain'] });
  const walk = set.sequences.get('walk')!;
  const player = new RetroPlayer(set, 'walk');
  const tic = 1 / TICS_PER_SECOND;
  const seen: number[] = [];
  for (let i = 0; i < walk.tics * 8; i++) {
    player.update(tic, 'walk');
    seen.push(player.frameIndex);
  }
  // each frame is held for exactly `tics` tics, the cycle loops
  for (let i = 0; i < seen.length; i++) assert.equal(seen[i], Math.floor((i + 1) / walk.tics) % 4, `tic ${i}`);
  // a state change restarts the new sequence; a non-looping one holds its last frame
  const f = player.update(tic, 'pain');
  assert.equal(player.frameIndex, 0);
  assert.equal(f, set.sequences.get('pain')!.frames[0]);
  for (let i = 0; i < 50; i++) player.update(tic, 'pain');
  assert.equal(player.frameIndex, 0);
  // a state that was not baked falls back
  assert.equal(player.update(tic, 'run'), walk.frames[0]);
  // rate scales the clock
  const p2 = new RetroPlayer(set, 'walk');
  p2.update(tic * walk.tics, 'walk', 0.5);
  assert.equal(p2.frameIndex, 0);
  p2.update(tic * walk.tics, 'walk', 0.5);
  assert.equal(p2.frameIndex, 1);
});

test('yaw snaps to 8 directions and states follow what a character does', () => {
  const q = Math.PI / 4;
  for (const y of [0, 0.3, 0.5, 1.2, -2.9, 3.1, 7]) {
    const s = snapYaw8(y);
    assert.ok(Math.abs(s / q - Math.round(s / q)) < 1e-9);
    assert.ok(Math.abs(s - y) <= q / 2 + 1e-9);
  }
  const base = { speed: 0, crouch: 0, aiming: false, firing: false, mood: 'normal' as const, pain: false };
  assert.equal(retroStateOf(base), 'idle');
  assert.equal(retroStateOf({ ...base, speed: 1.4 }), 'walk');
  assert.equal(retroStateOf({ ...base, speed: 4 }), 'run');
  assert.equal(retroStateOf({ ...base, crouch: 1, speed: 1 }), 'crouchWalk');
  assert.equal(retroStateOf({ ...base, aiming: true, firing: true }), 'fire');
  assert.equal(retroStateOf({ ...base, pain: true, firing: true }), 'pain');
  assert.equal(retroStateOf({ ...base, mood: 'cower' }), 'cower');
});
