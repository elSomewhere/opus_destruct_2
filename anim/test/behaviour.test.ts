import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  ACTIONS,
  ActionPlayer,
  Brawler,
  Character,
  FlatGround,
  H,
  HumanoidAnimator,
  NEUTRAL_STYLE,
  Track,
  actionDef,
  humanoidSkeleton,
  makeCivilian,
  makeKnife,
  makeLmg,
  makePistol,
  makeRifle,
  makeSmg,
  mirrorAction,
  qrotate,
  randomStyle,
  vdist,
  type ActionDef,
  type AnimEvent,
  type Prop,
  type GroundVariant,
  type HitKind,
  type V3,
} from '../src/index.ts';

const DT = 1 / 60;

/** An animator at the origin facing +y (yaw pi/2: model space = world space). */
function standing(yaw = Math.PI / 2, seed = 5): HumanoidAnimator {
  const a = new HumanoidAnimator(humanoidSkeleton(), new FlatGround(0), seed);
  a.place([0, 0, 0], yaw);
  return a;
}

/** Steps the animator in place for `seconds`, calling `each` after every frame. */
function run(a: HumanoidAnimator, seconds: number, each?: (a: HumanoidAnimator) => void): void {
  for (let i = 0; i < Math.round(seconds * 60); i++) {
    a.setRoot(a.rootPos, a.rootYaw);
    a.update(DT);
    each?.(a);
  }
}

function assertFinite(a: HumanoidAnimator, what: string): void {
  for (const p of a.world.p) for (const c of p) assert.ok(Number.isFinite(c), `${what}: non-finite bone position`);
  for (const q of a.world.q) for (const c of q) assert.ok(Number.isFinite(c), `${what}: non-finite bone rotation`);
}

// ---- 1. curves --------------------------------------------------------------------------------

test('curve: smooth keys pass through their values and clamp outside the range', () => {
  const tr = new Track([{ t: 0, v: 1 }, { t: 1, v: 3 }, { t: 2, v: -2 }, { t: 3, v: 0 }]);
  for (const [t, v] of [[0, 1], [1, 3], [2, -2], [3, 0]] as const) assert.ok(Math.abs(tr.sample(t)[0]! - v) < 1e-9, `t=${t}`);
  assert.equal(tr.sample(-5)[0], 1);
  assert.equal(tr.sample(10)[0], 0);
  assert.equal(tr.start, 0);
  assert.equal(tr.end, 3);
  // between keys the value stays between neighbours (roughly: cubic may overshoot a little)
  const mid = tr.sample(0.5)[0]!;
  assert.ok(mid > 1 && mid < 3.2, `mid ${mid}`);
});

test('curve: hold steps, snap reaches most of the change early, linear is linear', () => {
  const hold = new Track([{ t: 0, v: 0 }, { t: 1, v: 10, e: 'hold' }]);
  assert.equal(hold.sample(0.99)[0], 0);
  assert.equal(hold.sample(1)[0], 10);
  const snap = new Track([{ t: 0, v: 0 }, { t: 1, v: 1, e: 'snap' }]);
  assert.ok(snap.sample(0.33)[0]! > 0.75, `snap at 1/3: ${snap.sample(0.33)[0]}`);
  const lin = new Track([{ t: 0, v: 0 }, { t: 2, v: 4, e: 'linear' }]);
  assert.ok(Math.abs(lin.sample(0.5)[0]! - 1) < 1e-9);
  const inn = new Track([{ t: 0, v: 0 }, { t: 1, v: 1, e: 'in' }]);
  const out = new Track([{ t: 0, v: 0 }, { t: 1, v: 1, e: 'out' }]);
  assert.ok(inn.sample(0.5)[0]! < 0.5 && out.sample(0.5)[0]! > 0.5);
});

test('curve: vector tracks sample every component', () => {
  const tr = new Track([{ t: 0, v: [0, 1, 2] }, { t: 1, v: [4, 5, 6] }]);
  assert.equal(tr.dim, 3);
  const a = tr.sample(0);
  const b = tr.sample(1);
  assert.deepEqual(a, [0, 1, 2]);
  assert.deepEqual(b, [4, 5, 6]);
  const m = tr.sample(0.5);
  for (let i = 0; i < 3; i++) assert.ok(Math.abs(m[i]! - (i + 2)) < 1e-9, `component ${i}: ${m[i]}`);
});

// ---- 2. actions -------------------------------------------------------------------------------

