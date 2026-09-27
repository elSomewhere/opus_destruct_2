import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Character, FlatGround, H, makeRifle, makeSoldier, qrotate, type Quat, type V3 } from '../src/index.ts';

const DT = 1 / 60;

/** A soldier standing (or running) who dies from a shot (or a blast); returns per-frame traces. */
function death(opts: { speed?: number; dir?: V3; z?: number; collapse?: number; blast?: boolean; seed?: number }) {
  const s = makeSoldier(opts.seed ?? 1);
  const c = new Character({ model: s.model, palette: s.palette, collision: new FlatGround(0), weapon: makeRifle() });
  c.place([0, 0, 0], Math.PI / 2);
  let y = 0;
  for (let i = 0; i < 90; i++) {
    y += (opts.speed ?? 0) * DT;
    c.setRoot([0, y, 0], Math.PI / 2);
    c.update(DT);
  }
  const start: V3 = [c.pose.p[H.pelvis]![0], c.pose.p[H.pelvis]![1], 0];
  if (opts.blast) {
    c.die(undefined, undefined, 0);
    c.ragdoll!.blast([0.8, y - 0.5, 0.3], 3, 9);
  } else {
    const d = opts.dir ?? [0, -1, 0];
    const p = c.pose.p[H.chest]!;
    c.die([p[0], p[1], opts.z ?? 1.3], [d[0] * 2.8, d[1] * 2.8, d[2] * 2.8], opts.collapse ?? 0.6);
  }
  const sk = c.model.skeleton;
  let prev = c.pose.q.map((q) => [...q] as Quat);
  let worstTwist = 0;
  const pelvisZ: number[] = [];
  const headZ: number[] = [];
  let sleptAt = -1;
  for (let i = 1; i <= 60 * 6; i++) {
    c.update(DT);
    const q = c.pose.q;
    // roll change of every bone about its own length, frame to frame (a flip)
    for (let b = 1; b < q.length; b++) {
      if (b === H.weapon) continue;
      const r = [sk.restTail[b]![0] - sk.restHead[b]![0], sk.restTail[b]![1] - sk.restHead[b]![1], sk.restTail[b]![2] - sk.restHead[b]![2]] as V3;
      const rl = Math.hypot(...r) || 1;
      const axis = qrotate(q[b]!, [r[0] / rl, r[1] / rl, r[2] / rl]);
      const across: V3 = Math.abs(r[0] / rl) < 0.9 ? [1, 0, 0] : [0, 1, 0];
      const flat = (v: V3): V3 => {
        const d = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
        const w: V3 = [v[0] - axis[0] * d, v[1] - axis[1] * d, v[2] - axis[2] * d];
        const l = Math.hypot(...w) || 1;
        return [w[0] / l, w[1] / l, w[2] / l];
      };
      const x0 = flat(qrotate(prev[b]!, across)), x1 = flat(qrotate(q[b]!, across));
      if (i > 9) worstTwist = Math.max(worstTwist, Math.acos(Math.max(-1, Math.min(1, x0[0] * x1[0] + x0[1] * x1[1] + x0[2] * x1[2]))));
    }
    prev = q.map((x) => [...x] as Quat);
    pelvisZ.push(c.ragdoll!.body.particles[0]!.p[2]);
    headZ.push(c.pose.p[H.head]![2]);
    if (sleptAt < 0 && c.ragdoll!.asleep) sleptAt = i * DT;
  }
  const end = c.pose.p[H.pelvis]!;
  return { worstTwist: (worstTwist * 180) / Math.PI, pelvisZ, headZ, sleptAt, moved: [end[0] - start[0], end[1] - start[1]] as const };
}

test('ragdolls: limbs never flip about their length, whatever the death', () => {
  for (const o of [{}, { dir: [0, 1, 0] as V3 }, { speed: 4.5 }, { dir: [1, 0, 0] as V3, z: 1.6, collapse: 0.15 }, { blast: true }, { seed: 2, dir: [0, 1, 0] as V3 }]) {
    const r = death(o);
    assert.ok(r.worstTwist < 60, `${JSON.stringify(o)}: a bone rolled ${r.worstTwist.toFixed(0)} deg in one frame`);
  }
});

test('ragdolls: bodies come to rest on the ground and never bounce or blow up', () => {
  for (const o of [{}, { dir: [0, 1, 0] as V3 }, { speed: 4.5 }, { collapse: 0.15 }, { blast: true }, { seed: 2, dir: [0, 1, 0] as V3 }]) {
    const r = death(o);
    let low = Infinity, rise = 0;
    r.pelvisZ.forEach((z, i) => {
      if (i < 40) return;
      low = Math.min(low, z);
      rise = Math.max(rise, z - low);
    });
    assert.ok(rise < 0.12, `${JSON.stringify(o)}: the pelvis rose ${(rise * 100).toFixed(0)} cm after coming down`);
    assert.ok(r.sleptAt > 0 && r.sleptAt < 6, `${JSON.stringify(o)}: asleep at ${r.sleptAt} s`);
    assert.ok(r.pelvisZ[r.pelvisZ.length - 1]! < 0.3, `${JSON.stringify(o)}: lies on the ground`);
  }
});

test('ragdolls: a body shot crumples over a good half second; a head shot drops it', () => {
  const body = death({ collapse: 0.6 });
  // still mostly up after 0.2 s, on the ground by 1.5 s
  assert.ok(body.headZ[11]! > 1.0, `head at ${body.headZ[11]} m after 0.2 s`);
  assert.ok(body.headZ[89]! < 0.35, `head at ${body.headZ[89]} m after 1.5 s`);
  // never faster than falling (no whip into the ground)
  for (let i = 1; i < 90; i++) assert.ok(body.headZ[i - 1]! - body.headZ[i]! < 8 * DT, `the head drops ${((body.headZ[i - 1]! - body.headZ[i]!) / DT).toFixed(1)} m/s at ${(i * DT).toFixed(2)} s`);
  const head = death({ collapse: 0.15, z: 1.6 });
  assert.ok(head.headZ[24]! < body.headZ[24]! - 0.15, `a head shot drops the body sooner (head at ${head.headZ[24]!.toFixed(2)} vs ${body.headZ[24]!.toFixed(2)} m after 0.4 s)`);
});

test('ragdolls: bodies fall the way the killing shot pushes them', () => {
  const front = death({ dir: [0, -1, 0] });
  const back = death({ dir: [0, 1, 0] });
  // facing +y: shot from the front they go down backwards, from behind forwards
  assert.ok(front.moved[1] < -0.1, `shot from the front, the pelvis moved ${front.moved[1].toFixed(2)} m`);
  assert.ok(back.moved[1] > 0.1, `shot from behind, the pelvis moved ${back.moved[1].toFixed(2)} m`);
});
