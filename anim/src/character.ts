/**
 * A voxel character: the high-level object a host drives. It ties together
 *  - the voxel model (shared between characters until the first wound, then copied: damage is
 *    per character; `geometryVersion` tells the host to re-mesh),
 *  - the motion plan (motion/plan.ts): what the character means to do, from the host's inputs,
 *  - the physical body (body/humanoid.ts): rigid bodies and muscles that carry the plan out,
 *  - the behaviours (behaviour/controller.ts): the motor intelligence between them (balance,
 *    stagger, bracing, flinching, holding wounds, falling, getting up, dying),
 *  - the held prop (in the physical hand; dropped on death),
 *  - health, hit zones, wounds (voxels carved out, flesh and bone inside), severed limbs and
 *    heads, and gibbing by blasts (the pieces come back as gib specs for a GibSystem),
 *  - the retro presentation (baked frames, stepped time, 8-way facing) when a retro set is
 *    attached.
 *
 * The host moves it (setRoot, with its own collision) while the body follows its plan; when
 * the body leads (knocked off its feet, staggering, down, getting up) the host follows it
 * instead (`controlled`, `takeRootMotion`). It sets `motion.input`, calls update, and draws
 * `skin` (smooth), or `retroFrame` at `retroTransform` (retro).
 *
 * The physics costs time, so a calm character may run on its plan alone (`physics = false`,
 * the host's level of detail); anything that needs the body (a hit, a push, a fall, death)
 * wakes it.
 */
import type { Prop } from './characters/props.ts';
import { WorldPose } from './core/skeleton.ts';
import { B, BODY_COUNT, HumanoidBody } from './body/humanoid.ts';
import { Behaviours, type Perception } from './behaviour/controller.ts';
import { zoneOfPart, type HitInfo, type Zone } from './behaviour/injuries.ts';
import { MotionPlan, type AnimEvent } from './motion/plan.ts';
import { H } from './humanoid/rig.ts';
import { writeRigid } from './math/mat4.ts';
import { qconj, qmul, qnlerp, qrotate, qz, type Quat } from './math/quat.ts';
import { clamp, vadd, vcopy, vdist, vlerp, vnorm, vscale, vsub, type V3 } from './math/vec.ts';
import type { CollisionWorld } from './physics/collision.ts';
import type { Obstacle, RigidBody } from './physics/rigid.ts';
import { RetroPlayer, retroStateOf, snapYaw8, type RetroSet } from './retro/sequences.ts';
import { carveModel, detachSubtree, partIntegrity, raycastModel, severDisconnected, type CharacterHit, type RemovedVoxel } from './voxel/damage.ts';
import type { Palette, VoxelModel, VoxelPart } from './voxel/model.ts';
import type { Limb } from './motion/actions.ts';

/** A piece that came off a character: hand it to GibSystem.spawn. */
export interface GibSpec {
  part: VoxelPart;
  voxelSize: number;
  /** Its bone's world transform at the moment (x -> boneRot (x - boneRestHead) + bonePos). */
  bonePos: V3;
  boneRot: Quat;
  boneRestHead: V3;
  vel: V3;
  ang: V3;
  /** The piece is the held prop (drawn with the prop's palette slots). */
  prop: boolean;
}

export interface WoundResult {
  /** The shot killed the character now. */
  killed: boolean;
  headshot: boolean;
  /** Damage dealt (after hit-zone multipliers). */
  damage: number;
  /** Voxels carved out (blood and flesh bits for the host's effects). */
  removed: RemovedVoxel[];
  /** Pieces that came off (severed limbs, a head). */
  gibs: GibSpec[];
}

/** Damage multipliers by bone (rifle rounds ~ 30-40 damage, health 100). */
const ZONE: Partial<Record<number, number>> = {
  [H.head]: 4,
  [H.neck]: 3,
  [H.chest]: 1.1,
  [H.spine]: 1,
  [H.pelvis]: 0.9,
};

export interface CharacterOptions {
  model: VoxelModel;
  palette: Palette;
  collision: CollisionWorld;
  weapon?: Prop | null;
  health?: number;
  seed?: number;
  /** Shared retro frames for this model (see bakeRetroSet). */
  retro?: RetroSet | null;
  /** Total mass (kg); default by the rig's height. */
  mass?: number;
}

