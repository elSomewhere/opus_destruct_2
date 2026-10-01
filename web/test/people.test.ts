/**
 * People on the client (game/people.ts; protocol `characterMeshes`, `characters` and `blood`): the
 * `characters` message parsed and posed one tick behind, fading in and out, shadows on the ground
 * under those on their feet, gibs of one matrix, and the messages' guards and transfer lists.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import type { WorkerMessage } from '../src/engine/protocol.ts';
import {
  BLOOD_DROP_STRIDE,
  BLOOD_STAIN_STRIDE,
  CHARACTER_SKIN_FLOATS,
  CHARACTER_STRIDE,
  CharacterFlag,
  isEngineCommand,
  isWorkerMessage,
  workerMessageTransferables,
} from '../src/engine/protocol.ts';
import { CharacterTracker } from '../src/game/people.ts';

/** One character's record and skin: every bone translated to `at` (identity rotations). */
function character(id: number, at: [number, number, number], flags: number): { data: Float64Array; skin: Float32Array } {
  const data = new Float64Array(CHARACTER_STRIDE);
  data[0] = id;
  data[1] = 7; // mesh
  data[2] = 3; // palette
  data[3] = flags;
  data.set([at[0], at[1], at[2] + 0.9, 1.1], 4);
  data[8] = 0.5; // flash
  data[10] = 0.75; // health
  const skin = new Float32Array(CHARACTER_SKIN_FLOATS);
  for (let b = 0; b < CHARACTER_SKIN_FLOATS / 16; b++) {
    const o = b * 16;
    skin[o] = skin[o + 5] = skin[o + 10] = skin[o + 15] = 1;
    skin.set(at, o + 12);
  }
  return { data, skin };
}

test('people: poses come one tick behind, interpolated; they fade in, and out when gone', () => {
  const t = new CharacterTracker();
  const alive = CharacterFlag.Alive | CharacterFlag.Physical;
  const a = character(4, [0, 0, 0], alive);
  t.apply(a.data, a.skin, undefined, 10);
  const b = character(4, [1, 0, 0], alive);
  t.apply(b.data, b.skin, undefined, 10 + 1 / 60);
  assert.equal(t.size, 1);
  // halfway between the samples, a tick behind
  let [d] = t.frame(10 + 1 / 60 + 0.5 / 60);
  assert.ok(d);
  assert.ok(Math.abs(d.skin[12]! - 0.5) < 1e-6, `root x ${d.skin[12]}`);
  assert.ok(Math.abs(d.skin[16 * 5 + 12]! - 0.5) < 1e-6, 'every bone');
  assert.equal(d.mesh, 7);
  assert.equal(d.palette, 3);
  assert.equal(d.flash, 0.5);
  assert.deepEqual(d.shadow, [0.5, 0, 0], 'on its feet: a shadow under it');
  assert.equal(d.bones, 23);
  assert.equal(t.characters.get(4)!.health, 0.75);
  assert.ok(d.opacity > 0 && d.opacity < 0.1, `fading in: ${d.opacity}`);
  [d] = t.frame(12);
  assert.equal(d!.opacity, 1);
  assert.ok(Math.abs(d!.skin[12]! - 1) < 1e-6, 'the last pose once past it');
  // down (dead): no shadow
  const c = character(4, [1, 0, 0], CharacterFlag.Down);
  t.apply(c.data, c.skin, undefined, 12);
  [d] = t.frame(12.1);
  assert.equal(d!.shadow, null);
  // gone from the next message: fading out at its last pose, then gone
  t.apply(new Float64Array(0), new Float32Array(0), undefined, 13);
  assert.equal(t.size, 0);
  [d] = t.frame(13.3);
  assert.ok(d && d.opacity > 0 && d.opacity < 1, `fading out: ${d?.opacity}`);
  assert.equal(t.frame(14).length, 0);
  assert.equal(t.characters.size, 0);
});

test('people: the shadow lies at the lowest sole, not at the root of a physical body (its pelvis)', () => {
  const t = new CharacterTracker();
  const a = character(2, [3, 4, 0.063], CharacterFlag.Alive | CharacterFlag.Physical);
  a.skin[14] = -0.05; // (the root rides with the pelvis)
  a.skin[16 * 16 + 14] = 0.21; // (the left foot in the air)
  t.apply(a.data, a.skin, undefined, 1);
  const [d] = t.frame(2);
  assert.ok(d!.shadow);
  assert.ok(Math.abs(d!.shadow[2] - 0.063) < 1e-6, `shadow z ${d!.shadow[2]}`);
  assert.equal(d!.shadow[0], 3);
});

test('people: a gib is drawn with its first matrix alone, without a shadow', () => {
  const t = new CharacterTracker();
  const g = character(0x80000005, [1, 2, 0.3], CharacterFlag.Gib | CharacterFlag.Physical);
  t.apply(g.data, g.skin, undefined, 1);
  const [d] = t.frame(2);
  assert.equal(d!.bones, 1);
  assert.equal(d!.shadow, null);
  assert.equal(t.characters.get(0x80000005)!.id, 0x80000005);
});

test('people: after a gap the pose starts from the new sample', () => {
  const t = new CharacterTracker();
  const a = character(1, [0, 0, 0], CharacterFlag.Alive);
  t.apply(a.data, a.skin, undefined, 5);
  const b = character(1, [4, 0, 0], CharacterFlag.Alive);
  t.apply(b.data, b.skin, undefined, 6);
  const [d] = t.frame(6);
  assert.ok(Math.abs(d!.skin[12]! - 4) < 1e-6, `root x ${d!.skin[12]}`);
});

test('protocol: character messages and commands pass the guards; their buffers are transferred', () => {
  const vertices = new ArrayBuffer(40);
  const indices = new ArrayBuffer(12);
  const rgb = new Float32Array(48);
  const meshes: WorkerMessage = { type: 'characterMeshes', meshes: [{ id: 1, vertices, vertexCount: 2, indices, indexCount: 3 }], removed: [9], palettes: [{ id: 2, rgb }] };
  assert.ok(isWorkerMessage(meshes));
  assert.deepEqual(workerMessageTransferables(meshes), [vertices, indices, rgb.buffer]);
  const characters = new Float64Array(CHARACTER_STRIDE);
  const skin = new Float32Array(CHARACTER_SKIN_FLOATS);
  const props = new Float32Array(16);
  const poses: WorkerMessage = { type: 'characters', characters, skin, props, seq: 3 };
  assert.ok(isWorkerMessage(poses));
  assert.deepEqual(workerMessageTransferables(poses), [characters.buffer, skin.buffer, props.buffer]);
  assert.ok(isEngineCommand({ type: 'setPedestrians', pedestrians: { enabled: true, count: 8, nearRadius: 30, radius: 70, bodies: 2, maxDeep: 8 } }));
  assert.ok(isEngineCommand({ type: 'woundCharacter', id: 4, pos: [0, 0, 1], radius: 0.15, energy: 500 }));
  const drops = new Float32Array(BLOOD_DROP_STRIDE * 2);
  const stains = new Float32Array(BLOOD_STAIN_STRIDE);
  const blood: WorkerMessage = { type: 'blood', drops, stains };
  assert.ok(isWorkerMessage(blood));
  assert.deepEqual(workerMessageTransferables(blood), [drops.buffer, stains.buffer]);
});
