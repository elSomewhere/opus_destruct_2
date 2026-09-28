import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Character, FlatGround, H, VoxelCollision, gatherObstacles, makeCivilian, makeRifle, makeSoldier, qrotate, type CollisionWorld, type V3 } from '../src/index.ts';

const DT = 1 / 60;

function civilian(world: CollisionWorld = new FlatGround(0), seed = 3, yaw = Math.PI / 2, at: V3 = [0, 0, 0]): Character {
  const v = makeCivilian(seed);
  const c = new Character({ model: v.model, palette: v.palette, collision: world, seed });
  c.place(at, yaw);
  return c;
}

function soldier(world: CollisionWorld = new FlatGround(0)): Character {
  const v = makeSoldier(4);
  const c = new Character({ model: v.model, palette: v.palette, collision: world, weapon: makeRifle(), seed: 4 });
  c.place([0, 0, 0], Math.PI / 2);
  return c;
}

interface Host {
  pos: V3;
  yaw: number;
  /** The walking speed it has built up. */
  v: number;
}

/**
 * Runs a character as a game would: the host walks it along its facing at `speed` (m/s,
 * accelerating over half a second) and follows the body's root motion while the body leads.
 */
function host(c: Character, seconds: number, speed = 0, each?: (c: Character, t: number, h: Host) => void, h: Host = { pos: [...c.motion.rootPos] as V3, yaw: c.motion.rootYaw, v: 0 }): Host {
  let v = h.v;
  for (let i = 0; i < Math.round(seconds * 60); i++) {
    const rm = c.takeRootMotion();
    if (c.controlled) {
      h.pos[0] += rm[0];
      h.pos[1] += rm[1];
      h.pos[2] += rm[2];
      h.yaw = c.motion.rootYaw;
      v = 0;
    } else {
      v = Math.min(speed, v + (speed / 0.5) * DT);
      h.pos[0] += Math.cos(h.yaw) * v * DT;
      h.pos[1] += Math.sin(h.yaw) * v * DT;
    }
    c.setRoot(h.pos, h.yaw);
    c.update(DT);
    for (const p of c.pose.p) for (const x of p) assert.ok(Number.isFinite(x), 'finite pose');
    h.v = v;
    each?.(c, (i + 1) * DT, h);
  }
  return h;
}

function chestUp(c: Character): V3 {
  return qrotate(c.pose.q[H.chest]!, [0, 0, 1]);
}

// ---- carrying out the plan -----------------------------------------------------------------------

test('tracking: walking and running, the body follows its plan closely and stays on its feet', () => {
  for (const speed of [1.4, 3, 5]) {
    const c = civilian();
    let worst = 0;
    host(c, 4, speed, (x, t) => {
      if (t < 1) return;
      const a = x.pose.p[H.pelvis]!, b = x.motion.world.p[H.pelvis]!;
      worst = Math.max(worst, Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]));
      assert.equal(x.behaviours.mode, 'animated', `${speed} m/s at ${t.toFixed(2)} s: ${x.behaviours.mode}`);
    });
    assert.ok(worst < 0.08, `${speed} m/s: pelvis ${(worst * 100).toFixed(1)} cm off the plan`);
  }
});

test('tracking: a calm body can rest on its plan alone (physics off), and a hit wakes it', () => {
  const c = civilian();
  c.physics = false;
  host(c, 2);
  assert.ok(!c.behaviours.physical, 'resting on the plan');
  const p = c.pose.p[H.pelvis]!, q = c.motion.world.p[H.pelvis]!;
  assert.ok(Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]) < 1e-6, 'the plan is shown');
  const ch = c.pose.p[H.chest]!;
  c.hitAt({ point: [ch[0], ch[1] + 0.1, ch[2]], dir: [0, -1, 0], force: 1, kind: 'bullet', bone: H.chest });
  assert.ok(c.behaviours.physical, 'the hit woke the body');
});

// ---- hits ----------------------------------------------------------------------------------------

test('hits: a round in the chest rocks the trunk back along the shot, 5-35 degrees; one round does not fell', () => {
  const c = soldier();
  host(c, 1);
  const up0 = chestUp(c);
  const ch = c.pose.p[H.chest]!;
  c.hitAt({ point: [ch[0], ch[1] + 0.12, ch[2] + 0.1], dir: [0, -1, 0], force: 1.1, kind: 'bullet', bone: H.chest });
  let peak = 0, back = 0;
  host(c, 1.5, 0, (x) => {
    const u = chestUp(x);
    peak = Math.max(peak, Math.acos(Math.min(1, u[0] * up0[0] + u[1] * up0[1] + u[2] * up0[2])));
    back = Math.min(back, u[1]);
  });
  const deg = (peak * 180) / Math.PI;
  assert.ok(deg > 5 && deg < 35, `chest tipped ${deg.toFixed(1)} deg`);
  assert.ok(back < -0.05, 'back, along the shot');
  assert.ok(!c.down && c.alive, 'still up');
});