export class Character {
  /** The model drawn: shared until the first wound, then this character's own copy. */
  model: VoxelModel;
  readonly palette: Palette;
  /** What the character means to do (the host's inputs, actions, stances). */
  readonly motion: MotionPlan;
  /** What it is made of. */
  readonly body: HumanoidBody;
  /** How it carries itself (balance, reflexes, injuries, falls, death). */
  readonly behaviours: Behaviours;
  /** The pose shown (the body's, or the plan's while the physics rests), and the frame before. */
  readonly pose: WorldPose;
  readonly prevPose: WorldPose;
  weapon: Prop | null;
  health: number;
  readonly maxHealth: number;
  /** Bumped when `model` changes (a copy was made or voxels were carved): re-mesh. */
  geometryVersion = 0;
  /** True once `model` is this character's own copy. */
  ownsModel = false;
  /** Skin matrices of the current pose (16 floats per bone). */
  readonly skin: Float32Array;
  /** Hit flash 0..1 (the host tints the character). */
  flash = 0;
  /** Beaten unconscious (a melee knockout): down until it comes to. */
  knockedOut = false;
  /** Seconds since death. */
  deadTime = 0;
  /** Physics even while calm (the host's level of detail; bodies that need it always get it). */
  physics = true;
  /** The held prop's transform (world). */
  readonly weaponPos: V3 = [0, 0, 0];
  weaponRot: Quat = [0, 0, 0, 1];
  retro: RetroSet | null;
  private retroPlayer: RetroPlayer | null = null;
  /** Retro frame to draw and its transform (root position, facing snapped to 8 ways). */
  retroFrame: VoxelModel | null = null;
  readonly retroPos: V3 = [0, 0, 0];
  retroYaw = 0;
  private pain = 0;
  private firing = 0;
  private lastDt = 1 / 60;
  private calmFor = 0;
  /** Blend from the last shown pose after a switch between physics and plan (1: done). */
  private switchBlend = 1;
  private readonly switchFrom: WorldPose;
  private placed = false;

  constructor(o: CharacterOptions) {
    this.model = o.model;
    this.palette = o.palette;
    const sk = o.model.skeleton;
    this.motion = new MotionPlan(sk, o.collision, o.seed ?? 1);
    this.body = new HumanoidBody(sk, o.collision, o.mass !== undefined ? { mass: o.mass } : {});
    this.behaviours = new Behaviours(this.motion, this.body, o.seed ?? 1);
    this.pose = new WorldPose(sk);
    this.prevPose = new WorldPose(sk);
    this.switchFrom = new WorldPose(sk);
    this.weapon = o.weapon ?? null;
    this.motion.weapon = this.weapon;
    this.maxHealth = this.health = o.health ?? 100;
    this.skin = new Float32Array(sk.count * 16);
    this.retro = o.retro ?? null;
  }

  get alive(): boolean {
    return this.behaviours.alive;
  }

  /** The body leads (staggering, falling, down, getting up, dead): the host follows its root. */
  get controlled(): boolean {
    return this.behaviours.leading;
  }

  /** Badly hurt, down and writhing on the ground. */
  get writhing(): boolean {
    return this.behaviours.writhing;
  }

  /** Down on the ground (knocked down or out), or getting up. */
  get down(): boolean {
    const m = this.behaviours.mode;
    return m === 'falling' || m === 'lying' || m === 'rising';
  }

  /** The body rests (a corpse that stopped moving). */
  get asleep(): boolean {
    return !this.alive && this.body.system.asleep;
  }

  /** Places the character (feet on the ground at pos, facing yaw). */
  place(pos: Readonly<V3>, yaw: number): void {
    this.motion.place(pos, yaw);
    this.pose.copyFrom(this.motion.world);
    this.prevPose.copyFrom(this.pose);
    this.placed = true;
    if (this.behaviours.physical) this.body.setFromPose(this.pose, null, 0);
    this.pose.writeSkin(this.skin);
  }

  /** The host's root this frame (ignored while the body leads: see takeRootMotion). */
  setRoot(pos: Readonly<V3>, yaw: number): void {
    if (!this.alive || this.controlled) return;
    this.motion.setRoot(pos, yaw);
  }

  /**
   * Events of the motion (strikes landing, reloads done) since the last call, with the limb's
   * position taken from the body (where the fist or the foot really is).
   */
  takeEvents(): AnimEvent[] {
    const ev = this.motion.takeEvents();
    if (this.behaviours.physical) for (const e of ev) if (e.limb) this.limbPos(e.limb, e.pos);
    return ev;
  }

  /** How far the body moved the root since the last call (the host moves its character by it). */
  takeRootMotion(): V3 {
    return this.behaviours.takeRootMotion();
  }

  /** A shot fired (recoil; the retro fire frames). */
  fire(): void {
    if (!this.alive) return;
    this.motion.fire();
    this.firing = 0.15;
    if (this.behaviours.physical && this.weapon) {
      // the kick goes into the hands and the shoulder
      const pistol = this.weapon.kind === 'pistol';
      const dir = qrotate(this.weaponRot, [0, -1, 0.35]);
      const j = pistol ? 1.2 : 2.6;
      const hand = this.body.parts[B.handR]!;
      hand.applyImpulse(dir[0] * j, dir[1] * j, dir[2] * j, 0, 0, 0);
      if (!pistol) this.body.parts[B.chest]!.applyImpulse(dir[0] * j * 0.8, dir[1] * j * 0.8, 0, 0, 0, 0.1);
    }
  }

  // ---- the frame ------------------------------------------------------------------------------

