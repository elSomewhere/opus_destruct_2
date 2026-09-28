/**
 * Actors: soldiers and civilians in the game world, built on svx_anim.
 *
 * ActorWorld owns the characters and runs them every frame:
 *  - brains (brain.ts) decide at ~10 Hz what to do (city life, fear, combat) and set goals:
 *    destination, speed, facing, where to look and aim, stance, seat, conversation, mood;
 *  - movement: paths (nav.ts, budgeted per frame), steering with separation, the player's box
 *    collision with step-up on the engine's occupancy, gravity, the knockback of hits (the
 *    feet stumble after it), slower with a limp; no walking while sitting, kneeling or down;
 *  - animation: svx_anim Characters: a motion plan (procedural locomotion with personal styles,
 *    stances, actions, weapon handling) carried out by a physical body with muscles, and
 *    behaviours that keep it balanced and reacting (hits and shoves by where they land,
 *    staggering, falling and getting up, flinching from rounds landing close, holding wounds,
 *    bracing on walls, dying); the nearest bodies are simulated when calm too (a budget), the
 *    rest only while something happens to them; gibs and blood (GibSystem);
 *  - the city: benches and café tables placed around (seats taken and freed), conversations
 *    (pairs), brawls
 *    (Brawler: fists, kicks, knives) that bystanders stop to watch;
 *  - combat: hitscan rounds from each character's weapon (rifle, SMG, machine gun, pistol:
 *    damage, rate, spread, magazines and reloads) that hit the first thing on their line: a
 *    character (voxel-exact wounds), the player, or the world, where they carve the engine's
 *    voxels (the battle destroys the level); melee blows; player rounds and rockets come in
 *    through raycast / wound / blast; falling debris crushes characters;
 *  - rendering: characters (their own meshes once wounded), held weapons, furniture, gibs,
 *    blood drops and stains, blob shadows; the retro presentation.
 */
import {
  Brawler,
  Character,
  FURNITURE_PALETTE,
  gatherObstacles,
  GibSystem,
  ModelMesher,
  qrotate,
  qz,
  clamp,
  vadd,
  vdist,
  vnorm,
  vscale,
  vsub,
  type Carry,
  type CharacterHit,
  type Furniture,
  type Gib,
  type GibSpec,
  type GroundVariant,
  type Mood,
  type Obstacle,
  type Prop,
  type SeatInfo,
  type Stance,
  type V3,
  type WoundResult,
} from 'svx-anim';
import type { CharacterRenderer, GpuCharacterMesh } from '../render/characters.ts';
import type { IslandRenderer } from '../render/islands.ts';
import { CivilianBrain, SoldierBrain, type Brain, type Target } from './brain.ts';
import { ThugBrain } from './thug.ts';
import { Cast, WEAPON_STATS, type Faction, type Look } from './cast.ts';
import type { WorldAccess } from './env.ts';
import { Navigator } from './nav.ts';
import { pursue, steer, steerOptions, turn, wrap } from './steer.ts';

export interface Noise {
  pos: V3;
  radius: number;
  kind: 'shot' | 'impact' | 'explosion' | 'death' | 'scream' | 'shout' | 'fight';
  source: Actor | null;
}

export interface PlayerView {
  feet: V3;
  forward: V3;
  alive: boolean;
  /** Aiming at people frightens them (false while spectating: noclip). */
  threat: boolean;
  eye(): V3;
  chest(): V3;
}

/** What the actors need from the rest of the game. */
export interface ActorHooks {
  carve(pos: V3, radius: number): void;
  impact(pos: V3, normal: V3): void;
  hurtPlayer(damage: number, from: V3): void;
  /** A blow shoves the player (m/s). */
  pushPlayer(v: V3): void;
  muzzleFlash(pos: V3, dir: V3): void;
  tracer(from: V3, to: V3): void;
  bloodMist(pos: V3, dir: V3, amount: number): void;
}

export interface ActorSettings {
  ai: boolean;
  retro: boolean;
  retroVoxel: number;
  maxCorpses: number;
  playerDamage: boolean;
  /** Radius (m) of the voxel sphere a round carves out of the world (x the weapon's). */
  roundCarve: number;
  light: number;
}

/** A place to sit. */
export interface Seat {
  seat: V3;
  /** Where to stand before sitting down (the root stays there). */
  front: V3;
  yaw: number;
  /** Height of the table in front of it (sitting at a desk), or null. */
  desk: number | null;
  occupant: Actor | null;
}

/** A bench or a table placed in the world. */
interface Placed {
  furniture: Furniture;
  pos: V3;
  yaw: number;
  skin: Float32Array;
  seats: Seat[];
}

export interface Actor {
  id: number;
  faction: Faction;
  look: Look;
  char: Character;
  brain: Brain;
  /** The weapon held or holstered (civilians draw it when they need it). */
  weapon: Prop | null;
  /** Rounds left in the magazine. */
  mag: number;
  reloading: boolean;
  pos: V3;
  vel: V3;
  yaw: number;
  onGround: boolean;
  goal: V3 | null;
  path: V3[];
  needPath: boolean;
  speed: number;
  /** Where the body turns to. */
  face: V3 | null;
  /** Where the weapon points (aim), and where the head looks otherwise. */
  aimTarget: V3 | null;
  lookAt: V3 | null;
  crouch: number;
  carry: Carry;
  mood: Mood;
  stance: Stance;
  seat: SeatInfo | null;
  groundVariant: GroundVariant;
  talk: 'speak' | 'listen' | null;
  lean: number;
  /** Angular velocity of the facing (rad/s) and whether a standing body is turning to its target. */
  yawRate: number;
  turning: boolean;
  /** Personal pace factor (people walk at their own speed). */
  pace: number;
  brawler: Brawler | null;
  /** Fired at soldiers: they may target it. */
  hostile: boolean;
  meleeTarget: Target | null;
  stuck: number;
  thinkAcc: number;
  own: { mesher: ModelMesher; mesh: GpuCharacterMesh | null; version: number } | null;
  drawSkin: Float32Array;
  propSkin: Float32Array;
  opacity: number;
  diedAt: number;
}

interface GibUser {
  mesh: GpuCharacterMesh;
  paletteId: number;
  skin: Float32Array;
  shared: boolean;
}

const DEFAULT_SETTINGS: ActorSettings = { ai: true, retro: false, retroVoxel: 1 / 32, maxCorpses: 40, playerDamage: true, roundCarve: 1, light: 1 };
const BLOOD: [number, number, number] = [0.3, 0.012, 0.01];
const RETRO_STEP = 4 / 35;
/** Calm characters simulated physically (the nearest). */
const PHYSICS_BUDGET = 12;
const MELEE_FORCE: Record<string, number> = { riflePush: 1.4, cross: 1.2, jab: 0.8, hook: 1.45, uppercut: 1.5, frontKick: 1.8, roundhouse: 2.2, stab: 1.1, slash: 0.9, gutStab: 1.2, forehandSlash: 0.95 };
const BLADE_ACTIONS = new Set(['stab', 'slash', 'gutStab', 'forehandSlash']);

export class ActorWorld {
  readonly env: WorldAccess;
  readonly nav: Navigator;
  readonly cast: Cast;
  readonly gibs: GibSystem;
  readonly actors: Actor[] = [];
  readonly player: PlayerView;
  readonly hooks: ActorHooks;
  settings: ActorSettings = { ...DEFAULT_SETTINGS };
  time = 0;
  /** Chance that a conversation ends in a fight. */
  brawlChance = 0.04;
  private nextId = 1;
  private seed = 1;
  private readonly renderer: CharacterRenderer;
  private readonly islands: IslandRenderer | null;
  private readonly islandPrev = new Map<number, { pos: V3; t: number }>();
  private readonly gibUsers = new Set<Gib>();
  private readonly furniture: Placed[] = [];
  private readonly fights: { a: Actor; b: Actor; downFor: number }[] = [];
  private retroClock = 0;
  private pathCursor = 0;
  shotsFired = 0;
  /** Rounds that hit (and carved) the world. */
  worldHits = 0;
  kills = 0;