test('actions: mirroring swaps sides and mirrors positions and rotations', () => {
  const def: ActionDef = {
    name: 'x',
    duration: 1,
    ch: {
      handR: [{ t: 0, v: [0.2, 0.3, 0.4] }],
      handRrot: [{ t: 0, v: [10, 20, 30] }],
      strikeR: [{ t: 0, v: 1 }],
      chest: [{ t: 0, v: [5, 6, 7] }],
    },
    events: [{ t: 0.5, name: 'strike', limb: 'handR' }],
  };
  const m = mirrorAction(def);
  assert.equal(m.name, 'x.m');
  assert.deepEqual(m.ch.handL?.[0]?.v, [-0.2, 0.3, 0.4]);
  assert.deepEqual(m.ch.handLrot?.[0]?.v, [10, -20, -30]);
  assert.equal(m.ch.strikeL?.[0]?.v, 1);
  assert.deepEqual(m.ch.chest?.[0]?.v, [5, -6, -7]);
  assert.equal(m.ch.handR, undefined);
  assert.equal(m.events?.[0]?.limb, 'handL');
  // the library carries mirrored versions of everything
  assert.ok(ACTIONS.has('jab') && ACTIONS.has('jab.m'));
  assert.ok(actionDef('cross.m').ch.strikeL);
});

test('actions: players fade in and out; one-shot events fire once, loop events every cycle', () => {
  const one = new ActionPlayer(actionDef('jab'));
  let strikes = 0;
  const weights: number[] = [];
  for (let t = 0; t < 1; t += 1 / 60) {
    strikes += one.advance(1 / 60).filter((e) => e.name === 'strike').length;
    weights.push(one.weight);
  }
  assert.equal(strikes, 1);
  assert.ok(one.done);
  assert.ok(weights[0]! < 1, 'fades in');
  assert.ok(Math.max(...weights) > 0.99, 'reaches full weight');
  assert.ok(weights[weights.length - 1]! < 0.01, 'fades out');

  const loopDef: ActionDef = { name: 'l', duration: 0.5, loop: true, ch: { head: [{ t: 0, v: [0, 0, 0] }] }, events: [{ t: 0.25, name: 'beat' }] };
  const loop = new ActionPlayer(loopDef);
  let beats = 0;
  for (let t = 0; t < 2.01; t += 1 / 60) beats += loop.advance(1 / 60).length;
  assert.equal(beats, 4);
  assert.ok(!loop.done);
  loop.stop();
  for (let i = 0; i < 30; i++) loop.advance(1 / 60);
  assert.ok(loop.done, 'stopped loop finishes after its fade');
});

test('actions: every action samples finite values on all channels over its duration', () => {
  for (const [name, def] of ACTIONS) {
    const p = new ActionPlayer(def);
    const frame = {};
    for (let t = 0; t <= def.duration + 1e-9; t += def.duration / 24) {
      p.time = t;
      p.sample(frame);
      for (const [c, v] of Object.entries(frame) as [string, number[]][]) for (const x of v) assert.ok(Number.isFinite(x), `${name}.${c} at ${t}`);
    }
    assert.ok(Object.keys(frame).length > 0, `${name} has channels`);
  }
});

// ---- 3. stances -------------------------------------------------------------------------------

test('stances: kneel, prone, sit and ground are reached with sensible heights, then back to standing', () => {
  const cases: { stance: 'kneel' | 'prone' | 'sit' | 'ground'; check: (a: HumanoidAnimator) => void }[] = [
    {
      stance: 'kneel',
      check: (a) => {
        const pz = a.world.p[H.pelvis]![2];
        assert.ok(pz > 0.5 && pz < 0.65, `kneel pelvis z ${pz}`);
        assert.ok(a.world.p[H.footL]![2] < 0.2, `kneel: the left (front) foot is on the ground (ankle z ${a.world.p[H.footL]![2]})`);
      },
    },
    {
      stance: 'prone',
      check: (a) => {
        assert.ok(a.world.p[H.pelvis]![2] < 0.3, `prone pelvis z ${a.world.p[H.pelvis]![2]}`);
        assert.ok(a.world.p[H.head]![2] < 0.6, `prone head z ${a.world.p[H.head]![2]}`);
      },
    },
    {
      stance: 'sit',
      check: (a) => {
        const p = a.world.p[H.pelvis]!;
        assert.ok(p[2] > 0.5 && p[2] < 0.65, `sit pelvis z ${p[2]}`);
        assert.ok(p[1] < -0.2, `the pelvis sits on the seat behind the root (y ${p[1]})`);
        for (const f of [H.footL, H.footR]) assert.ok(a.world.p[f]![2] < 0.2, `sitting: feet on the ground (ankle z ${a.world.p[f]![2]})`);
      },
    },
    { stance: 'ground', check: (a) => assert.ok(a.world.p[H.pelvis]![2] < 0.3, `ground pelvis z ${a.world.p[H.pelvis]![2]}`) },
  ];
  for (const c of cases) {
    const a = standing();
    a.input.stance = c.stance;
    if (c.stance === 'sit') a.input.seat = { pos: [0, -0.38, 0.46], backrest: true };
    run(a, 3);
    assert.equal(a.stance, c.stance, `reached ${c.stance}`);
    assertFinite(a, c.stance);
    c.check(a);
    a.input.stance = 'stand';
    run(a, 3);
    assert.equal(a.stance, 'stand', `${c.stance} -> stand`);
    assert.ok(a.world.p[H.pelvis]![2] > 0.85, `standing again after ${c.stance}: pelvis z ${a.world.p[H.pelvis]![2]}`);
    assertFinite(a, `${c.stance} -> stand`);
  }
});