test('hits: a gut wound folds the body over it and a hand goes to it; a leg wound leaves a limp on that side', () => {
  const c = civilian();
  host(c, 0.5);
  const sp = c.pose.p[H.spine]!;
  const wound: V3 = [sp[0] + 0.03, sp[1] + 0.12, sp[2] + 0.06];
  c.hitAt({ point: wound, dir: [0, -1, 0], force: 1, kind: 'bullet', bone: H.spine });
  let fold = 0, near = Infinity;
  const f0 = qrotate(c.pose.q[H.chest]!, [0, 1, 0])[2];
  host(c, 2, 0, (x) => {
    fold = Math.max(fold, f0 - qrotate(x.pose.q[H.chest]!, [0, 1, 0])[2]);
    const w = x.pose.p[H.spine]!;
    for (const hb of [H.handL, H.handR]) near = Math.min(near, Math.hypot(x.pose.p[hb]![0] - w[0], x.pose.p[hb]![1] - w[1] - 0.1, x.pose.p[hb]![2] - w[2]));
  });
  assert.ok(fold > 0.08, `the trunk folds ${fold.toFixed(2)}`);
  assert.ok(near < 0.2, `a hand came within ${near.toFixed(2)} m of the wound`);

  const d = civilian();
  host(d, 0.5);
  const th = d.pose.p[H.thighL]!;
  d.hitAt({ point: [th[0], th[1] + 0.08, th[2] - 0.15], dir: [0, -1, 0], force: 1, kind: 'bullet', bone: H.thighL });
  host(d, 3);
  const inj = d.behaviours.injuries;
  assert.ok(inj.legL > 0.2 && inj.legR === 0, `limp: left ${inj.legL.toFixed(2)}, right ${inj.legR}`);
  assert.ok(d.motion.control.limp[0] > 0.2, 'the plan limps on the left');
});

test('hits: a kick shoves the body along the blow: it steps with it and recovers; the host follows the root', () => {
  const c = civilian();
  const h = host(c, 0.5);
  const x0 = h.pos[0];
  const sp = c.pose.p[H.spine]!;
  c.hitAt({ point: [sp[0] - 0.1, sp[1], sp[2]], dir: [1, 0, 0], force: 1.8, kind: 'blunt', bone: H.spine });
  let reacted = false;
  host(c, 4, 0, (x) => (reacted ||= x.behaviours.mode === 'reacting'), h);
  assert.ok(reacted, 'knocked off its plan');
  assert.ok(h.pos[0] - x0 > 0.15, `the host followed ${(h.pos[0] - x0).toFixed(2)} m along the kick`);
  assert.equal(c.behaviours.mode, 'animated', 'back in balance');
  assert.ok(!c.down, 'on its feet');
});

// ---- balance -------------------------------------------------------------------------------------

test('balance: a light shove is taken in place, harder ones take steps, the hardest fell; it gets up', () => {
  const steps: number[] = [];
  let fell = false;
  for (const s of [0.5, 1.2, 1.8]) {
    const c = civilian();
    host(c, 0.5);
    c.push([1, 0, 0], s);
    let most = 0;
    host(c, 3, 0, (x) => {
      most = Math.max(most, x.behaviours.steps);
      if (x.down) fell = true;
    });
    steps.push(most);
    assert.equal(c.behaviours.mode, 'animated', `shove ${s}: recovered`);
  }
  assert.ok(!fell, 'moderate shoves do not fell');
  assert.ok(steps[0]! <= 1, `a light shove takes ${steps[0]} steps`);
  assert.ok(steps[2]! > steps[0]!, `harder shoves take more steps: ${steps.join(', ')}`);
  const c = civilian();
  host(c, 0.5);
  c.push([0, -1, 0], 3.5);
  let down = false, lying = false;
  host(c, 9, 0, (x) => {
    if (x.down) down = true;
    if (x.behaviours.mode === 'lying') lying = true;
  });
  assert.ok(down && lying, 'thrown down');
  assert.equal(c.behaviours.mode, 'animated', 'up again');
  assert.equal(c.motion.stance, 'stand');
  assert.ok(c.pose.p[H.pelvis]![2] > 0.7, 'standing');
});