  constructor(renderer: CharacterRenderer, env: WorldAccess, player: PlayerView, hooks: ActorHooks, islands: IslandRenderer | null = null) {
    this.renderer = renderer;
    this.env = env;
    this.player = player;
    this.hooks = hooks;
    this.islands = islands;
    this.nav = new Navigator(env);
    this.cast = new Cast(renderer);
    this.gibs = new GibSystem(env, { maxGibs: 160 });
  }

  // ---- population -----------------------------------------------------------------------

  spawn(faction: Faction, pos: V3, yaw: number): Actor {
    const seed = this.seed++;
    const look = this.cast.look(faction, seed);
    const weapon = this.cast.loadout(faction, seed);
    // soldiers hold their weapon; civilians keep theirs out of sight until they need it
    const held = faction === 'soldier' ? weapon : null;
    const char = new Character({ model: look.model, palette: look.palette, collision: this.env, weapon: held, health: faction === 'soldier' ? 100 : faction === 'thug' ? 90 : 70, seed });
    char.motion.style = look.style;
    char.place(pos, yaw);
    const a: Actor = {
      id: this.nextId++,
      faction,
      look,
      char,
      brain: faction === 'soldier' ? new SoldierBrain(pos) : faction === 'thug' ? new ThugBrain() : new CivilianBrain(),
      weapon,
      mag: weapon ? WEAPON_STATS[weapon.kind].mag : 0,
      reloading: false,
      pos: [...pos],
      vel: [0, 0, 0],
      yaw,
      onGround: true,
      goal: null,
      path: [],
      needPath: false,
      speed: 0,
      face: null,
      aimTarget: null,
      lookAt: null,
      crouch: 0,
      carry: 'relaxed',
      mood: 'normal',
      stance: 'stand',
      seat: null,
      groundVariant: 'kneesUp',
      talk: null,
      lean: 0,
      yawRate: 0,
      turning: false,
      pace: 0.93 + Math.random() * 0.14,
      brawler: null,
      hostile: faction === 'thug',
      meleeTarget: null,
      stuck: 0,
      thinkAcc: Math.random() * 0.1,
      own: null,
      drawSkin: new Float32Array(char.skin.length),
      propSkin: new Float32Array(16),
      opacity: 1,
      diedAt: 0,
    };
    this.actors.push(a);
    return a;
  }

  /**
   * Scatters `civilians` and `soldiers` on standable ground between rMin and rMax of `center`
   * (soldiers in a group further out), with benches and café tables for the civilians. Returns how many were
   * placed.
   */
  populate(center: V3, civilians: number, soldiers: number, rMin = 6, rMax = 35, thugs = 0): number {
    let placed = 0;
    for (let i = 0; i < thugs; i++) {
      const p = this.nav.randomPoint(center, rMin, rMax, undefined, 30);
      if (!p) continue;
      this.spawn('thug', p, Math.random() * Math.PI * 2);
      placed++;
    }
    if (civilians > 0) {
      this.placeFurniture(this.cast.bench, center, Math.max(2, Math.round(civilians / 4)), rMin, rMax);
      this.placeFurniture(this.cast.table, center, Math.max(1, Math.round(civilians / 6)), rMin, rMax);
    }
    for (let i = 0; i < civilians; i++) {
      const p = this.nav.randomPoint(center, rMin, rMax, undefined, 30);
      if (!p) continue;
      this.spawn('civilian', p, Math.random() * Math.PI * 2);
      placed++;
    }
    if (soldiers > 0) {
      const post = this.nav.randomPoint(center, rMax * 0.6, rMax, undefined, 30) ?? this.nav.randomPoint(center, rMin, rMax, undefined, 30);
      for (let i = 0; i < soldiers && post; i++) {
        const p = this.nav.randomPoint(post, 0.5, 6, undefined, 30);
        if (!p) continue;
        const yaw = Math.atan2(center[1] - p[1], center[0] - p[0]) + (Math.random() - 0.5);
        this.spawn('soldier', p, yaw);
        placed++;
      }
    }
    return placed;
  }

  /** Benches or tables on flat open ground around a point (their footprint and a place to stand free). */
  placeFurniture(f: Furniture, center: V3, n: number, rMin: number, rMax: number): void {
    // footprint in the prop's frame (+y: where the sitter faces)
    const table = f.deskHeight !== undefined;
    const foot: [number, number][] = table
      ? [[-0.3, -0.3], [0.3, -0.3], [-0.4, 1.0], [0.4, 1.0], [0, 0.35], [-0.6, 0], [0.6, 0]]
      : [[-0.8, 0], [0.8, 0], [-0.8, 0.7], [0.8, 0.7], [0, 1.1]];
    for (let k = 0; k < n; k++) {
      for (let tries = 0; tries < 20; tries++) {
        const p = this.nav.randomPoint(center, rMin, rMax, undefined, 1);
        if (!p) continue;
        const yaw = Math.round(Math.random() * 4) * (Math.PI / 2);
        const q = qz(yaw - Math.PI / 2);
        const ok = foot.every(([x, y]) => {
          const o = qrotate(q, [x, y, 0]);
          const g = this.env.groundHeight(p[0] + o[0], p[1] + o[1], p[2] + 0.3, p[2] - 0.3);
          return g !== null && Math.abs(g - p[2]) < 0.07 && this.env.fits([p[0] + o[0], p[1] + o[1], g], 0.3, 1.2);
        });
        if (!ok || this.furniture.some((b) => vdist(b.pos, p) < 3)) continue;
        const c = Math.cos(yaw - Math.PI / 2), s = Math.sin(yaw - Math.PI / 2);
        const skin = new Float32Array([c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, p[0], p[1], p[2], 1]);
        const b: Placed = { furniture: f, pos: p, yaw, skin, seats: [] };
        const approach = f.approach ?? 0.38;
        for (const along of f.kind === 'bench' ? [-0.45, 0.45] : [0]) {
          const o = qrotate(q, [f.seat[0] + along, f.seat[1], f.seat[2]]);
          const seat: V3 = [p[0] + o[0], p[1] + o[1], p[2] + o[2]];
          const front: V3 = [seat[0] + Math.cos(yaw) * approach, seat[1] + Math.sin(yaw) * approach, p[2]];
          b.seats.push({ seat, front, yaw, desk: f.deskHeight ?? null, occupant: null });
        }
        this.furniture.push(b);
        break;
      }
    }
  }

  clear(): void {
    for (const a of this.actors) this.releaseActor(a);
    this.actors.length = 0;
    for (const g of this.gibs.gibs) this.releaseGib(g);
    this.gibs.clear();
    this.gibUsers.clear();
    this.islandPrev.clear();
    this.furniture.length = 0;
    this.fights.length = 0;
  }

  // ---- services for the brains ------------------------------------------------------------

  goTo(a: Actor, p: Readonly<V3>, speed: number): void {
    a.goal = [p[0], p[1], p[2]];
    a.speed = speed;
    a.path = [];
    a.needPath = true;
    a.stuck = 0;
  }

  stop(a: Actor): void {
    a.goal = null;
    a.path = [];
    a.speed = 0;
    a.needPath = false;
  }

  arrived(a: Actor): boolean {
    return a.goal === null;
  }

  eyes(a: Actor): V3 {
    return a.char.eyes();
  }

  canSee(a: Actor, p: Readonly<V3>): boolean {
    return this.env.lineOfSight(this.eyes(a), p);
  }

  canSeeFrom(eye: Readonly<V3>, p: Readonly<V3>): boolean {
    return this.env.lineOfSight(eye, p);
  }