  update(dtIn: number): void {
    const dt = Math.min(0.05, Math.max(0, dtIn));
    if (dt > 0) this.lastDt = dt;
    this.flash = Math.max(0, this.flash - dt * 6);
    this.pain = Math.max(0, this.pain - dt);
    this.firing = Math.max(0, this.firing - dt);
    if (!this.placed) this.place(this.motion.rootPos, this.motion.rootYaw);
    const b = this.behaviours;
    if (!this.alive) this.deadTime += dt;
    if (b.mode === 'dead' && this.body.system.asleep) {
      this.pose.writeSkin(this.skin);
      return;
    }
    // physics on or off (level of detail)
    const need = b.needsPhysics || !this.alive;
    this.calmFor = need ? 0 : this.calmFor + dt;
    if (!b.physical && (need || this.physics)) this.wake();
    else if (b.physical && !need && !this.physics && this.calmFor > 0.6) this.rest();
    b.prepare(dt, this.pose);
    this.motion.update(dt);
    this.prevPose.copyFrom(this.pose);
    if (b.physical) {
      b.drive(dt, this.pose);
      // a body thrown about is shown turning no bone more than about 35 degrees a frame (a
      // limb spinning about its length reads as a flip)
      if (b.mode === 'dying' || b.mode === 'dead' || b.mode === 'falling') this.limitTurns(0.6);
    } else this.pose.copyFrom(this.motion.world);
    if (this.switchBlend < 1) {
      // (a switch between physics and plan eases over a few frames)
      this.switchBlend = Math.min(1, this.switchBlend + dt / 0.2);
      const w = this.switchBlend * this.switchBlend * (3 - 2 * this.switchBlend);
      for (let i = 0; i < this.pose.p.length; i++) {
        vlerp(this.switchFrom.p[i]!, this.pose.p[i]!, w, this.pose.p[i]);
        this.pose.q[i] = qnlerp(this.switchFrom.q[i]!, this.pose.q[i]!, w);
      }
    }
    if (this.knockedOut && b.conscious && b.mode !== 'lying') this.knockedOut = false;
    this.placeWeapon();
    this.pose.writeSkin(this.skin);
    if (this.retro) this.updateRetro(dt);
    else this.retroFrame = null;
  }