test('stances: every way of sitting on the ground keeps the pelvis low', () => {
  for (const v of ['cross', 'kneesUp', 'legsOut'] as GroundVariant[]) {
    const a = standing();
    a.input.stance = 'ground';
    a.input.groundVariant = v;
    run(a, 3);
    assert.equal(a.stance, 'ground');
    assert.ok(a.world.p[H.pelvis]![2] < 0.3, `${v}: pelvis z ${a.world.p[H.pelvis]![2]}`);
    assertFinite(a, v);
  }
});

// ---- 4. knockdown -----------------------------------------------------------------------------

test('knockdown: the body goes down, lies a while, and gets back up', () => {
  const a = standing();
  run(a, 0.5);
  a.knockDown(true);
  assert.ok(a.knockedDown);
  let downAt = -1;
  let t = 0;
  run(a, 7, (b) => {
    t += DT;
    if (downAt < 0 && b.stance === 'down') downAt = t;
  });
  assert.ok(downAt > 0 && downAt <= 1.05, `down after ${downAt} s`);
  assert.equal(a.stance, 'stand');
  assert.ok(!a.knockedDown, 'up again');
  assert.ok(a.world.p[H.pelvis]![2] > 0.85);
  assertFinite(a, 'knockdown');
});

// ---- 5. reactions -----------------------------------------------------------------------------

interface Trace {
  chestFwdZ0: number;
  minChestFwdZ: number;
  /** Largest displacement of the neck along the shot direction. */
  neckAlongShot: number;
  pelvisDrop: number;
  knockback: V3;
  a: HumanoidAnimator;
}

function shootBody(bone: number, off: V3, dir: V3, force: number, kind: HitKind, yaw = Math.PI / 2): Trace {
  const a = standing(yaw);
  run(a, 0.5);
  const chestFwdZ0 = qrotate(a.world.q[H.chest]!, [0, 1, 0])[2];
  const neck0 = [...a.world.p[H.neck]!];
  const pz0 = a.world.p[H.pelvis]![2];
  const p = a.world.p[bone]!;
  a.hitAt({ point: [p[0] + off[0], p[1] + off[1], p[2] + off[2]], dir, force, kind, bone });
  const knockback: V3 = [a.reactions.knockback[0], a.reactions.knockback[1], a.reactions.knockback[2]];
  let minChestFwdZ = Infinity, neckAlongShot = 0, minPz = Infinity;
  run(a, 0.6, (b) => {
    minChestFwdZ = Math.min(minChestFwdZ, qrotate(b.world.q[H.chest]!, [0, 1, 0])[2]);
    const n = b.world.p[H.neck]!;
    neckAlongShot = Math.max(neckAlongShot, (n[0] - neck0[0]!) * dir[0] + (n[1] - neck0[1]!) * dir[1] + (n[2] - neck0[2]!) * dir[2]);
    minPz = Math.min(minPz, b.world.p[H.pelvis]![2]);
    assertFinite(b, 'reaction');
  });
  return { chestFwdZ0, minChestFwdZ, neckAlongShot, pelvisDrop: pz0 - minPz, knockback, a };
}

test('reactions: a round in the chest rocks the trunk back along the shot', () => {
  const r = shootBody(H.chest, [0, 0.12, 0.1], [0, -1, 0], 1, 'bullet');
  assert.ok(r.neckAlongShot > 0.05, `the upper body gives along the shot (${r.neckAlongShot} m)`);
  // ...but one round rocks it, it does not fold it in half
  assert.ok(r.neckAlongShot < 0.25, `the neck moves ${r.neckAlongShot} m`);
  assert.ok(!r.a.knockedDown, 'one round does not knock a body down');
});

