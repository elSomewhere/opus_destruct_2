/**
 * svx_anim lab (lab.html): the animation engine on its own, without the physics engine.
 *
 * A test course with scripted groups (camera bookmarks in the panel):
 * - city life: a conversation, bench and desk sitters, people sitting on the ground, people
 *   waiting (idle postures and fidgets), strollers, a commuter, a jogger and a runner;
 * - soldiers: kneeling fire, prone and crawling, peeking round a corner, a pistol shooter
 *   walking while firing, machine-gun hip fire, reloading, crouch-walking, a patrol, a sprint;
 * - fights: a fist fight and a knife fight (Brawler), with hits landing where they land;
 * - reactions: a line-up to shoot at (click: shoot where you click; shift-click: rocket).
 * Smooth or retro presentation, time scale. `window.__lab` drives it from scripts.
 */
import '../styles.css';
import {
  bakeRetroSet,
  Brawler,
  Character,
  GibSystem,
  makeBench,
  makeCivilian,
  makeDesk,
  makeKnife,
  makeLmg,
  makePistol,
  makeRifle,
  makeSoldier,
  meshPart,
  ModelMesher,
  qrotate,
  qz,
  randomStyle,
  VoxelCollision,
  type Furniture,
  type Gib,
  type GibSpec,
  type GroundVariant,
  type HumanVariant,
  type Prop,
  type RetroSet,
  type SitVariant,
  type V3,
  type VoxelModel,
} from 'svx-anim';
import { DebugView, type Vec3 } from '../engine/protocol.ts';
import type { GpuCharacterMesh } from '../render/characters.ts';
import { cross, normalize } from '../render/math.ts';
import { Renderer } from '../render/renderer.ts';
import { buildLabLevel, LAB_H } from './level.ts';
import { steer, steerOptions, turn } from '../actors/steer.ts';

type Group = 'city' | 'soldiers' | 'fights' | 'reactions';

interface Actor {
  name: string;
  group: Group;
  variant: HumanVariant;
  prop: Prop | null;
  char: Character;
  palette: number;
  script: (a: Actor, t: number, dt: number) => void;
  pos: V3;
  yaw: number;
  /** Horizontal velocity and facing turn rate (weighted steering). */
  vel: [number, number];
  yawRate: number;
  /** Seconds to the next shot (shooters). */
  cool: number;
  brawler: Brawler | null;
  own: { mesher: ModelMesher; mesh: GpuCharacterMesh | null; version: number } | null;
  propSkin: Float32Array;
  stepSkin: Float32Array;
}

interface Placed {
  f: Furniture;
  pos: V3;
  yaw: number;
  skin: Float32Array;
  mesh: GpuCharacterMesh;
  palette: number;
}

type Style = 'smooth' | 'retro' | 'retro-chunky';
const FOV = (50 * Math.PI) / 180;
const BLOOD: [number, number, number] = [0.3, 0.012, 0.01];