  noise(n: Noise): void {
    if (!this.settings.ai) return;
    for (const a of this.actors) if (a.char.alive && vdist(a.pos, n.pos) < n.radius) a.brain.hear(a, n, this);
  }

  spreadOf(a: Actor): number {
    const w = a.char.weapon;
    return (w ? WEAPON_STATS[w.kind].spread : 1) * (a.carry === 'hip' ? 1.4 : 1);
  }

  intervalOf(a: Actor): number {
    const w = a.char.weapon;
    return w ? WEAPON_STATS[w.kind].interval : 0.3;
  }

  markHostile(a: Actor): void {
    a.hostile = true;
    // draw the holstered gun
    if (!a.char.weapon && a.weapon && a.weapon.kind !== 'knife') {
      a.char.weapon = a.weapon;
      a.char.motion.weapon = a.weapon;
    }
  }

  /** The nearest hostile (a thug, an armed civilian who shot at soldiers) `a` can see within r. */
  nearestHostile(a: Actor, r: number): Actor | null {
    let best: Actor | null = null;
    let bd = r;
    const eye = this.eyes(a);
    for (const b of this.actors) {
      if (b === a || !b.hostile || !b.char.alive || b.opacity < 0.5 || b.faction === a.faction) continue;
      const d = vdist(a.pos, b.pos);
      if (d >= bd || !this.env.lineOfSight(eye, b.char.eyes())) continue;
      bd = d;
      best = b;
    }
    return best;
  }

  /** Takes the carried weapon in hand (a thug's knife, a civilian's pistol). */
  drawWeapon(a: Actor): void {
    if (!a.char.weapon && a.weapon) {
      a.char.weapon = a.weapon;
      a.char.motion.weapon = a.weapon;
    }
  }

  /** Puts the weapon away (out of sight, still carried). */
  holster(a: Actor): void {
    if (a.faction === 'soldier' || !a.char.weapon) return;
    a.char.weapon = null;
    a.char.motion.weapon = null;
  }

  /** Something worth a glance for `a`: the player close by, people near, a fight (eyes). */
  pointOfInterest(a: Actor): V3 | null {
    const pl = this.player;
    const dp = vdist(a.pos, pl.feet);
    if (pl.alive && dp < 9 && Math.random() < 0.5) return pl.eye();
    let best: Actor | null = null;
    let bw = 0;
    for (const b of this.actors) {
      if (b === a || !b.char.alive || b.opacity < 0.5) continue;
      const d = vdist(a.pos, b.pos);
      if (d > 8) continue;
      const w = (b.brawler ? 5 : 1) * (1 + Math.random()) / (1 + d * 0.3);
      if (w > bw) {
        bw = w;
        best = b;
      }
    }
    return best ? best.char.eyes() : null;
  }

  /** A civilian free to talk within r of a. */
  nearestFree(a: Actor, r: number): Actor | null {
    let best: Actor | null = null;
    let bd = r;
    for (const b of this.actors) {
      if (b === a || b.faction !== 'civilian' || !b.char.alive || b.brawler) continue;
      const s = (b.brain as CivilianBrain).state;
      if (s !== 'stroll' && s !== 'wait') continue;
      if ((b.brain as CivilianBrain).partner) continue;
      const d = vdist(a.pos, b.pos);
      if (d < bd) {
        bd = d;
        best = b;
      }
    }
    return best;
  }

  /** Two civilians start talking (a walks up to b). */
  meet(a: Actor, b: Actor): void {
    const ba = b.brain as CivilianBrain;
    const aa = a.brain as CivilianBrain;
    aa.talkWith(a, b, true);
    ba.talkWith(b, a, false);
    this.stop(b);
    const dir = vnorm([a.pos[0] - b.pos[0], a.pos[1] - b.pos[1], 0]);
    this.goTo(a, [b.pos[0] + dir[0] * 1.1, b.pos[1] + dir[1] * 1.1, b.pos[2]], 1.3);
  }

  freeSeat(p: V3, r: number): Seat | null {
    let best: Seat | null = null;
    let bd = r;
    for (const b of this.furniture)
      for (const s of b.seats) {
        if (s.occupant) continue;
        const d = vdist(p, s.front);
        if (d < bd) {
          bd = d;
          best = s;
        }
      }
    return best;
  }

  claimSeat(s: Seat, a: Actor): void {
    s.occupant = a;
  }

  releaseSeat(s: Seat, a: Actor): void {
    if (s.occupant === a) s.occupant = null;
  }

  /** A fight breaks out between two characters (fists, or a knife if one carries one). */
  startBrawl(a: Actor, b: Actor): void {
    for (const [x, y] of [
      [a, b],
      [b, a],
    ] as const) {
      x.brawler = new Brawler(x.char, { seed: x.id, aggression: 0.4 + Math.random() * 0.4, skill: 0.2 + Math.random() * 0.3 });
      x.brawler.opponent = y.char;
      if (x.weapon?.kind === 'knife') {
        x.char.weapon = x.weapon;
        x.char.motion.weapon = x.weapon;
      }
      if (x.faction === 'civilian') (x.brain as CivilianBrain).brawl(x, y);
      else if (x.faction === 'thug') (x.brain as ThugBrain).brawl(x, y);
      x.talk = null;
      this.stop(x);
    }
    this.fights.push({ a, b, downFor: 0 });
    const mid: V3 = [(a.pos[0] + b.pos[0]) / 2, (a.pos[1] + b.pos[1]) / 2, a.pos[2] + 1.2];
    this.noise({ pos: mid, radius: 14, kind: 'fight', source: a });
  }

  brawling(a: Actor): boolean {
    return a.brawler !== null && this.fights.some((f) => f.a === a || f.b === a);
  }

  endBrawl(a: Actor): void {
    const i = this.fights.findIndex((f) => f.a === a || f.b === a);
    if (i < 0) return;
    const f = this.fights[i]!;
    this.fights.splice(i, 1);
    for (const x of [f.a, f.b]) {
      x.brawler = null;
      x.char.motion.input.guard = false;
      if (x.faction === 'civilian' && x.char.weapon?.kind === 'knife') {
        x.char.weapon = null;
        x.char.motion.weapon = null;
      }
    }
  }

  /** A melee blow from `a` at a target (the player or a character). */
  melee(a: Actor, t: Target, action: string): void {
    const chest = t.kind === 'player' ? this.player.chest() : t.actor.char.pose.p[3]!;
    if (a.char.motion.play(action, [chest[0], chest[1], chest[2]])) a.meleeTarget = t;
  }

  /** A soldier or armed civilian fires one round at `target` with angular spread. */
  fireAt(a: Actor, target: Readonly<V3>, spread: number): boolean {
    const gun = a.char.weapon;
    if (!gun || gun.kind === 'knife') return false;
    if (a.reloading) return false;
    if (a.mag <= 0) {
      this.reload(a);
      return false;
    }
    const eye = this.eyes(a);
    let origin = a.char.muzzle();
    if (!this.env.lineOfSight(eye, origin)) origin = eye;
    const d = vnorm(vsub(target, origin));
    const range = vdist(target, origin);
    for (const b of this.actors) {
      if (b === a || !b.char.alive || b.faction !== a.faction) continue;
      const c = vadd(b.pos, [0, 0, 1.1]);
      const t = (c[0] - origin[0]) * d[0] + (c[1] - origin[1]) * d[1] + (c[2] - origin[2]) * d[2];
      if (t < 0 || t > range) continue;
      if (vdist(c, vadd(origin, vscale(d, t))) < 0.7) return false;
    }
    const dir = jitter(d, spread);
    a.char.fire();
    a.mag--;
    this.shotsFired++;
    this.hooks.muzzleFlash(a.char.muzzle(), dir);
    this.noise({ pos: origin, radius: 45, kind: 'shot', source: a });
    const st = WEAPON_STATS[gun.kind];
    const end = this.resolveShot(origin, dir, 180, a, st.damage, st.carve * this.settings.roundCarve);
    this.hooks.tracer(origin, end);
    return true;
  }

