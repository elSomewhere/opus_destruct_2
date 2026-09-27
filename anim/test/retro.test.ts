import assert from 'node:assert/strict';
import { test } from 'node:test';
import { makeCivilian, makeKnife, makePistol, makeRifle, makeSoldier, meshPart, ModelMesher, Pose, WorldPose, type VoxelModel } from '../src/index.ts';
import { bakePose } from '../src/retro/bake.ts';
import { BASIC_RETRO_STATES, bakeRetroSet, RETRO_STATES, RetroPlayer, retroStateFits, retroStateOf, snapYaw8, TICS_PER_SECOND } from '../src/retro/sequences.ts';

/** Extent of a frame's voxels along an axis (0 x, 1 y, 2 z), metres. */
function span(m: VoxelModel, axis: 0 | 1 | 2): [number, number] {
  const p = m.parts[0]!;
  const [nx, ny, nz] = p.dims;
  let lo = Infinity, hi = -Infinity;
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++)
      for (let x = 0; x < nx; x++)
        if (p.cells[x + nx * (y + ny * z)] !== 0) {
          const c = [x, y, z][axis]!;
          lo = Math.min(lo, (p.origin[axis] + c) * m.voxelSize);
          hi = Math.max(hi, (p.origin[axis] + c + 1) * m.voxelSize);
        }
  return [lo, hi];
}