test('reactions: a round in the chest tips the trunk back 10-35 degrees, not more', () => {
  const a = standing();
  run(a, 0.5);
  const up0 = qrotate(a.world.q[H.chest]!, [0, 0, 1]);
  const p = a.world.p[H.chest]!;
  a.hitAt({ point: [p[0], p[1] + 0.12, p[2] + 0.1], dir: [0, -1, 0], force: 1, kind: 'bullet', bone: H.chest });
  let peak = 0;
  run(a, 1, (b) => {
    const up = qrotate(b.world.q[H.chest]!, [0, 0, 1]);
    peak = Math.max(peak, Math.acos(Math.min(1, up[0] * up0[0] + up[1] * up0[1] + up[2] * up0[2])));
  });
  const deg = (peak * 180) / Math.PI;
  assert.ok(deg > 10 && deg < 35, `chest tipped ${deg.toFixed(1)} deg`);
});

test('reactions: a gut hit folds the trunk forward', () => {
  const r = shootBody(H.spine, [0, 0.12, 0.05], [0, -1, 0], 1, 'bullet');
  assert.ok(r.minChestFwdZ < r.chestFwdZ0 - 0.1, `chest forward z ${r.chestFwdZ0} -> ${r.minChestFwdZ}`);
});

test('reactions: a leg hit buckles the leg and leaves a limp on that side', () => {
  const r = shootBody(H.thighL, [0, 0.08, -0.15], [0, -1, 0], 1, 'bullet');
  assert.ok(r.pelvisDrop > 0.05, `pelvis drop ${r.pelvisDrop}`);
  assert.ok(r.a.reactions.limp[0] > 0, 'left leg limps');
  assert.equal(r.a.reactions.limp[1], 0, 'right leg does not');
});

test('reactions: knockback follows the shot in world space, whatever the facing', () => {
  for (const [yaw, dir] of [
    [Math.PI / 2, [0, -1, 0]],
    [0, [-1, 0, 0]],
    [2.3, [0.6, 0.8, 0]],
  ] as [number, V3][]) {
    const a = standing(yaw);
    run(a, 0.3);
    const c = a.world.p[H.chest]!;
    a.hitAt({ point: [c[0] - dir[0] * 0.12, c[1] - dir[1] * 0.12, c[2]], dir, force: 1, kind: 'bullet', bone: H.chest });
    const kb = a.reactions.knockback;
    const along = kb[0] * dir[0] + kb[1] * dir[1];
    const l = Math.hypot(kb[0], kb[1]);
    assert.ok(l > 0 && along / l > 0.95, `yaw ${yaw}: knockback ${kb.map((v) => v.toFixed(2))} vs shot ${dir}`);
    const step = a.takeKnockback(0.1);
    assert.ok(step[0] * dir[0] + step[1] * dir[1] > 0, 'the host moves the root along the shot');
  }
});

test('reactions: a hard blow to the head knocks the body down', () => {
  const r = shootBody(H.head, [0, 0.1, 0.1], [0, -1, 0], 2.3, 'blunt');
  assert.ok(r.a.knockedDown, 'knocked down');
  const soft = shootBody(H.head, [0, 0.1, 0.1], [0, -1, 0], 0.7, 'blunt');
  assert.ok(!soft.a.knockedDown, 'a jab does not knock down');
});

// ---- 6. weapons -------------------------------------------------------------------------------

const FAR: V3 = [1.5, 10, 1.4];

/** Cosine between the held prop's barrel (+y) and the direction from its muzzle to `t`. */
function barrelCos(a: HumanoidAnimator, t: V3): number {
  const ax = qrotate(a.weaponRot, [0, 1, 0]);
  const m = a.propPoint(a.weapon!.muzzle);
  const d = [t[0] - m[0], t[1] - m[1], t[2] - m[2]];
  const l = Math.hypot(d[0]!, d[1]!, d[2]!);
  return (ax[0] * d[0]! + ax[1] * d[1]! + ax[2] * d[2]!) / l;
}

function armed(prop: Prop, carry: 'aim' | 'hip' | 'ready' | 'relaxed'): HumanoidAnimator {
  const a = standing();
  a.weapon = prop;
  a.input.carry = carry;
  a.input.aimAt = FAR;
  run(a, 2);
  return a;
}