  /** Reloads the held gun (the action; the magazine refills when it says so). */
  reload(a: Actor): void {
    const gun = a.char.weapon;
    if (!gun || a.reloading) return;
    if (a.char.motion.play(gun.kind === 'pistol' ? 'reloadPistol' : 'reloadRifle')) a.reloading = true;
  }

  /**
   * One hitscan round from origin along dir: the first character, the player (unless the
   * shooter is the player) or the world. Returns the end point.
   */
  resolveShot(origin: V3, dir: V3, maxDist: number, shooter: Actor | null, damage: number, carveRadius: number): V3 {
    const actorHit = this.raycast(origin, dir, maxDist, shooter);
    const worldT = this.env.raycast(origin, dir, maxDist);
    let best = actorHit ? actorHit.hit.distance : Infinity;
    let playerT = Infinity;
    if (shooter && this.player.alive) {
      playerT = rayCapsule(origin, dir, vadd(this.player.feet, [0, 0, 0.35]), vsub(this.player.eye(), [0, 0, 0.1]), 0.32);
      if (playerT < best) best = playerT;
    }
    const wt = worldT >= 0 ? worldT : Infinity;
    if (wt < best) {
      const p = vadd(origin, vscale(dir, wt));
      const inside = vadd(p, vscale(dir, this.env.h * 0.5));
      if (carveRadius > 0) {
        this.hooks.carve(inside, carveRadius);
        this.worldHits++;
      }
      this.hooks.impact(p, faceNormal(p, dir, this.env.h));
      // a round smacking in close by makes people flinch
      for (const b of this.actors) if (b !== shooter && b.char.alive && vdist(b.char.pose.p[3]!, p) < 3.5) b.char.perceive({ point: p, strength: 1, kind: 'impact' });
      this.noise({ pos: p, radius: 10, kind: 'impact', source: shooter });
      this.whiz(origin, p, shooter, null);
      return p;
    }
    if (playerT <= best && playerT < Infinity && shooter) {
      if (this.settings.playerDamage) this.hooks.hurtPlayer(damage * (0.28 + Math.random() * 0.12), origin);
      const end = vadd(origin, vscale(dir, playerT));
      this.whiz(origin, end, shooter, null);
      return end;
    }
    if (actorHit) {
      this.whiz(origin, actorHit.hit.point, shooter, actorHit.actor);
      this.wound(actorHit.actor, actorHit.hit, dir, damage, shooter ? shooter.pos : null, 0.045, shooter);
      return actorHit.hit.point;
    }
    const end = vadd(origin, vscale(dir, maxDist));
    this.whiz(origin, end, shooter, null);
    return end;
  }

  /** A round passing close to people's heads makes them flinch (the crack of a near miss). */
  private whiz(from: V3, to: V3, shooter: Actor | null, hit: Actor | null): void {
    const d = vsub(to, from);
    const l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (l2 < 1e-6) return;
    for (const b of this.actors) {
      if (b === shooter || b === hit || !b.char.alive || b.opacity < 0.5) continue;
      const e = b.char.eyes();
      const t = Math.max(0, Math.min(1, ((e[0] - from[0]) * d[0] + (e[1] - from[1]) * d[1] + (e[2] - from[2]) * d[2]) / l2));
      const c: V3 = [from[0] + d[0] * t, from[1] + d[1] * t, from[2] + d[2] * t];
      if (vdist(c, e) < 1.1) b.char.perceive({ point: c, strength: 0.9, kind: 'whiz' });
    }
  }

  // ---- damage -------------------------------------------------------------------------------

  raycast(origin: Readonly<V3>, dir: Readonly<V3>, maxDist: number, skip: Actor | null = null): { actor: Actor; hit: CharacterHit } | null {
    let best: { actor: Actor; hit: CharacterHit } | null = null;
    for (const a of this.actors) {
      if (a === skip || a.opacity < 0.5) continue;
      const hit = a.char.raycast(origin, dir, best ? best.hit.distance : maxDist);
      if (hit && (!best || hit.distance < best.hit.distance)) best = { actor: a, hit };
    }
    return best;
  }

  /** A round in a character: wound, blood, severed pieces, death. */
  wound(a: Actor, hit: CharacterHit, dir: V3, damage: number, from: V3 | null, radius = 0.045, by: Actor | null = null): WoundResult {
    const wasAlive = a.char.alive;
    const r = a.char.wound(hit, dir, damage, radius, 2.8);
    this.bleed(a, hit.point, dir, r.removed, r.headshot ? 2 : 1);
    for (const g of r.gibs) this.spawnGib(g, a, 30);
    if (wasAlive && r.killed) this.onDeath(a, dir);
    else if (wasAlive && this.settings.ai) a.brain.hurt(a, from, this, by);
    return r;
  }

  private bleed(a: Actor, point: V3, dir: V3, removed: { slot: number }[], amount: number): void {
    this.gibs.spray(point, dir, 6 + Math.min(18, removed.length >> 1), 3.2, 0.5);
    this.gibs.spray(point, vscale(dir, -1), 3, 1.6, 0.9);
    this.hooks.bloodMist(point, dir, amount);
    const pal = a.look.palette;
    for (let i = 0; i < removed.length; i += 4) {
      const c = pal[removed[i]!.slot] ?? BLOOD;
      this.gibs.spray(point, dir, 1, 2.8, 0.7, [c[0], c[1], c[2]]);
    }
  }

  blast(pos: V3, radius: number, strength = 1): void {
    const thrown = new Set<Actor>();
    for (const a of this.actors) {
      if (a.opacity < 0.5) continue;
      const wasAlive = a.char.alive;
      const r = a.char.blast(pos, radius, strength);
      if (r.damage <= 0 && !r.gibbed) continue;
      thrown.add(a);
      if (r.gibbed) {
        const c = a.char.bounds().center;
        this.gibs.spray(c, [0, 0, 1], 60, 5, 1.2);
        this.hooks.bloodMist(c, [0, 0, 1], 4);
        for (const g of r.gibs) this.spawnGib(g, a, 40);
        a.opacity = 0;
      }
      if (!r.gibbed && r.damage > 15) {
        const c = a.char.bounds().center;
        this.gibs.spray(c, vnorm(vsub(c, pos), [0, 0, 1]), Math.min(40, Math.round(r.damage / 6)), 3, 1);
        this.hooks.bloodMist(c, [0, 0, 1], 1);
      }
      if (wasAlive && !a.char.alive) this.onDeath(a, vnorm(vsub(a.pos, pos)));
      else if (wasAlive && this.settings.ai) a.brain.hurt(a, pos, this, null);
    }
    // the blast's push: the living stagger away from it (those it hurt were thrown by it
    // already), everyone flinches from it
    for (const a of this.actors) {
      if (!a.char.alive || a.opacity < 0.5) continue;
      const c = a.char.pose.p[3]!;
      const d = vdist(c, pos);
      const reach = radius * 4 + 2;
      if (d > reach * 1.5) continue;
      const push = strength * 2.6 * (1 - d / reach);
      a.char.perceive({ point: pos, strength: 1.2 * strength, kind: 'blast' });
      if (push > 0.3 && !thrown.has(a) && !a.char.down) a.char.push([c[0] - pos[0], c[1] - pos[1], 0], push);
    }
    this.gibs.impulse(pos, radius * 4, 11 * strength);
    this.noise({ pos, radius: 70, kind: 'explosion', source: null });
  }

  private onDeath(a: Actor, dir: V3): void {
    this.kills++;
    a.diedAt = this.time;
    const w = a.char.dropWeapon();
    if (w) {
      w.vel = vadd(w.vel, vscale(dir, 1.5));
      this.spawnGib(w, a, 0);
    }
    this.endBrawl(a);
    this.noise({ pos: a.pos, radius: 25, kind: 'death', source: a });
  }

