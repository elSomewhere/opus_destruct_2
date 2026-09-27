import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  ARMED_FIDGETS,
  Brawler,
  Character,
  FlatGround,
  H,
  HumanoidAnimator,
  KNIFE_ATTACKS,
  humanoidSkeleton,
  makeCivilian,
  makeKnife,
  makeRifle,
  makeThug,
  randomStyle,
  vdist,
  type AnimEvent,
  type LandedBlow,
  type V3,
} from '../src/index.ts';

const DT = 1 / 60;

function standing(yaw = Math.PI / 2, seed = 5): HumanoidAnimator {
  const a = new HumanoidAnimator(humanoidSkeleton(), new FlatGround(0), seed);
  a.place([0, 0, 0], yaw);
  return a;
}

/** Runs the animator with the host moving the root by the knockback (as games do). */
function live(a: HumanoidAnimator, seconds: number, speed = 0, each?: (a: HumanoidAnimator, t: number) => void): void {
  const pos: V3 = [a.rootPos[0], a.rootPos[1], a.rootPos[2]];
  const f: V3 = [Math.cos(a.rootYaw), Math.sin(a.rootYaw), 0];
  for (let i = 0; i < Math.round(seconds * 60); i++) {
    const kb = a.takeKnockback(DT);
    pos[0] += f[0] * speed * DT + kb[0];
    pos[1] += f[1] * speed * DT + kb[1];
    a.setRoot(pos, a.rootYaw);
    a.update(DT);
    each?.(a, (i + 1) * DT);
    for (const p of a.world.p) for (const c of p) assert.ok(Number.isFinite(c), 'finite pose');
  }
}

// ---- knives ----------------------------------------------------------------------------------

test('knife: a knife fighter holds a knife guard, blade forward', () => {
  const a = standing();
  a.weapon = makeKnife();
  a.input.guard = true;
  live(a, 1);
  assert.equal((a as unknown as { poseAct: { def: { name: string } } | null }).poseAct?.def.name, 'knifeGuard');
  const tip = a.propPoint(a.weapon.muzzle);
  const chest = a.world.p[H.chest]!;
  assert.ok(tip[1] - chest[1] > 0.3, `the blade is ${(tip[1] - chest[1]).toFixed(2)} m in front of the chest`);
});

test('knife: every knife attack reaches a body in front of it', () => {
  for (const name of KNIFE_ATTACKS) {
    const a = standing();
    a.weapon = makeKnife();
    a.input.guard = true;
    live(a, 0.8);
    a.takeEvents();
    const target: V3 = [0, 0.85, name === 'gutStab' ? 1.0 : 1.2];
    assert.ok(a.play(name, target), name);
    let hit: AnimEvent | null = null;
    live(a, 1, 0, (b) => {
      for (const e of b.takeEvents()) if (e.name === 'strike') hit = e;
    });
    assert.ok(hit, `${name}: a strike`);
    const h = hit as AnimEvent;
    assert.equal(h.limb, 'blade');
    assert.ok(vdist(h.pos, target) < 0.3, `${name}: the blade ${vdist(h.pos, target).toFixed(2)} m from the target`);
  }
});

test('knife: a thug with a knife cuts a civilian in a fight, with a variety of attacks', () => {
  const g = new FlatGround(0);
  const t = makeThug(3);
  const v = makeCivilian(8);
  const thug = new Character({ model: t.model, palette: t.palette, collision: g, weapon: makeKnife(), seed: 3 });
  const civ = new Character({ model: v.model, palette: v.palette, collision: g, seed: 8 });
  thug.animator.style = randomStyle(3, 'thug');
  thug.place([0, 0, 0], 0);
  civ.place([1, 0, 0], Math.PI);
  const f = [
    { c: thug, br: new Brawler(thug, { seed: 3, aggression: 0.85, skill: 0.2 }), pos: [0, 0, 0] as V3 },
    { c: civ, br: new Brawler(civ, { seed: 8, aggression: 0.3, skill: 0.3 }), pos: [1, 0, 0] as V3 },
  ];
  f[0]!.br.opponent = civ;
  f[1]!.br.opponent = thug;
  const blows: LandedBlow[] = [];
  const attacks = new Set<string>();
  for (let i = 0; i < 10 * 60; i++) {
    for (const x of f) {
      x.br.update(DT);
      if (x.c.alive) {
        const an = x.c.animator;
        if (!an.transitioning && !an.knockedDown) {
          x.pos[0] += x.br.move[0] * DT;
          x.pos[1] += x.br.move[1] * DT;
        }
        const kb = an.takeKnockback(DT);
        x.pos[0] += kb[0];
        x.pos[1] += kb[1];
        x.c.setRoot(x.pos, x.br.yaw);
      }
      x.c.update(DT);
      if (x.c === thug && thug.animator.actionName) attacks.add(thug.animator.actionName.replace('.m', ''));
      blows.push(...x.br.resolve(x.c.animator.takeEvents()));
    }
  }
  const cuts = blows.filter((b) => b.attacker === thug && b.kind === 'blade');
  assert.ok(cuts.length >= 2, `${cuts.length} cuts`);
  assert.ok(civ.health < civ.maxHealth * 0.8 || !civ.alive, `civilian health ${civ.health}`);
  const used = [...attacks].filter((n) => (KNIFE_ATTACKS as readonly string[]).includes(n));
  assert.ok(used.length >= 2, `knife attacks used: ${used.join(', ')}`);
});