async function main(): Promise<void> {
  const canvas = document.querySelector<HTMLCanvasElement>('#view')!;
  const renderer = await Renderer.create(canvas, (r) => console.error('device lost', r));
  const level = buildLabLevel();
  renderer.setTextures(level.textures);
  for (const m of level.meshes) renderer.chunks.upsert(m);
  const collision = new VoxelCollision(LAB_H, level.solidAt);
  const groundAt = (x: number, y: number, z: number): number => collision.groundHeight(x, y, z + 0.7, z - 1.5) ?? z;
  const cc = renderer.characters;
  const gibs = new GibSystem(collision);

  const mesher = new ModelMesher();
  const meshes = new Map<VoxelModel, GpuCharacterMesh>();
  const meshOf = (m: VoxelModel): GpuCharacterMesh => {
    let g = meshes.get(m);
    if (!g) {
      g = cc.uploadMesh(mesher.mesh(m), m.name);
      meshes.set(m, g);
    }
    return g;
  };
  const props = { rifle: makeRifle(), lmg: makeLmg(), pistol: makePistol(), knife: makeKnife() };

  // furniture
  const placed: Placed[] = [];
  const place = (f: Furniture, x: number, y: number, yaw: number): Placed => {
    const p: Placed = { f, pos: [x, y, groundAt(x, y, 1)], yaw, skin: new Float32Array(16), mesh: meshOf(f.model), palette: cc.palette(f.palette) };
    const c = Math.cos(yaw - Math.PI / 2), s = Math.sin(yaw - Math.PI / 2);
    p.skin.set([c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, p.pos[0], p.pos[1], p.pos[2], 1]);
    placed.push(p);
    return p;
  };
  /** World seat point of a placed seat, `along` metres along it (benches seat several). */
  const seatOf = (p: Placed, along = 0): { seat: V3; front: V3 } => {
    const q = qz(p.yaw - Math.PI / 2);
    const s = qrotate(q, [p.f.seat[0] + along, p.f.seat[1], p.f.seat[2]]);
    const seat: V3 = [p.pos[0] + s[0], p.pos[1] + s[1], p.pos[2] + s[2]];
    const fwd: V3 = [Math.cos(p.yaw), Math.sin(p.yaw), 0];
    return { seat, front: [seat[0] + fwd[0] * 0.38, seat[1] + fwd[1] * 0.38, p.pos[2]] };
  };
  const bench1 = place(makeBench(), -6.2, 14.7, -Math.PI / 2);
  const bench2 = place(makeBench(), -12.5, 14.7, -Math.PI / 2);
  const desk = place(makeDesk(), -2.8, 12.9, Math.PI / 2);

  const retroSets = new Map<string, RetroSet>();
  let style: Style = (new URLSearchParams(location.search).get('anim') as Style) ?? 'smooth';
  if (!['smooth', 'retro', 'retro-chunky'].includes(style)) style = 'smooth';
  const retroFor = (a: Actor): RetroSet | null => {
    if (style === 'smooth') return null;
    const vs = style === 'retro-chunky' ? 1 / 16 : 1 / 32;
    const key = `${a.variant.spec.name}|${a.prop?.kind ?? 'none'}|${vs}`;
    let set = retroSets.get(key);
    if (!set) {
      set = bakeRetroSet(a.variant.model, { weapon: a.prop, voxelSize: vs });
      retroSets.set(key, set);
    }
    return set;
  };

  const actors: Actor[] = [];
  interface Spec {
    name: string;
    group: Group;
    variant: () => HumanVariant;
    prop: Prop | null;
    pos: V3;
    yaw: number;
    script: Actor['script'];
    init?: (a: Actor) => void;
  }
  const specs: Spec[] = [];
  const spawn = (s: Spec, index: number): Actor => {
    const variant = s.variant();
    const char = new Character({ model: variant.model, palette: variant.palette, collision, weapon: s.prop, seed: index + 1 });
    char.animator.style = randomStyle(index * 17 + 3, variant.spec.name.startsWith('soldier') ? 'soldier' : variant.spec.female ? 'civilianFemale' : 'civilian');
    const pos: V3 = [s.pos[0], s.pos[1], groundAt(s.pos[0], s.pos[1], 1)];
    char.place(pos, s.yaw);
    const a: Actor = { name: s.name, group: s.group, variant, prop: s.prop, char, palette: cc.palette(variant.palette), script: s.script, pos, yaw: s.yaw, vel: [0, 0], yawRate: 0, cool: 0, brawler: null, own: null, propSkin: new Float32Array(16), stepSkin: new Float32Array(char.skin.length) };
    a.char.retro = retroFor(a);
    s.init?.(a);
    return a;
  };
  const add = (s: Spec): void => {
    specs.push(s);
  };

  // movement helpers
  // (the same weighted steering as the game: momentum, a limited turn rate, rounded corners)
  const moveTo = (a: Actor, x: number, y: number, speed: number, dt: number, face = true, stop = false): boolean => {
    const dx = x - a.pos[0], dy = y - a.pos[1];
    const d = Math.hypot(dx, dy);
    if (d < (stop ? 0.08 : 0.35)) return true;
    const want = stop ? Math.min(speed, Math.sqrt(2 * 3.6 * 0.8 * Math.max(0, d - 0.05)) + 0.1) : speed;
    a.vel = steer(a.vel, [dx / d, dy / d], want, dt, steerOptions(speed));
    a.pos[0] += a.vel[0] * dt;
    a.pos[1] += a.vel[1] * dt;
    a.pos[2] = groundAt(a.pos[0], a.pos[1], a.pos[2]);
    if (face && Math.hypot(a.vel[0], a.vel[1]) > 0.2) turnTo(a, Math.atan2(a.vel[1], a.vel[0]), dt, 3.2);
    return false;
  };
  const turnTo = (a: Actor, yaw: number, dt: number, maxRate = 2.6): void => {
    const t = turn(a.yaw, a.yawRate, yaw, dt, maxRate, 9);
    a.yaw = t.yaw;
    a.yawRate = t.rate;
  };
  const circle = (cx: number, cy: number, r: number, speed: number, dir = 1) => (a: Actor, t: number): void => {
    const th = (speed / r) * dir * t;
    const tx = cx + r * Math.cos(th), ty = cy + r * Math.sin(th);
    a.pos[0] = tx;
    a.pos[1] = ty;
    a.pos[2] = groundAt(tx, ty, a.pos[2]);
    a.yaw = th + (dir * Math.PI) / 2;
  };
  const waypoints = (pts: [number, number][], speed: number) => {
    let i = 0;
    return (a: Actor, _t: number, dt: number): void => {
      const p = pts[i % pts.length]!;
      if (moveTo(a, p[0], p[1], speed, dt)) i++;
    };
  };
  const inp = (a: Actor) => a.char.animator.input;
  const face = (a: Actor, p: V3, dt: number, rate = 2.6): void => {
    turnTo(a, Math.atan2(p[1] - a.pos[1], p[0] - a.pos[0]), dt, rate);
  };
  const shoot = (a: Actor, dt: number, every: number, burst: boolean, t: number): void => {
    a.cool -= dt;
    if (a.cool > 0 || (burst && Math.sin(t * 1.1 + a.pos[0]) < 0.2)) return;
    a.char.fire();
    a.cool = every;
    const m = a.char.muzzle();
    renderer.particles.spawn({ pos: m, vel: [0, 0, 0], life: 0.05, size: 0.07, color: [3, 2, 0.8, 1], additive: true, gravity: 0 });
  };
  const byName = (n: string): Actor | undefined => actors.find((x) => x.name === n);

  // ---- city life ----------------------------------------------------------------------------
  const talkC: V3 = [-10.5, 12.2, 0];
  const talkers = ['talker A', 'talker B', 'talker C'];
  talkers.forEach((name, i) => {
    const ang = (i / 3) * Math.PI * 2 + 0.4;
    const p: V3 = [talkC[0] + Math.cos(ang) * 0.68, talkC[1] + Math.sin(ang) * 0.68, 0];
    add({
      name,
      group: 'city',
      variant: () => makeCivilian(20 + i),
      prop: null,
      pos: p,
      yaw: ang + Math.PI,
      script: (a, t) => {
        // take turns speaking; everyone looks at the speaker
        const speaker = Math.floor(t / 4.5) % 3;
        const sp = byName(talkers[speaker]!);
        const next = byName(talkers[(i + 1) % 3]!);
        inp(a).talk = speaker === i ? 'speak' : 'listen';
        inp(a).lookAt = speaker === i ? (next ? next.char.animator.eyes() : null) : sp ? sp.char.animator.eyes() : null;
      },
    });
  });
  const sitter = (name: string, p: Placed, along: number, variant: SitVariant | null, seed: number): void => {
    const s = seatOf(p, along);
    add({
      name,
      group: 'city',
      variant: () => makeCivilian(seed),
      prop: null,
      pos: s.front,
      yaw: p.yaw,
      script: (a, t) => {
        inp(a).stance = 'sit';
        inp(a).seat = { pos: s.seat, backrest: p.f.backrest, deskHeight: p.f.deskHeight ?? null, ...(variant ? { variant } : {}) };
        // now and then look at the passers-by
        const w = byName('stroller');
        inp(a).lookAt = w && Math.sin(t * 0.3 + seed) > 0.2 ? w.char.animator.eyes() : null;
      },
    });
  };
  sitter('bench upright', bench1, -0.45, 'upright', 31);
  sitter('bench legs crossed', bench1, 0.45, 'crossLegs', 32);
  sitter('bench lean back', bench2, 0.2, 'leanBack', 33);
  sitter('elbows on knees', bench2, -0.55, 'elbowsOnKnees', 34);
  sitter('desk worker', desk, 0, 'desk', 35);
  (['cross', 'kneesUp', 'legsOut'] as GroundVariant[]).forEach((v, i) => {
    add({
      name: `ground ${v}`,
      group: 'city',
      variant: () => makeCivilian(40 + i),
      prop: null,
      pos: [-14.4, 10 + i * 1.3, 0],
      yaw: 0,
      script: (a) => {
        inp(a).stance = 'ground';
        inp(a).groundVariant = v;
      },
    });
  });
  for (let i = 0; i < 2; i++) {
    add({ name: `waiting ${i + 1}`, group: 'city', variant: () => makeCivilian(50 + i), prop: null, pos: [-8.2 - i * 1.4, 10.3 - i * 0.4, 0], yaw: Math.PI / 2 + i, script: () => {} });
  }
  add({
    name: 'stroller',
    group: 'city',
    variant: () => makeCivilian(60),
    prop: null,
    pos: [-14.5, 9.3, 0],
    yaw: 0,
    script: (a, t, dt) => {
      waypoints([[-1.8, 9.3], [-1.8, 10.8], [-14.5, 10.8], [-14.5, 9.3]], 1.0)(a, t, dt);
      // looks at the conversation while passing
      const d = Math.hypot(a.pos[0] - talkC[0], a.pos[1] - talkC[1]);
      inp(a).lookAt = d < 5 ? [talkC[0], talkC[1], 1.6] : null;
    },
  });
  add({ name: 'commuter', group: 'city', variant: () => makeCivilian(61), prop: null, pos: [-2, 9.6, 0], yaw: Math.PI, script: waypoints([[-14.5, 9.6], [-14.5, 11.2], [-2, 11.2], [-2, 9.6]], 1.6) });
  const perimeter: [number, number][] = [[-15.2, -15.2], [15.2, -15.2], [15.2, 15.3], [-15.2, 15.3]];
  add({ name: 'jogger', group: 'city', variant: () => makeCivilian(62), prop: null, pos: [-15.2, -15.2, 0], yaw: 0, script: waypoints(perimeter, 2.8) });
  add({ name: 'runner', group: 'city', variant: () => makeCivilian(63), prop: null, pos: [15.2, 15.3, 0], yaw: Math.PI, script: waypoints([...perimeter].reverse(), 4.6) });
  add({ name: 'stairs', group: 'city', variant: () => makeCivilian(3), prop: null, pos: [2, 4.5, 0], yaw: 0, script: waypoints([[15, 4.5], [2, 4.5]], 1.4) });

  // ---- soldiers -----------------------------------------------------------------------------
  add({
    name: 'kneeling fire',
    group: 'soldiers',
    variant: () => makeSoldier(1),
    prop: props.rifle,
    pos: [-11, -8, 0],
    yaw: Math.PI / 2,
    script: (a, t, dt) => {
      inp(a).stance = 'kneel';
      inp(a).carry = 'aim';
      inp(a).aimAt = [-11 + 3 * Math.sin(t * 0.3), 6, 1.4];
      shoot(a, dt, 0.12, true, t);
    },
  });
  add({
    name: 'prone',
    group: 'soldiers',
    variant: () => makeSoldier(2),
    prop: props.rifle,
    pos: [-13.6, -11.5, 0],
    yaw: Math.PI / 2,
    script: (a, t, dt) => {
      inp(a).stance = 'prone';
      inp(a).carry = 'aim';
      inp(a).aimAt = [-13.6 + 2 * Math.sin(t * 0.4), 6, 0.6];
      shoot(a, dt, 0.35, true, t);
    },
  });
  add({
    name: 'crawler',
    group: 'soldiers',
    variant: () => makeSoldier(3),
    prop: props.rifle,
    pos: [-12, -14.2, 0],
    yaw: 0,
    script: (a, t, dt) => {
      inp(a).stance = 'prone';
      inp(a).carry = 'ready';
      if (a.char.animator.transitioning) return;
      waypoints([[-6.5, -14.2], [-12, -14.2]], 0.45)(a, t, dt);
    },
  });
  add({
    name: 'peeker',
    group: 'soldiers',
    variant: () => makeSoldier(4),
    prop: props.rifle,
    pos: [2.95, -7.4, 0],
    yaw: Math.PI / 2,
    script: (a, t) => {
      // behind the corner of the wall, leaning out to look and aim, then back
      const peek = Math.sin(t * 0.8) > 0;
      inp(a).lean = peek ? -1 : 0;
      inp(a).carry = peek ? 'aim' : 'ready';
      inp(a).aimAt = [-2, 1, 1.4];
      inp(a).crouch = 0.2;
    },
  });
  add({
    name: 'pistol walk+fire',
    group: 'soldiers',
    variant: () => makeSoldier(6),
    prop: props.pistol,
    pos: [-4, -12, 0],
    yaw: Math.PI / 2,
    script: (a, t, dt) => {
      const target: V3 = [-3, 2, 1.4];
      inp(a).carry = 'aim';
      inp(a).aimAt = target;
      moveTo(a, -3 + 2.2 * Math.sin(t * 0.4), -12 + 0.8 * Math.sin(t * 0.23), 1.3, dt, false);
      face(a, target, dt);
      shoot(a, dt, 0.3, true, t);
    },
  });
  add({
    name: 'pistol one-handed',
    group: 'soldiers',
    variant: () => makeCivilian(64),
    prop: props.pistol,
    pos: [0.5, -13.5, 0],
    yaw: Math.PI / 2,
    script: (a, t, dt) => {
      const target: V3 = [0, 2, 1.4];
      inp(a).carry = Math.sin(t * 0.3) > 0 ? 'hip' : 'relaxed';
      inp(a).aimAt = target;
      face(a, target, dt);
      if (inp(a).carry === 'hip') shoot(a, dt, 0.45, false, t);
    },
  });
  add({
    name: 'machine gun hip fire',
    group: 'soldiers',
    variant: () => makeSoldier(7),
    prop: props.lmg,
    pos: [6.5, -13, 0],
    yaw: Math.PI / 2,
    script: (a, t, dt) => {
      const target: V3 = [8, 3, 1.4];
      inp(a).carry = 'hip';
      inp(a).aimAt = target;
      moveTo(a, 8 + 2 * Math.sin(t * 0.35), -13, 1.2, dt, false);
      face(a, target, dt);
      shoot(a, dt, 0.08, true, t);
    },
  });
  add({
    name: 'reload',
    group: 'soldiers',
    variant: () => makeSoldier(8),
    prop: props.rifle,
    pos: [-8.5, -10, 0],
    yaw: Math.PI / 2,
    script: (a, t, dt) => {
      inp(a).carry = 'aim';
      inp(a).aimAt = [-8.5, 5, 1.4];
      if (!a.char.animator.busy) {
        if (Math.floor(t / 5) !== Math.floor((t - dt) / 5)) a.char.animator.play('reloadRifle');
        else if (Math.sin(t * 2) > 0.6) shoot(a, dt, 0.11, false, t);
      }
    },
  });
  add({
    name: 'crouch walk',
    group: 'soldiers',
    variant: () => makeSoldier(9),
    prop: props.rifle,
    pos: [-1.5, -9, 0],
    yaw: 0,
    script: (a, t, dt) => {
      inp(a).crouch = 1;
      inp(a).carry = 'ready';
      waypoints([[1.2, -9], [1.2, -10.5], [-1.5, -10.5], [-1.5, -9]], 1.0)(a, t, dt);
    },
  });
  add({ name: 'patrol', group: 'soldiers', variant: () => makeSoldier(10), prop: props.rifle, pos: [-8, -3, 0], yaw: 0, script: waypoints([[-2, -3], [-2, 0], [-8, 0], [-8, -3]], 1.5) });
  add({ name: 'sprint', group: 'soldiers', variant: () => makeSoldier(12), prop: props.rifle, pos: [11, 11, 0], yaw: 0, script: circle(8, 11, 3, 5) });

  // ---- fights -------------------------------------------------------------------------------
  const fighter = (name: string, other: string, x: number, y: number, yaw: number, seed: number, prop: Prop | null, soldier = false): void => {
    add({
      name,
      group: 'fights',
      variant: () => (soldier ? makeSoldier(seed) : makeCivilian(seed)),
      prop,
      pos: [x, y, 0],
      yaw,
      init: (a) => {
        a.brawler = new Brawler(a.char, { seed, aggression: 0.55, skill: 0.3 });
      },
      script: (a, _t, dt) => {
        const b = a.brawler!;
        b.opponent = byName(other)?.char ?? null;
        b.update(dt);
        if (!a.char.animator.transitioning && !a.char.animator.knockedDown) {
          a.pos[0] += b.move[0] * dt;
          a.pos[1] += b.move[1] * dt;
          a.pos[2] = groundAt(a.pos[0], a.pos[1], a.pos[2]);
        }
        a.yaw = turnTowards(a.yaw, b.yaw, 8 * dt);
      },
    });
  };
  fighter('brawler A', 'brawler B', 11.8, -10, 0, 70, null);
  fighter('brawler B', 'brawler A', 12.8, -10, Math.PI, 71, null);
  fighter('knife', 'unarmed', 11.8, -13.2, 0, 72, props.knife);
  fighter('unarmed', 'knife', 12.7, -13.2, Math.PI, 73, null, true);

  // ---- reactions line-up ---------------------------------------------------------------------
  const lineup: [string, () => HumanVariant, Prop | null][] = [
    ['target soldier', () => makeSoldier(13), props.rifle],
    ['target civilian', () => makeCivilian(80), null],
    ['target civilian 2', () => makeCivilian(81), null],
    ['target soldier 2', () => makeSoldier(14), props.rifle],
  ];
  lineup.forEach(([name, v, prop], i) => {
    add({
      name,
      group: 'reactions',
      variant: v,
      prop,
      pos: [-1.5 + i * 1.3, 1, 0],
      yaw: -Math.PI / 2,
      script: (a) => {
        if (prop) inp(a).carry = 'ready';
      },
    });
  });

  // gibs: meshes per loose part
  const gibMeshes = new Map<Gib, { mesh: GpuCharacterMesh; palette: number; skin: Float32Array; own: boolean }>();
  const spawnGib = (spec: GibSpec, a: Actor, bleed: number): void => {
    const own = !spec.prop;
    const mesh = own ? cc.uploadMesh(meshPart(spec.part, spec.voxelSize), 'gib') : meshOf(a.prop!.model);
    const g = gibs.spawn(spec.part, spec.voxelSize, spec.bonePos, spec.boneRot, spec.boneRestHead, spec.vel, spec.ang);
    g.bleed = bleed;
    gibMeshes.set(g, { mesh, palette: a.palette, skin: new Float32Array(16), own });
  };
  const releaseGib = (g: Gib): void => {
    const m = gibMeshes.get(g);
    if (m?.own) cc.releaseMesh(m.mesh);
    gibMeshes.delete(g);
  };
  const killed = (a: Actor): void => {
    const w = a.char.dropWeapon();
    if (w) spawnGib(w, a, 0);
  };
  const reset = (group?: Group): void => {
    for (let i = actors.length - 1; i >= 0; i--) {
      const a = actors[i]!;
      if (group && a.group !== group) continue;
      if (a.own?.mesh) cc.releaseMesh(a.own.mesh);
      actors.splice(i, 1);
    }
    specs.forEach((s, i) => {
      if (!group || s.group === group) actors.push(spawn(s, i));
    });
    if (!group) {
      for (const g of gibs.gibs) releaseGib(g);
      gibs.clear();
    }
  };

  // camera
  const cam = { target: [0, 0, 1.0] as Vec3, dist: 7, yaw: Math.PI / 2 + 0.3, pitch: 0.18, follow: -1 };
  const views: Record<Group | 'overview', { target: Vec3; dist: number; yaw: number; pitch: number }> = {
    overview: { target: [0, 0, 0.5], dist: 30, yaw: -Math.PI / 2 - 0.25, pitch: 0.75 },
    city: { target: [-8.5, 12, 0.8], dist: 11, yaw: Math.PI / 2 + 0.12, pitch: 0.32 },
    soldiers: { target: [-5, -11, 0.7], dist: 12, yaw: -Math.PI / 2 + 0.35, pitch: 0.38 },
    fights: { target: [12.3, -11.6, 0.9], dist: 5.5, yaw: -Math.PI / 2 - 0.5, pitch: 0.22 },
    reactions: { target: [0.5, 1, 1.0], dist: 5.5, yaw: -Math.PI / 2 - 0.2, pitch: 0.12 },
  };
  const setView = (g: Group | 'overview'): void => {
    const v = views[g];
    cam.follow = -1;
    cam.target = [...v.target];
    cam.dist = v.dist;
    cam.yaw = v.yaw;
    cam.pitch = v.pitch;
  };
  const view = (): { eye: Vec3; fwd: Vec3 } => {
    const cp = Math.cos(cam.pitch);
    const fwd: Vec3 = [-cp * Math.cos(cam.yaw), -cp * Math.sin(cam.yaw), -Math.sin(cam.pitch)];
    return { eye: [cam.target[0] - fwd[0] * cam.dist, cam.target[1] - fwd[1] * cam.dist, cam.target[2] - fwd[2] * cam.dist], fwd };
  };
  const pixelRay = (px: number, py: number): { o: V3; d: V3 } => {
    const { eye, fwd } = view();
    const right = normalize(cross(fwd, [0, 0, 1]));
    const up = cross(right, fwd);
    const w = canvas.clientWidth, h = canvas.clientHeight;
    const t = Math.tan(FOV / 2);
    const x = ((px / w) * 2 - 1) * t * (w / h);
    const y = (1 - (py / h) * 2) * t;
    return { o: eye, d: normalize([fwd[0] + right[0] * x + up[0] * y, fwd[1] + right[1] * x + up[1] * y, fwd[2] + right[2] * x + up[2] * y]) };
  };
  const bleed = (a: Actor, point: V3, d: V3, removed: { slot: number }[]): void => {
    gibs.spray(point, d, 8 + Math.min(16, removed.length >> 1), 3.2, 0.5);
    gibs.spray(point, [-d[0], -d[1], -d[2]], 3, 1.5, 0.9);
    for (let i = 0; i < removed.length; i += 4) {
      const c = a.variant.palette[removed[i]!.slot] ?? BLOOD;
      gibs.spray(point, d, 1, 2.8, 0.7, [c[0], c[1], c[2]]);
    }
  };
  const shootRay = (o: V3, d: V3, damage = 34): boolean => {
    let best: { a: Actor; hit: NonNullable<ReturnType<Character['raycast']>> } | null = null;
    for (const a of actors) {
      const hit = a.char.raycast(o, d, best ? best.hit.distance : 100);
      if (hit && (!best || hit.distance < best.hit.distance)) best = { a, hit };
    }
    if (!best) return false;
    const { a, hit } = best;
    const wasAlive = a.char.alive;
    const r = a.char.wound(hit, d, damage);
    bleed(a, hit.point, d, r.removed);
    for (const g of r.gibs) spawnGib(g, a, 30);
    if (wasAlive && !a.char.alive) killed(a);
    return true;
  };
  const rocket = (p: V3): void => {
    for (const a of actors) {
      const wasAlive = a.char.alive;
      const r = a.char.blast(p, 1, 1);
      if (r.gibbed) {
        gibs.spray(a.char.bounds().center, [0, 0, 1], 60, 5, 1.2);
        for (const g of r.gibs) spawnGib(g, a, 40);
      }
      if (wasAlive && !a.char.alive) killed(a);
    }
    gibs.impulse(p, 4, 11);
    renderer.particles.spawn({ pos: p, vel: [0, 0, 0], life: 0.3, size: 1.2, grow: 3, color: [4, 1.8, 0.45, 0.9], additive: true });
  };

  let drag: { x: number; y: number; moved: boolean } | null = null;
  canvas.addEventListener('pointerdown', (e) => {
    drag = { x: e.clientX, y: e.clientY, moved: false };
    canvas.setPointerCapture(e.pointerId);
  });
  canvas.addEventListener('pointerup', (e) => {
    if (drag && !drag.moved) {
      const r = pixelRay(e.offsetX, e.offsetY);
      if (e.shiftKey) {
        const t = collision.raycast(r.o, r.d, 200);
        if (t > 0) rocket([r.o[0] + r.d[0] * t, r.o[1] + r.d[1] * t, r.o[2] + r.d[2] * t]);
      } else shootRay(r.o, r.d);
    }
    drag = null;
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!drag) return;
    if (Math.abs(e.clientX - drag.x) + Math.abs(e.clientY - drag.y) > 3) drag.moved = true;
    if (!drag.moved) return;
    cam.follow = -1;
    cam.yaw -= (e.clientX - drag.x) * 0.006;
    cam.pitch = Math.max(-0.2, Math.min(1.4, cam.pitch + (e.clientY - drag.y) * 0.006));
    drag.x = e.clientX;
    drag.y = e.clientY;
  });
  canvas.addEventListener('wheel', (e) => {
    cam.dist = Math.max(1, Math.min(60, cam.dist * Math.exp(e.deltaY * 0.001)));
  });

  // panel
  const ui = document.querySelector<HTMLElement>('#ui')!;
  const panel = document.createElement('div');
  panel.style.cssText = 'position:absolute;top:8px;left:8px;pointer-events:auto;background:rgba(0,0,0,.55);padding:8px 10px;border-radius:6px;font:12px system-ui;color:#e6e8eb;max-width:300px;line-height:1.5;max-height:calc(100vh - 16px);overflow:auto';
  panel.innerHTML = `<b>svx_anim lab</b><br>drag: orbit · wheel: zoom · click: shoot · shift-click: rocket<br>
    <label>time <input id="ts" type="range" min="0" max="1.5" step="0.05" value="1"></label><br>
    <label>animation <select id="style"><option value="smooth">smooth</option><option value="retro">retro (Voxel Doom)</option><option value="retro-chunky">retro, chunky</option></select></label><br>
    <div id="views"></div><button id="reset">reset all</button> <button id="kill">kill all</button><div id="cast"></div>`;
  ui.append(panel);
  const viewsDiv = panel.querySelector<HTMLDivElement>('#views')!;
  for (const g of ['overview', 'city', 'soldiers', 'fights', 'reactions'] as const) {
    const b = document.createElement('button');
    b.textContent = g;
    b.onclick = () => setView(g);
    viewsDiv.append(b);
  }
  const cast = panel.querySelector<HTMLDivElement>('#cast')!;
  const follow = (i: number): void => {
    cam.follow = i;
  };
  specs.forEach((s, i) => {
    const b = document.createElement('button');
    b.textContent = s.name;
    b.onclick = () => follow(i);
    cast.append(b);
  });
  let timeScale = 1;
  panel.querySelector<HTMLInputElement>('#ts')!.oninput = (e) => (timeScale = Number((e.target as HTMLInputElement).value));
  const styleSel = panel.querySelector<HTMLSelectElement>('#style')!;
  styleSel.value = style;
  const setStyle = (s: Style): void => {
    style = s;
    styleSel.value = s;
    for (const a of actors) a.char.retro = retroFor(a);
  };
  styleSel.onchange = () => setStyle(styleSel.value as Style);
  panel.querySelector<HTMLButtonElement>('#reset')!.onclick = () => reset();
  panel.querySelector<HTMLButtonElement>('#kill')!.onclick = () => {
    for (const a of actors)
      if (a.char.alive) {
        a.char.die(a.char.bounds().center, [(Math.random() - 0.5) * 4, (Math.random() - 0.5) * 4, 1]);
        killed(a);
      }
  };

  reset();
  setView('overview');
  let simT = 0;
  let last = 0;
  let retroClock = 0;
  let fightsOver = 0;
  const step = (dt: number): void => {
    simT += dt;
    retroClock += dt;
    const retroTick = retroClock >= 4 / 35;
    if (retroTick) retroClock %= 4 / 35;
    for (const a of actors) {
      if (a.char.alive) {
        a.script(a, simT, dt);
        // knockback from hits moves the character (the feet stumble after it)
        const kb = a.char.animator.takeKnockback(dt);
        a.pos[0] += kb[0];
        a.pos[1] += kb[1];
        a.char.setRoot(a.pos, a.yaw);
      }
      a.char.update(dt);
      if (style === 'smooth' || a.char.retroFrame || retroTick) a.stepSkin.set(a.char.skin);
      const events = a.char.animator.takeEvents();
      if (a.brawler) {
        for (const blow of a.brawler.resolve(events)) {
          const v = actors.find((x) => x.char === blow.victim)!;
          if (!blow.blocked && (blow.kind === 'blade' || (blow.result.zone === 'head' && blow.result.damage > 18))) bleed(v, blow.point, blow.dir, blow.result.removed.slice(0, 8));
          for (const g of blow.result.gibs) spawnGib(g, v, 30);
          if (blow.result.killed) killed(v);
        }
      }
    }
    // a fight that ended starts again after a while
    const fightDone = actors.some((a) => a.group === 'fights' && !a.char.alive);
    fightsOver = fightDone ? fightsOver + dt : 0;
    if (fightsOver > 6) {
      fightsOver = 0;
      reset('fights');
    }
    gibs.update(dt);
    const live = new Set(gibs.gibs);
    for (const [g, m] of gibMeshes) {
      if (!live.has(g)) releaseGib(g);
      else if (style === 'smooth' || retroTick) gibs.writeSkin(g, m.skin);
    }
  };
  const labApi = {
    cam,
    actors,
    follow,
    setView,
    step,
    shoot: shootRay,
    rocket,
    reset,
    setStyle,
    setTimeScale: (s: number) => (timeScale = s),
    names: () => specs.map((s) => s.name),
    byName,
    gibs,
  };
  (window as unknown as { __lab: typeof labApi }).__lab = labApi;

  const frame = (tMs: number): void => {
    const dt = last === 0 ? 1 / 60 : Math.min(0.05, (tMs - last) / 1000);
    last = tMs;
    if (timeScale > 0) step(dt * timeScale);
    cc.begin();
    for (const p of placed) cc.add(p.mesh, p.skin, 1, p.palette, { center: [p.pos[0], p.pos[1], p.pos[2] + 0.5], radius: 1.2 });
    for (const a of actors) {
      const ch = a.char;
      const b = ch.bounds();
      const tint: [number, number, number, number] = [1, 0.15, 0.1, ch.flash * 0.45];
      if (ch.retroFrame && ch.alive) {
        ch.writeRetroSkin(a.propSkin);
        cc.add(meshOf(ch.retroFrame), a.propSkin, 1, a.palette, { center: b.center, radius: 1.3, tint });
      } else {
        let mesh: GpuCharacterMesh;
        if (ch.ownsModel) {
          a.own ??= { mesher: new ModelMesher(), mesh: null, version: -1 };
          if (a.own.version !== ch.geometryVersion || !a.own.mesh) {
            if (a.own.mesh) cc.releaseMesh(a.own.mesh);
            a.own.mesh = cc.uploadMesh(a.own.mesher.mesh(ch.model), a.name);
            a.own.version = ch.geometryVersion;
          }
          mesh = a.own.mesh;
        } else mesh = meshOf(ch.model);
        cc.add(mesh, a.stepSkin, ch.model.skeleton.count, a.palette, { center: b.center, radius: b.radius + 0.3, tint });
        if (ch.weapon && ch.alive) {
          ch.animator.writePropSkin(a.propSkin);
          cc.add(meshOf(ch.weapon.model), a.propSkin, 1, a.palette, { center: ch.animator.weaponPos, radius: 0.8 });
        }
      }
      if (ch.alive && ch.animator.stance === 'stand') cc.decal([a.pos[0], a.pos[1], ch.animator.rootPos[2] + 0.004], [0, 0, 1], 0.42, [0, 0, 0, 0.5], 0);
    }
    for (const [g, m] of gibMeshes) cc.add(m.mesh, m.skin, 1, m.palette, { center: g.pos, radius: g.radius + 0.05 });
    gibs.forEachDrop((p, size, c) => cc.bit(p, size, [0, 0, 0, 1], c));
    gibs.forEachStain((p, n, size, age, c) => cc.decal(p, n, Math.max(0.045, size * 2.4), [c[0] * 0.8, c[1] * 0.8, c[2] * 0.8, Math.min(0.92, 0.5 + age)], 1));
    if (cam.follow >= 0 && actors[cam.follow]) {
      const p = actors[cam.follow]!.char.pose.p[1]!;
      cam.target = [p[0], p[1], p[2] + 0.1];
    }
    const { eye, fwd } = view();
    renderer.render({ camera: { eye, forward: fwd, fovY: FOV, near: 0.05 }, timeS: tMs / 1000, debugView: DebugView.None, flashPos: [0, 0, 0], flashIntensity: 0, voxelSize: LAB_H });
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  (window as unknown as { __structvox: unknown }).__structvox = { state: () => ({ ready: true, render: { chunksDrawn: 1 } }) };
}

function turnTowards(cur: number, want: number, maxStep: number): number {
  let d = want - cur;
  while (d > Math.PI) d -= 2 * Math.PI;
  while (d < -Math.PI) d += 2 * Math.PI;
  return cur + Math.max(-maxStep, Math.min(maxStep, d));
}

main().catch((e: unknown) => {
  console.error(e);
  document.body.append(Object.assign(document.createElement('pre'), { className: 'boot-error', textContent: `lab failed: ${String(e)}` }));
});
