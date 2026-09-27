/**
 * Actors: soldiers and civilians in the game world, built on svx_anim.
 *
 * ActorWorld owns the characters and runs them every frame:
 *  - brains (brain.ts) decide at ~10 Hz where to go, what to face and how to stand;
 *  - movement: paths (nav.ts, budgeted per frame), steering with separation, and the player's
 *    box collision with step-up on the engine's occupancy, with gravity;
 *  - animation: svx_anim Characters (procedural locomotion, weapon handling, moods), ragdolls
 *    when they die, gibs and blood (GibSystem);
 *  - combat: soldiers fire hitscan rounds at the player; a round hits the first thing on its
 *    line: a character (voxel-exact, wounds carve the voxel body), the player, or the world,
 *    where it carves the engine's voxels (the battle destroys the level). Player rounds and
 *    rockets come in through raycast / wound / blast; falling debris crushes characters;
 *  - rendering: characters (their own meshes once wounded), held rifles, gibs, blood drops and
 *    stains, blob shadows; the retro presentation (baked frames, stepped time, 8-way facing).
 */
import {
  Character,
  GibSystem,
  ModelMesher,
  vadd,
  vdist,
  vnorm,
  vscale,
  vsub,
  type Carry,
  type CharacterHit,
  type Gib,
  type GibSpec,
  type Mood,
  type V3,
  type WoundResult,
} from 'svx-anim';
import type { CharacterRenderer, GpuCharacterMesh } from '../render/characters.ts';
import type { IslandRenderer } from '../render/islands.ts';
import { CivilianBrain, SoldierBrain, type Brain } from './brain.ts';
import { Cast, type Faction, type Look } from './cast.ts';
import type { WorldAccess } from './env.ts';
import { Navigator } from './nav.ts';

export interface Noise {
  pos: V3;
  radius: number;
  kind: 'shot' | 'impact' | 'explosion' | 'death' | 'scream' | 'shout';
  source: Actor | null;
}

export interface PlayerView {
  feet: V3;
  forward: V3;
  alive: boolean;
  eye(): V3;
  chest(): V3;
}

/** What the actors need from the rest of the game. */
export interface ActorHooks {
  /** A round hit the world: carve it. */
  carve(pos: V3, radius: number): void;
  /** Bullet impact effects on the world. */
  impact(pos: V3, normal: V3): void;
  /** The player was hit (damage points, from where). */
  hurtPlayer(damage: number, from: V3): void;
  muzzleFlash(pos: V3, dir: V3): void;
  tracer(from: V3, to: V3): void;
  /** A burst of blood mist (particles) where a body was hit. */
  bloodMist(pos: V3, dir: V3, amount: number): void;
}

export interface ActorSettings {
  ai: boolean;
  retro: boolean;
  /** Voxel size of retro frames (1/32 fine, 1/16 chunky). */
  retroVoxel: number;
  /** Bodies kept before the oldest fade away. */
  maxCorpses: number;
  /** Rounds from soldiers hurt the player. */
  playerDamage: boolean;
}

export interface Actor {
  id: number;
  faction: Faction;
  look: Look;
  char: Character;
  brain: Brain;
  pos: V3;
  vel: V3;
  yaw: number;
  onGround: boolean;
  goal: V3 | null;
  path: V3[];
  needPath: boolean;
  speed: number;
  face: V3 | null;
  crouch: number;
  carry: Carry;
  mood: Mood;
  /** Seconds the actor has been failing to move where it wants. */
  stuck: number;
  thinkAcc: number;
  own: { mesher: ModelMesher; mesh: GpuCharacterMesh | null; version: number } | null;
  /** Skin used for drawing (held between retro steps). */
  drawSkin: Float32Array;
  propSkin: Float32Array;
  opacity: number;
  diedAt: number;
}

interface GibUser {
  mesh: GpuCharacterMesh;
  paletteId: number;
  skin: Float32Array;
}