  private spawnGib(spec: GibSpec, a: Actor, bleed: number): void {
    const shared = spec.prop || this.cast.isProp(spec.part);
    const mesh = shared ? this.cast.mesh(propModelOf(this.cast, spec.part)) : this.cast.partMesh(spec.part, spec.voxelSize);
    const user: GibUser = { mesh, paletteId: a.look.paletteId, skin: new Float32Array(16), shared };
    const g = this.gibs.spawn(spec.part, spec.voxelSize, spec.bonePos, spec.boneRot, spec.boneRestHead, spec.vel, spec.ang, user);
    g.bleed = bleed;
    this.gibUsers.add(g);
  }

  private releaseGib(g: Gib): void {
    const u = g.user as GibUser | undefined;
    if (u && !u.shared) this.cast.releasePart(g.part);
  }

  private releaseActor(a: Actor): void {
    if (a.own?.mesh) this.renderer.releaseMesh(a.own.mesh);
    a.own = null;
  }

  // ---- simulation ---------------------------------------------------------------------------

  private readonly debrisSpheres: Obstacle[] = [];
  /** Time the last updates took (ms, smoothed), and how many bodies were simulated. */
  updateMs = 0;
  bodies = 0;

  update(dt: number): void {
    const t0 = performance.now();
    this.updateInner(dt);
    this.updateMs += (performance.now() - t0 - this.updateMs) * 0.05;
    let n = 0;
    for (const a of this.actors) if (a.char.behaviours.physical && !a.char.asleep) n++;
    this.bodies = n;
  }

  private updateInner(dt: number): void {
    this.time += dt;
    this.retroClock += dt;
    let retroTick = false;
    if (this.retroClock >= RETRO_STEP) {
      this.retroClock %= RETRO_STEP;
      retroTick = true;
    }
    this.planPaths(3);
    this.crushByDebris(dt);
    this.updateFights(dt);
    this.assignPhysics();
    // bodies bump into each other, feet catch on the dead and on loose pieces lying about
    this.debrisSpheres.length = 0;
    for (const g of this.gibs.gibs) if (g.radius < 0.35) this.debrisSpheres.push({ c: g.pos, r: g.radius * 0.7 });
    // the player's body: people walked into give way, stumble, or go down when run into hard
    const pl = this.player;
    if (pl.alive && pl.threat) {
      const f = pl.feet;
      for (const z of [0.58, 0.95, 1.3]) this.debrisSpheres.push({ c: [f[0], f[1], f[2] + z], r: 0.28 });
    }
    gatherObstacles(this.actors.filter((a) => a.opacity >= 0.5).map((a) => a.char), 2.2, this.debrisSpheres);
    for (const a of this.actors) {
      const an = a.char.motion;
      if (a.char.alive) {
        if (this.settings.ai) {
          a.thinkAcc += dt;
          if (a.thinkAcc >= 0.1) {
            a.brain.think(a, this, a.thinkAcc);
            a.thinkAcc = 0;
          }
          a.brain.tick(a, this, dt);
          a.brawler?.update(dt);
        }
        this.move(a, dt);
        // now and then a foot catches (besides what the feet really catch on): running in a
        // panic most, rarely otherwise, hardly ever a soldier
        const hs = Math.hypot(a.vel[0], a.vel[1]);
        if (hs > 1.2 && a.onGround && !an.busy && an.stance === 'stand' && !a.brawler && !a.char.controlled) {
          const perSecond = hs > 2.6 ? (a.mood === 'panic' ? 1 / 120 : 1 / 400) : 1 / 1500;
          if (Math.random() < (a.faction === 'soldier' ? 0.15 : 1) * perSecond * dt) a.char.trip();
        }
        const inp = an.input;
        inp.crouch = a.crouch;
        inp.carry = a.carry;
        inp.mood = a.mood;
        inp.stance = a.stance;
        inp.seat = a.seat;
        inp.groundVariant = a.groundVariant;
        inp.talk = a.talk;
        inp.lean = a.lean;
        const aiming = a.carry === 'aim' || a.carry === 'hip';
        inp.aimAt = aiming ? (a.aimTarget ?? a.face) : null;
        if (!a.brawler) inp.lookAt = aiming ? null : (a.lookAt ?? a.face);
        inp.airborne = !a.onGround && a.vel[2] < -2.5;
        a.char.setRoot(a.pos, a.yaw);
      }
      if (this.settings.retro && a.char.alive) {
        // (the set of what it holds now: civilians draw weapons)
        const w = a.char.weapon;
        if (!a.char.retro || a.char.retro.weapon !== (w?.kind ?? null) || a.char.retro.voxelSize !== this.settings.retroVoxel) a.char.retro = this.cast.retroSet(a.look.model, w, this.settings.retroVoxel) ?? a.char.retro;
      } else if (!this.settings.retro) a.char.retro = null;
      if (!(a.char.asleep && a.char.deadTime > 3)) a.char.update(dt);
      if (!this.settings.retro || a.char.retroFrame || retroTick) a.drawSkin.set(a.char.skin);
      this.handleEvents(a);
      if (a.reloading && !an.busy) {
        // interrupted (a hit): the magazine goes in anyway
        a.reloading = false;
        if (a.char.weapon) a.mag = WEAPON_STATS[a.char.weapon.kind].mag;
      }
    }
    this.cast.pump();
    this.gibs.update(dt);
    const live = new Set(this.gibs.gibs);
    for (const g of this.gibUsers) {
      if (!live.has(g)) {
        this.releaseGib(g);
        this.gibUsers.delete(g);
      } else if (!this.settings.retro || retroTick) this.gibs.writeSkin(g, (g.user as GibUser).skin);
    }
    this.fadeCorpses(dt);
  }