test('weapons: every prop has voxels (small ones on a finer lattice) and drops as a gib', () => {
  for (const make of [makeRifle, makeSmg, makeLmg, makePistol, makeKnife]) {
    const prop = make();
    const part = prop.model.parts[0];
    assert.ok(part && part.count >= 40, `${prop.kind}: ${part?.count ?? 0} voxels`);
    const c = new Character({ model: makeCivilian(1).model, palette: makeCivilian(1).palette, collision: new FlatGround(0), weapon: prop });
    c.place([0, 0, 0], 0);
    c.update(DT);
    const g = c.dropWeapon();
    assert.ok(g && g.part === part && g.voxelSize === prop.model.voxelSize, `${prop.kind} drops`);
  }
});

test('weapons: an aimed pistol points at the target with both hands on the grip', () => {
  const a = armed(makePistol(), 'aim');
  assert.ok(barrelCos(a, FAR) > 0.99, `barrel cos ${barrelCos(a, FAR)}`);
  const grip = a.propPoint(a.weapon!.grip);
  assert.ok(vdist(a.world.p[H.handR]!, grip) < 0.15, 'right wrist at the grip');
  assert.ok(vdist(a.world.p[H.handL]!, grip) < 0.15, 'left wrist wraps the gun hand');
});

test('weapons: a pistol fired one-handed is held by the right hand only', () => {
  const a = armed(makePistol(), 'hip');
  assert.ok(barrelCos(a, FAR) > 0.99, `barrel cos ${barrelCos(a, FAR)}`);
  const grip = a.propPoint(a.weapon!.grip);
  assert.ok(vdist(a.world.p[H.handR]!, grip) < 0.15, 'right hand holds it');
  assert.ok(vdist(a.world.p[H.handL]!, grip) > 0.3, 'the left hand is elsewhere');
});

test('weapons: a machine gun fired from the hip points roughly at the target', () => {
  const a = armed(makeLmg(), 'hip');
  assert.ok(barrelCos(a, FAR) > 0.95, `barrel cos ${barrelCos(a, FAR)}`);
  assert.ok(vdist(a.world.p[H.handR]!, a.propPoint(a.weapon!.grip)) < 0.15);
  const sk = a.skeleton;
  const h = sk.restHead[H.handL]!, t = sk.restTail[H.handL]!;
  const palm = a.world.pointOf(H.handL, [h[0] + (t[0] - h[0]) * 0.42, h[1] + (t[1] - h[1]) * 0.42, h[2] + (t[2] - h[2]) * 0.42]);
  assert.ok(vdist(palm, a.propPoint(a.weapon!.support)) < 0.05, `support palm ${vdist(palm, a.propPoint(a.weapon!.support))} m from the handguard`);
});

test('weapons: a knife stays in the right hand while walking', () => {
  const a = standing();
  a.weapon = makeKnife();
  run(a, 0.5);
  const sk = a.skeleton;
  const h = sk.restHead[H.handR]!, t = sk.restTail[H.handR]!;
  const palmRest: V3 = [h[0] + (t[0] - h[0]) * 0.42, h[1] + (t[1] - h[1]) * 0.42, h[2] + (t[2] - h[2]) * 0.42];
  let y = 0;
  let worst = 0;
  for (let i = 0; i < 120; i++) {
    y += 1.3 * DT;
    a.setRoot([0, y, 0], Math.PI / 2);
    a.update(DT);
    worst = Math.max(worst, vdist(a.propPoint(a.weapon.grip), a.world.pointOf(H.handR, palmRest)));
  }
  assert.ok(worst < 0.1, `knife grip ${worst} m from the palm`);
});

// ---- 7. actions on the body -------------------------------------------------------------------

/** Plays a strike from the guard and returns its strike event and the support foot's slide. */
function strike(name: string, target: V3): { ev: AnimEvent | null; slide: number } {
  const a = standing();
  a.input.guard = true;
  run(a, 1);
  a.takeEvents();
  const left0 = a.footState()[0]!;
  assert.ok(a.play(name, target));
  let ev: AnimEvent | null = null;
  let slide = 0;
  run(a, 1.2, (b) => {
    const l = b.footState()[0]!;
    if (l.planted && left0.planted) slide = Math.max(slide, vdist(l.pos, left0.pos));
    for (const e of b.takeEvents()) if (e.name === 'strike') ev = e;
    assertFinite(b, name);
  });
  return { ev, slide };
}

test('strikes: a jab lands on a target within arm reach', () => {
  const t: V3 = [0, 0.75, 1.55];
  const { ev } = strike('jab', t);
  assert.ok(ev, 'strike event');
  assert.equal(ev!.limb, 'handL');
  assert.ok(vdist(ev!.pos, t) < 0.12, `fist ${vdist(ev!.pos, t)} m from the target`);
  assert.deepEqual(ev!.target, t);
});