test('trips: a foot caught mid-stride pitches the body forward; it catches itself or goes down, and carries on', () => {
  for (const speed of [1.5, 4]) {
    const c = civilian();
    const h = host(c, 1.2, speed);
    const y0 = c.pose.p[H.pelvis]![1];
    c.trip();
    let reacted = false;
    host(c, 0.8, speed, (x) => (reacted ||= x.controlled), h);
    assert.ok(reacted, `${speed} m/s: stumbled`);
    assert.ok(c.pose.p[H.pelvis]![1] > y0 + 0.2, `${speed} m/s: carried forward`);
    host(c, 8, 0, undefined, h);
    assert.equal(c.behaviours.mode, 'animated', `${speed} m/s: carries on`);
  }
});

test('trips: running over a beam without lifting the feet catches a foot on it', () => {
  // a 15 cm beam across the way at y = 3
  const h = 0.125;
  const world = new VoxelCollision(h, (_i, j, k) => k < 0 || (k === 0 && j === 24));
  let tripped = 0;
  for (const seed of [1, 2, 3, 5, 8, 13]) {
    const c = civilian(world, seed);
    c.motion.input.mood = 'panic';
    host(c, 2.2, 4.5, (x) => {
      if (x.behaviours.mode === 'reacting' || x.down) tripped++;
    });
  }
  assert.ok(tripped > 0, 'no runner ever caught a foot');
});

// ---- reflexes ------------------------------------------------------------------------------------

test('flinch: a round landing close turns the head away and brings a hand up between the face and it', () => {
  const c = civilian();
  host(c, 1);
  const head0 = [...c.pose.p[H.head]!] as V3;
  const f0 = qrotate(c.pose.q[H.head]!, [0, 1, 0]);
  // just in front, to the right of the face
  const p: V3 = [head0[0] + 0.5, head0[1] + 0.5, head0[2]];
  c.perceive({ point: p, strength: 1, kind: 'impact' });
  let away = 0, hand = Infinity, duck = 0;
  host(c, 0.5, 0, (x) => {
    const f = qrotate(x.pose.q[H.head]!, [0, 1, 0]);
    // the face turns from the danger (+x)
    away = Math.max(away, f0[0] - f[0]);
    duck = Math.max(duck, head0[2] - x.pose.p[H.head]![2]);
    const e = x.eyes();
    for (const hb of [H.handL, H.handR]) hand = Math.min(hand, Math.hypot(x.pose.p[hb]![0] - (e[0] + 0.2), x.pose.p[hb]![1] - (e[1] + 0.2), x.pose.p[hb]![2] - e[2]));
  });
  assert.ok(away > 0.15, `the face turned away (${away.toFixed(2)})`);
  assert.ok(duck > 0.03, `ducked ${duck.toFixed(3)} m`);
  assert.ok(hand < 0.3, `a hand came up to ${hand.toFixed(2)} m from the face's danger side`);
  host(c, 2.5);
  assert.ok(c.pose.p[H.head]![2] > head0[2] - 0.05, 'and straightens up again');
});

test('bracing: shoved towards a wall, a hand goes out to it and holds on', () => {
  // a wall at x = 0.75 (a plane of voxels), the character beside it facing +y
  const h = 0.125;
  const world = new VoxelCollision(h, (i, _j, k) => k < 0 || (i >= 6 && i <= 7 && k < 24));
  const c = civilian(world);
  host(c, 0.6);
  c.push([1, 0, 0], 1.6);
  let held = false, palm = Infinity;
  host(c, 2, 0, (x) => {
    const b = x.behaviours.brace;
    if (b?.holding) held = true;
    for (const hb of [H.handL, H.handR]) palm = Math.min(palm, Math.abs(x.pose.p[hb]![0] - 0.6));
  });
  assert.ok(held, 'a hand held on to the wall');
  assert.ok(palm < 0.2, `a hand at the wall (${palm.toFixed(2)} m)`);
  assert.ok(!c.down, 'kept its feet');
});

test('knockout: a heavy blow drops the body; it stays down, then gets up', () => {
  const c = civilian();
  host(c, 0.5);
  const head = c.pose.p[H.head]!;
  c.health = 5;
  c.melee([head[0], head[1] + 0.1, head[2] + 0.05], [0, -1, 0], 'blunt', 2.3);
  assert.ok(c.knockedOut);
  let lay = 0;
  host(c, 4, 0, (x) => {
    if (x.behaviours.mode === 'lying') lay += DT;
  });
  assert.ok(lay > 1.5, `down ${lay.toFixed(1)} s`);
  host(c, 12);
  assert.equal(c.behaviours.mode, 'animated', 'up again');
  assert.ok(!c.knockedOut);
});

