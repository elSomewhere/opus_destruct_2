/**
 * The humanoid animator: procedural animation of the humanoid rig from a handful of inputs.
 *
 * The host moves the character (root position on the ground and facing yaw, each frame, with
 * whatever collision it has) and says what it is doing: stance, crouch, weapon carry, aim and
 * look targets, mood, talking, guard; it starts actions (strikes, reloads, gestures) and reports
 * hits. The animator makes the body follow, in layers:
 *
 * 1. Stances (stances.ts): standing (with the locomotion below), kneeling, prone and crawling,
 *    sitting on a seat or the ground, knocked down; transitions blend through intermediate
 *    stances (stand -> kneel -> prone).
 * 2. Locomotion: a gait clock drives a foot planter (feet stay where they land, no sliding at
 *    any speed or turn rate) with heel-toe roll, landing on the ground found by the
 *    CollisionWorld. The walk has weight: a personal style (style.ts), footfall compression,
 *    lean into acceleration with overshoot, banking into turns, a tactical walk while aiming,
 *    a limp on a wounded leg.
 * 3. Trunk and head: aim and look spread over spine, chest, neck and head; peeking (lean);
 *    posture; breathing.
 * 4. Actions (actions.ts): a held posture layer (guard, idle poses, talking) and a one-shot
 *    layer (strikes that land on their target, blocks, reloads, gestures, fidgets). When
 *    standing still and free the animator picks idles and fidgets itself; when talking, gestures
 *    and nods.
 * 5. Arms: the held weapon (rifle, SMG, machine gun: relaxed, ready, shouldered, hip, port
 *    arms; pistol: two-handed, one-handed, low ready, lowered; knife in the hand) with both
 *    hands placed by IK; else guard, moods (panic, cower, surrender), rest hands of the stance,
 *    swing; action hands on top; a hand to a wound.
 * 6. Reactions (reactions.ts): hits by location and force as springs on the trunk, head and
 *    arms, a gut fold, a knockback the host applies, knockdowns.
 * 7. Legs by IK last, so every pelvis motion bends the knees with the feet where they are.
 *
 * Output: `pose` (local), `world` (world transforms; `prevWorld` of the frame before), the
 * held prop's world transform, and events (a strike landing, a reload done) in `events`.
 */
import { ModelFK, frameRotation, setModelRotation, solveTwoBone } from '../core/ik.ts';
import { Pose, WorldPose, type Skeleton } from '../core/skeleton.ts';
import { Spring, Spring3 } from '../core/spring.ts';
import { gaitFor, type GaitParams } from '../locomotion/gait.ts';
import { writeRigid } from '../math/mat4.ts';
import { Rng, valueNoise } from '../math/random.ts';
import { qconj, qeuler, qexp, qmirrorX, qmul, qnlerp, qrotate, qx, qz, type Quat } from '../math/quat.ts';
import { DEG, clamp, fract, lerp, smoothstep, vadd, vcopy, vdist, vlerp, vnorm, vscale, vsub, wrapAngle, type V3 } from '../math/vec.ts';
import type { CollisionWorld } from '../physics/collision.ts';
import type { Prop } from '../characters/props.ts';
import { actionDef, ActionPlayer, FIDGETS, GESTURES, IDLE_POSES, type ActionDef, type ChannelFrame, type Limb } from './actions.ts';
import { Reactions, type HitInfo, type Zone } from './reactions.ts';
import { H } from './rig.ts';
import {
  blendSamples,
  downSample,
  groundSample,
  kneelSample,
  newSample,
  proneSample,
  sitSample,
  stanceRoute,
  transitionTime,
  type Dims,
  type GroundVariant,
  type SitVariant,
  type Stance,
  type StanceSample,
} from './stances.ts';
import { NEUTRAL_STYLE, type GaitStyle } from './style.ts';

export type Carry = 'relaxed' | 'ready' | 'aim' | 'hip';
export type Mood = 'normal' | 'panic' | 'cower' | 'surrender';

/** A seat to sit on (world). The character's root stays where it stood, in front of it. */
export interface SeatInfo {
  /** Seat surface centre. */
  pos: V3;
  backrest: boolean;
  /** Desk top height above the floor (m), for sitting at a desk. */
  deskHeight?: number | null;
  variant?: SitVariant;
}

export interface HumanoidInput {
  /** 0 standing .. 1 crouched (standing stance). */
  crouch: number;
  stance: Stance;
  seat: SeatInfo | null;
  groundVariant: GroundVariant;
  /** How the weapon is held (ignored without one). */
  carry: Carry;
  /** World point to aim the weapon (and the eyes) at. */
  aimAt: V3 | null;
  /** World point to look at (when not aiming). */
  lookAt: V3 | null;
  mood: Mood;
  /** Off the ground (falling, jumping): legs tuck, feet are not planted. */
  airborne: boolean;
  /** Peek around a corner: -1 left .. 1 right. */
  lean: number;
  /** In a conversation: speaking (gestures) or listening (nods). */
  talk: 'speak' | 'listen' | null;
  /** Fists up (a fight). */
  guard: boolean;
  /** Pick idle postures and fidgets by itself when standing still. */
  idle: boolean;
}

export interface AnimEvent {
  name: string;
  action: string;
  limb?: Limb;
  /** World position of the limb (fist, foot, blade tip, muzzle) at the event. */
  pos: V3;
  /** The action's target (world), if any. */
  target: V3 | null;
}

interface Foot {
  side: -1 | 1;
  thigh: number;
  shin: number;
  foot: number;
  toe: number;
  offset: number;
  planted: boolean;
  /** Taken over by an action (a kick): the planter leaves it alone. */
  held: boolean;
  pos: V3;
  yaw: number;
  lift: V3;
  liftYaw: number;
  swing: number;
  swingRate: number;
  target: V3;
  targetYaw: number;
  ankle: V3;
  pitch: number;
}

const TAU = Math.PI * 2;

function angleLerp(a: number, b: number, t: number): number {
  return a + wrapAngle(b - a) * t;
}

function tuple(v: readonly number[] | undefined, fallback: V3): V3 {
  return v ? [v[0] ?? 0, v[1] ?? 0, v[2] ?? 0] : [fallback[0], fallback[1], fallback[2]];
}

export class HumanoidAnimator {
  readonly skeleton: Skeleton;
  readonly pose: Pose;
  readonly fk: ModelFK;
  readonly world: WorldPose;
  readonly prevWorld: WorldPose;
  readonly input: HumanoidInput = {
    crouch: 0,
    stance: 'stand',
    seat: null,
    groundVariant: 'kneesUp',
    carry: 'relaxed',
    aimAt: null,
    lookAt: null,
    mood: 'normal',
    airborne: false,
    lean: 0,
    talk: null,
    guard: false,
    idle: true,
  };
  collision: CollisionWorld;
  weapon: Prop | null = null;
  style: GaitStyle = { ...NEUTRAL_STYLE };
  readonly reactions = new Reactions();
  /** Events of the last updates (take them with takeEvents). */
  readonly events: AnimEvent[] = [];
  /** Character root: ground position and facing (radians, 0 = +x, CCW; +y is yaw pi/2). */
  readonly rootPos: V3 = [0, 0, 0];
  rootYaw = 0;
  time = 0;
  /** The held prop's transform (world) after update. */
  readonly weaponPos: V3 = [0, 0, 0];
  weaponRot: Quat = [0, 0, 0, 1];
  phase = 0;
  gait: GaitParams = gaitFor(0, 0);
  /** Smoothed world velocity. */
  readonly velocity: V3 = [0, 0, 0];
  /** The settled stance (while a transition runs: the one it comes from). */
  stance: Stance = 'stand';

  // proportions
  private readonly k: number;
  private readonly legLen: number;
  private readonly ankleH: number;
  private readonly ballFwd: number;
  private readonly heelBack: number;
  private readonly footX: number;
  private readonly restPelvisZ: number;
  private readonly dims: Dims;

  // locomotion state
  private readonly feet: [Foot, Foot];
  private readonly lastPos: V3 = [0, 0, 0];
  private lastYaw = 0;
  private placed = false;
  private readonly velSpring = new Spring3(9, 1);
  private readonly accel: V3 = [0, 0, 0];
  private yawRate = 0;
  private readonly visZ = new Spring(16, 1);
  private readonly crouchS = new Spring(7, 1);
  private readonly lowerYaw = new Spring(7, 1);
  private readonly pelvisZ = new Spring(26, 1);
  private readonly impact = new Spring(15, 0.45);
  private readonly trunkLean = new Spring(6, 0.55);
  private readonly bank = new Spring(6, 0.8);
  private readonly swingAmp = new Spring(5, 1);
  private readonly aimW = new Spring(11, 1);
  private readonly readyW = new Spring(8, 1);
  private readonly sprintW = new Spring(6, 1);
  private readonly hipW = new Spring(9, 1);
  private readonly moodW = new Spring(6, 1);
  private readonly airW = new Spring(8, 1);
  private readonly leanS = new Spring(7, 1);
  private readonly aimYaw = new Spring(14, 1);
  private readonly aimPitch = new Spring(14, 1);
  private readonly headYaw = new Spring(7, 0.9);
  private readonly headPitch = new Spring(7, 0.9);
  private readonly kickBack = new Spring(32, 0.55);
  private readonly kickPitch = new Spring(26, 0.5);
  private readonly sway = new Spring3(7, 0.7);
  private readonly shift = new Spring(1.5, 1);
  private moodKind: Mood = 'normal';
  private stepping = false;
  private wasAirborne = false;
  private readonly seed: number;
  private readonly rng: Rng;

  // stances
  private stanceTo: Stance = 'stand';
  private stanceP = 1;
  private stanceDur = 0.5;
  private readonly stanceQueue: Stance[] = [];
  private downTimer = 0;
  private downBack = true;
  private readonly sA = newSample();
  private readonly sB = newSample();
  private readonly sOut = newSample();
  private readonly sC = newSample();
  private crawlPhase = 0;