const DEFAULT_SETTINGS: ActorSettings = { ai: true, retro: false, retroVoxel: 1 / 32, maxCorpses: 40, playerDamage: true };
const RIFLE_DAMAGE = 34;
const BLOOD: [number, number, number] = [0.3, 0.012, 0.01];
/** Retro stepping: Doom's 35 Hz tics, 4 per step. */
const RETRO_STEP = 4 / 35;

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
  private nextId = 1;
  private seed = 1;
  private readonly renderer: CharacterRenderer;
  private readonly islands: IslandRenderer | null;
  private readonly islandPrev = new Map<number, { pos: V3; t: number }>();
  private readonly gibUsers = new Set<Gib>();
  private retroClock = 0;
  private retroStep = 0;
  private pathCursor = 0;
  /** Rounds fired by soldiers, kills, etc. (HUD). */
  shotsFired = 0;
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
    const armed = faction === 'soldier';
    const char = new Character({ model: look.model, palette: look.palette, collision: this.env, weapon: armed ? this.cast.rifle : null, health: armed ? 100 : 70, seed });
    char.place(pos, yaw);
    const a: Actor = {
      id: this.nextId++,
      faction,
      look,
      char,
      brain: armed ? new SoldierBrain(pos) : new CivilianBrain(),
      pos: [...pos],
      vel: [0, 0, 0],
      yaw,
      onGround: true,
      goal: null,
      path: [],
      needPath: false,
      speed: 0,
      face: null,
      crouch: 0,
      carry: 'relaxed',
      mood: 'normal',
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
   * (soldiers in a group further out). Returns how many were placed.
   */
  populate(center: V3, civilians: number, soldiers: number, rMin = 6, rMax = 35): number {
    let placed = 0;
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

  clear(): void {
    for (const a of this.actors) this.releaseActor(a);
    this.actors.length = 0;
    for (const g of this.gibs.gibs) this.releaseGib(g);
    this.gibs.clear();
    this.gibUsers.clear();
    this.islandPrev.clear();
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

  private eyes(a: Actor): V3 {
    return [a.pos[0], a.pos[1], a.pos[2] + (a.crouch > 0.5 ? 1.05 : 1.6)];
  }

  canSee(a: Actor, p: Readonly<V3>): boolean {
    return this.env.lineOfSight(this.eyes(a), p);
  }

  noise(n: Noise): void {
    if (!this.settings.ai) return;
    for (const a of this.actors) if (a.char.alive && vdist(a.pos, n.pos) < n.radius) a.brain.hear(a, n, this);
  }

  /** A soldier fires one round at `target` with angular spread. False if a friend is in the way. */
  fireAt(a: Actor, target: Readonly<V3>, spread: number): boolean {
    const eye = this.eyes(a);
    let origin = a.char.muzzle();
    if (!this.env.lineOfSight(eye, origin)) origin = eye;
    const d = vnorm(vsub(target, origin));
    const range = vdist(target, origin);
    // friends in the line of fire
    for (const b of this.actors) {
      if (b === a || !b.char.alive || b.faction !== a.faction) continue;
      const c = vadd(b.pos, [0, 0, 1.1]);
      const t = (c[0] - origin[0]) * d[0] + (c[1] - origin[1]) * d[1] + (c[2] - origin[2]) * d[2];
      if (t < 0 || t > range) continue;
      const off = vdist(c, vadd(origin, vscale(d, t)));
      if (off < 0.7) return false;
    }
    const dir = jitter(d, spread);
    a.char.fire();
    this.shotsFired++;
    this.hooks.muzzleFlash(a.char.muzzle(), dir);
    this.noise({ pos: origin, radius: 45, kind: 'shot', source: a });
    const end = this.resolveShot(origin, dir, 180, a, RIFLE_DAMAGE, 0.1);
    this.hooks.tracer(origin, end);
    return true;
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
      this.hooks.carve(inside, carveRadius);
      this.hooks.impact(p, faceNormal(p, dir, this.env.h));
      this.noise({ pos: p, radius: 10, kind: 'impact', source: shooter });
      return p;
    }
    if (playerT <= best && playerT < Infinity && shooter) {
      if (this.settings.playerDamage) this.hooks.hurtPlayer(damage * (0.28 + Math.random() * 0.12), origin);
      return vadd(origin, vscale(dir, playerT));
    }
    if (actorHit) {
      this.wound(actorHit.actor, actorHit.hit, dir, damage, shooter ? shooter.pos : null);
      return actorHit.hit.point;
    }
    return vadd(origin, vscale(dir, maxDist));
  }

  // ---- damage -------------------------------------------------------------------------------

  /** The first character voxel on a ray (unit dir), excluding `skip`. */
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
  wound(a: Actor, hit: CharacterHit, dir: V3, damage: number, from: V3 | null, radius = 0.045): WoundResult {
    const wasAlive = a.char.alive;
    const r = a.char.wound(hit, dir, damage, radius, 2.8);
    // blood out of the exit side, a little back towards the shooter, and a mist
    this.gibs.spray(hit.point, dir, 6 + Math.min(18, r.removed.length >> 1), 3.2, 0.5);
    this.gibs.spray(hit.point, vscale(dir, -1), 3, 1.6, 0.9);
    this.hooks.bloodMist(hit.point, dir, r.headshot ? 2 : 1);
    // bits of what was shot away (cloth, skin, flesh) fly with it
    const pal = a.look.palette;
    for (let i = 0; i < r.removed.length; i += 4) {
      const v = r.removed[i]!;
      const c = pal[v.slot] ?? BLOOD;
      this.gibs.spray(hit.point, dir, 1, 2.8, 0.7, [c[0], c[1], c[2]]);
    }
    for (const g of r.gibs) this.spawnGib(g, a, 30);
    if (wasAlive && r.killed) this.onDeath(a, dir);
    else if (wasAlive && this.settings.ai) a.brain.hurt(a, from, this);
    return r;
  }

  blast(pos: V3, radius: number, strength = 1): void {
    for (const a of this.actors) {
      if (a.opacity < 0.5) continue;
      const wasAlive = a.char.alive;
      const r = a.char.blast(pos, radius, strength);
      if (r.damage <= 0 && !r.gibbed) continue;
      if (r.gibbed) {
        const c = a.char.bounds().center;
        this.gibs.spray(c, [0, 0, 1], 60, 5, 1.2);
        this.hooks.bloodMist(c, [0, 0, 1], 4);
        for (const g of r.gibs) this.spawnGib(g, a, 40);
        a.opacity = 0; // nothing left to draw
      }
      if (!r.gibbed && r.damage > 15) {
        // shrapnel wounds
        const c = a.char.bounds().center;
        this.gibs.spray(c, vnorm(vsub(c, pos), [0, 0, 1]), Math.min(40, Math.round(r.damage / 6)), 3, 1);
        this.hooks.bloodMist(c, [0, 0, 1], 1);
      }
      if (wasAlive && !a.char.alive) this.onDeath(a, vnorm(vsub(a.pos, pos)));
      else if (wasAlive && this.settings.ai) a.brain.hurt(a, pos, this);
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
    this.noise({ pos: a.pos, radius: 25, kind: 'death', source: a });
  }

  private spawnGib(spec: GibSpec, a: Actor, bleed: number): void {
    const mesh = spec.prop ? this.cast.rifleMesh : this.cast.partMesh(spec.part, spec.voxelSize);
    const user: GibUser = { mesh, paletteId: a.look.paletteId, skin: new Float32Array(16) };
    const g = this.gibs.spawn(spec.part, spec.voxelSize, spec.bonePos, spec.boneRot, spec.boneRestHead, spec.vel, spec.ang, user);
    g.bleed = bleed;
    this.gibUsers.add(g);
  }

  private releaseGib(g: Gib): void {
    const u = g.user as GibUser | undefined;
    if (u && u.mesh !== this.cast.rifleMesh) this.cast.releasePart(g.part);
  }

  private releaseActor(a: Actor): void {
    if (a.own?.mesh) this.renderer.releaseMesh(a.own.mesh);
    a.own = null;
  }

  // ---- simulation ---------------------------------------------------------------------------

  update(dt: number): void {
    this.time += dt;
    // retro clock: poses of the dead (and gibs) advance in Doom-tic steps
    this.retroClock += dt;
    let retroTick = false;
    if (this.retroClock >= RETRO_STEP) {
      this.retroClock %= RETRO_STEP;
      this.retroStep++;
      retroTick = true;
    }
    this.planPaths(2);
    this.crushByDebris(dt);
    for (const a of this.actors) {
      if (a.char.alive) {
        if (this.settings.ai) {
          a.thinkAcc += dt;
          if (a.thinkAcc >= 0.1) {
            a.brain.think(a, this, a.thinkAcc);
            a.thinkAcc = 0;
          }
          a.brain.tick(a, this, dt);
        }
        this.move(a, dt);
        const an = a.char.animator.input;
        an.crouch = a.crouch;
        an.carry = a.carry;
        an.mood = a.mood;
        an.aimAt = a.carry === 'aim' ? a.face : null;
        an.lookAt = a.carry === 'aim' ? null : a.face;
        an.airborne = !a.onGround && a.vel[2] < -2.5;
        a.char.setRoot(a.pos, a.yaw);
      }
      // retro presentation
      if (this.settings.retro && a.char.alive) {
        a.char.retro ??= this.cast.retroSet(a.look.model, a.char.weapon, this.settings.retroVoxel);
      } else if (!this.settings.retro) a.char.retro = null;
      if (!a.char.alive && a.char.ragdoll?.asleep && a.char.deadTime > 3) {
        // settled bodies cost nothing
      } else a.char.update(dt);
      if (!this.settings.retro || a.char.alive || retroTick) a.drawSkin.set(a.char.skin);
    }
    this.cast.pump();
    this.gibs.update(dt);
    // release the meshes of gibs the system dropped
    const live = new Set(this.gibs.gibs);
    for (const g of this.gibUsers) {
      if (!live.has(g)) {
        this.releaseGib(g);
        this.gibUsers.delete(g);
      } else if (!this.settings.retro || retroTick) this.gibs.writeSkin(g, (g.user as GibUser).skin);
    }
    this.fadeCorpses(dt);
  }

  private planPaths(budget: number): void {
    const n = this.actors.length;
    for (let k = 0; k < n && budget > 0; k++) {
      const a = this.actors[(this.pathCursor + k) % n]!;
      if (!a.needPath || !a.goal || !a.char.alive) continue;
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
    let dx = 0, dy = 0;
    if (a.goal) {
      let next = a.path[0] ?? a.goal;
      let d = Math.hypot(next[0] - a.pos[0], next[1] - a.pos[1]);
      while (d < 0.35 && a.path.length > 0) {
        a.path.shift();
        next = a.path[0] ?? a.goal;
        d = Math.hypot(next[0] - a.pos[0], next[1] - a.pos[1]);
      }
      if (a.path.length === 0 && d < 0.4) a.goal = null;
      else if (d > 1e-3) {
        // slow down into the destination
        const s = a.path.length > 0 ? a.speed : Math.min(a.speed, 0.6 + d * 1.5);
        dx = ((next[0] - a.pos[0]) / d) * s;
        dy = ((next[1] - a.pos[1]) / d) * s;
      }
    }
    // keep apart
    for (const b of this.actors) {
      if (b === a || !b.char.alive) continue;
      const ox = a.pos[0] - b.pos[0], oy = a.pos[1] - b.pos[1];
      const d2 = ox * ox + oy * oy;
      if (d2 > 0.64 || d2 < 1e-6 || Math.abs(a.pos[2] - b.pos[2]) > 1) continue;
      const d = Math.sqrt(d2);
      const push = (0.8 - d) * 3.5;
      dx += (ox / d) * push;
      dy += (oy / d) * push;
    }
    const k = 1 - Math.exp(-(a.onGround ? 9 : 1.5) * dt);
    a.vel[0] += (dx - a.vel[0]) * k;
    a.vel[1] += (dy - a.vel[1]) * k;
    a.vel[2] = Math.max(-40, a.vel[2] - 20 * dt);
    const height = a.crouch > 0.5 ? 1.2 : 1.72;
    const r = 0.24;
    const min: V3 = [a.pos[0] - r, a.pos[1] - r, a.pos[2]];
    const max: V3 = [a.pos[0] + r, a.pos[1] + r, a.pos[2] + height];
    const want: V3 = [a.vel[0] * dt, a.vel[1] * dt, a.vel[2] * dt];
    // pushed up out of the ground when voxels appear under it (never stuck inside)
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
      // a long fall kills
      if (fell < -15) {
        a.char.die();
        this.onDeath(a, [0, 0, -1]);
        return;
      }
    }
    // stuck: wanted to move and could not
    const wanted = Math.hypot(dx, dy);
    const got = Math.hypot(res.move[0], res.move[1]) / Math.max(dt, 1e-4);
    if (a.goal && wanted > 0.3 && got < wanted * 0.25) a.stuck += dt;
    else a.stuck = Math.max(0, a.stuck - dt);
    if (a.stuck > 0.8 && a.goal) {
      a.needPath = true;
      if (a.stuck > 2.5) this.stop(a);
    }
    // facing
    let want2: number | null = null;
    if (a.face) want2 = Math.atan2(a.face[1] - a.pos[1], a.face[0] - a.pos[0]);
    else if (Math.hypot(a.vel[0], a.vel[1]) > 0.3) want2 = Math.atan2(a.vel[1], a.vel[0]);
    if (want2 !== null) {
      let d = want2 - a.yaw;
      while (d > Math.PI) d -= 2 * Math.PI;
      while (d < -Math.PI) d += 2 * Math.PI;
      const rate = (a.face ? 6 : 8) * dt;
      a.yaw += Math.max(-rate, Math.min(rate, d));
    }
    // falling out of the world
    if (a.pos[2] < -60) {
      a.char.die();
      a.opacity = 0;
    }
  }

  /** Falling pieces (the engine's rigid debris) crush the characters they land on. */
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
      for (const a of this.actors) {
        if (a.opacity < 0.5) continue;
        const c = a.char.bounds().center;
        if (vdist(c, pos) > isl.radius * 0.75 + 0.35) continue;
        if (a.char.alive) {
          a.char.die(c, vscale(v, 0.6));
          this.gibs.spray(c, [0, 0, 1], 25, 3, 1.2);
          this.hooks.bloodMist(c, [0, 0, 1], 2);
          this.onDeath(a, vnorm(v));
        } else a.char.ragdoll?.hit(c, vscale(v, 0.5));
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
    for (const a of this.actors) {
      if (a.opacity <= 0) continue;
      const ch = a.char;
      const tint: [number, number, number, number] = [1, 0.15, 0.1, ch.flash * 0.45];
      const b = ch.bounds();
      if (ch.retroFrame && ch.alive) {
        const skin = a.propSkin;
        ch.writeRetroSkin(skin);
        cc.add(this.cast.mesh(ch.retroFrame), skin, 1, a.look.paletteId, { center: b.center, radius: 1.3, tint, opacity: a.opacity });
      } else {
        cc.add(this.meshOf(a), a.drawSkin, ch.model.skeleton.count, a.look.paletteId, { center: b.center, radius: b.radius + 0.3, tint, opacity: a.opacity });
        if (ch.weapon && ch.alive) {
          ch.animator.writePropSkin(a.propSkin);
          cc.add(this.cast.rifleMesh, a.propSkin, 1, a.look.paletteId, { center: ch.animator.weaponPos, radius: 0.7, opacity: a.opacity });
        }
      }
      if (ch.alive) cc.decal([a.pos[0], a.pos[1], a.pos[2] + 0.004], [0, 0, 1], 0.4, [0, 0, 0, 0.5 * a.opacity], 0);
      else if (ch.ragdoll?.asleep && a.opacity > 0.5) {
        // a pool of blood spreads under a body at rest
        const p = ch.pose.p[3]!; // chest
        const grow = Math.min(1, (ch.deadTime - 1) / 8);
        const g = this.env.groundHeight(p[0], p[1], p[2] + 0.3, p[2] - 0.8);
        if (grow > 0 && g !== null) cc.decal([p[0], p[1], g], [0, 0, 1], 0.2 + 0.45 * grow, [BLOOD[0] * 0.7, BLOOD[1], BLOOD[2], 0.85 * a.opacity], 1);
      }
    }
    for (const g of this.gibUsers) {
      const u = g.user as GibUser;
      cc.add(u.mesh, u.skin, 1, u.paletteId, { center: g.pos, radius: g.radius + 0.05 });
    }
    this.gibs.forEachDrop((p, size, c) => cc.bit(p, size, [0, 0, 0, 1], c));
    this.gibs.forEachStain((p, n, size, age, c) => cc.decal(p, n, Math.max(0.045, size * 2.4), [c[0] * 0.8, c[1] * 0.8, c[2] * 0.8, Math.min(0.92, 0.5 + age)], 1));
  }

  /** The mesh drawn for an actor: the shared model's, or its own once wounded. */
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

  stats(): { civilians: number; soldiers: number; dead: number; gibs: number; stains: number; bakes: number } {
    let civilians = 0, soldiers = 0, dead = 0;
    for (const a of this.actors) {
      if (!a.char.alive) dead++;
      else if (a.faction === 'soldier') soldiers++;
      else civilians++;
    }
    return { civilians, soldiers, dead, gibs: this.gibs.gibs.length, stains: this.gibs.stains.length, bakes: this.cast.pendingBakes };
  }
}

/** Unit direction jittered inside a cone of half-angle ~spread (radians), gaussian-ish. */
function jitter(d: V3, spread: number): V3 {
  const up: V3 = Math.abs(d[2]) < 0.95 ? [0, 0, 1] : [1, 0, 0];
  const r = vnorm([d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]]);
  const u: V3 = [r[1] * d[2] - r[2] * d[1], r[2] * d[0] - r[0] * d[2], r[0] * d[1] - r[1] * d[0]];
  const g = (): number => (Math.random() + Math.random() + Math.random() - 1.5) / 1.5;
  const x = g() * spread, y = g() * spread;
  return vnorm([d[0] + r[0] * x + u[0] * y, d[1] + r[1] * x + u[1] * y, d[2] + r[2] * x + u[2] * y]);
}

/** Distance along a ray (unit dir) to a capsule (segment a-b, radius r), or Infinity. */
function rayCapsule(o: V3, d: V3, a: V3, b: V3, r: number): number {
  // sample the segment: closest approach of the ray to points along it
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

/** Axis normal of the voxel face a ray entered at p. */
function faceNormal(p: V3, dir: V3, h: number): V3 {
  // the face is the one whose plane the hit point lies on (closest to a voxel boundary)
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