// Brawler fights at ~0.9-1 m between roots: strikes step in to targets out of reach.
test('strikes: a jab steps in to reach a head 0.95 m away', () => {
  const t: V3 = [0, 0.95, 1.6];
  const { ev } = strike('jab', t);
  assert.ok(ev && vdist(ev.pos, t) < 0.12, `fist ${ev ? vdist(ev.pos, t) : 'none'} m from the target`);
});

test('strikes: a front kick reaches the belly and the support foot stays planted', () => {
  const t: V3 = [0, 0.85, 1.0];
  const { ev, slide } = strike('frontKick', t);
  assert.ok(ev && ev.limb === 'footR', 'kick event from the right foot');
  assert.ok(vdist(ev!.pos, t) < 0.2, `foot ${vdist(ev!.pos, t)} m from the target`);
  assert.ok(slide < 1e-6, `support foot slid ${slide} m`);
});

test('strikes: a front kick drives the hips in to reach a belly 1.1 m away', () => {
  const t: V3 = [0, 1.1, 1.0];
  const { ev } = strike('frontKick', t);
  assert.ok(ev && vdist(ev.pos, t) < 0.2, `foot ${ev ? vdist(ev.pos, t) : 'none'} m from the target`);
});

test('actions: a rifle reload ends with a reloaded event', () => {
  const a = armed(makeRifle(), 'aim');
  a.takeEvents();
  assert.ok(a.play('reloadRifle'));
  assert.ok(a.busy);
  const names: string[] = [];
  run(a, 3, (b) => names.push(...b.takeEvents().map((e) => e.name)));
  assert.ok(names.includes('reloaded'), `events: ${names}`);
  assert.ok(!a.busy);
  assert.ok(barrelCos(a, FAR) > 0.99, 'back on target after the reload');
});

test('actions: a speaker gestures; an idle character takes postures and fidgets', () => {
  const talker = standing();
  talker.input.talk = 'speak';
  const said = new Set<string>();
  run(talker, 6, (b) => {
    if (b.actionName) said.add(b.actionName);
  });
  assert.ok(said.size > 0, 'gestures while speaking');

  const idle = standing();
  const hand0: V3 = [...idle.world.p[H.handR]!];
  const done = new Set<string>();
  let moved = 0;
  run(idle, 25, (b) => {
    if (b.actionName) done.add(b.actionName);
    moved = Math.max(moved, vdist(b.world.p[H.handR]!, hand0));
  });
  assert.ok(done.size > 0 || moved > 0.15, `idle: actions ${[...done]}, hand moved ${moved}`);
  assertFinite(idle, 'idle');
});

test('gait: the feet alternate (half a cycle apart) walking and running, from a standstill and turning', () => {
  for (const [speed, turn, styleSeed] of [[1.4, 0, 0], [2.2, 0, 0], [3.5, 0, 3], [5, 0, 0], [6, 0, 4], [4.5, 1.2, 0]] as const) {
    const a = standing(Math.PI / 2);
    if (styleSeed) a.style = randomStyle(styleSeed, 'soldier');
    run(a, 0.5);
    let x = 0, y = 0, yaw = Math.PI / 2, t = 0;
    const prev = [true, true];
    const lands: [number[], number[]] = [[], []];
    for (let i = 0; i < 60 * 7; i++) {
      t += DT;
      const v = Math.min(speed, t * 4);
      yaw += turn * DT;
      x += Math.cos(yaw) * v * DT;
      y += Math.sin(yaw) * v * DT;
      a.setRoot([x, y, 0], yaw);
      a.update(DT);
      a.footState().forEach((f, k) => {
        if (f.planted && !prev[k] && t > 3) lands[k]!.push(t);
        prev[k] = f.planted;
      });
    }
    const [L, R] = lands;
    const cycle = (L[L.length - 1]! - L[0]!) / (L.length - 1);
    // one landing per foot per gait cycle, the other foot half a cycle later
    assert.ok(Math.abs(cycle * a.gait.freq - 1) < 0.1, `${speed} m/s: cycle ${cycle.toFixed(2)} s vs gait ${(1 / a.gait.freq).toFixed(2)} s`);
    for (const l of L) {
      const r = R.find((v) => v > l);
      if (r === undefined) continue;
      const off = ((r - l) / cycle) % 1;
      assert.ok(off > 0.4 && off < 0.6, `${speed} m/s turning ${turn}: right lands ${off.toFixed(2)} of a cycle after left`);
    }
  }
});