  // actions
  private poseAct: ActionPlayer | null = null;
  private act: ActionPlayer | null = null;
  private readonly chPose: ChannelFrame = {};
  private readonly chAct: ChannelFrame = {};
  private idleTime = 0;
  private nextFidget = 6;
  private nextIdlePose = 3;
  private nextGesture = 1;
  private clutchW = new Spring(6, 1);

  constructor(skeleton: Skeleton, collision: CollisionWorld, seed = 1) {
    this.skeleton = skeleton;
    this.collision = collision;
    this.seed = seed;
    this.rng = new Rng(seed * 31 + 7);
    this.pose = new Pose(skeleton);
    this.fk = new ModelFK(skeleton);
    this.world = new WorldPose(skeleton);
    this.prevWorld = new WorldPose(skeleton);
    const rh = skeleton.restHead;
    this.restPelvisZ = rh[H.pelvis]![2];
    this.k = this.restPelvisZ / 0.97;
    this.legLen = vdist(rh[H.thighL]!, rh[H.shinL]!) + vdist(rh[H.shinL]!, rh[H.footL]!);
    this.ankleH = rh[H.footL]![2];
    this.ballFwd = rh[H.toeL]![1] - rh[H.footL]![1];
    this.heelBack = 0.06 * this.k;
    this.footX = Math.abs(rh[H.footL]![0]);
    this.dims = { k: this.k, ankleH: this.ankleH, footX: this.footX };
    const foot = (side: -1 | 1): Foot => ({
      side,
      thigh: side < 0 ? H.thighL : H.thighR,
      shin: side < 0 ? H.shinL : H.shinR,
      foot: side < 0 ? H.footL : H.footR,
      toe: side < 0 ? H.toeL : H.toeR,
      offset: side < 0 ? 0 : 0.5,
      planted: true,
      held: false,
      pos: [0, 0, 0],
      yaw: 0,
      lift: [0, 0, 0],
      liftYaw: 0,
      swing: 0,
      swingRate: 1,
      target: [0, 0, 0],
      targetYaw: 0,
      ankle: [0, 0, 0],
      pitch: 0,
    });
    this.feet = [foot(-1), foot(1)];
  }

  /** Forward unit vector (world) of a facing yaw. */
  static forward(yaw: number): V3 {
    return [Math.cos(yaw), Math.sin(yaw), 0];
  }

  private rootRot(): Quat {
    return qz(this.rootYaw - Math.PI / 2);
  }

  /** Puts the character at `pos` facing `yaw`, feet planted in the default stance. */
  place(pos: Readonly<V3>, yaw: number): void {
    vcopy(pos, this.rootPos);
    vcopy(pos, this.lastPos);
    this.rootYaw = yaw;
    this.lastYaw = yaw;
    this.visZ.x = pos[2];
    this.visZ.v = 0;
    this.velSpring.reset();
    vcopy([0, 0, 0], this.velocity);
    this.resetFeet(yaw);
    this.pelvisZ.x = this.restPelvisZ;
    this.placed = true;
    this.update(0);
    this.prevWorld.copyFrom(this.world);
  }

  private resetFeet(yaw: number): void {
    for (const f of this.feet) {
      this.nominalFoot(f, this.rootPos, yaw, f.pos);
      f.pos[2] = this.ground(f.pos[0], f.pos[1], this.rootPos[2], this.rootPos[2]);
      f.yaw = yaw - f.side * this.style.toeOut;
      f.planted = true;
      f.held = false;
      f.swing = 0;
      f.pitch = 0;
      vcopy(f.pos, f.target);
      this.ankleFromPlant(f.pos, f.yaw, 0, f.ankle);
    }
  }

  /** The character's root this frame (ground position under it, facing yaw). */
  setRoot(pos: Readonly<V3>, yaw: number): void {
    if (!this.placed || vdist(pos, this.rootPos) > 2) {
      this.place(pos, yaw);
      return;
    }
    vcopy(pos, this.rootPos);
    this.rootYaw = yaw;
  }

  /** Busy with a one-shot action. */
  get busy(): boolean {
    return this.act !== null && !this.act.done;
  }

  get actionName(): string | null {
    return this.act && !this.act.done ? this.act.def.name : null;
  }

  /** Mid stance transition (or knocked down): the host should not move the root. */
  get transitioning(): boolean {
    return this.stanceP < 1 || this.stanceQueue.length > 0 || this.stance === 'down';
  }

  /** Knocked down: falling, lying, or getting back up. */
  get knockedDown(): boolean {
    return this.downState;
  }

  private downState = false;

  /**
   * Starts a one-shot action (strike, block, reload, gesture, fidget), optionally aimed at a
   * world `target` (strikes). A held posture (guard, idle pose) goes on the pose layer. Returns
   * false if the body cannot (knocked down, mid transition).
   */
  play(name: string, target: V3 | null = null, rate = 1): boolean {
    const def = actionDef(name);
    if (this.stance === 'down' || this.stanceP < 1) return false;
    if (def.layer === 'pose') {
      this.setPoseAction(def);
      return true;
    }
    this.act?.stop();
    this.act = new ActionPlayer(def, target ? [target[0], target[1], target[2]] : null, rate);
    return true;
  }

  /** Updates the target of the running action (a strike follows a moving opponent). */
  aimAction(target: V3): void {
    if (this.act) this.act.target = [target[0], target[1], target[2]];
  }

  private setPoseAction(def: ActionDef | null): void {
    if (this.poseAct && def && this.poseAct.def === def && !this.poseAct.done) return;
    this.poseAct?.stop();
    if (def) this.poseAct = new ActionPlayer(def);
  }

  takeEvents(): AnimEvent[] {
    return this.events.splice(0, this.events.length);
  }

  /** Recoil of one shot. */
  fire(strength = 1): void {
    const pistol = this.weapon?.kind === 'pistol';
    this.kickBack.kick((pistol ? 0.8 : 1.1) * strength);
    this.kickPitch.kick((pistol ? 12 : 7) * strength);
    const s = this.reactions.springs.get(H.chest);
    s?.kick([1, 0, 0], (pistol ? 0.15 : 0.35) * strength);
  }

  /**
   * A hit on the body (world): `point` where it lands, `dir` the direction it travels, `force`
   * (1 ~ a rifle round or a punch), its kind and, if known, the bone. Returns the zone.
   */
  hitAt(info: HitInfo): Zone {
    const inv = qconj(this.rootRot());
    const origin: V3 = [this.rootPos[0], this.rootPos[1], this.visZ.x];
    const pm = qrotate(inv, vsub(info.point, origin));
    const dm = qrotate(inv, vnorm(info.dir));
    const bone = info.bone ?? this.nearestBone(info.point);
    const knockBefore: V3 = [this.reactions.knockback[0], this.reactions.knockback[1], 0];
    const zone = this.reactions.hit(this.skeleton, (b) => this.fk.p[b]!, pm, dm, info.force, info.kind, bone);
    // knockback came out in model space: turn the added part into world space
    const add = qrotate(this.rootRot(), [this.reactions.knockback[0] - knockBefore[0], this.reactions.knockback[1] - knockBefore[1], 0]);
    this.reactions.knockback[0] = knockBefore[0] + add[0];
    this.reactions.knockback[1] = knockBefore[1] + add[1];
    // the wound, for a hand to go to
    const c = this.reactions.clutch;
    if (c && c.bone === bone) {
      const q = this.world.q[bone]!;
      const local = qrotate(qconj(q), vsub(info.point, this.world.p[bone]!));
      c.rest = vadd(this.skeleton.restHead[bone]!, local);
    }
    // a strong hit interrupts what the body was doing
    if (info.force > 0.8 && this.act && !this.act.def.name.startsWith('block')) this.act.stop();
    if (this.reactions.down && this.stanceTo !== 'down') {
      this.downBack = this.reactions.down.back;
      this.reactions.down = null;
      this.knockDown(this.downBack);
    }
    return zone;
  }

  /** Legacy hit: a direction, a strength and a height 0 (legs) .. 1 (head). */
  hit(dir: Readonly<V3>, strength = 1, height = 0.7): void {
    const bone = height > 0.9 ? H.head : height > 0.65 ? H.chest : height > 0.45 ? H.spine : height > 0.3 ? H.pelvis : H.thighR;
    const p = this.world.p[bone]!;
    this.hitAt({ point: [p[0], p[1], p[2]], dir: [dir[0], dir[1], dir[2]], force: strength, kind: 'bullet', bone });
  }

  /** Knocked down: falls (backwards or forwards) and gets up after a while. */
  knockDown(back: boolean): void {
    this.downBack = back;
    this.stanceQueue.length = 0;
    this.act?.stop();
    this.beginTransition('down');
    this.downTimer = 1.4 + this.rng.next() * 1.4;
    this.downState = true;
  }

  /** Knockback displacement for this frame (world, m): hosts move the root by it. */
  takeKnockback(dt: number): V3 {
    return this.reactions.takeKnockback(dt);
  }