test('down: the plan lies as the body lies (front or back), and a badly hurt body face down crawls away', () => {
  for (const seed of [2, 4]) {
    const c = civilian(new FlatGround(0), seed);
    host(c, 0.3);
    c.addInjury(H.thighL, 0.8);
    c.health = 26;
    const ch = c.pose.p[H.chest]!;
    c.hitAt({ point: [ch[0], ch[1] - 0.1, ch[2]], dir: [0, 1, 0], force: 3, kind: 'blast', bone: H.chest });
    c.push([0, 1, 0], 3);
    c.behaviours.collapse(10);
    let crawled = false;
    const h = host(c, 6, 0, (x) => {
      if (x.behaviours.mode === 'lying') {
        // the pelvis's forward points up on the back, down face down
        const f = qrotate(x.pose.q[H.pelvis]!, [0, 1, 0]);
        if (Math.abs(f[2]) > 0.8) assert.equal(x.motion.lyingOnBack, f[2] > 0, 'the plan lies the way the body does');
        if (!x.motion.lyingOnBack) x.motion.input.stance = 'prone';
      }
      crawled ||= x.behaviours.mode === 'animated' && x.motion.stance === 'prone';
    });
    if (!crawled) continue;
    // crawling: the host moves it slowly, the body stays low and goes with it
    const p0 = [...c.pose.p[H.pelvis]!];
    host(c, 4, 0.35, undefined, h);
    const p1 = c.pose.p[H.pelvis]!;
    const along = (p1[0] - p0[0]!) * Math.cos(h.yaw) + (p1[1] - p0[1]!) * Math.sin(h.yaw);
    assert.ok(c.pose.p[H.head]![2] < 0.45, `crawling low: head at ${c.pose.p[H.head]![2].toFixed(2)}`);
    assert.ok(along > 0.8, `crawled ${along.toFixed(2)} m`);
    return;
  }
  assert.fail('no body face down crawled away');
});

test('dying: the muscles fade, the body goes down within a couple of seconds and comes to rest', () => {
  const c = soldier();
  host(c, 0.5);
  const ch = c.pose.p[H.chest]!;
  c.hitAt({ point: [ch[0], ch[1] + 0.12, ch[2] + 0.1], dir: [0, -1, 0], force: 1.2, kind: 'bullet', bone: H.chest });
  c.die(null, null, 0.8);
  host(c, 2);
  assert.ok(c.pose.p[H.head]![2] < 0.5, `head at ${c.pose.p[H.head]![2].toFixed(2)}`);
  assert.equal(c.behaviours.mode, 'dead');
  host(c, 4);
  assert.ok(c.asleep, 'at rest');
});

// ---- bodies among bodies ---------------------------------------------------------------------------

test('bodies: a body shoved into a bystander knocks into it, and the bystander gives way', () => {
  const a = civilian(new FlatGround(0), 3, Math.PI / 2, [0, 0, 0]);
  const b = civilian(new FlatGround(0), 5, Math.PI / 2, [0.7, 0, 0]);
  const hosts = [a, b].map((c) => ({ pos: [...c.motion.rootPos] as V3, yaw: c.motion.rootYaw, v: 0 }));
  const run = (seconds: number, each?: () => void): void => {
    for (let i = 0; i < seconds * 60; i++) {
      gatherObstacles([a, b]);
      [a, b].forEach((c, k) => {
        const h = hosts[k]!;
        const rm = c.takeRootMotion();
        if (c.controlled) {
          h.pos[0] += rm[0];
          h.pos[1] += rm[1];
        }
        c.setRoot(h.pos, h.yaw);
        c.update(DT);
      });
      each?.();
    }
  };
  run(0.5);
  const bx = b.pose.p[H.pelvis]![0];
  a.push([1, 0, 0], 2.4);
  let bumped = false;
  run(2, () => (bumped ||= b.behaviours.mode !== 'animated' || b.body.parts.some((p) => p.bumped > 0)));
  assert.ok(bumped, 'the bystander was hit');
  assert.ok(b.pose.p[H.pelvis]![0] - bx > 0.05, `the bystander was pushed ${(b.pose.p[H.pelvis]![0] - bx).toFixed(2)} m`);
  // they never pass through each other
  assert.ok(b.pose.p[H.pelvis]![0] - a.pose.p[H.pelvis]![0] > 0.2, 'they stay apart');
});

test('bodies: a runner catches a foot on a body lying across the way', () => {
  let tripped = 0;
  for (const seed of [1, 2, 3]) {
    const dead = civilian(new FlatGround(0), 9, 0, [0.55, 2.5, 0]);
    dead.die(null, null, 0.05);
    for (let i = 0; i < 180; i++) dead.update(DT);
    const c = civilian(new FlatGround(0), seed);
    c.motion.input.mood = 'panic';
    host(c, 2, 4.5, (x) => {
      gatherObstacles([x, dead]);
      if (x.behaviours.mode === 'reacting' || x.down) tripped++;
    });
  }
  assert.ok(tripped > 0, 'nobody caught a foot on the body');
});