test('gait: walking keeps the trunk upright (a peek lean, a body turning to its target)', () => {
  for (const lean of [0, 1]) {
    const a = standing(0);
    a.style = randomStyle(2, 'soldier');
    a.weapon = makeRifle();
    a.input.carry = 'aim';
    a.input.lean = lean;
    run(a, 0.5);
    let x = 0, yaw = 0, worst = 0;
    for (let i = 0; i < 60 * 5; i++) {
      x += 1.5 * DT;
      // the body swings round to a target off to the side while walking straight on
      yaw = Math.min(1.2, yaw + 0.6 * DT);
      a.input.aimAt = [x + Math.cos(yaw) * 20, Math.sin(yaw) * 20, 1.4];
      a.setRoot([x, 0, 0], yaw);
      a.update(DT);
      if (i < 90) continue;
      const u = qrotate(a.world.q[H.chest]!, [0, 0, 1]);
      worst = Math.max(worst, Math.abs(Math.asin(u[0] * Math.sin(yaw) - u[1] * Math.cos(yaw))));
    }
    assert.ok(worst < 0.07, `lean ${lean}: the chest rolls ${((worst * 180) / Math.PI).toFixed(1)} deg while walking`);
  }
});

test('turning on the spot: the legs point with their feet, the trunk leads within its twist, the feet follow', () => {
  const yawOf = (q: readonly number[]): number => {
    const f = qrotate(q as [number, number, number, number], [0, 1, 0]);
    return Math.atan2(f[1], f[0]);
  };
  const wrap = (x: number): number => Math.atan2(Math.sin(x), Math.cos(x));
  const a = standing(0);
  a.style = randomStyle(3, 'soldier');
  a.weapon = makeRifle();
  a.input.carry = 'aim';
  a.input.aimAt = [20, 0, 1.4];
  run(a, 1.5);
  a.input.aimAt = [0, 20, 1.4];
  let yaw = 0, legWorst = 0, trunkWorst = 0;
  for (let i = 0; i < 60 * 3; i++) {
    yaw = Math.min(Math.PI / 2, yaw + 2.5 * DT);
    a.setRoot([0, 0, 0], yaw);
    a.update(DT);
    const fs = a.footState();
    for (const [k, thigh] of [[0, H.thighL], [1, H.thighR]] as const) {
      const f = fs[k]!;
      if (f.planted) legWorst = Math.max(legWorst, Math.abs(wrap(yawOf(a.world.q[thigh]!) - f.yaw)));
    }
    trunkWorst = Math.max(trunkWorst, Math.abs(wrap(yawOf(a.world.q[H.chest]!) - yawOf(a.world.q[H.pelvis]!))));
  }
  assert.ok(legWorst < 0.45, `a planted leg points ${((legWorst * 180) / Math.PI).toFixed(0)} deg off its foot`);
  assert.ok(trunkWorst < 0.9, `the chest turns ${((trunkWorst * 180) / Math.PI).toFixed(0)} deg off the hips`);
  // the feet came round to the new facing (bladed: a little to the right of it)
  for (const f of a.footState()) assert.ok(Math.abs(wrap(f.yaw - (Math.PI / 2 - 0.42))) < 0.55, `a foot ends at ${((f.yaw * 180) / Math.PI).toFixed(0)} deg`);
});

// ---- 8. styles ---------------------------------------------------------------------------------

test('styles: characters walk differently; soldiers are heavy', () => {
  const walk = (seed: number): { freq: number; bob: number; cycles: number } => {
    const a = standing();
    a.style = randomStyle(seed, 'civilian');
    let y = 0, cycles = 0, last = a.phase;
    for (let i = 0; i < 300; i++) {
      y += 1.4 * DT;
      a.setRoot([0, y, 0], Math.PI / 2);
      a.update(DT);
      const d = a.phase - last;
      cycles += d < 0 ? d + 1 : d;
      last = a.phase;
    }
    return { freq: a.gait.freq, bob: a.gait.bob, cycles };
  };
  const a = walk(1), b = walk(2);
  assert.ok(Math.abs(a.freq - b.freq) > 0.01 || Math.abs(a.bob - b.bob) > 0.002, `same gait: ${JSON.stringify([a, b])}`);
  assert.ok(Math.abs(a.cycles - b.cycles) > 0.05, 'different cadence over 5 s');
  for (let s = 1; s <= 10; s++) assert.ok(randomStyle(s, 'soldier').heavy > NEUTRAL_STYLE.heavy, `soldier ${s} heavy`);
  const mean = (k: 'soldier' | 'civilian'): number => {
    let m = 0;
    for (let s = 1; s <= 20; s++) m += randomStyle(s, k).heavy / 20;
    return m;
  };
  assert.ok(mean('soldier') > mean('civilian'));
});

