import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Character, FlatGround, H, makeRifle, makeSoldier, vnorm, type V3 } from '../src/index.ts';

function soldier(): Character {
  const v = makeSoldier(4);
  const c = new Character({ model: v.model, palette: v.palette, collision: new FlatGround(0), weapon: makeRifle() });
  c.place([0, 0, 0], Math.PI / 2); // facing +y
  for (let i = 0; i < 30; i++) c.update(1 / 60);
  return c;
}

test('a head shot kills, turns the body into a ragdoll and leaves the shared model alone', () => {
  const c = soldier();
  const shared = c.model;
  const head = c.pose.p[H.head]!;
  const origin: V3 = [head[0], head[1] + 5, head[2] + 0.1];
  const dir = vnorm([0, -1, 0]);
  const hit = c.raycast(origin, dir, 20);
  assert.ok(hit, 'hit');
  assert.ok(hit!.bone === H.head || hit!.bone === H.neck, `bone ${hit!.bone}`);
  const r = c.wound(hit!, dir, 35);
  assert.ok(r.headshot && r.killed && !c.alive);
  assert.ok(r.removed.length > 0);
  assert.notEqual(c.model, shared);
  assert.equal(shared.voxelCount, makeSoldier(4).model.voxelCount, 'the shared model is untouched');
  const g = c.dropWeapon();
  assert.ok(g && g.prop);
  for (let i = 0; i < 240; i++) c.update(1 / 60);
  assert.ok(c.pose.p[H.head]![2] < 0.6, 'lying down');
});

test('limb shots sever the limb with the parts below it; a blast gibs the body', () => {
  const c = soldier();
  let severed = false;
  let bones: number[] = [];
  for (let k = 0; k < 12 && !severed; k++) {
    // shoot the left forearm from the side
    const p = c.pose.pointOf(H.forearmL, c.model.skeleton.restHead[H.forearmL]!.map((v, i) => v + [0, 0, -0.1][i]!) as V3);
    const dir: V3 = [1, 0, 0];
    const off = [[0, 0], [0.025, 0], [-0.025, 0], [0, 0.025], [0, -0.025]][k % 5]!;
    const hit = c.raycast([p[0] - 3, p[1] + off[0]!, p[2] + off[1]!], dir, 10);
    if (!hit) continue;
    const r = c.wound(hit, dir, 10, 0.05);
    if (r.gibs.length > 0) {
      severed = true;
      bones = r.gibs.map((g) => g.part.bone);
    }
  }
  assert.ok(severed, 'forearm severed');
  assert.ok(bones.includes(H.handL), `hand came off with it (${bones})`);
  const b = c.blast(c.pose.p[H.chest]!, 1, 1);
  assert.ok(b.gibbed && b.gibs.length >= 8, `gibs ${b.gibs.length}`);
});
