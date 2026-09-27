import assert from 'node:assert/strict';
import { test } from 'node:test';
import { FlatGround, H, HumanoidAnimator, humanoidSkeleton, makeRifle, vdist, type V3 } from '../src/index.ts';

function walk(speed: number, seconds: number, opts: { crouch?: number; rifle?: boolean; aim?: boolean } = {}) {
  const sk = humanoidSkeleton();
  const an = new HumanoidAnimator(sk, new FlatGround(0), 7);
  if (opts.rifle) an.weapon = makeRifle();
  an.input.crouch = opts.crouch ?? 0;
  if (opts.aim) {
    an.input.carry = 'aim';
    an.input.aimAt = [20, 5, 1.4];
  }
  an.place([0, 0, 0], 0);
  const dt = 1 / 60;
  let maxSlide = 0;
  let maxIkErr = 0;
  let maxSwingErr = 0;
  let planted = 0;
  const last: (V3 | null)[] = [null, null];
  let x = 0;
  for (let i = 0; i < seconds * 60; i++) {
    const t = (i + 1) * dt;
    x += speed * Math.min(1, t / 0.4) * dt; // hosts accelerate over a few tenths of a second
    an.setRoot([x, 0, 0], 0);
    an.update(dt);
    const feet = an.footState();
    feet.forEach((f, k) => {
      if (f.planted) {
        planted++;
        if (last[k]) maxSlide = Math.max(maxSlide, vdist(last[k]!, f.pos));
        last[k] = f.pos;
      } else last[k] = null;
      const ankle = an.world.p[k === 0 ? H.footL : H.footR]!;
      // a planted foot must be reached exactly; a swinging one may pass a little short
      if (f.planted) maxIkErr = Math.max(maxIkErr, vdist(ankle, f.ankle));
      else maxSwingErr = Math.max(maxSwingErr, vdist(ankle, f.ankle));
    });
    for (const p of an.world.p) for (const c of p) assert.ok(Number.isFinite(c), 'finite pose');
  }
  return { an, maxSlide, maxIkErr, maxSwingErr, planted };
}

test('walking plants feet without sliding and the legs reach them', () => {
  const r = walk(1.4, 4);
  assert.ok(r.maxSlide < 1e-9, `planted feet slide ${r.maxSlide}`);
  assert.ok(r.maxIkErr < 0.03, `planted ankle off target by ${r.maxIkErr}`);
  assert.ok(r.maxSwingErr < 0.1, `swinging ankle off target by ${r.maxSwingErr}`);
  assert.ok(r.planted > 200);
  // the character got where the root went and the pelvis is at a plausible height
  const pelvis = r.an.world.p[H.pelvis]!;
  assert.ok(Math.abs(pelvis[0] - (1.4 * 4 - 0.28)) < 0.3, `pelvis x ${pelvis[0]}`);
  assert.ok(pelvis[2] > 0.85 && pelvis[2] < 1.0, `pelvis z ${pelvis[2]}`);
});

test('running and crouch-walking stay within reach', () => {
  const run = walk(4.5, 3);
  assert.ok(run.maxSlide < 1e-9);
  assert.ok(run.maxIkErr < 0.03, `run: planted ankle error ${run.maxIkErr}`);
  assert.ok(run.maxSwingErr < 0.15, `run: swing ankle error ${run.maxSwingErr}`);
  const cr = walk(1.0, 3, { crouch: 1 });
  assert.ok(cr.maxIkErr < 0.02, `crouch ankle error ${cr.maxIkErr}`);
  const pelvis = cr.an.world.p[H.pelvis]!;
  assert.ok(pelvis[2] < 0.72, `crouched pelvis ${pelvis[2]}`);
});

test('a shouldered rifle points at the target and both hands hold it', () => {
  const r = walk(0, 2, { rifle: true, aim: true });
  const an = r.an;
  const rifle = an.weapon!;
  const muzzle = an.propPoint(rifle.muzzle);
  const stock = an.propPoint(rifle.stock);
  const dir = [muzzle[0] - stock[0], muzzle[1] - stock[1], muzzle[2] - stock[2]];
  const l = Math.hypot(dir[0]!, dir[1]!, dir[2]!);
  const toT = [20 - stock[0], 5 - stock[1], 1.4 - stock[2]];
  const lt = Math.hypot(toT[0]!, toT[1]!, toT[2]!);
  const cos = (dir[0]! * toT[0]! + dir[1]! * toT[1]! + dir[2]! * toT[2]!) / (l * lt);
  assert.ok(cos > 0.995, `rifle alignment cos ${cos}`);
  const grip = an.propPoint(rifle.grip);
  const handR = an.world.pointOf(H.handR, an.skeleton.restHead[H.handR]!);
  assert.ok(vdist(handR, grip) < 0.12, `right wrist ${vdist(handR, grip)} from grip`);
});