// ---- 9. brawls ---------------------------------------------------------------------------------

function brawl(knife: boolean): { blows: import('../src/index.ts').LandedBlow[]; a: Character; b: Character } {
  const g = new FlatGround(0);
  const mk = (seed: number, x: number, yaw: number, weapon: Prop | null): Character => {
    const v = makeCivilian(seed);
    const c = new Character({ model: v.model, palette: v.palette, collision: g, weapon, seed });
    c.place([x, 0, 0], yaw);
    return c;
  };
  const a = mk(70, 0, 0, knife ? makeKnife() : null);
  const b = mk(71, 1, Math.PI, null);
  const fighters = [
    { c: a, br: new Brawler(a, { seed: 70, aggression: 0.55, skill: 0.3 }), pos: [0, 0, 0] as V3 },
    { c: b, br: new Brawler(b, { seed: 71, aggression: 0.55, skill: 0.3 }), pos: [1, 0, 0] as V3 },
  ];
  fighters[0]!.br.opponent = b;
  fighters[1]!.br.opponent = a;
  const blows: import('../src/index.ts').LandedBlow[] = [];
  for (let i = 0; i < 8 * 60; i++) {
    for (const f of fighters) {
      f.br.update(DT);
      if (f.c.alive) {
        const an = f.c.animator;
        if (!an.transitioning && !an.knockedDown) {
          f.pos[0] += f.br.move[0] * DT;
          f.pos[1] += f.br.move[1] * DT;
        }
        const kb = an.takeKnockback(DT);
        f.pos[0] += kb[0];
        f.pos[1] += kb[1];
        f.c.setRoot(f.pos, f.br.yaw);
      }
      f.c.update(DT);
      blows.push(...f.br.resolve(f.c.animator.takeEvents()));
      for (const p of f.c.pose.p) for (const x of p) assert.ok(Number.isFinite(x), 'finite fighter pose');
    }
  }
  return { blows, a, b };
}

test('brawl: fists land blows that hurt', () => {
  const { blows, a, b } = brawl(false);
  const landed = blows.filter((x) => x.result.damage > 0);
  assert.ok(landed.length >= 3, `${landed.length} blows landed`);
  assert.ok(blows.every((x) => x.kind === 'blunt'));
  assert.ok(a.health < a.maxHealth && b.health < b.maxHealth, `health ${a.health}, ${b.health}`);
});

test('brawl: a knife cuts voxels out of the opponent', () => {
  const { blows, b } = brawl(true);
  const cuts = blows.filter((x) => x.attacker !== b && x.kind === 'blade');
  assert.ok(cuts.length >= 1, `${cuts.length} cuts`);
  assert.ok(cuts.reduce((n, x) => n + x.result.removed.length, 0) > 0, 'cuts remove voxels');
  assert.ok(b.health < b.maxHealth);
});

// ---- 10. melee on a Character ------------------------------------------------------------------

function civilian(): Character {
  const v = makeCivilian(5);
  const c = new Character({ model: v.model, palette: v.palette, collision: new FlatGround(0) });
  c.place([0, 0, 0], Math.PI / 2);
  for (let i = 0; i < 20; i++) c.update(DT);
  return c;
}

test('melee: a blade cut carves voxels; a punch to the head hurts and is a head hit', () => {
  const c = civilian();
  const v0 = c.geometryVersion;
  const chest = c.pose.p[H.chest]!;
  const cut = c.melee([chest[0], chest[1] + 0.12, chest[2] + 0.05], [0, -1, 0], 'blade', 1);
  assert.ok(cut.removed.length > 0, 'voxels cut out');
  assert.ok(c.geometryVersion > v0, 'geometry changed');
  assert.ok(c.ownsModel);

  const d = civilian();
  const h0 = d.health;
  const head = d.pose.p[H.head]!;
  const punch = d.melee([head[0], head[1] + 0.1, head[2] + 0.1], [0, -1, 0], 'blunt', 1);
  assert.equal(punch.zone, 'head');
  assert.ok(punch.headshot);
  assert.ok(d.health < h0 && punch.damage > 0, `health ${h0} -> ${d.health}`);
  assert.equal(punch.removed.length, 0, 'a punch cuts nothing');
});