  /**
   * The player's knife: a cut into whoever is within reach in front (on the line of sight, or a
   * little off it). Returns whether it cut someone.
   */
  playerMelee(origin: V3, dir: V3, reach = 1.9): boolean {
    let target: { a: Actor; point: V3 } | null = null;
    const hit = this.raycast(origin, dir, reach);
    if (hit) target = { a: hit.actor, point: hit.hit.point };
    else {
      let best = Infinity;
      for (const a of this.actors) {
        if (!a.char.alive || a.opacity < 0.5) continue;
        const c = a.char.pose.p[3]!;
        const v = vsub(c, origin);
        const d = Math.hypot(v[0], v[1], v[2]);
        if (d > reach + 0.2 || d < 1e-3) continue;
        if ((v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2]) / d < 0.8) continue;
        if (d < best) {
          best = d;
          target = { a, point: [c[0], c[1], c[2] + 0.1] };
        }
      }
    }
    this.noise({ pos: [origin[0], origin[1], origin[2]], radius: 6, kind: 'fight', source: null });
    if (!target) return false;
    const { a, point } = target;
    const wasAlive = a.char.alive;
    const r = a.char.melee(point, dir, 'blade', 1.3);
    if (wasAlive) this.bleed(a, point, dir, r.removed.slice(0, 10), 1);
    for (const g of r.gibs) this.spawnGib(g, a, 30);
    if (wasAlive && r.killed) this.onDeath(a, dir);
    else if (wasAlive && this.settings.ai) a.brain.hurt(a, this.player.feet, this, null);
    return true;
  }

  /** Animation events: reloads done, blows landing. */
  private handleEvents(a: Actor): void {
    const events = a.char.takeEvents();
    if (events.length === 0) return;
    for (const e of events) {
      if (e.name === 'reloaded') {
        a.reloading = false;
        if (a.char.weapon) a.mag = WEAPON_STATS[a.char.weapon.kind].mag;
      }
    }
    if (a.brawler) {
      for (const blow of a.brawler.resolve(events)) {
        const v = this.actors.find((x) => x.char === blow.victim);
        if (!v) continue;
        if (!blow.blocked && (blow.kind === 'blade' || (blow.result.zone === 'head' && blow.result.damage > 18))) this.bleed(v, blow.point, blow.dir, blow.result.removed.slice(0, 8), 1);
        for (const g of blow.result.gibs) this.spawnGib(g, v, 30);
        if (blow.result.killed) this.onDeath(v, blow.dir);
      }
      return;
    }
    const t = a.meleeTarget;
    if (!t) return;
    for (const e of events) {
      if (e.name !== 'strike') continue;
      a.meleeTarget = null;
      const force = MELEE_FORCE[e.action.replace('.m', '')] ?? 1;
      const dir = vnorm(vsub(e.target ?? e.pos, a.char.pose.p[3]!));
      if (t.kind === 'player') {
        const chest = this.player.chest();
        if (vdist(e.pos, chest) < 0.6 || vdist(e.pos, this.player.eye()) < 0.5) {
          const blade = BLADE_ACTIONS.has(e.action.replace('.m', ''));
          if (this.settings.playerDamage) this.hooks.hurtPlayer((blade ? 13 : 9) * force, a.pos);
          if (blade) this.hooks.bloodMist(e.pos, dir, 1);
          this.hooks.pushPlayer([dir[0] * (blade ? 1 : 3) * force, dir[1] * (blade ? 1 : 3) * force, 0.4]);
        }
      } else {
        const v = t.actor;
        const bone = v.char.nearestBone(e.pos);
        const bp = v.char.pose.p[bone]!;
        if (vdist(e.pos, bp) < 0.45) {
          const blade = BLADE_ACTIONS.has(e.action.replace('.m', '')) && a.char.weapon?.kind === 'knife';
          const r = v.char.melee(e.pos, dir, blade ? 'blade' : 'blunt', force);
          if (blade) this.bleed(v, e.pos, dir, r.removed.slice(0, 8), 1);
          for (const g of r.gibs) this.spawnGib(g, v, 30);
          if (r.killed) this.onDeath(v, dir);
          else if (this.settings.ai) v.brain.hurt(v, a.pos, this, a);
        }
      }
    }
  }

  private updateFights(dt: number): void {
    for (let i = this.fights.length - 1; i >= 0; i--) {
      const f = this.fights[i]!;
      const out = f.a.char.knockedOut || f.b.char.knockedOut;
      f.downFor = out ? f.downFor + dt : 0;
      // over when someone is dead or knocked out (a knockdown alone: they get up and go on)
      if (!f.a.char.alive || !f.b.char.alive || f.downFor > 1.5) this.endBrawl(f.a);
      else if (Math.random() < dt * 0.4) this.noise({ pos: f.a.pos, radius: 12, kind: 'fight', source: f.a });
    }
  }

  /** Path searches for this frame: at most `budget`, and stop once `ms` are spent (at least one). */
  private planPaths(budget: number, ms = 2.5): void {
    const n = this.actors.length;
    const t0 = performance.now();
    for (let k = 0; k < n && budget > 0; k++) {
      const a = this.actors[(this.pathCursor + k) % n]!;
      if (!a.needPath || !a.goal || !a.char.alive) continue;
      if (performance.now() - t0 > ms) break;
      a.needPath = false;
      budget--;
      const p = this.nav.findPath(a.pos, a.goal, 2500, a.crouch > 0.5 ? 1.2 : 1.7);
      a.path = p ? p.points : [];
      if (p && p.partial && a.path.length > 0) a.goal = a.path[a.path.length - 1]!;
      if (!p) a.goal = null;
    }
    this.pathCursor = (this.pathCursor + 1) % Math.max(1, n);
  }

  private move(a: Actor, dt: number): void {
    const an = a.char.motion;
    const settled = an.stance;
    // the body leads (staggering, falling, down, getting up): the character follows it
    const led = a.char.controlled;
    // the body cannot walk while sitting, kneeling, down or getting up; prone crawls
    const locked = led || an.transitioning || settled === 'sit' || settled === 'ground' || settled === 'kneel' || settled === 'down' || a.stance !== 'stand' && a.stance !== 'prone';
    // where to go and how fast: along the path, aiming a little ahead (corners are cut, not
    // turned on the spot), braking into the goal; a fighter's footwork from the Brawler
    let wantDir: [number, number] | null = null;
    let wantSpeed = 0;
    const hs = Math.hypot(a.vel[0], a.vel[1]);
    if (a.brawler && !locked) {
      const m = a.brawler.move;
      const l = Math.hypot(m[0], m[1]);
      if (l > 1e-3) {
        wantDir = [m[0] / l, m[1] / l];
        wantSpeed = l;
      }
    } else if (a.goal && !locked) {
      while (a.path.length > 0 && Math.hypot(a.path[0]![0] - a.pos[0], a.path[0]![1] - a.pos[1]) < 0.45) a.path.shift();
      const { point, remaining } = pursue(a.pos, a.path, a.goal, 0.6 + 0.3 * hs);
      const d = Math.hypot(point[0] - a.pos[0], point[1] - a.pos[1]);
      if (remaining < 0.3) a.goal = null;
      else if (d > 1e-3) {
        const inj = a.char.behaviours.injuries;
        const limp = Math.max(inj.legL, inj.legR);
        // (people slow down on stairs, more going up than down)
        const sl = an.slope;
        const climb = sl > 0 ? Math.min(1, sl / 0.33) * 0.5 : Math.min(1, -sl / 0.33) * 0.3;
        let s = a.speed * (1 - 0.5 * limp) * (1 - 0.3 * inj.pain) * a.pace * (1 - climb);
        if (settled === 'prone') s = Math.min(s, 0.45);
        // arriving: slow down to stop on the spot
        s = Math.min(s, Math.sqrt(2 * 3.6 * 0.8 * Math.max(0, remaining - 0.2)) + 0.15);
        wantDir = [(point[0] - a.pos[0]) / d, (point[1] - a.pos[1]) / d];
        // setting off away from where the body faces: it turns first, then walks
        if (!a.face && hs < 0.6 && Math.abs(wrap(Math.atan2(wantDir[1], wantDir[0]) - a.yaw)) > 1.1) s = Math.min(s, 0.3);
        wantSpeed = s;
      }
    }
    // keep apart (not the two in a fight)
    let sepX = 0, sepY = 0;
    for (const b of this.actors) {
      if (b === a || !b.char.alive || locked) continue;
      if (a.brawler && a.brawler.opponent === b.char) continue;
      const ox = a.pos[0] - b.pos[0], oy = a.pos[1] - b.pos[1];
      const d2 = ox * ox + oy * oy;
      if (d2 > 4 || d2 < 1e-6 || Math.abs(a.pos[2] - b.pos[2]) > 1) continue;
      const d = Math.sqrt(d2);
      // (bodies are shoulders wide: people keep a little space between them)
      if (d < 0.8) {
        const push = (0.8 - d) * (d < 0.55 ? 9 : 4);
        sepX += (ox / d) * push;
        sepY += (oy / d) * push;
      }
      // and they see each other coming: where the two will be in a moment, a step aside now
      const rvx = a.vel[0] - b.vel[0], rvy = a.vel[1] - b.vel[1];
      const rv2 = rvx * rvx + rvy * rvy;
      if (rv2 > 0.25) {
        const t = clamp(-(ox * rvx + oy * rvy) / rv2, 0, 0.6);
        const px = ox + rvx * t, py = oy + rvy * t;
        const pd = Math.hypot(px, py);
        if (t > 0 && pd < 0.75) {
          // (sideways to the closing motion, the way the miss already leans)
          const side = pd > 1e-3 ? 1 / pd : 0;
          const k = (0.75 - pd) * 5 * (1 - t / 0.6);
          sepX += (pd > 1e-3 ? px * side : -rvy / Math.sqrt(rv2)) * k;
          sepY += (pd > 1e-3 ? py * side : rvx / Math.sqrt(rv2)) * k;
        }
      }
    }
    // the body's move this frame when it leads
    const rm = a.char.takeRootMotion();
    if (led) {
      a.vel[0] = dt > 0 ? rm[0] / dt : 0;
      a.vel[1] = dt > 0 ? rm[1] / dt : 0;
    } else if (a.onGround) {
      // the body's own momentum: it speeds up, brakes and changes heading within limits
      const v = steer([a.vel[0], a.vel[1]], wantDir ?? [Math.cos(a.yaw), Math.sin(a.yaw)], wantDir ? wantSpeed : 0, dt, steerOptions(a.brawler ? 3 : wantSpeed || a.speed));
      const k = 1 - Math.exp(-6 * dt);
      a.vel[0] = v[0] + sepX * k;
      a.vel[1] = v[1] + sepY * k;
    }
    const dx = wantDir ? wantDir[0] * wantSpeed : 0, dy = wantDir ? wantDir[1] * wantSpeed : 0;
    a.vel[2] = Math.max(-40, a.vel[2] - 20 * dt);
    const height = settled === 'prone' || settled === 'ground' || settled === 'down' ? 0.5 : a.crouch > 0.5 || settled === 'kneel' || settled === 'sit' ? 1.2 : 1.72;
    const r = 0.24;
    const min: V3 = [a.pos[0] - r, a.pos[1] - r, a.pos[2]];
    const max: V3 = [a.pos[0] + r, a.pos[1] + r, a.pos[2] + height];
    const want: V3 = led ? [rm[0], rm[1], a.vel[2] * dt] : [a.vel[0] * dt, a.vel[1] * dt, a.vel[2] * dt];
    const lift = this.env.occupancy.ready ? this.env.occupancy.depenetrate(min, max, 1) : 0;
    if (lift !== null && lift > 0) {
      a.pos[2] += lift;
      min[2] += lift;
      max[2] += lift;
    }
    const res = this.env.moveBox(min, max, want, 0.55, a.onGround);
    const fell = a.vel[2];
    a.pos[0] += res.move[0];
    a.pos[1] += res.move[1];
    a.pos[2] += res.move[2];
    if (Math.abs(res.move[0] - want[0]) > 1e-5 && res.stepped === 0) a.vel[0] = 0;
    if (Math.abs(res.move[1] - want[1]) > 1e-5 && res.stepped === 0) a.vel[1] = 0;
    a.onGround = res.onGround;
    if (a.onGround && a.vel[2] < 0) {
      a.vel[2] = 0;
      if (fell < -15) {
        a.char.die();
        this.onDeath(a, [0, 0, -1]);
        return;
      }
    }
    const wanted = Math.hypot(dx, dy);
    const got = Math.hypot(res.move[0], res.move[1]) / Math.max(dt, 1e-4);
    if (a.goal && wanted > 0.3 && got < wanted * 0.25 && !locked) a.stuck += dt;
    else a.stuck = Math.max(0, a.stuck - dt);
    if (a.stuck > 0.8 && a.goal) {
      a.needPath = true;
      if (a.stuck > 2.5) this.stop(a);
    }
    // facing: the body turns with angular momentum (speeding up and braking into the new
    // direction); standing and aiming, the trunk takes the small corrections and the body only
    // turns for large ones
    let wantYaw: number | null = null;
    let maxRate = 3.2;
    let accel = 9;
    const hsNow = Math.hypot(a.vel[0], a.vel[1]);
    if (led) {
      // (the body's heading)
      a.yaw = a.char.motion.rootYaw;
      a.yawRate = 0;
    } else if (a.brawler) {
      wantYaw = a.brawler.yaw;
      maxRate = 5;
      accel = 18;
    } else if (a.face) {
      wantYaw = Math.atan2(a.face[1] - a.pos[1], a.face[0] - a.pos[0]);
      maxRate = hsNow > 2.5 ? 1.8 : 2.6;
      if (hsNow < 0.3 && (a.carry === 'aim' || a.carry === 'hip')) {
        const off = Math.abs(wrap(wantYaw - a.yaw));
        if (!a.turning && off < 0.35) wantYaw = null;
        a.turning = off > 0.05 && (a.turning || off >= 0.35);
      } else a.turning = false;
    } else if (wantDir && !locked) wantYaw = Math.atan2(wantDir[1], wantDir[0]);
    else if (hsNow > 0.3 && !locked) wantYaw = Math.atan2(a.vel[1], a.vel[0]);
    if (wantYaw !== null && !(locked && settled !== 'kneel' && settled !== 'prone')) {
      const t = turn(a.yaw, a.yawRate, wantYaw, dt, maxRate * (settled === 'prone' ? 0.3 : 1), accel);
      a.yaw = t.yaw;
      a.yawRate = t.rate;
    } else a.yawRate *= Math.exp(-12 * dt);
    if (a.pos[2] < -60) {
      a.char.die();
      a.opacity = 0;
    }
  }

  /**
   * Physics for the calm, by distance to the player: the nearest bodies in view are simulated
   * all the time (their secondary motion, ready to be hit), the rest run on their motion plans
   * until something happens to them (a hit, a shove, a fall, death wake the body at once).
   */
  private assignPhysics(): void {
    const feet = this.player.feet;
    const near: { a: Actor; d: number }[] = [];
    for (const a of this.actors) {
      if (!a.char.alive) continue;
      const d = vdist(a.pos, feet);
      if (d < 32) near.push({ a, d });
      else a.char.physics = false;
    }
    near.sort((x, y) => x.d - y.d);
    near.forEach((n, i) => (n.a.char.physics = i < PHYSICS_BUDGET));
  }

  private crushByDebris(dt: number): void {
    if (!this.islands || dt <= 0) return;
    const seen = new Set<number>();
    for (const isl of this.islands.list) {
      seen.add(isl.id);
      const prev = this.islandPrev.get(isl.id);
      const pos: V3 = [isl.position[0], isl.position[1], isl.position[2]];
      this.islandPrev.set(isl.id, { pos, t: this.time });
      if (!prev || isl.opacity <= 0 || isl.radius < 0.2 || isl.radius > 4) continue;
      const span = this.time - prev.t;
      if (span <= 0) continue;
      const v = vscale(vsub(pos, prev.pos), 1 / span);
      const speed = Math.hypot(v[0], v[1], v[2]);
      if (speed < 3.5) continue;
      // (how hard it hits: a lump of rubble strikes a blow, a slab crushes)
      const mass = 900 * isl.radius ** 3;
      const momentum = mass * speed;
      for (const a of this.actors) {
        if (a.opacity < 0.5) continue;
        const c = a.char.bounds().center;
        const d = vdist(c, pos);
        // falling close by: a flinch, the arms up
        if (a.char.alive && d < isl.radius + 3 && d > isl.radius * 0.75 + 0.35) a.char.perceive({ point: pos, strength: clamp(0.4 + isl.radius, 0.4, 1.2), kind: 'impact' });
        if (d > isl.radius * 0.75 + 0.35) continue;
        if (!a.char.alive) {
          a.char.impulse(c, vscale(v, 0.5));
          continue;
        }
        if (isl.radius < 0.7 && momentum < 900) {
          // struck where it meets the body, knocked along its way
          const dir = vnorm(v);
          const at = vadd(c, vscale(vnorm(vsub(pos, c)), 0.2));
          at[2] = Math.max(at[2], c[2] + 0.2 * Math.max(0, pos[2] - c[2]));
          const wasAlive = a.char.alive;
          a.char.melee(at, dir, 'blunt', clamp(momentum / 130, 0.5, 4));
          a.char.health -= momentum / 14;
          a.char.push([dir[0], dir[1], 0], Math.min(4, momentum / 90));
          if (a.char.health <= 0 && wasAlive) {
            a.char.die(c, vscale(v, 0.3));
            this.onDeath(a, dir);
          } else if (this.settings.ai) a.brain.hurt(a, pos, this, null);
          this.hooks.bloodMist(at, vscale(dir, -1), 0.6);
          continue;
        }
        a.char.die(c, vscale(v, 0.6));
        this.gibs.spray(c, [0, 0, 1], 25, 3, 1.2);
        this.hooks.bloodMist(c, [0, 0, 1], 2);
        this.onDeath(a, vnorm(v));
      }
      this.gibs.impulse(pos, isl.radius + 0.5, speed * 0.4);
    }
    for (const id of this.islandPrev.keys()) if (!seen.has(id)) this.islandPrev.delete(id);
  }

  private fadeCorpses(dt: number): void {
    const dead = this.actors.filter((a) => !a.char.alive);
    const extra = dead.length - this.settings.maxCorpses;
    dead.sort((x, y) => x.diedAt - y.diedAt);
    for (let i = 0; i < dead.length; i++) {
      const a = dead[i]!;
      if (i < extra) a.opacity = Math.max(0, a.opacity - dt * 0.5);
    }
    for (let i = this.actors.length - 1; i >= 0; i--) {
      const a = this.actors[i]!;
      if (a.opacity <= 0 && !a.char.alive) {
        this.releaseActor(a);
        this.actors.splice(i, 1);
      }
    }
  }

  // ---- rendering ----------------------------------------------------------------------------

  draw(): void {
    const cc = this.renderer;
    cc.begin();
    const light = this.settings.light;
    const furniturePalette = cc.palette(FURNITURE_PALETTE);
    for (const b of this.furniture) cc.add(this.cast.mesh(b.furniture.model), b.skin, 1, furniturePalette, { center: [b.pos[0], b.pos[1], b.pos[2] + 0.5], radius: 1.2, light });
    for (const a of this.actors) {
      if (a.opacity <= 0) continue;
      const ch = a.char;
      const tint: [number, number, number, number] = [1, 0.15, 0.1, ch.flash * 0.45];
      const b = ch.bounds();
      if (ch.retroFrame && ch.alive) {
        const skin = a.propSkin;
        ch.writeRetroSkin(skin);
        cc.add(this.cast.mesh(ch.retroFrame), skin, 1, a.look.paletteId, { center: b.center, radius: 1.3, tint, opacity: a.opacity, light });
      } else {
        cc.add(this.meshOf(a), a.drawSkin, ch.model.skeleton.count, a.look.paletteId, { center: b.center, radius: b.radius + 0.3, tint, opacity: a.opacity, light });
        if (ch.weapon && ch.alive) {
          ch.writePropSkin(a.propSkin);
          cc.add(this.cast.mesh(ch.weapon.model), a.propSkin, 1, a.look.paletteId, { center: ch.weaponPos, radius: 0.7, opacity: a.opacity, light });
        }
      }
      if (ch.alive && ch.motion.stance === 'stand') cc.decal([a.pos[0], a.pos[1], a.pos[2] + 0.004], [0, 0, 1], 0.4, [0, 0, 0, 0.5 * a.opacity], 0);
      else if (ch.asleep && a.opacity > 0.5) {
        const p = ch.pose.p[3]!;
        const grow = Math.min(1, (ch.deadTime - 1) / 8);
        const g = this.env.groundHeight(p[0], p[1], p[2] + 0.3, p[2] - 0.8);
        if (grow > 0 && g !== null) cc.decal([p[0], p[1], g], [0, 0, 1], 0.2 + 0.45 * grow, [BLOOD[0] * 0.7, BLOOD[1], BLOOD[2], 0.85 * a.opacity], 1);
      }
    }
    for (const g of this.gibUsers) {
      const u = g.user as GibUser;
      cc.add(u.mesh, u.skin, 1, u.paletteId, { center: g.pos, radius: g.radius + 0.05, light });
    }
    this.gibs.forEachDrop((p, size, c) => cc.bit(p, size, [0, 0, 0, 1], c, light));
    this.gibs.forEachStain((p, n, size, age, c) => cc.decal(p, n, Math.max(0.045, size * 2.4), [c[0] * 0.8, c[1] * 0.8, c[2] * 0.8, Math.min(0.92, 0.5 + age)], 1));
  }

  private meshOf(a: Actor): GpuCharacterMesh {
    const ch = a.char;
    if (!ch.ownsModel) return this.cast.mesh(ch.model);
    a.own ??= { mesher: new ModelMesher(), mesh: null, version: -1 };
    if (a.own.version !== ch.geometryVersion || !a.own.mesh) {
      if (a.own.mesh) this.renderer.releaseMesh(a.own.mesh);
      a.own.mesh = this.renderer.uploadMesh(a.own.mesher.mesh(ch.model), `actor ${a.id}`);
      a.own.version = ch.geometryVersion;
    }
    return a.own.mesh;
  }

  stats(): { civilians: number; soldiers: number; thugs: number; dead: number; gibs: number; stains: number; bakes: number; fights: number; talking: number; sitting: number } {
    let civilians = 0, soldiers = 0, thugs = 0, dead = 0, talking = 0, sitting = 0;
    for (const a of this.actors) {
      if (!a.char.alive) dead++;
      else if (a.faction === 'soldier') soldiers++;
      else if (a.faction === 'thug') thugs++;
      else civilians++;
      if (a.char.alive && a.talk) talking++;
      if (a.char.alive && a.char.motion.stance === 'sit') sitting++;
    }
    return { civilians, soldiers, thugs, dead, gibs: this.gibs.gibs.length, stains: this.gibs.stains.length, bakes: this.cast.pendingBakes, fights: this.fights.length, talking, sitting };
  }
}