// ---- balance -------------------------------------------------------------------------------------

test('balance: a stumble pushes the body its way and the feet catch it', () => {
  const a = standing();
  live(a, 0.5);
  const x0 = a.rootPos[0];
  a.stumble([1, 0, 0], 1);
  assert.equal(a.actionName, 'stumble');
  live(a, 2.5);
  assert.ok(a.rootPos[0] - x0 > 0.35, `pushed ${(a.rootPos[0] - x0).toFixed(2)} m`);
  assert.ok(!a.knockedDown && a.stance === 'stand', 'still standing');
  assert.ok(a.footState().every((f) => f.planted), 'both feet down again');
  assert.ok(a.world.p[H.pelvis]![2] > 0.85, 'upright');
});

test('balance: a strong push fells the body the way it is pushed, and it gets up', () => {
  const a = standing();
  live(a, 0.5);
  a.stumble([0, -1, 0], 2.2);
  let down = false;
  live(a, 1.2, 0, (b) => {
    if (b.knockedDown) down = true;
  });
  assert.ok(down, 'knocked down');
  live(a, 7);
  assert.ok(!a.knockedDown && a.stance === 'stand', 'back on its feet');
});

test('balance: a trip pitches a walker forward; it catches itself or falls on its front', () => {
  const a = standing();
  live(a, 1, 1.5);
  a.trip(false);
  assert.equal(a.actionName, 'trip');
  live(a, 2, 1.5);
  assert.ok(!a.knockedDown && a.stance === 'stand', 'caught itself');
  const b = standing();
  live(b, 1, 3);
  const y0 = b.rootPos[1];
  b.trip(true);
  let down = false;
  live(b, 1, 0, (x) => {
    if (x.knockedDown) down = true;
  });
  assert.ok(down, 'fell');
  assert.ok(b.rootPos[1] > y0, 'forwards');
});

test('balance: a flinch is quick and does not interrupt an action already running', () => {
  const a = standing();
  live(a, 0.5);
  a.flinch();
  assert.equal(a.actionName, 'flinch');
  live(a, 1);
  assert.ok(!a.busy, 'over within a second');
  a.play('wave');
  a.flinch();
  assert.equal(a.actionName, 'wave');
});

// ---- a soldier's pauses ------------------------------------------------------------------------

test('pauses: an armed body standing easy fidgets (helmet, brow, shoulders, weapon, a look round); aiming, it does not', () => {
  for (const carry of ['ready', 'aim'] as const) {
    const a = standing();
    a.weapon = makeRifle();
    a.input.carry = carry;
    a.input.aimAt = carry === 'aim' ? [0, 20, 1.4] : null;
    const seen = new Set<string>();
    live(a, 40, 0, (b) => {
      if (b.actionName) seen.add(b.actionName.replace('.m', ''));
    });
    const fidgets = [...seen].filter((n) => (ARMED_FIDGETS as readonly string[]).includes(n));
    if (carry === 'ready') assert.ok(fidgets.length >= 2, `fidgets: ${fidgets.join(', ')}`);
    else assert.equal(fidgets.length, 0, `aiming, it fidgeted: ${fidgets.join(', ')}`);
  }
});

test('pauses: a soldier catches a breath with the weapon lowered, and reloads', () => {
  const a = standing();
  a.weapon = makeRifle();
  a.input.carry = 'ready';
  live(a, 1);
  assert.ok(a.play('catchBreath'));
  live(a, 3);
  assert.ok(!a.busy);
  a.takeEvents();
  a.play('reloadRifle');
  const names: string[] = [];
  live(a, 3, 0, (b) => names.push(...b.takeEvents().map((e) => e.name)));
  assert.ok(names.includes('reloaded'));
});

// ---- thugs ---------------------------------------------------------------------------------------

test('thugs: their own looks and a swagger', () => {
  for (let i = 1; i <= 4; i++) {
    const t = makeThug(i);
    assert.ok(t.model.voxelCount > 3000, `thug ${i}: ${t.model.voxelCount} voxels`);
    assert.equal(t.spec.bottom, 'trousers');
  }
  let thug = 0, civ = 0;
  for (let s = 1; s <= 20; s++) {
    thug += randomStyle(s, 'thug').sway + randomStyle(s, 'thug').arms;
    civ += randomStyle(s, 'civilian').sway + randomStyle(s, 'civilian').arms;
  }
  assert.ok(thug > civ, 'more swing in the walk');
});