  /** Limits how far each shown bone turns from the frame before (rad), about the bone's head. */
  private limitTurns(max: number): void {
    const p = this.pose, q0 = this.prevPose;
    for (let i = 0; i < p.q.length; i++) {
      const a = q0.q[i]!, b = p.q[i]!;
      const cos = Math.min(1, Math.abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]));
      const angle = 2 * Math.acos(cos);
      if (angle > max) p.q[i] = qnlerp(a, b, max / angle);
    }
  }

  /** Starts the physics from the shown pose (with its momentum). */
  private wake(): void {
    const b = this.behaviours;
    this.body.setFromPose(this.pose, this.prevPose, this.lastDt);
    b.physical = true;
  }

  /** Back to the plan alone (calm and far): eases over from the body's pose. */
  private rest(): void {
    this.behaviours.physical = false;
    this.switchFrom.copyFrom(this.pose);
    this.switchBlend = 0;
  }

  /** The prop in the (physical) right hand, held as the plan holds it. */
  private placeWeapon(): void {
    const m = this.motion;
    if (!this.weapon) return;
    if (!this.behaviours.physical) {
      vcopy(m.weaponPos, this.weaponPos);
      this.weaponRot = [...m.weaponRot] as Quat;
      return;
    }
    // the plan's prop relative to the plan's hand, carried by the body's hand
    const ph = m.world, bh = this.pose;
    const hq = ph.q[H.handR]!, hp = ph.p[H.handR]!;
    const inv = qconj(hq);
    const relP = qrotate(inv, vsub(m.weaponPos, hp));
    const relQ = qmul(inv, m.weaponRot);
    const q = bh.q[H.handR]!;
    vcopy(vadd(bh.p[H.handR]!, qrotate(q, relP)), this.weaponPos);
    this.weaponRot = qmul(q, relQ);
  }

  private updateRetro(dt: number): void {
    const m = this.motion;
    if (!this.alive || this.ownsModel || this.controlled || this.behaviours.brace) {
      // dead, wounded (the baked frames show no wounds) or thrown about: the host draws the
      // model itself, posed in retro steps
      this.retroFrame = null;
      return;
    }
    // (a new set when the character changes what it holds)
    if (this.retroPlayer?.set !== this.retro) this.retroPlayer = new RetroPlayer(this.retro!);
    const speed = Math.hypot(m.velocity[0], m.velocity[1]);
    const inp = m.input;
    const state = retroStateOf({
      speed,
      crouch: inp.crouch,
      aiming: m.weapon !== null && (inp.carry === 'aim' || inp.carry === 'hip') && inp.aimAt !== null,
      firing: this.firing > 0,
      mood: inp.mood,
      pain: this.pain > 0,
      stance: m.stance,
      talk: inp.talk,
      guard: inp.guard,
      action: m.actionName,
      weapon: m.weapon?.kind ?? null,
      carry: inp.carry,
      lean: inp.lean,
      knockedDown: m.down,
      transitioning: m.transitioning,
      desk: inp.seat?.deskHeight !== undefined,
    });
    const seq = this.retroPlayer.sequence(state);
    const rate = seq.speed > 0 ? Math.max(0.3, speed / seq.speed) : 1;
    this.retroFrame = this.retroPlayer.update(dt, state, rate);
    vcopy(m.rootPos, this.retroPos);
    this.retroYaw = snapYaw8(m.rootYaw);
  }

  // ---- senses and blows -------------------------------------------------------------------------

  /** Something close by (a round smacking in, a whiz past the head, a blast): the body flinches. */
  perceive(p: Perception): void {
    this.behaviours.perceive(p);
  }

  /**
   * A blow on the body (world): `point` where it lands, `dir` the direction it travels, `force`
   * (1 ~ a rifle round or a punch), its kind and, if known, the bone. Returns the zone.
   */
  hitAt(info: HitInfo): Zone {
    if (!this.behaviours.physical) this.wake();
    const part = info.bone !== undefined ? HumanoidBody.bodyOfBone(info.bone) : this.body.nearestPart(info.point);
    return this.behaviours.hit(info, part, this.pose);
  }

  /**
   * Thrown off balance by a push (a blast close by, a shove): `dir` is the push (world),
   * `strength` about 0.5 (a jolt) .. 3 (thrown).
   */
  push(dir: Readonly<V3>, strength: number): void {
    if (!this.alive) return;
    if (!this.behaviours.physical) this.wake();
    const d = vnorm([dir[0], dir[1], 0]);
    const s = clamp(strength, 0, 4);
    this.behaviours.push([d[0] * s * 0.85, d[1] * s * 0.85, s * 0.25], s > 1.8 ? clamp((s - 1.8) * 0.45, 0, 0.8) : 0);
  }

  /** A foot catches (on something, or at the host's say): a stumble, perhaps a fall. */
  trip(): void {
    if (!this.alive) return;
    if (!this.behaviours.physical) this.wake();
    this.behaviours.trip();
  }

  /** Knocked out: down for `seconds`, then back up. */
  knockOut(seconds: number): void {
    if (!this.alive) return;
    if (!this.behaviours.physical) this.wake();
    this.knockedOut = true;
    this.behaviours.knockOut(seconds);
  }

  /**
   * An old wound (no blow now): the body carries it from here on (a limp, a weak arm, a hand
   * that goes to it now and then). `severity` 0..1.
   */
  addInjury(bone: number, severity: number): void {
    const part = HumanoidBody.bodyOfBone(bone);
    const zone = zoneOfPart(part);
    const s = clamp(severity, 0, 1);
    this.behaviours.injuries.add({ part, local: [0, 0.06 * this.motion.k, 0], normal: [0, 1, 0], zone, kind: 'bullet', severity: s, lasting: s, age: 30, holdUntil: 0 });
  }

  /**
   * The body's collision spheres in the world (appended to `out`): what other bodies bump into
   * and trip over (hand them to their setObstacles).
   */
  collisionSpheres(out: Obstacle[]): Obstacle[] {
    this.body.spheresOf(this.behaviours.physical ? null : this.pose, out);
    return out;
  }

  /**
   * What is about this frame (spheres, world: the bodies of others near by, the dead, debris):
   * the body collides with them (a stagger knocks into people, a foot catches on a corpse) and
   * its steps clear what they see of them.
   */
  setObstacles(list: readonly Obstacle[]): void {
    this.body.system.obstacles = list;
    this.motion.feetPlanner.obstacles = list;
  }

  /** Another body's push on one of this body's parts (an impulse, N s, at a world point). */
  pushedAt(part: RigidBody, j: Readonly<V3>, at: Readonly<V3>): void {
    if (!this.behaviours.physical) {
      // (a body at rest on its plan wakes where it is: the push goes to the bodies there)
      this.wake();
    }
    this.body.system.wake();
    // (a light part takes what it can - a few m/s - the body the rest: a shoulder barged
    // moves the man, it does not fling his forearm)
    const jl = Math.hypot(j[0], j[1], j[2]);
    if (!(jl > 0) || !Number.isFinite(jl)) return;
    const take = Math.min(jl, 2.5 * part.mass);
    const k = take / jl;
    part.updateInertia();
    part.applyImpulse(j[0] * k, j[1] * k, j[2] * k, at[0] - part.x[0], at[1] - part.x[1], at[2] - part.x[2]);
    const rest = (jl - take) / (jl * this.body.totalMass);
    if (rest > 0) this.body.shove(j[0] * rest, j[1] * rest, j[2] * rest * 0.3);
    this.behaviours.bumped(jl);
  }

  /** A push on the body at `point` (velocity change dv, m/s), alive or dead. */
  impulse(point: Readonly<V3>, dv: Readonly<V3>, carry = 0): void {
    if (!this.behaviours.physical) this.wake();
    this.body.system.wake();
    const i = this.body.nearestPart(point);
    const b = this.body.parts[i]!;
    const r = vsub(point, b.x);
    b.applyImpulse(dv[0] * b.mass, dv[1] * b.mass, dv[2] * b.mass, r[0], r[1], r[2]);
    if (carry > 0) for (const j of [B.spine, B.chest, B.head, B.upperarmL, B.upperarmR]) if (j !== i) {
      const o = this.body.parts[j]!;
      o.v[0] += dv[0] * carry;
      o.v[1] += dv[1] * carry;
      o.v[2] += dv[2] * carry;
    }
  }

  /**
   * A radial push from `center`: the body gains up to `speed` away from it (the parts nearer
   * the blast a little more: the body tumbles, it is not torn apart at the joints).
   */
  blastPush(center: Readonly<V3>, radius: number, speed: number): void {
    if (!this.behaviours.physical) this.wake();
    this.body.system.wake();
    const c = this.body.com();
    const d0 = vsub(c, center);
    const l0 = Math.hypot(d0[0], d0[1], d0[2]);
    if (l0 >= radius) return;
    const n0 = l0 > 1e-6 ? vscale(d0, 1 / l0) : ([0, 0, 1] as V3);
    const s0 = speed * (1 - l0 / radius);
    for (let i = 0; i < BODY_COUNT; i++) {
      const b = this.body.parts[i]!;
      // (nearer the blast than the centre of mass: up to a quarter more)
      const near = clamp(-((b.x[0] - c[0]) * n0[0] + (b.x[1] - c[1]) * n0[1] + (b.x[2] - c[2]) * n0[2]) / 0.5, -1, 1);
      const s = s0 * (1 + 0.25 * near);
      b.v[0] += n0[0] * s;
      b.v[1] += n0[1] * s;
      b.v[2] += Math.abs(n0[2]) * s + s * 0.35;
    }
  }

  // ---- damage ---------------------------------------------------------------------------------

  /** Bounding sphere (world) of the body. */
  bounds(): { center: V3; radius: number } {
    const w = this.pose;
    return { center: vcopy(w.p[H.pelvis]!), radius: this.alive ? 1.05 : 1.2 };
  }

  /** The voxel hit by a ray (unit dir), or null. */
  raycast(origin: Readonly<V3>, dir: Readonly<V3>, maxDist: number): CharacterHit | null {
    const b = this.bounds();
    const oc = vsub(origin, b.center);
    const t = -(oc[0] * dir[0] + oc[1] * dir[1] + oc[2] * dir[2]);
    const c2 = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - t * t;
    if (c2 > b.radius * b.radius || t < -b.radius || t - b.radius > maxDist) return null;
    return raycastModel(this.model, this.skin, origin, dir, maxDist);
  }

  private ownModel(): void {
    if (this.ownsModel) return;
    this.model = this.model.clone();
    this.ownsModel = true;
    this.geometryVersion++;
  }

  /**
   * A bullet (or blade) wound at `hit` travelling along `dir`: carves a hole of `radius`
   * voxels' worth, deals `damage` times the zone multiplier, makes the body react (or pushes
   * the dead) with `impulse` m/s, and may sever limbs or the head.
   */
  wound(hit: CharacterHit, dir: Readonly<V3>, damage: number, radius = 0.045, impulse = 2.5): WoundResult {
    const res: WoundResult = { killed: false, headshot: false, damage: 0, removed: [], gibs: [] };
    this.ownModel();
    carveModel(this.model, hit.restPoint, radius, undefined, res.removed);
    // the exit side: a round also opens the body a little further along its path
    const exit = vadd(hit.restPoint, vscale(this.restDir(hit.bone, dir), radius * 2.2));
    carveModel(this.model, exit, radius * 0.8, undefined, res.removed);
    this.geometryVersion++;
    const mult = ZONE[hit.bone] ?? 0.6;
    res.damage = damage * mult;
    res.headshot = hit.bone === H.head || hit.bone === H.neck;
    this.flash = 1;
    const wasAlive = this.alive;
    this.health -= res.damage;
    res.gibs.push(...this.severAfterDamage(hit.bone, dir));
    if (wasAlive) {
      this.pain = 0.25;
      const info: HitInfo = { point: [...hit.point] as V3, dir: [dir[0], dir[1], dir[2]], force: damage / 30, kind: 'bullet', bone: hit.bone };
      this.hitAt(info);
      if (this.health <= 0 || res.gibs.some((g) => g.part.bone === H.head)) {
        // (a head shot drops the body at once; elsewhere it goes over a moment)
        this.die(null, null, res.headshot ? 0.08 : 0.55 + Math.random() * 0.6);
        res.killed = true;
      } else if (this.health < this.maxHealth * 0.3 || Math.max(this.behaviours.injuries.legL, this.behaviours.injuries.legR) > 0.8) {
        // too hurt to stand: down, writhing
        if (Math.random() < 0.75) this.behaviours.collapse(5 + Math.random() * 9);
      }
    } else this.impulse(hit.point, vscale(vnorm(dir), impulse * 1.4));
    return res;
  }

  /**
   * A melee blow landing at `point` (world) travelling along `dir`: a fist or a foot ('blunt',
   * force ~1 a punch, ~1.8 a kick) or a blade (a slice of voxels is cut out, it bleeds). Damage
   * by zone; the body reacts where it was hit.
   */
  melee(point: Readonly<V3>, dir: Readonly<V3>, kind: 'blunt' | 'blade', force = 1): WoundResult & { zone: string } {
    const res: WoundResult & { zone: string } = { killed: false, headshot: false, damage: 0, removed: [], gibs: [], zone: 'chest' };
    if (!this.alive) {
      this.impulse(point, vscale(vnorm(dir), 2 * force));
      return res;
    }
    const bone = this.nearestBone(point);
    const mult = kind === 'blade' ? (ZONE[bone] ?? 0.6) * 1.2 : bone === H.head || bone === H.neck ? 1.6 : bone === H.spine ? 1.2 : 0.8;
    res.damage = (kind === 'blade' ? 22 : 6) * force * mult;
    res.headshot = bone === H.head || bone === H.neck;
    if (kind === 'blade') {
      this.ownModel();
      const q = this.pose.q[bone]!;
      // the edge meets the body at its surface: from the bone's axis out towards the blade, a
      // body's depth at most (a blade stopped by the skin still cuts into it)
      const a = this.pose.p[bone]!;
      const tail = this.pose.tail(bone);
      const ab = vsub(tail, a);
      const l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
      const u = l2 > 0 ? clamp(((point[0] - a[0]) * ab[0] + (point[1] - a[1]) * ab[1] + (point[2] - a[2]) * ab[2]) / l2, 0, 1) : 0;
      const axis = vadd(a, vscale(ab, u));
      const out = vsub(point, axis);
      const ol = Math.hypot(out[0], out[1], out[2]);
      const depth = (bone === H.chest || bone === H.spine || bone === H.pelvis ? 0.09 : bone === H.head ? 0.07 : 0.035) * this.motion.k;
      const at = ol > depth ? vadd(axis, vscale(out, depth / ol)) : ([point[0], point[1], point[2]] as V3);
      const rest = vadd(this.model.skeleton.restHead[bone]!, qrotate([-q[0], -q[1], -q[2], q[3]], vsub(at, this.pose.p[bone]!)));
      carveModel(this.model, rest, 0.028, undefined, res.removed);
      const exit = vadd(rest, vscale(this.restDir(bone, dir), 0.035));
      carveModel(this.model, exit, 0.022, undefined, res.removed);
      this.geometryVersion++;
      res.gibs.push(...this.severAfterDamage(bone, dir));
    }
    this.flash = kind === 'blade' ? 1 : 0.6;
    // fists and feet knock people out rather than kill them
    const knockout = kind === 'blunt' && (this.health - res.damage <= 0 || (this.health - res.damage < this.maxHealth * 0.3 && res.headshot && force >= 1.2));
    this.health = kind === 'blunt' ? Math.max(1, this.health - res.damage) : this.health - res.damage;
    res.zone = this.hitAt({ point: [point[0], point[1], point[2]], dir: [dir[0], dir[1], dir[2]], force: kind === 'blade' ? force * 0.8 : force, kind, bone });
    this.pain = 0.3;
    if (knockout) this.knockOut(6 + Math.random() * 5);
    else if (kind === 'blunt' && force >= 2 && (res.zone === 'head' || res.zone === 'chest')) {
      // a hard blow puts the body down for a moment
      this.behaviours.knockOut(0.8 + Math.random());
      this.knockedOut = false;
    }
    if (this.health <= 0) {
      this.die(null, null, 0.5);
      res.killed = true;
    }
    return res;
  }

  /** Direction `dir` (world) in the rest space of bone b. */
  private restDir(b: number, dir: Readonly<V3>): V3 {
    const q = this.pose.q[b]!;
    return qrotate([-q[0], -q[1], -q[2], q[3]], vnorm(dir));
  }

  private severAfterDamage(bone: number, dir: Readonly<V3>): GibSpec[] {
    const out: GibSpec[] = [];
    const m = this.model;
    const sk = m.skeleton;
    const bones = new Set([bone, sk.parents[bone]!, ...sk.children[bone]!]);
    for (const b of bones) {
      if (b <= H.pelvis || b === H.weapon || b === H.spine || b === H.chest) continue;
      const pi = m.partOfBone[b]!;
      if (pi < 0) continue;
      const part = m.parts[pi]!;
      if (part.count === 0) continue;
      let pieces: VoxelPart[];
      if (partIntegrity(part) < 0.55) pieces = detachSubtree(m, b, true);
      else {
        pieces = severDisconnected(m, pi, 0.045 * (sk.restHead[H.pelvis]![2] / 0.97));
        const tail = sk.restTail[b]!;
        if (pieces.some((p) => nearTail(p, tail, m.voxelSize))) {
          for (const c of sk.children[b]!) pieces.push(...detachSubtree(m, c, true));
        }
      }
      for (const p of pieces) out.push(this.gibSpec(p, dir, 2.5));
    }
    if (out.length > 0) {
      this.geometryVersion++;
      // a limb that came off leaves its muscle behind
      for (const g of out) {
        const i = HumanoidBody.bodyOfBone(g.part.bone);
        this.body.tone[i] = 0;
      }
    }
    return out;
  }

  private gibSpec(p: VoxelPart, dir: Readonly<V3>, speed: number): GibSpec {
    const w = this.pose;
    const b = p.bone;
    const d = vnorm(dir);
    const r = (): number => Math.random() * 2 - 1;
    return {
      part: p,
      voxelSize: this.model.voxelSize,
      bonePos: vcopy(w.p[b]!),
      boneRot: [...w.q[b]!] as Quat,
      boneRestHead: vcopy(this.model.skeleton.restHead[b]!),
      vel: [d[0] * speed + r() * 0.8, d[1] * speed + r() * 0.8, d[2] * speed + 1.5 + Math.random()],
      ang: [r() * 8, r() * 8, r() * 8],
      prop: false,
    };
  }

  /**
   * Death: the muscles fade over `collapse` seconds (0: at once, as from a head shot or a
   * blast); a killing blow at `point` (velocity change `dv`) sends the body its way.
   */
  die(point: Readonly<V3> | null = null, dv: Readonly<V3> | null = null, collapse = 0.6): void {
    if (!this.alive) return;
    this.health = Math.min(this.health, 0);
    if (!this.behaviours.physical) this.wake();
    this.behaviours.die(collapse);
    if (point && dv) this.impulse(point, dv, 0.35);
    this.retroFrame = null;
  }

  /** The held prop as a gib (on death), or null. The character lets go of it. */
  dropWeapon(): GibSpec | null {
    const prop = this.weapon;
    if (!prop) return null;
    this.weapon = null;
    this.motion.weapon = null;
    const part = prop.model.parts[0];
    if (!part || part.count === 0) return null;
    const hand = this.body.parts[B.handR]!;
    const v = this.behaviours.physical ? hand.v : vscale(vsub(this.pose.p[H.chest]!, this.prevPose.p[H.chest]!), 1 / this.lastDt);
    return {
      part,
      voxelSize: prop.model.voxelSize,
      bonePos: vcopy(this.weaponPos),
      boneRot: [...this.weaponRot] as Quat,
      boneRestHead: [0, 0, 0],
      vel: [v[0] + (Math.random() - 0.5), v[1] + (Math.random() - 0.5), v[2] + 1],
      ang: [Math.random() * 6 - 3, Math.random() * 6 - 3, Math.random() * 6 - 3],
      prop: true,
    };
  }

  /**
   * A blast at `center` (radius of full effect, m; strength 1 ~ a rocket). Returns damage dealt,
   * whether it killed, and gib specs when the body is torn apart (close to the centre).
   */
  blast(center: Readonly<V3>, radius: number, strength = 1): { damage: number; killed: boolean; gibbed: boolean; gibs: GibSpec[] } {
    const c = this.bounds().center;
    const d = vdist(c, center);
    const reach = radius * 3.5;
    if (d > reach) return { damage: 0, killed: false, gibbed: false, gibs: [] };
    const f = Math.max(0, 1 - d / reach);
    const damage = 330 * strength * f * f;
    const away = vnorm(vsub(c, center), [0, 0, 1]);
    const wasAlive = this.alive;
    this.health -= damage;
    this.flash = 1;
    const gibs: GibSpec[] = [];
    let gibbed = false;
    if (this.health <= -40 || d < radius * 1.3) {
      gibbed = true;
      this.ownModel();
      if (wasAlive) this.die(null, null, 0);
      const m = this.model;
      for (const p of m.parts) {
        if (p.count === 0) continue;
        const sk = m.skeleton;
        const mid = [(sk.restHead[p.bone]![0] + sk.restTail[p.bone]![0]) / 2, (sk.restHead[p.bone]![1] + sk.restTail[p.bone]![1]) / 2, (sk.restHead[p.bone]![2] + sk.restTail[p.bone]![2]) / 2] as V3;
        carveModel(m, [mid[0] + (Math.random() - 0.5) * 0.1, mid[1] + (Math.random() - 0.5) * 0.1, mid[2]], 0.05, [m.parts.indexOf(p)]);
      }
      for (let b = 0; b < m.skeleton.count; b++) {
        const pi = m.partOfBone[b]!;
        if (pi < 0 || m.parts[pi]!.count === 0) continue;
        for (const piece of detachSubtree(m, b, false)) {
          const g = this.gibSpec(piece, away, 4 + 8 * f * strength);
          const pc = this.pose.p[b]!;
          const out = vnorm(vsub(pc, center), [0, 0, 1]);
          g.vel = [out[0] * (5 + 9 * f) + (Math.random() - 0.5) * 3, out[1] * (5 + 9 * f) + (Math.random() - 0.5) * 3, Math.abs(out[2]) * 6 + 3 + Math.random() * 4];
          gibs.push(g);
        }
      }
      this.geometryVersion++;
    } else if (this.health <= 0) {
      if (wasAlive) this.die(null, null, 0);
      this.blastPush(center, reach, 9 * f * strength + 2);
    } else {
      // thrown: a hit on the trunk, the whole body shoved away, dazed
      const ch = this.pose.p[H.chest]!;
      this.hitAt({ point: [ch[0], ch[1], ch[2]], dir: away, force: 2.5 * f * strength, kind: 'blast', bone: H.chest });
      this.blastPush(center, reach, 5.5 * f * strength);
      this.pain = 0.3;
    }
    if (!wasAlive && !gibbed) this.blastPush(center, reach, 9 * f * strength);
    return { damage, killed: wasAlive && !this.alive, gibbed, gibs };
  }

  // ---- where things are ---------------------------------------------------------------------------

  /** World position of the muzzle of the held prop (or the eyes without one). */
  muzzle(out: V3 = [0, 0, 0]): V3 {
    if (this.weapon) return this.propPoint(this.weapon.muzzle, out);
    return this.eyes(out);
  }

  /** World position of a point on the held prop (prop space). */
  propPoint(p: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
    return vadd(this.weaponPos, qrotate(this.weaponRot, p), out);
  }

  /** Skin matrix of the held prop (its model's single bone). */
  writePropSkin(out: Float32Array, offset = 0): void {
    writeRigid(out, offset, this.weaponPos, this.weaponRot, [0, 0, 0]);
  }

  /** World position between the eyes. */
  eyes(out: V3 = [0, 0, 0]): V3 {
    const sk = this.model.skeleton;
    const k = this.motion.k;
    const h = sk.restHead[H.head]!;
    return this.pose.pointOf(H.head, [h[0], h[1] + 0.09 * k, h[2] + 0.12 * k], out);
  }

  /** World position of a limb's striking point (fist, foot, blade tip, muzzle). */
  limbPos(limb?: Limb, out: V3 = [0, 0, 0]): V3 {
    const w = this.pose;
    const sk = this.model.skeleton;
    switch (limb) {
      case 'handR':
      case 'handL': {
        const b = limb === 'handR' ? H.handR : H.handL;
        return w.pointOf(b, vlerp(sk.restHead[b]!, sk.restTail[b]!, 0.55), out);
      }
      case 'footR':
      case 'footL':
        return w.pointOf(limb === 'footR' ? H.toeR : H.toeL, sk.restTail[limb === 'footR' ? H.toeR : H.toeL]!, out);
      case 'blade':
      case 'muzzle':
        if (this.weapon) return this.propPoint(this.weapon.muzzle, out);
        return w.pointOf(H.handR, sk.restTail[H.handR]!, out);
      default:
        return vcopy(w.p[H.chest]!, out);
    }
  }

  /** The bone nearest to a world point (by bone segments). */
  nearestBone(p: Readonly<V3>): number {
    let best: number = H.chest, bd = Infinity;
    const w = this.pose;
    const sk = this.model.skeleton;
    for (let b = 1; b < sk.count; b++) {
      if (b === H.weapon || b === H.toeL || b === H.toeR) continue;
      const a = w.p[b]!;
      const t = w.tail(b);
      const ab = vsub(t, a);
      const l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
      const ap = vsub(p, a);
      const u = l2 > 0 ? clamp((ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2, 0, 1) : 0;
      const d = vdist(p, vadd(a, vscale(ab, u)));
      if (d < bd) {
        bd = d;
        best = b;
      }
    }
    return best;
  }

  /** The skin matrix of a retro frame (a one-bone model at the root, facing snapped). */
  writeRetroSkin(out: Float32Array, offset = 0): void {
    writeRigid(out, offset, this.retroPos, qz(this.retroYaw - Math.PI / 2), [0, 0, 0]);
  }
}