  /** The bone nearest to a world point (by bone segments). */
  nearestBone(p: Readonly<V3>): number {
    let best: number = H.chest, bd = Infinity;
    const w = this.world;
    for (let b = 1; b < this.skeleton.count; b++) {
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

  private nominalFoot(f: Foot, root: Readonly<V3>, yaw: number, out: V3): V3 {
    const c = this.crouchS.x;
    const x = f.side * this.footX * this.style.width * (1 + 0.45 * c);
    const o = qrotate(qz(yaw - Math.PI / 2), [x, -0.02 * this.k, 0]);
    out[0] = root[0] + o[0];
    out[1] = root[1] + o[1];
    out[2] = root[2];
    return out;
  }

  private ground(x: number, y: number, zRef: number, fallback: number): number {
    const g = this.collision.groundHeight(x, y, zRef + 0.6 * this.k, zRef - 0.9 * this.k);
    return g ?? fallback;
  }

  // ---- stances ------------------------------------------------------------------------------

  private beginTransition(to: Stance): void {
    // (a transition interrupted by another starts from where the first was going)
    const from = this.stanceP >= 1 ? this.stance : this.stanceTo;
    this.stance = from;
    this.stanceTo = to;
    this.stanceP = 0;
    this.stanceDur = transitionTime(from, to);
  }

  private updateStance(dt: number): void {
    if (this.stanceP < 1) {
      this.stanceP = Math.min(1, this.stanceP + dt / this.stanceDur);
      if (this.stanceP >= 1) {
        this.stance = this.stanceTo;
        const next = this.stanceQueue.shift();
        if (next) this.beginTransition(next);
      }
      return;
    }
    // knocked down: lie, then get up to what the host wants
    if (this.stance === 'down') {
      this.downTimer -= dt;
      if (this.downTimer > 0) return;
    }
    let want = this.input.stance;
    if (want === 'sit' && !this.input.seat) want = 'stand';
    if (want === 'down') want = this.stance;
    if (want === this.stance && this.stanceQueue.length === 0) this.downState = false;
    if (want !== this.stance) {
      const route = stanceRoute(this.stance, want);
      const first = route.shift();
      if (first) {
        this.stanceQueue.push(...route);
        this.beginTransition(first);
      }
    }
  }

  private weightOf(s: Stance): number {
    const e = this.stanceTo === 'down' ? this.stanceP * this.stanceP : this.stanceP * this.stanceP * (3 - 2 * this.stanceP);
    if (this.stanceP >= 1) return s === this.stance ? 1 : 0;
    return (s === this.stanceTo ? e : 0) + (s === this.stance ? 1 - e : 0);
  }

  private sampleStance(s: Stance, out: StanceSample, standing: StanceSample, toModel: (w: Readonly<V3>) => V3): StanceSample {
    const d = this.dims;
    switch (s) {
      case 'stand':
        return blendSamples(standing, standing, 0, out);
      case 'kneel':
        return kneelSample(d, out);
      case 'prone': {
        const sp = Math.hypot(this.velocity[0], this.velocity[1]);
        const crawl = smoothstep(0.05, 0.3, sp);
        return proneSample(d, crawl, this.crawlPhase, out);
      }
      case 'sit': {
        const seat = this.input.seat;
        const pos = seat ? toModel(seat.pos) : ([0, -0.38 * this.k, 0.46 * this.k] as V3);
        const desk = seat?.deskHeight ?? null;
        const variant: SitVariant = seat?.variant ?? (desk !== null && desk !== undefined ? 'desk' : 'upright');
        return sitSample(d, { pos, backrest: seat?.backrest ?? false, desk: desk !== null && desk !== undefined ? desk + (this.visZ.x - this.rootPos[2]) : null, variant }, this.time + this.seed, out);
      }
      case 'ground':
        return groundSample(d, this.input.groundVariant, this.time + this.seed, out);
      case 'down':
        return downSample(d, this.downBack, this.time, out);
    }
  }

  // ---- update -------------------------------------------------------------------------------

  update(dtIn: number): void {
    const dt = Math.min(0.05, Math.max(0, dtIn));
    this.prevWorld.copyFrom(this.world);
    this.time += dt;
    const inp = this.input;
    const k = this.k;
    const st = this.style;

    // ---- motion estimate ------------------------------------------------------------------
    if (dt > 0) {
      const raw: V3 = vscale(vsub(this.rootPos, this.lastPos), 1 / dt);
      raw[2] = 0;
      const before = vcopy(this.velSpring.x);
      this.velSpring.omega = lerp(11, 6.5, st.heavy);
      this.velSpring.update(raw, dt);
      const a = vscale(vsub(this.velSpring.x, before), 1 / dt);
      for (let i = 0; i < 3; i++) this.accel[i] = lerp(this.accel[i]!, a[i]!, 1 - Math.exp(-dt * 6));
      const yr = wrapAngle(this.rootYaw - this.lastYaw) / dt;
      this.yawRate = lerp(this.yawRate, yr, 1 - Math.exp(-dt * 8));
    }
    vcopy(this.velSpring.x, this.velocity);
    vcopy(this.rootPos, this.lastPos);
    this.lastYaw = this.rootYaw;
    if (Math.abs(this.rootPos[2] - this.visZ.x) > 0.6 * k) this.visZ.x = this.rootPos[2];
    this.visZ.update(this.rootPos[2], dt);

    // ---- reactions, stance, actions --------------------------------------------------------
    const R = this.reactions;
    R.update(dt);
    this.updateStance(dt);
    this.updateActions(dt);
    const chP = this.poseAct && !this.poseAct.done ? this.chPose : null;
    const chA = this.act && !this.act.done ? this.chAct : null;
    const wP = chP ? this.poseAct!.weight : 0;
    const wA = chA ? this.act!.weight : 0;
    const add = (c: keyof ChannelFrame, i: number): number => (chP?.[c]?.[i] ?? 0) * wP + (chA?.[c]?.[i] ?? 0) * wA;

    const crouchIn = inp.mood === 'cower' ? 1 : inp.crouch;
    this.crouchS.update(clamp(crouchIn + add('crouch', 0), 0, 1), dt);
    const crouch = clamp(this.crouchS.x, 0, 1);
    const vel = this.velocity;
    const speed = Math.hypot(vel[0], vel[1]);
    const rootRot = this.rootRot();
    const inv = qconj(rootRot);
    const vLocal = qrotate(inv, vel);
    const aLocal = qrotate(inv, this.accel);
    const armed = this.weapon !== null && this.weapon.kind !== 'knife';
    const aiming = armed && (inp.carry === 'aim' || inp.carry === 'hip') && inp.aimAt !== null;
    const standW = this.weightOf('stand');

    // lower body heading relative to the facing: bladed when shouldering a rifle, towards the
    // motion when strafing
    let lower = 0;
    if (speed > 0.3) {
      const th = Math.atan2(-vLocal[0], vLocal[1]);
      if (Math.abs(th) < 1.75) lower = clamp(th, -1.0, 1.0) * 0.65;
      else lower = clamp(wrapAngle(th - Math.PI), -1.0, 1.0) * 0.65;
    } else if (aiming && this.weapon?.kind !== 'pistol') lower = -0.42;
    this.lowerYaw.update(lower, dt);

    // ---- gait -------------------------------------------------------------------------------
    const aimMove = this.aimW.x;
    let g = gaitFor(speed, crouch, k);
    // personal style, the tactical walk while aiming, a limp, pain
    const tactical = clamp(aimMove, 0, 1) * (1 - g.run);
    const limpL = R.limp[0], limpR = R.limp[1];
    const pain = R.pain;
    g = {
      ...g,
      freq: g.freq / Math.pow(st.stride * (1 - 0.15 * tactical), 0.7),
      bob: g.bob * st.bounce * (1 - 0.7 * tactical),
      sway: g.sway * st.sway * (1 - 0.4 * tactical),
      hipYaw: g.hipYaw * st.sway * (1 - 0.5 * tactical),
      hipRoll: g.hipRoll * st.sway * (1 - 0.3 * tactical),
      armSwing: g.armSwing * st.arms * (1 - 0.35 * pain),
      elbow: g.elbow + st.elbow,
      sink: g.sink + 0.055 * k * tactical + 0.02 * k * pain,
      lean: g.lean * (1 + 0.4 * st.heavy) + 0.05 * tactical,
    };
    this.gait = g;
    const moving = speed > 0.12 && !inp.airborne && standW > 0.99;
    let prevPhase = this.phase;
    const bodyYaw = this.rootYaw + this.lowerYaw.x;
    if (standW < 0.999) {
      // another stance: the feet wait at the standing stance's spots
      this.resetFeet(bodyYaw);
      this.stepping = false;
    } else if (moving) {
      if (!this.stepping) {
        const dir = vnorm([vel[0], vel[1], 0]);
        const d0 = (this.feet[0].pos[0] - this.rootPos[0]) * dir[0] + (this.feet[0].pos[1] - this.rootPos[1]) * dir[1];
        const d1 = (this.feet[1].pos[0] - this.rootPos[0]) * dir[0] + (this.feet[1].pos[1] - this.rootPos[1]) * dir[1];
        const lead = d0 <= d1 ? this.feet[0] : this.feet[1];
        if (this.feet[0].planted && this.feet[1].planted) {
          this.phase = fract(g.duty - 0.02 - lead.offset);
          prevPhase = this.phase;
        }
        this.stepping = true;
      }
      // a limp hurries the step off the wounded leg
      const inStanceL = fract(this.phase) < g.duty;
      const inStanceR = fract(this.phase + 0.5) < g.duty;
      const hurry = 1 + 0.9 * (inStanceL ? limpL : 0) + 0.9 * (inStanceR ? limpR : 0);
      this.phase = fract(this.phase + g.freq * hurry * dt);
    } else if (!inp.airborne) {
      let need = false;
      for (const f of this.feet) {
        if (f.held) continue;
        if (!f.planted) need = true;
        else {
          const nom = this.nominalFoot(f, this.rootPos, bodyYaw, [0, 0, 0]);
          const err = Math.hypot(nom[0] - f.pos[0], nom[1] - f.pos[1]);
          const yawErr = Math.abs(wrapAngle(bodyYaw - f.side * st.toeOut - f.yaw));
          if (err > 0.16 * k || yawErr > 0.6 || (this.stepping && err > 0.07 * k)) need = true;
        }
      }
      if (need) this.phase = fract(this.phase + 1.5 * dt);
      else this.stepping = false;
    }
    if (standW >= 0.999) this.updateFeet(dt, g, prevPhase, moving, speed, bodyYaw, vel);

    // crawling clock (prone)
    this.crawlPhase = fract(this.crawlPhase + dt * Math.min(1.2, speed / 0.35));

    // ---- the standing sample -----------------------------------------------------------------
    const origin: V3 = [this.rootPos[0], this.rootPos[1], this.visZ.x];
    const toModel = (w: Readonly<V3>): V3 => qrotate(inv, vsub(w, origin));
    const ph = this.phase;
    const D = g.duty;
    const run = g.run;
    const moveAmt = smoothstep(0.1, 0.9, speed);
    const idle = 1 - moveAmt;
    const walkBob = moving ? g.bob * (1 - 2 * run) * Math.cos(TAU * 2 * (ph - D / 2)) : 0;
    const swayX = moving ? -g.sway * Math.cos(TAU * (ph - D / 2)) : 0;
    const hipYawOsc = moving ? -g.hipYaw * Math.cos(TAU * ph) : 0;
    let hipRoll = moving ? -g.hipRoll * Math.cos(TAU * (ph - (1 + D) / 2)) : 0;
    this.moodKind = inp.mood;
    this.moodW.update(inp.mood === 'normal' ? 0 : 1, dt);
    this.airW.update(inp.airborne ? 1 : 0, dt);
    const breathe = Math.sin(this.time * 1.9 + this.seed);
    // idle weight shift (contrapposto): the hips settle over one leg, the free knee bends
    const shiftTarget = idle > 0.5 && !aiming && !inp.guard ? (Math.floor((this.time + this.seed * 3.7) / (5 + 4 * (1 - st.fidget))) % 2 === 0 ? 1 : -1) : 0;
    this.shift.update(shiftTarget, dt);
    const shiftX = this.shift.x * 0.035 * k * idle;
    hipRoll += this.shift.x * 0.07 * idle;
    const crouchDrop = crouch * 0.4 * k;
    // limp: the pelvis dips over the wounded leg while it bears weight
    const limpDip = moving ? (fract(ph) < D ? limpL : 0) * 0.05 * k + (fract(ph + 0.5) < D ? limpR : 0) * 0.05 * k : 0;
    let pz = this.restPelvisZ - g.sink - crouchDrop + walkBob - 0.012 * k * idle - limpDip;
    const pelvisYaw = this.lowerYaw.x + hipYawOsc;
    // banking into turns and a spring-loaded lean into (de)acceleration
    this.bank.update(clamp(-this.yawRate * speed * 0.04, -0.2, 0.2), dt);
    this.trunkLean.omega = lerp(7.5, 5, st.heavy);
    this.trunkLean.update(clamp(aLocal[1] * (0.03 + 0.02 * st.heavy), -0.2, 0.24), dt);
    const pelvisRot = qmul(qz(pelvisYaw), qeuler(-(0.1 * crouch + 0.05 * g.lean), hipRoll + this.bank.x * 0.6, 0));
    const px = swayX + shiftX;
    const py = -0.07 * crouch * k;
    const lowest = this.restPelvisZ - 0.2 * k - crouchDrop;
    for (const f of this.feet) {
      const a = toModel(f.ankle);
      const hipOff = qrotate(pelvisRot, vsub(this.skeleton.restHead[f.thigh]!, this.skeleton.restHead[H.pelvis]!));
      const dx = px + hipOff[0] - a[0];
      const dy = py + hipOff[1] - a[1];
      const reach2 = (0.995 * this.legLen) ** 2 - dx * dx - dy * dy;
      let maxZ = (reach2 > 0 ? Math.sqrt(reach2) : 0) + a[2] - hipOff[2];
      if (!f.planted) maxZ = lerp(pz, maxZ, smoothstep(0.45, 0.9, f.swing));
      if (f.held) maxZ = pz;
      if (pz > maxZ) pz = Math.max(maxZ, lowest);
    }
    if (inp.airborne) pz = this.restPelvisZ - 0.04 * k;
    this.pelvisZ.update(pz, dt);
    this.impact.update(0, dt);
    const pzS = Math.min(this.pelvisZ.x, pz + 0.01 * k);
    const S0 = this.sA;
    S0.pelvisPos = [px, py, pzS];
    S0.pelvisRot = pelvisRot;
    const lean = g.lean + this.trunkLean.x + crouch * 0.38 + (this.moodKind === 'panic' ? this.moodW.x * 0.12 : 0) + (this.moodKind === 'cower' ? this.moodW.x * 0.25 : 0);
    // posture: slouched (chest and neck forward) .. upright (chest up)
    const posture = st.posture * 0.07;
    const counter = -hipYawOsc * 1.6 * (1 - 0.8 * tactical);
    S0.spine = qeuler(-lean * 0.5 + (0.1 * crouch + 0.05 * g.lean) + posture, -hipRoll * 0.6 - this.bank.x * 0.3, counter * 0.4);
    S0.chest = qeuler(-lean * 0.45 + 0.02 * breathe * (1 - 0.5 * moveAmt) + posture * 0.8, -hipRoll * 0.4 - this.bank.x * 0.2, counter * 0.6);
    S0.neck = qx(posture * 0.6);
    S0.head = qx(0);
    for (let i = 0; i < 2; i++) {
      const f = this.feet[i]!;
      const fp = S0.feet[i]!;
      fp.ankle = toModel(f.ankle);
      const fy = wrapAngle(f.yaw - this.rootYaw);
      const footFwd: V3 = [-Math.sin(fy), Math.cos(fy), 0];
      fp.pole = vnorm(vadd(footFwd, [f.side * (0.15 + 0.22 * crouch), 0, 0.1]));
      fp.rot = qmul(qz(fy), qx(f.pitch));
      fp.toe = f.planted && f.pitch < 0 ? -f.pitch * 0.9 : this.airW.x * 0.2;
    }
    S0.hands = [null, null];
    S0.turn = 1;

    // ---- blend with the other stance -----------------------------------------------------------
    let S = S0;
    if (standW < 0.999) {
      const other = this.stanceP >= 1 ? this.stance : this.stance === 'stand' ? this.stanceTo : this.stanceTo === 'stand' ? this.stance : null;
      if (other && other !== 'stand') {
        // stand <-> other
        const so = this.sampleStance(other, this.sB, S0, toModel);
        S = blendSamples(S0, so, 1 - standW, this.sOut);
      } else {
        // between two non-standing stances
        const a = this.sampleStance(this.stance, this.sB, S0, toModel);
        const b = this.sampleStance(this.stanceTo, this.sC, S0, toModel);
        S = blendSamples(a, b, this.weightOf(this.stanceTo), this.sOut);
      }
      // people lean forward while getting down or up
      if (this.stanceP < 1 && (this.stanceTo === 'sit' || this.stance === 'sit' || this.stanceTo === 'kneel' || this.stance === 'kneel')) {
        const bend = Math.sin(Math.PI * this.stanceP) * 0.35;
        S.spine = qmul(qx(-bend), S.spine);
        S.chest = qmul(qx(-bend * 0.6), S.chest);
      }
    }

    // ---- pose: pelvis and trunk ------------------------------------------------------------------
    const pose = this.pose;
    pose.reset();
    const shiftR = R.shift.x;
    const impactZ = this.impact.x;
    vcopy([S.pelvisPos[0] + add('pelvis', 0) * k + shiftR[0], S.pelvisPos[1] + add('pelvis', 1) * k + shiftR[1], S.pelvisPos[2] + add('pelvis', 2) * k + shiftR[2] + impactZ], pose.t[H.pelvis]);
    const pr: V3 = [add('pelvisRot', 0), add('pelvisRot', 1), add('pelvisRot', 2)];
    pose.r[H.pelvis] = qmul(S.pelvisRot, qeuler(pr[0] * DEG, pr[1] * DEG, pr[2] * DEG));

    // aim and look: yaw and pitch of the target seen from the chest (model space)
    let wantYaw = 0;
    let wantPitch = 0;
    const tgt = aiming ? inp.aimAt : (inp.lookAt ?? (armed && inp.carry !== 'relaxed' ? inp.aimAt : null));
    const chestRest = this.skeleton.restHead[H.chest]!;
    if (tgt) {
      const m = toModel(tgt);
      const dd = vsub(m, [0, 0, chestRest[2] + 0.2 * k]);
      wantYaw = clamp(Math.atan2(-dd[0], dd[1]), -1.9, 1.9);
      wantPitch = clamp(Math.atan2(dd[2], Math.hypot(dd[0], dd[1])), -1.1, 1.1);
    }
    this.aimYaw.update(wantYaw, dt);
    this.aimPitch.update(wantPitch, dt);
    this.aimW.update(aiming ? 1 : 0, dt);
    this.leanS.update(clamp(inp.lean, -1, 1), dt);
    const aw = this.aimW.x;
    const rifle = this.weapon !== null && this.weapon.kind !== 'pistol' && this.weapon.kind !== 'knife';
    const blade = aw * (rifle && inp.carry === 'aim' ? 0.3 : 0);
    const turn = S.turn;
    const trunkYaw = ((aiming || (tgt && inp.carry !== 'relaxed')) && tgt ? this.aimYaw.x - pelvisYaw * standW - blade : 0) * turn;
    const trunkPitch = (tgt && (aiming || inp.carry !== 'relaxed') ? this.aimPitch.x * lerp(0.55, 0.75, aw) : 0) * turn;
    // peeking: the trunk leans out sideways (the head stays upright)
    const peek = this.leanS.x * 0.3;
    const fold = R.fold.x * 0.55;
    const sp: V3 = [add('spine', 0) * DEG, add('spine', 1) * DEG, add('spine', 2) * DEG];
    const ch: V3 = [add('chest', 0) * DEG, add('chest', 1) * DEG, add('chest', 2) * DEG];
    pose.r[H.spine] = qmul(qmul(qeuler(trunkPitch * 0.3 - fold * 0.55, peek * 0.55, trunkYaw * 0.35), S.spine), qeuler(sp[0], sp[1], sp[2]));
    pose.r[H.chest] = qmul(qmul(qeuler(trunkPitch * 0.45 - fold * 0.45, peek * 0.45, trunkYaw * 0.45), S.chest), qeuler(ch[0], ch[1], ch[2]));
    if (inp.lean !== 0 || Math.abs(this.leanS.x) > 0.01) pose.t[H.pelvis]![0] += this.leanS.x * 0.07 * k;
    this.fk.update(pose, 0);
    // hit reactions of the trunk (model-space springs)
    for (const b of [H.pelvis, H.spine, H.chest]) this.applyReaction(b);

    // ---- head ---------------------------------------------------------------------------------
    const lookW = clamp(chA?.look?.[0] !== undefined ? lerp(1, chA.look[0]!, wA) : chP?.look?.[0] !== undefined ? lerp(1, chP.look[0]!, wP) : 1, 0, 1);
    let lookYaw = 0;
    let lookPitch = -0.06 - lean * 0.3;
    if (tgt) {
      lookYaw = this.aimYaw.x;
      lookPitch = this.aimPitch.x;
    } else if (idle > 0.5 && this.moodKind === 'normal' && !inp.talk) {
      lookYaw = (valueNoise(this.time * 0.18, 3.1, this.seed, 1, this.seed + 9) * 2 - 1) * 1.0;
      lookPitch = (valueNoise(this.time * 0.13, 7.7, this.seed, 1, this.seed + 3) * 2 - 1) * 0.25;
    } else if (this.moodKind === 'panic') {
      lookYaw = Math.sin(this.time * 2.7 + this.seed) * 0.7;
    }
    if (this.moodKind === 'cower') lookPitch = lerp(lookPitch, -0.7, this.moodW.x);
    // saccades: the head turns fast to a new target, then settles
    const far = Math.abs(lookYaw - this.headYaw.x) > 0.5;
    this.headYaw.omega = far ? 10 : 6;
    this.headYaw.update(lookYaw, dt);
    this.headPitch.update(lookPitch, dt);
    const chestQ = this.fk.q[H.chest]!;
    const chestFwd = qrotate(chestQ, [0, 1, 0]);
    const chestYawNow = Math.atan2(-chestFwd[0], chestFwd[1]);
    const chestPitchNow = Math.asin(clamp(chestFwd[2], -1, 1));
    const relYaw = clamp(wrapAngle(this.headYaw.x - chestYawNow), -1.3, 1.3) * lookW;
    const relPitch = clamp(this.headPitch.x - chestPitchNow, -0.9, 0.8) * lookW;
    const nk: V3 = [add('neck', 0) * DEG, add('neck', 1) * DEG, add('neck', 2) * DEG];
    const hd: V3 = [add('head', 0) * DEG, add('head', 1) * DEG, add('head', 2) * DEG];
    pose.r[H.neck] = qmul(qmul(qeuler(relPitch * 0.4, -peek * 0.4, relYaw * 0.4), S.neck), qeuler(nk[0], nk[1], nk[2]));
    pose.r[H.head] = qmul(qmul(qeuler(relPitch * 0.6, -0.12 * aw * (rifle ? 1 : 0) - peek * 0.4, relYaw * 0.6), S.head), qeuler(hd[0], hd[1], hd[2]));
    this.fk.update(pose, H.neck);
    this.applyReaction(H.neck);
    this.applyReaction(H.head);
    this.fk.update(pose, H.neck);

    // ---- legs (last: every pelvis motion bends the knees, the feet stay) ---------------------
    for (let i = 0; i < 2; i++) {
      const f = this.feet[i]!;
      const fp = S.feet[i]!;
      let target = fp.ankle;
      let rot = fp.rot;
      let pole = fp.pole;
      const side = i === 0 ? 'L' : 'R';
      const fw = clamp((chA?.[`foot${side}w` as 'footRw']?.[0] ?? 0) * wA, 0, 1);
      f.held = fw > 0.02;
      if (fw > 0.02 && chA) {
        let a = tuple(chA[`foot${side}` as 'footR'], target);
        a = [a[0] * k, a[1] * k, a[2] * k];
        const s = chA[`strikeFoot${side}` as 'strikeFootR']?.[0] ?? 0;
        if (s > 0 && this.act?.target) a = vlerp(a, toModel(this.act.target), clamp(s, 0, 1.2));
        target = vlerp(target, a, fw);
        pole = vlerp(pole, [0.1 * f.side, 1, 0.4], fw);
        rot = qnlerp(rot, qx(-0.6), fw);
      }
      solveTwoBone(pose, this.fk, f.thigh, f.shin, f.foot, target, pole, 0.02);
      setModelRotation(pose, this.fk, f.foot, rot);
      pose.r[f.toe] = qx(fp.toe);
      this.fk.updateBone(pose, f.toe);
    }

    // ---- arms ---------------------------------------------------------------------------------
    this.armSwing(dt, g, moving, speed, breathe, idle);
    this.fk.update(pose, H.clavicleL);
    // stance hands (seated on the thighs, prone on the elbows...)
    this.restHands(S, 1 - standW);
    let heldProp = false;
    if (this.weapon && this.weapon.kind !== 'knife') {
      heldProp = this.holdWeapon(dt, aiming, run, moving, toModel, chP, chA, wP, wA);
    } else this.weaponRot = qmul(rootRot, [0, 0, 0, 1]);
    const freeHands = !heldProp;
    if (freeHands && this.moodW.x > 0.01 && this.moodKind !== 'normal') this.moodArms(this.moodW.x);
    // action hands: the posture layer, then one-shots on top
    if (chP && wP > 0) this.actionHands(chP, wP, this.poseAct!, toModel, heldProp);
    if (chA && wA > 0) this.actionHands(chA, wA, this.act!, toModel, heldProp);
    // clavicle channels (shrugs)
    for (const [c, b] of [['clavR', H.clavicleR], ['clavL', H.clavicleL]] as const) {
      const e: V3 = [add(c, 0) * DEG, add(c, 1) * DEG, add(c, 2) * DEG];
      if (e[0] !== 0 || e[1] !== 0 || e[2] !== 0) {
        pose.rotateLocal(b, qeuler(e[0], e[1], e[2]));
        this.fk.updateSubtree(pose, b);
      }
    }
    this.clutchHand(dt, heldProp);
    for (const b of [H.clavicleL, H.clavicleR, H.upperarmL, H.upperarmR, H.forearmL, H.forearmR]) this.applyReaction(b);
    this.fk.update(pose, H.clavicleL);

    // ---- props, world, events ---------------------------------------------------------------------
    this.world.compute(pose, origin, rootRot);
    if (this.weapon) {
      if (this.weapon.kind === 'knife' || (this.weapon.kind === 'pistol' && !heldProp)) this.propInHand(H.handR);
      else {
        vcopy(this.world.p[H.weapon]!, this.weaponPos);
        this.weaponRot = [...this.world.q[H.weapon]!] as Quat;
      }
    }
    this.flushEvents();
  }

  /** Applies bone b's reaction spring as a model-space rotation on top of its pose. */
  private applyReaction(b: number): void {
    const o = this.reactions.offset(b);
    if (!o || (o[0] === 0 && o[1] === 0 && o[2] === 0)) return;
    if (Math.abs(o[0]) + Math.abs(o[1]) + Math.abs(o[2]) < 1e-5) return;
    setModelRotation(this.pose, this.fk, b, qmul(qexp(o), this.fk.q[b]!));
    this.fk.updateSubtree(this.pose, b);
  }

  // ---- locomotion ---------------------------------------------------------------------------

  private updateFeet(dt: number, g: GaitParams, prevPhase: number, moving: boolean, speed: number, bodyYaw: number, vel: V3): void {
    const inp = this.input;
    const k = this.k;
    const st = this.style;
    if (this.wasAirborne && !inp.airborne) {
      for (const f of this.feet) {
        this.nominalFoot(f, this.rootPos, bodyYaw, f.pos);
        f.pos[2] = this.ground(f.pos[0], f.pos[1], this.rootPos[2], this.rootPos[2]);
        f.yaw = bodyYaw - f.side * st.toeOut;
        f.planted = true;
      }
      this.impact.kick(-1.6);
    }
    this.wasAirborne = inp.airborne;
    const D = g.duty;
    const freq = moving ? g.freq : 1.5;
    const stanceT = D / freq;
    const moveAmt = smoothstep(0.1, 0.9, speed);
    for (const f of this.feet) {
      if (f.held) {
        // an action has the foot: when it lets go the foot lands where it is
        f.planted = false;
        f.swingRate = 0;
        const a = this.fk.p[f.foot]!;
        const w = vadd(qrotate(this.rootRot(), a), [this.rootPos[0], this.rootPos[1], this.visZ.x]);
        f.pos = [w[0], w[1], this.ground(w[0], w[1], this.rootPos[2], this.rootPos[2])];
        f.ankle = [w[0], w[1], w[2]];
        continue;
      }
      if (!f.planted && f.swingRate === 0) {
        // released after an action: plant now
        f.planted = true;
        f.pos[2] = this.ground(f.pos[0], f.pos[1], this.rootPos[2], this.rootPos[2]);
        this.ankleFromPlant(f.pos, f.yaw, 0, f.ankle);
        continue;
      }
      const p0 = fract(prevPhase + f.offset);
      const p1 = fract(this.phase + f.offset);
      const advanced = this.phase !== prevPhase;
      if (inp.airborne) f.planted = false;
      else if (f.planted) {
        const wrapped = p1 < p0;
        const crossedLift = advanced && ((!wrapped && p0 < D && p1 >= D) || (wrapped && p0 < D));
        const far = Math.hypot(f.pos[0] - this.rootPos[0], f.pos[1] - this.rootPos[1]) > 0.62 * this.legLen;
        if (crossedLift || far) {
          f.planted = false;
          vcopy(f.pos, f.lift);
          f.liftYaw = f.yaw;
          f.swing = 0;
          f.swingRate = crossedLift ? freq / (1 - D) : 1 / 0.2;
        }
      }
      if (!inp.airborne && !f.planted) {
        f.swing = Math.min(1, f.swing + f.swingRate * dt);
        const remain = (1 - f.swing) / f.swingRate;
        const pred: V3 = [this.rootPos[0] + vel[0] * remain, this.rootPos[1] + vel[1] * remain, this.rootPos[2]];
        const tgt = this.nominalFoot(f, pred, bodyYaw, [0, 0, 0]);
        const reach = lerp(0.52, 0.34, g.run) * this.legLen;
        const ahead = stanceT * lerp(0.5, 0.36, g.run);
        let hx = vel[0] * ahead, hy = vel[1] * ahead;
        const hl = Math.hypot(hx, hy);
        if (hl > reach) {
          hx *= reach / hl;
          hy *= reach / hl;
        }
        tgt[0] += hx;
        tgt[1] += hy;
        tgt[2] = this.ground(tgt[0], tgt[1], this.rootPos[2], this.rootPos[2]);
        vcopy(tgt, f.target);
        f.targetYaw = bodyYaw - f.side * st.toeOut;
        if (f.swing >= 1) {
          f.planted = true;
          vcopy(f.target, f.pos);
          f.yaw = f.targetYaw;
          // footfall: the body settles onto the leg (heavier characters and faster gaits more),
          // the head nods with it unless it is held still
          if (moving) {
            const sp = Math.min(1.4, speed / 2.5);
            this.impact.kick(-(0.18 + 0.4 * st.heavy) * sp);
            this.reactions.springs.get(H.head)?.kick([1, 0, 0], -0.5 * sp * (1 - st.headStill));
            this.reactions.springs.get(H.neck)?.kick([1, 0, 0], -0.25 * sp * (1 - st.headStill));
          }
        }
      }
      if (inp.airborne) {
        const nom = this.nominalFoot(f, this.rootPos, bodyYaw, [0, 0, 0]);
        nom[2] = this.visZ.x + 0.12 * k + (f.side < 0 ? 0.05 : 0) * k;
        vcopy(nom, f.ankle);
        f.pitch = -0.3;
        f.yaw = bodyYaw;
        vcopy(nom, f.pos);
        f.pos[2] -= this.ankleH;
        f.lift = vcopy(f.pos);
        continue;
      }
      if (f.planted) {
        const gz = this.ground(f.pos[0], f.pos[1], f.pos[2] + 0.2 * k, f.pos[2]);
        if (gz < f.pos[2] - 0.01) f.pos[2] = Math.max(gz, f.pos[2] - 3 * dt);
        const u = p1 < D ? p1 / D : 0.3;
        let pitch = 0;
        if (moving) {
          const hs = lerp(0.28, 0.1, g.run) * moveAmt;
          const to = lerp(0.45, 0.65, g.run) * moveAmt;
          pitch = hs * (1 - smoothstep(0, 0.18, u)) - to * smoothstep(0.5, 1.0, u);
        }
        f.pitch = pitch;
        this.ankleFromPlant(f.pos, f.yaw, pitch, f.ankle);
      } else {
        const s = f.swing;
        const sh = lerp(s, Math.pow(s, 1.6), g.run);
        const eh = sh * sh * (3 - 2 * sh);
        const hz = vlerp(f.lift, f.target, eh);
        const peak = Math.sin(Math.PI * Math.pow(s, lerp(1, 0.62, g.run)));
        const lift = (moving ? g.lift : 0.06 * k) * peak + Math.max(0, f.target[2] - f.lift[2]) * 0.3 * peak;
        hz[2] += lift;
        const e = s * s * (3 - 2 * s);
        const yaw = angleLerp(f.liftYaw, f.targetYaw, e);
        const to = moving ? lerp(0.45, 0.65, g.run) * moveAmt : 0.15;
        const hs = moving ? lerp(0.28, 0.1, g.run) * moveAmt : 0.05;
        f.pitch = lerp(-to, hs, smoothstep(0.15, 0.95, s));
        f.ankle[0] = hz[0];
        f.ankle[1] = hz[1];
        f.ankle[2] = hz[2] + this.ankleH;
        f.yaw = yaw;
      }
    }
  }

  /** Ankle position (world) of a foot planted at `plant` with heading `yaw` and pitch. */
  private ankleFromPlant(plant: Readonly<V3>, yaw: number, pitch: number, out: V3): V3 {
    const fwd: V3 = [Math.cos(yaw), Math.sin(yaw), 0];
    let dy = 0;
    let dz = this.ankleH;
    if (pitch < 0) {
      const c = Math.cos(pitch), s = Math.sin(pitch);
      const by = -this.ballFwd, bz = this.ankleH;
      dy = this.ballFwd + by * c - bz * s;
      dz = by * s + bz * c;
    } else if (pitch > 0) {
      const c = Math.cos(pitch), s = Math.sin(pitch);
      const by = this.heelBack, bz = this.ankleH;
      dy = -this.heelBack + by * c - bz * s;
      dz = by * s + bz * c;
    }
    out[0] = plant[0] + fwd[0] * dy;
    out[1] = plant[1] + fwd[1] * dy;
    out[2] = plant[2] + dz;
    return out;
  }

  // ---- actions ------------------------------------------------------------------------------

  private updateActions(dt: number): void {
    const inp = this.input;
    const speed = Math.hypot(this.velocity[0], this.velocity[1]);
    const free = this.stance === 'stand' && this.stanceP >= 1 && inp.mood === 'normal';
    const armed = this.weapon !== null && this.weapon.kind !== 'knife';
    // the posture layer: guard, talk, idle poses
    let want: string | null = null;
    if (inp.guard && free) want = 'guard';
    else if (inp.talk === 'speak' && free && !armed) want = 'talk';
    else if (inp.talk === 'listen' && free && !armed) want = this.idlePoseFor(true);
    else if (inp.idle && free && !armed && speed < 0.15 && !inp.aimAt) {
      this.idleTime += dt;
      if (this.idleTime > this.nextIdlePose) {
        this.nextIdlePose = this.idleTime + 7 + this.rng.next() * 12 * (1.2 - this.style.fidget);
        this.idleChoice = this.rng.chance(0.3) ? null : this.idlePoseFor(false);
      }
      want = this.idleTime > 2 ? this.idleChoice : null;
    } else {
      this.idleTime = 0;
      this.nextIdlePose = 2 + this.rng.next() * 2;
    }
    if (want) this.setPoseAction(actionDef(want));
    else if (this.poseAct && !this.poseAct.done) this.poseAct.stop();
    // one-shots the animator starts itself: gestures and nods in conversation, fidgets
    if (!this.busy && free) {
      if (inp.talk === 'speak') {
        this.nextGesture -= dt;
        if (this.nextGesture <= 0) {
          this.nextGesture = 1.2 + this.rng.next() * 2.8;
          const pool = this.rng.chance(0.25) ? ['nod', 'shakeHead'] : [...GESTURES];
          this.play(this.rng.pick(pool));
        }
      } else if (inp.talk === 'listen') {
        this.nextGesture -= dt;
        if (this.nextGesture <= 0) {
          this.nextGesture = 1.5 + this.rng.next() * 3;
          const r = this.rng.next();
          this.play(r < 0.65 ? 'nod' : r < 0.8 ? 'laugh' : r < 0.9 ? 'shakeHead' : 'shrug');
        }
      } else if (inp.idle && !armed && speed < 0.15 && this.idleTime > 3 && !inp.aimAt) {
        this.nextFidget -= dt;
        if (this.nextFidget <= 0) {
          this.nextFidget = 5 + this.rng.next() * 10 * (1.3 - this.style.fidget);
          if (this.rng.chance(0.7)) this.play(this.rng.pick([...FIDGETS]));
        }
      }
    }
    // advance and sample
    for (const [p, out] of [
      [this.poseAct, this.chPose],
      [this.act, this.chAct],
    ] as const) {
      if (!p) continue;
      for (const e of p.advance(dt)) this.pendingEvents.push({ e, p });
      for (const key of Object.keys(out)) delete out[key as keyof ChannelFrame];
      if (!p.done) p.sample(out);
    }
    if (this.poseAct?.done) this.poseAct = null;
    if (this.act?.done) this.act = null;
  }

  private idleChoice: string | null = null;
  private readonly pendingEvents: { e: { name: string; limb?: Limb }; p: ActionPlayer }[] = [];

  /** An idle posture that suits the character (stable per character). */
  private idlePoseFor(listening: boolean): string {
    const r = new Rng(this.seed * 131 + (listening ? 7 : Math.floor(this.time / 15)));
    const pool = listening ? ['armsCrossed', 'pockets', 'handsFolded', 'handsBehind', 'handsOnHips'] : [...IDLE_POSES];
    return r.pick(pool);
  }

  private flushEvents(): void {
    for (const { e, p } of this.pendingEvents) {
      const pos = this.limbPos(e.limb);
      this.events.push({ name: e.name, action: p.def.name, ...(e.limb ? { limb: e.limb } : {}), pos, target: p.target ? [...p.target] as V3 : null });
    }
    this.pendingEvents.length = 0;
  }

  /** World position of a limb's striking point. */
  limbPos(limb?: Limb, out: V3 = [0, 0, 0]): V3 {
    const w = this.world;
    const sk = this.skeleton;
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

  // ---- arms ---------------------------------------------------------------------------------

  /** Arms with the gait (or relaxed), as local joint rotations; mirrored for the right side. */
  private armSwing(dt: number, g: GaitParams, moving: boolean, speed: number, breathe: number, idle: number): void {
    const pose = this.pose;
    const ph = this.phase;
    const run = g.run;
    // the swing dies out over a few swings when stopping (inertia), rather than at once
    this.swingAmp.update(moving ? g.armSwing : 0, dt);
    const swing = this.swingAmp.x;
    const elbow = lerp(0.18 + 0.03 * breathe + this.style.elbow * 0.6, g.elbow, smoothstep(0.1, 1, speed));
    for (const side of [-1, 1] as const) {
      const c = Math.cos(TAU * (ph - (side < 0 ? 0.5 : 0) - 0.04));
      const flex = swing * c + run * 0.25 + 0.02 * breathe * idle;
      const adduct = -0.2 + run * 0.02;
      let ua = qeuler(flex, adduct, -0.1 * run);
      let fa = qeuler(elbow + Math.max(0, c) * swing * 0.35, 0, 0.3 * run);
      let hd = qeuler(0.12, 0, 0);
      let cl = qeuler(0, 0, -0.05 * c * swing);
      if (side > 0) {
        ua = qmirrorX(ua);
        fa = qmirrorX(fa);
        hd = qmirrorX(hd);
        cl = qmirrorX(cl);
      }
      pose.r[side < 0 ? H.upperarmL : H.upperarmR] = ua;
      pose.r[side < 0 ? H.forearmL : H.forearmR] = fa;
      pose.r[side < 0 ? H.handL : H.handR] = hd;
      pose.r[side < 0 ? H.clavicleL : H.clavicleR] = cl;
    }
  }

  /** Free hands of a stance (thighs, desk, ground, elbows) with weight w. */
  private restHands(S: StanceSample, w: number): void {
    if (w <= 0.01) return;
    for (let i = 0; i < 2; i++) {
      const h = S.hands[i];
      if (!h) continue;
      const side = i === 0 ? 'L' : 'R';
      const pole: V3 = qrotate(this.fk.q[H.chest]!, vnorm([i === 0 ? -0.8 : 0.8, -0.5, -0.5]));
      this.handIK(side, h, null, pole, w);
    }
  }

  /**
   * Puts a hand's palm at `target` (model space), oriented `rot` (model, the canonical fist
   * frame) or left as is, elbow towards `pole`, blending the arm by w.
   */
  private handIK(side: 'L' | 'R', target: V3, rot: Quat | null, pole: V3, w: number): void {
    const pose = this.pose;
    const fk = this.fk;
    const ua = side === 'L' ? H.upperarmL : H.upperarmR;
    const fa = side === 'L' ? H.forearmL : H.forearmR;
    const hand = side === 'L' ? H.handL : H.handR;
    const saved = w < 0.999 ? [[...pose.r[ua]!], [...pose.r[fa]!], [...pose.r[hand]!]] as Quat[] : null;
    const handQ = rot ? qmul(rot, this.canonical(side)) : fk.q[hand]!;
    const palm = qrotate(handQ, this.palmOffset(side));
    solveTwoBone(pose, fk, ua, fa, hand, vsub(target, palm), pole, 0.03);
    if (rot) setModelRotation(pose, fk, hand, handQ);
    if (saved) {
      pose.r[ua] = qnlerp(saved[0]!, pose.r[ua]!, w);
      pose.r[fa] = qnlerp(saved[1]!, pose.r[fa]!, w);
      pose.r[hand] = qnlerp(saved[2]!, pose.r[hand]!, w);
    }
    fk.updateSubtree(pose, ua);
  }

  /** Rotation from a hand's rest frame to the canonical fist frame (knuckles +y, palm -z). */
  private canonical(side: 'L' | 'R'): Quat {
    const cached = side === 'L' ? this.canonL : this.canonR;
    if (cached) return cached;
    const sk = this.skeleton;
    const b = side === 'L' ? H.handL : H.handR;
    const dir = vsub(sk.restTail[b]!, sk.restHead[b]!);
    const q = frameRotation(dir, [side === 'L' ? 1 : -1, 0, 0], [0, 1, 0], [0, 0, -1]);
    if (side === 'L') this.canonL = q;
    else this.canonR = q;
    return q;
  }

  private canonL: Quat | null = null;
  private canonR: Quat | null = null;

  /** Palm centre relative to the wrist in rest space. */
  private palmOffset(side: 'L' | 'R'): V3 {
    const sk = this.skeleton;
    const b = side === 'L' ? H.handL : H.handR;
    return vscale(vsub(sk.restTail[b]!, sk.restHead[b]!), 0.42);
  }

  /** Hand channels of an action layer. */
  private actionHands(ch: ChannelFrame, w: number, player: ActionPlayer, toModel: (p: Readonly<V3>) => V3, heldProp: boolean): void {
    const k = this.k;
    const chestQ = this.fk.q[H.chest]!;
    const chestP = this.fk.p[H.chest]!;
    for (const side of ['R', 'L'] as const) {
      const c = ch[`hand${side}`];
      if (!c) continue;
      // a hand on the held weapon stays there unless the action says otherwise
      if (heldProp && side === 'R' && !ch.strikeR && player.def.name !== 'riflePush') continue;
      const cw = ch[`hand${side}w`]?.[0] ?? 1;
      const ww = clamp(w * cw, 0, 1);
      if (ww <= 0.01) continue;
      let palm = vadd(chestP, qrotate(chestQ, [c[0]! * k, c[1]! * k, c[2]! * k]));
      const s = ch[`strike${side}`]?.[0] ?? 0;
      if (s !== 0 && player.target) palm = vlerp(palm, toModel(player.target), clamp(s, -0.3, 1.1));
      const r = ch[`hand${side}rot`];
      const rot = r ? qmul(chestQ, qeuler(r[0]! * DEG, r[1]! * DEG, r[2]! * DEG)) : null;
      const pc = ch[`elbow${side}`];
      const pole = qrotate(chestQ, vnorm(pc ? [pc[0]!, pc[1]!, pc[2]!] : [side === 'R' ? 0.7 : -0.7, -0.3, -0.7]));
      this.handIK(side, palm, rot, pole, ww);
    }
  }

  /** A free hand pressed to a wound for a while. */
  private clutchHand(dt: number, heldProp: boolean): void {
    const c = this.reactions.clutch;
    this.clutchW.update(c && this.stance === 'stand' ? 1 : 0, dt);
    if (!c || this.clutchW.x < 0.02) return;
    const side = heldProp ? 'L' : c.hand;
    const b = c.bone;
    // the wound's point in model space, a little out from the body
    const q = this.fk.q[b]!;
    const p = vadd(this.fk.p[b]!, qrotate(q, vsub(c.rest, this.skeleton.restHead[b]!)));
    const chestQ = this.fk.q[H.chest]!;
    const out = qrotate(chestQ, [0, 0.05 * this.k, 0]);
    const pole = qrotate(chestQ, vnorm([side === 'R' ? 0.9 : -0.9, -0.2, -0.5]));
    this.handIK(side, vadd(p, out), qmul(chestQ, qeuler(-20 * DEG, side === 'R' ? 100 * DEG : -100 * DEG, 0)), pole, clamp(this.clutchW.x, 0, 1) * 0.9);
  }

  /** Mood arm poses by IK (blended over the swing pose by weight w). */
  private moodArms(w: number): void {
    const fk = this.fk;
    const k = this.k;
    const headP = fk.p[H.head]!;
    const headQ = fk.q[H.head]!;
    const chestQ = fk.q[H.chest]!;
    const t = this.time;
    for (const side of [-1, 1] as const) {
      const ua = side < 0 ? H.upperarmL : H.upperarmR;
      let target: V3;
      let pole: V3;
      let rot: Quat;
      if (this.moodKind === 'cower') {
        target = vadd(headP, qrotate(headQ, [side * 0.06 * k, -0.05 * k, 0.16 * k]));
        pole = qrotate(chestQ, vnorm([side * 0.6, 0.8, 0.1]));
        rot = qmul(headQ, qeuler(-60 * DEG, side * -90 * DEG, 0));
      } else if (this.moodKind === 'surrender') {
        const sh = fk.p[ua]!;
        target = vadd(sh, qrotate(chestQ, [side * 0.2 * k, 0.06 * k, 0.5 * k + 0.02 * Math.sin(t * 3 + side)]));
        pole = qrotate(chestQ, vnorm([side, -0.2, -0.4]));
        rot = qmul(chestQ, qeuler(90 * DEG, 0, 0));
      } else {
        const b = Math.sin(TAU * this.phase + (side < 0 ? 0 : Math.PI)) * 0.06 * k;
        target = vadd(headP, qrotate(chestQ, [side * 0.16 * k, 0.1 * k, 0.12 * k + b]));
        pole = qrotate(chestQ, vnorm([side, 0.2, -0.5]));
        rot = qmul(chestQ, qeuler(70 * DEG, side * -90 * DEG, 0));
      }
      this.handIK(side < 0 ? 'L' : 'R', target, rot, pole, w);
    }
  }

  // ---- weapons ------------------------------------------------------------------------------

  /**
   * Places the weapon socket in the carry pose and puts the hands on the prop. Returns true
   * when the prop is held in the socket (both hands, or the gun hand, busy with it).
   */
  private holdWeapon(
    dt: number,
    aiming: boolean,
    run: number,
    moving: boolean,
    toModel: (w: Readonly<V3>) => V3,
    chP: ChannelFrame | null,
    chA: ChannelFrame | null,
    wP: number,
    wA: number,
  ): boolean {
    const prop = this.weapon!;
    const pose = this.pose;
    const fk = this.fk;
    const k = this.k;
    const inp = this.input;
    const pistol = prop.kind === 'pistol';
    this.readyW.update(inp.carry === 'relaxed' ? 0 : 1, dt);
    this.hipW.update(inp.carry === 'hip' ? 1 : 0, dt);
    this.sprintW.update(moving && !aiming ? run : 0, dt);
    this.kickBack.update(0, dt);
    this.kickPitch.update(0, dt);
    // a lowered pistol hangs in the hand
    if (pistol && this.readyW.x < 0.05 && !aiming) return false;
    const chestQ = fk.q[H.chest]!;
    const chestP = fk.p[H.chest]!;
    const shoulder = fk.p[H.upperarmR]!;
    const pocket = vadd(shoulder, qrotate(chestQ, [-0.06 * k, 0.07 * k, -0.035 * k]));
    const cf = qrotate(chestQ, [0, 1, 0]);
    const chestYawQ = frameRotation([0, 1, 0], [0, 0, 1], vnorm([cf[0], cf[1], 0]), [0, 0, 1]);
    const eyes = vadd(fk.p[H.head]!, qrotate(fk.q[H.head]!, [0, 0.09 * k, 0.1 * k]));
    let aimDir: V3 = vnorm([cf[0], cf[1], 0]);
    if (inp.aimAt) aimDir = vnorm(vsub(toModel(inp.aimAt), pistol ? eyes : pocket));
    const aimRot = frameRotation([0, 1, 0], [0, 0, 1], aimDir, [0, 0, 1]);
    const rw = clamp(this.readyW.x, 0, 1);
    const aw = clamp(this.aimW.x, 0, 1);
    const hw = clamp(this.hipW.x, 0, 1);
    let rot: Quat;
    let pos: V3;
    let twoHanded = true;
    if (pistol) {
      // lowered (in the hand) -> low ready (two hands, muzzle down) -> aimed (two hands at eye
      // level) or one-handed (arm out at the target)
      const readyRot = qmul(aimRot, qeuler(-0.7, 0, 0));
      const readyGrip = vadd(chestP, qrotate(chestYawQ, [0.03 * k, 0.34 * k, 0.02 * k]));
      const aimGrip = vadd(eyes, vadd(vscale(aimDir, 0.47 * k), [0, 0, -0.07 * k]));
      const oneGrip = vadd(shoulder, vscale(aimDir, 0.56 * k));
      rot = qnlerp(readyRot, aimRot, aw);
      pos = vlerp(readyGrip, vlerp(aimGrip, oneGrip, hw), aw);
      twoHanded = hw < 0.5;
    } else {
      const relaxedRot = qmul(chestYawQ, qeuler(-0.95, 0.15, 0.55));
      const relaxedGrip = vadd(chestP, qrotate(chestYawQ, [0.13 * k, 0.2 * k, -0.2 * k]));
      const readyRot = qmul(aimRot, qeuler(-0.5, 0, 0.3));
      const readyStock = vadd(pocket, qrotate(chestQ, [0.01 * k, 0.0, -0.05 * k]));
      const readyGrip = vsub(readyStock, qrotate(readyRot, prop.stock));
      const aimGrip = vsub(pocket, qrotate(aimRot, prop.stock));
      // hip fire: stock under the arm, level at the target (machine guns on the move)
      const hipRot = qnlerp(aimRot, frameRotation([0, 1, 0], [0, 0, 1], vnorm([aimDir[0], aimDir[1], aimDir[2] * 0.5]), [0, 0, 1]), 0.5);
      const hipGrip = vadd(chestP, qrotate(chestYawQ, [0.15 * k, 0.26 * k, -0.2 * k]));
      const portRot = qmul(chestYawQ, frameRotation([0, 1, 0], [0, 0, 1], vnorm([-0.55, 0.3, 0.78]), vnorm([0.1, 1, 0.1])));
      const portGrip = vadd(chestP, qrotate(chestYawQ, [0.12 * k, 0.2 * k, -0.08 * k]));
      rot = qnlerp(relaxedRot, readyRot, rw);
      pos = vlerp(relaxedGrip, readyGrip, rw);
      const sw = clamp(this.sprintW.x, 0, 1) * (1 - aw);
      rot = qnlerp(rot, portRot, sw);
      pos = vlerp(pos, portGrip, sw);
      const aimR = qnlerp(aimRot, hipRot, hw);
      const aimP = vlerp(aimGrip, hipGrip, hw);
      rot = qnlerp(rot, aimR, aw);
      pos = vlerp(pos, aimP, aw);
    }
    // recoil, weapon sway, a hit on the arm knocking the aim off
    const fwd = qrotate(rot, [0, 1, 0]);
    pos = vadd(pos, vscale(fwd, -(pistol ? 0.03 : 0.035) * this.kickBack.x));
    rot = qmul(rot, qx((pistol ? 0.07 : 0.05) * this.kickPitch.x - 0.25 * this.reactions.aimOff.x));
    const bob = moving ? Math.sin(TAU * 2 * this.phase) * 0.008 * k * (1 - aw * 0.7) : 0;
    pos = vadd(pos, this.sway.update([0, 0, bob], dt));
    // action offsets of the weapon (reloads, a rifle jab)
    for (const [ch, w] of [
      [chP, wP],
      [chA, wA],
    ] as const) {
      if (!ch || w <= 0) continue;
      const wr = ch.weaponRot, wpos = ch.weaponPos;
      if (wr) rot = qmul(rot, qeuler(wr[0]! * DEG * w, wr[1]! * DEG * w, wr[2]! * DEG * w));
      if (wpos) pos = vadd(pos, qrotate(chestQ, [wpos[0]! * k * w, wpos[1]! * k * w, wpos[2]! * k * w]));
    }
    pose.t[H.weapon] = [pos[0], pos[1], pos[2]];
    pose.r[H.weapon] = rot;
    fk.updateBone(pose, H.weapon);

    // hands on the prop
    const grip = pos;
    const handR = qmul(rot, this.gripR(pistol));
    const wristR = vsub(grip, qrotate(handR, this.palmOffset('R')));
    const poleR = qrotate(chestQ, vnorm(pistol ? [0.5, -0.2, -0.9] : [0.7, -0.3, -0.75]));
    pose.r[H.clavicleR] = qeuler(0, 0, (pistol ? 0.08 : 0.12) * rw);
    pose.r[H.clavicleL] = qeuler(0, 0, -(pistol ? 0.08 : 0.18) * rw);
    fk.update(pose, H.clavicleL);
    solveTwoBone(pose, fk, H.upperarmR, H.forearmR, H.handR, wristR, poleR, 0.02);
    setModelRotation(pose, fk, H.handR, handR);
    if (twoHanded) {
      const support = vadd(pos, qrotate(rot, prop.support));
      const handL = qmul(rot, this.gripL(pistol));
      const wristL = vsub(support, qrotate(handL, this.palmOffset('L')));
      const poleL = qrotate(chestQ, vnorm(pistol ? [-0.5, -0.2, -0.9] : [-0.7, 0.1, -0.8]));
      solveTwoBone(pose, fk, H.upperarmL, H.forearmL, H.handL, wristL, poleL, 0.04);
      setModelRotation(pose, fk, H.handL, handL);
    }
    return true;
  }

  // grip frames (hand rotation relative to the prop)
  private gripR(pistol: boolean): Quat {
    const sk = this.skeleton;
    const dir = vsub(sk.restTail[H.handR]!, sk.restHead[H.handR]!);
    return frameRotation(dir, [-1, 0, 0], vnorm(pistol ? [0, -0.2, -1] : [0, -0.3, -1]), [-1, 0, 0]);
  }

  private gripL(pistol: boolean): Quat {
    const sk = this.skeleton;
    const dir = vsub(sk.restTail[H.handL]!, sk.restHead[H.handL]!);
    // a pistol's support hand wraps the gun hand from the left
    if (pistol) return frameRotation(dir, [1, 0, 0], vnorm([0.4, -0.1, -1]), vnorm([1, 0.2, 0.1]));
    return frameRotation(dir, [1, 0, 0], vnorm([0.35, 0.8, 0.1]), vnorm([0.3, 0, 1]));
  }

  /** A prop held in the hand (a knife, a lowered pistol): along the knuckles, grip in the palm. */
  private propInHand(b: number): void {
    const w = this.world;
    const side = b === H.handL ? 'L' : 'R';
    const q = qmul(w.q[b]!, qconj(this.canonical(side)));
    const palm = vadd(w.p[b]!, qrotate(w.q[b]!, this.palmOffset(side)));
    vcopy(palm, this.weaponPos);
    // a knife points out of the fist along the knuckles; a pistol hangs muzzle down-forward
    this.weaponRot = this.weapon?.kind === 'pistol' ? qmul(q, qx(-0.3)) : q;
  }

  /** World position of a point on the held prop (prop space), e.g. its muzzle. */
  propPoint(p: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
    return vadd(this.weaponPos, qrotate(this.weaponRot, p), out);
  }

  /** Skin matrix of the held prop (its model's single bone). */
  writePropSkin(out: Float32Array, offset = 0): void {
    writeRigid(out, offset, this.weaponPos, this.weaponRot, [0, 0, 0]);
  }

  /** The feet (world), for debugging and tests. */
  footState(): { planted: boolean; pos: V3; ankle: V3 }[] {
    return this.feet.map((f) => ({ planted: f.planted, pos: [...f.pos] as V3, ankle: [...f.ankle] as V3 }));
  }

  /** World position between the eyes (line of sight, muzzle-less aiming). */
  eyes(out: V3 = [0, 0, 0]): V3 {
    const h = this.skeleton.restHead[H.head]!;
    return this.world.pointOf(H.head, [h[0], h[1] + 0.09 * this.k, h[2] + 0.12 * this.k], out);
  }
}