/** The shared prop model a part belongs to. */
function propModelOf(cast: Cast, part: import('svx-anim').VoxelPart): import('svx-anim').VoxelModel {
  for (const p of Object.values(cast.props)) if (p.model.parts[0] === part) return p.model;
  return cast.props.rifle.model;
}

function jitter(d: V3, spread: number): V3 {
  const up: V3 = Math.abs(d[2]) < 0.95 ? [0, 0, 1] : [1, 0, 0];
  const r = vnorm([d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]]);
  const u: V3 = [r[1] * d[2] - r[2] * d[1], r[2] * d[0] - r[0] * d[2], r[0] * d[1] - r[1] * d[0]];
  const g = (): number => (Math.random() + Math.random() + Math.random() - 1.5) / 1.5;
  const x = g() * spread, y = g() * spread;
  return vnorm([d[0] + r[0] * x + u[0] * y, d[1] + r[1] * x + u[1] * y, d[2] + r[2] * x + u[2] * y]);
}

function rayCapsule(o: V3, d: V3, a: V3, b: V3, r: number): number {
  let best = Infinity;
  for (let i = 0; i <= 6; i++) {
    const p = vadd(a, vscale(vsub(b, a), i / 6));
    const w = vsub(p, o);
    const t = w[0] * d[0] + w[1] * d[1] + w[2] * d[2];
    if (t < 0) continue;
    const c = vadd(o, vscale(d, t));
    const off = vdist(c, p);
    if (off < r) best = Math.min(best, t - Math.sqrt(r * r - off * off));
  }
  return best;
}

function faceNormal(p: V3, dir: V3, h: number): V3 {
  let best = 0, bd = Infinity;
  for (let a = 0; a < 3; a++) {
    const f = p[a]! / h + 0.5;
    const dd = Math.abs(f - Math.round(f));
    if (dd < bd) {
      bd = dd;
      best = a;
    }
  }
  const n: V3 = [0, 0, 0];
  n[best] = dir[best]! > 0 ? -1 : 1;
  return n;
}