function nearTail(p: VoxelPart, tail: Readonly<V3>, s: number): boolean {
  const [nx, ny, nz] = p.dims;
  const r2 = (2.5 * s) ** 2;
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++)
      for (let x = 0; x < nx; x++) {
        if (p.cells[x + nx * (y + ny * z)] === 0) continue;
        const cx = (p.origin[0] + x + 0.5) * s - tail[0];
        const cy = (p.origin[1] + y + 0.5) * s - tail[1];
        const cz = (p.origin[2] + z + 0.5) * s - tail[2];
        if (cx * cx + cy * cy + cz * cz < r2) return true;
      }
  return false;
}

const NO_OBSTACLES: readonly Obstacle[] = [];

/**
 * Lets characters collide with each other this frame: every simulated body gets the spheres of
 * the bodies (alive or dead) within `reach` metres as obstacles, plus `extra` (debris) near it.
 * Hosts call it once a frame before updating the characters.
 */
export function gatherObstacles(chars: readonly Character[], reach = 2.2, extra: readonly Obstacle[] = []): void {
  const cache = new Map<Character, Obstacle[]>();
  const spheres = (c: Character): Obstacle[] => {
    let s = cache.get(c);
    if (!s) {
      s = c.collisionSpheres([]);
      cache.set(c, s);
    }
    return s;
  };
  // what they did to each other last frame: every push handed on to the body it hit
  const owner = new Map<RigidBody, Character>();
  for (const c of chars) for (const r of c.body.system.reactions) {
    if (owner.size === 0) for (const d of chars) for (const p of d.body.parts) owner.set(p, d);
    const o = owner.get(r.body);
    if (!o) continue;
    o.pushedAt(r.body, r.j, r.at);
  }
  for (const a of chars) {
    if (!a.behaviours.physical || a.asleep) {
      a.setObstacles(NO_OBSTACLES);
      continue;
    }
    const pa = a.pose.p[H.pelvis]!;
    let list: Obstacle[] | null = null;
    for (const b of chars) {
      if (b === a) continue;
      const pb = b.pose.p[H.pelvis]!;
      if (Math.abs(pa[0] - pb[0]) > reach || Math.abs(pa[1] - pb[1]) > reach || Math.abs(pa[2] - pb[2]) > reach) continue;
      list ??= [];
      for (const s of spheres(b)) list.push(s);
    }
    for (const o of extra) {
      if (Math.abs(pa[0] - o.c[0]) > reach || Math.abs(pa[1] - o.c[1]) > reach || Math.abs(pa[2] - o.c[2]) > reach) continue;
      (list ??= []).push(o);
    }
    a.setObstacles(list ?? NO_OBSTACLES);
  }
}