const zSpan = (m: VoxelModel): [number, number] => span(m, 2);

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
  const set = bakeRetroSet(s.model, { weapon: makeRifle(), states: BASIC_RETRO_STATES });
  const t1 = performance.now();
  const coarse = bakeRetroSet(s.model, { weapon: makeRifle(), voxelSize: 1 / 16, states: BASIC_RETRO_STATES });
  const t2 = performance.now();
  let frames = 0;
  for (const seq of set.sequences.values()) frames += seq.frames.length;
  console.log(`retro set 1/32: ${(t1 - t0).toFixed(0)} ms (${frames} frames); 1/16: ${(t2 - t1).toFixed(0)} ms`);
  const counts: Record<string, number> = { idle: 2, walk: 4, run: 4, crouch: 1, crouchWalk: 4, aim: 1, fire: 2, pain: 1, panic: 4, cower: 1, surrender: 2 };
  for (const st of BASIC_RETRO_STATES) {
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

test('the new states bake for what the character holds, and their frames look the part', () => {
  const soldier = makeSoldier(2).model;
  const civ = makeCivilian(3).model;
  const t0 = performance.now();
  const rifle = bakeRetroSet(soldier, { weapon: makeRifle() });
  const t1 = performance.now();
  const pistol = bakeRetroSet(soldier, { weapon: makePistol() });
  const bare = bakeRetroSet(civ, {});
  const knife = bakeRetroSet(civ, { weapon: makeKnife() });
  const coarse = bakeRetroSet(soldier, { weapon: makeRifle(), voxelSize: 1 / 16 });
  const t2 = performance.now();
  let frames = 0;
  for (const seq of rifle.sequences.values()) frames += seq.frames.length;
  console.log(`full retro set (rifle) 1/32: ${(t1 - t0).toFixed(0)} ms, ${rifle.sequences.size} states, ${frames} frames; pistol + unarmed + knife + 1/16 rifle: ${(t2 - t1).toFixed(0)} ms`);
  const expect = (set: typeof rifle, states: string[], absent: string[] = []): void => {
    for (const st of states) {
      const seq = set.sequences.get(st as never);
      assert.ok(seq && seq.frames.length > 0, `state ${st}`);
      for (const f of seq!.frames) assert.ok(f.voxelCount > (set.voxelSize < 0.04 ? 1000 : 100), `${st}: ${f.voxelCount} voxels`);
    }
    for (const st of absent) assert.ok(!set.sequences.has(st as never), `no ${st}`);
  };
  const common = ['talk', 'listen', 'sit', 'sitDesk', 'ground', 'kneel', 'prone', 'crawl', 'peek', 'punch', 'kick', 'down', 'getUp'];
  expect(rifle, [...common, 'kneelFire', 'proneFire', 'reload', 'hipFire'], ['pistolAim', 'pistolFire', 'stab', 'guard', 'block']);
  expect(pistol, [...common, 'pistolAim', 'pistolFire', 'reload', 'guard', 'block'], ['kneelFire', 'hipFire', 'stab']);
  expect(bare, [...common, 'guard', 'block'], ['reload', 'pistolAim', 'hipFire', 'stab']);
  expect(knife, [...common, 'stab', 'guard'], ['reload']);
  expect(coarse, [...common, 'hipFire']);
  for (const st of RETRO_STATES) assert.equal(rifle.sequences.has(st), retroStateFits(st, 'rifle'), st);

  // sitting: on a seat behind the root, lower than standing
  const sit = bare.sequences.get('sit')!.frames[0]!;
  assert.ok(span(sit, 1)[0] < -0.3, `the seated body reaches back ${span(sit, 1)[0]}`);
  assert.ok(zSpan(sit)[1] < 1.45, `seated head ${zSpan(sit)[1]}`);
  // prone and down lie low and long; crawling moves
  for (const st of ['prone', 'down'] as const) {
    const f = rifle.sequences.get(st)!.frames.at(-1)!;
    assert.ok(zSpan(f)[1] < 0.75, `${st} top ${zSpan(f)[1]}`);
    const [y0, y1] = span(f, 1);
    assert.ok(y1 - y0 > 1.2, `${st} length ${y1 - y0}`);
  }
  const crawl = rifle.sequences.get('crawl')!;
  assert.equal(crawl.frames.length, 4);
  assert.ok(crawl.loop && crawl.speed > 0);
  assert.ok(new Set(crawl.frames.map((f) => f.voxelCount + ':' + f.parts[0]!.dims.join(','))).size >= 2, 'crawl frames differ');
  // the fall ends lying; getting up rises
  const down = bare.sequences.get('down')!;
  assert.ok(!down.loop && zSpan(down.frames[0]!)[1] > zSpan(down.frames.at(-1)!)[1], 'the fall goes down');
  const up = bare.sequences.get('getUp')!;
  assert.ok(!up.loop && zSpan(up.frames[0]!)[1] < zSpan(up.frames.at(-1)!)[1], 'getting up rises');
  // kneeling is lower than standing, higher than sitting on the ground
  const kneel = zSpan(rifle.sequences.get('kneel')!.frames[0]!)[1];
  assert.ok(kneel < 1.5 && kneel > 1.0, `kneel top ${kneel}`);
  assert.ok(zSpan(bare.sequences.get('ground')!.frames[0]!)[1] < 1.1);
  // strikes reach out in front: the kick's extended frame reaches further forward than guard
  const guard = span(bare.sequences.get('guard')!.frames[0]!, 1)[1];
  const kick = Math.max(...bare.sequences.get('kick')!.frames.map((f) => span(f, 1)[1]));
  assert.ok(kick > guard + 0.15, `kick reach ${kick} vs guard ${guard}`);
  const punch = Math.max(...bare.sequences.get('punch')!.frames.map((f) => span(f, 1)[1]));
  assert.ok(punch > guard + 0.1, `punch reach ${punch} vs guard ${guard}`);
  // the knife is in the stabbing frames: they reach further than the bare punch's guard
  const stab = Math.max(...knife.sequences.get('stab')!.frames.map((f) => span(f, 1)[1]));
  assert.ok(stab > guard + 0.1, `stab reach ${stab}`);
  // desk typing moves
  const desk = bare.sequences.get('sitDesk')!;
  assert.equal(desk.frames.length, 3);
  // everything meshes
  const mesher = new ModelMesher();
  for (const st of ['sit', 'crawl', 'kick', 'getUp'] as const) assert.ok(mesher.mesh(bare.sequences.get(st)?.frames[0] ?? rifle.sequences.get(st)!.frames[0]!).indexCount > 0);
});

test('retro states follow stances, conversations, fights, weapons and knockdowns', () => {
  const base = { speed: 0, crouch: 0, aiming: false, firing: false, mood: 'normal' as const, pain: false };
  assert.equal(retroStateOf({ ...base, stance: 'sit' }), 'sit');
  assert.equal(retroStateOf({ ...base, stance: 'sit', desk: true }), 'sitDesk');
  assert.equal(retroStateOf({ ...base, talk: 'speak' }), 'talk');
  assert.equal(retroStateOf({ ...base, talk: 'listen' }), 'listen');
  assert.equal(retroStateOf({ ...base, guard: true }), 'guard');
  assert.equal(retroStateOf({ ...base, guard: true, action: 'jab' }), 'punch');
  assert.equal(retroStateOf({ ...base, guard: true, action: 'cross.m' }), 'punch');
  assert.equal(retroStateOf({ ...base, guard: true, action: 'roundhouse' }), 'kick');
  assert.equal(retroStateOf({ ...base, action: 'slash', weapon: 'knife' }), 'stab');
  assert.equal(retroStateOf({ ...base, action: 'reloadRifle', aiming: true, weapon: 'rifle' }), 'reload');
  assert.equal(retroStateOf({ ...base, stance: 'prone', speed: 0.4 }), 'crawl');
  assert.equal(retroStateOf({ ...base, stance: 'prone', firing: true, aiming: true }), 'proneFire');
  assert.equal(retroStateOf({ ...base, stance: 'kneel', aiming: true }), 'kneel');
  assert.equal(retroStateOf({ ...base, stance: 'kneel', firing: true }), 'kneelFire');
  assert.equal(retroStateOf({ ...base, stance: 'ground' }), 'ground');
  assert.equal(retroStateOf({ ...base, lean: -1, aiming: true }), 'peek');
  assert.equal(retroStateOf({ ...base, knockedDown: true, stance: 'stand', transitioning: true }), 'down');
  assert.equal(retroStateOf({ ...base, knockedDown: true, stance: 'down' }), 'down');
  assert.equal(retroStateOf({ ...base, knockedDown: true, stance: 'kneel', transitioning: true }), 'getUp');
  assert.equal(retroStateOf({ ...base, aiming: true, weapon: 'pistol' }), 'pistolAim');
  assert.equal(retroStateOf({ ...base, aiming: true, firing: true, weapon: 'pistol' }), 'pistolFire');
  assert.equal(retroStateOf({ ...base, aiming: true, firing: true, weapon: 'lmg', carry: 'hip' }), 'hipFire');
  assert.equal(retroStateOf({ ...base, aiming: true, weapon: 'rifle' }), 'aim');
  // pain wins over an action; a knockdown over pain
  assert.equal(retroStateOf({ ...base, pain: true, action: 'jab' }), 'pain');
  assert.equal(retroStateOf({ ...base, pain: true, knockedDown: true, stance: 'stand' }), 'down');
  // fallbacks: a pistol set played as a rifle state, a crawl played without crawl frames
  const set = bakeRetroSet(makeSoldier(4).model, { weapon: makePistol(), states: ['idle', 'aim', 'prone'] });
  const p = new RetroPlayer(set);
  assert.equal(p.sequence('pistolFire').state, 'aim');
  assert.equal(p.sequence('crawl').state, 'prone');
  assert.equal(p.sequence('kick').state, 'idle');
});
