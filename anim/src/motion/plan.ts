/**
 * The motion plan: what the body means to do, as a pose (the intent the physical body's
 * muscles track).
 *
 * The host moves the character (root position on the ground and facing yaw, each frame) and
 * says what it is doing: stance, crouch, weapon carry, aim and look targets, mood, talking,
 * guard; it starts actions (strikes, reloads, gestures). The body's behaviours (behaviour/)
 * add what the situation asks for through `control`: a hand to a wound, on a wall or out to
 * break a fall, arms out for balance, the trunk ducking or folding, the head turned away, a
 * limp, balance steps, a pelvis where the physics has it.
 *
 * The plan is built in layers:
 * 1. Stances (stances.ts): standing, kneeling, prone and crawling, sitting on a seat or the
 *    ground, lying (and getting up from it); transitions blend through intermediate stances.
 * 2. Locomotion (feet.ts): the gait clock and the foot planter (planted feet, heel-toe roll,
 *    ground under every step, obstacles cleared), balance steps. A personal style (style.ts),
 *    footfall compression, lean into acceleration, banking into turns, a tactical walk while
 *    aiming, a limp.
 * 3. Trunk and head: aim and look spread over spine, chest, neck and head; peeking; posture;
 *    breathing; behaviour offsets.
 * 4. Actions (actions.ts): a posture layer (guard, idle poses, talking) and a one-shot layer
 *    (strikes that land on their target, blocks, reloads, gestures, fidgets).
 * 5. Arms (arms.ts): the held weapon with both hands on it; guard, moods, stance hands, the
 *    swing; action hands; behaviour hand tasks on top.
 * 6. Legs by IK last, so every pelvis motion bends the knees with the feet where they are.
 *
 * Output: `pose` (local), `world` (world transforms; `prevWorld` of the frame before), the
 * held prop's transform, the feet, and events (a strike landing, a reload done).
 */
import { ModelFK, setModelRotation, solveTwoBone } from '../core/ik.ts';
import { Pose, WorldPose, type Skeleton } from '../core/skeleton.ts';
import { Spring, Spring3 } from '../core/spring.ts';
import { writeRigid } from '../math/mat4.ts';
import { Rng, valueNoise } from '../math/random.ts';
import { qconj, qeuler, qmirrorX, qmul, qnlerp, qrotate, qx, qz, type Quat } from '../math/quat.ts';
import { DEG, clamp, fract, lerp, smoothstep, vadd, vcopy, vdist, vlerp, vnorm, vscale, vsub, wrapAngle, type V3 } from '../math/vec.ts';
import type { CollisionWorld } from '../physics/collision.ts';
import type { Prop } from '../characters/props.ts';
import { H } from '../humanoid/rig.ts';
import { actionDef, ActionPlayer, ARMED_FIDGETS, FIDGETS, GESTURES, IDLE_POSES, type ActionDef, type ChannelFrame, type Limb } from './actions.ts';
import { ArmRig, WeaponHold, type Carry, type Side } from './arms.ts';
import { FootPlanner } from './feet.ts';
import { gaitFor, type GaitParams } from './gait.ts';
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

export type { Carry } from './arms.ts';
export type Mood = 'normal' | 'panic' | 'cower' | 'surrender';

/** Where a knee points in the rest pose (the legs twist with their knees). */
const KNEE_REST: V3 = [0, 1, 0];
const TAU = Math.PI * 2;

/** A seat to sit on (world). The character's root stays where it stood, in front of it. */
export interface SeatInfo {
  /** Seat surface centre. */
  pos: V3;
  backrest: boolean;
  /** Desk top height above the floor (m), for sitting at a desk. */
  deskHeight?: number | null;
  variant?: SitVariant;
}

export interface MotionInput {
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

/** A hand sent somewhere by a behaviour (world). */
export interface ArmTask {
  /** Palm target. */
  target: V3;
  /** Hand rotation (the canonical fist frame: knuckles +y, palm -z), or null: as planned. */
  rot: Quat | null;
  /** Elbow direction, or null: out and down. */
  pole: V3 | null;
  /** 0..1 over the planned arm. */
  weight: number;
}

/** What the body's behaviours ask of the plan this frame (reset by them every frame). */
export class PlanControl {
  /** Hand tasks, left and right. */
  readonly arms: [ArmTask | null, ArmTask | null] = [null, null];
  /** Extra trunk and head rotations (euler x, y, z in the joints' frames, rad). */
  readonly spine: V3 = [0, 0, 0];
  readonly chest: V3 = [0, 0, 0];
  readonly neck: V3 = [0, 0, 0];
  readonly head: V3 = [0, 0, 0];
  /** Forward fold of the trunk (gut wound, pain), rad. */
  fold = 0;
  /** Added crouch 0..1. */
  crouch = 0;
  /** Shoulders drawn up (0..1). */
  shrug = 0;
  /** Limp per leg (0..1) and pain (0..1: a hunch, slower). */
  readonly limp: [number, number] = [0, 0];
  pain = 0;
  /** Where the eyes go instead (world), with a weight. */
  look: V3 | null = null;
  lookWeight = 0;
  /**
   * The pelvis where the physics has it (world), blended in by `pelvisWeight`: across the
   * ground always, its height only with `pelvisHeight` (else the plan says how high the legs
   * hold it).
   */
  pelvisPos: V3 | null = null;
  readonly pelvisRot: Quat = [0, 0, 0, 1];
  pelvisWeight = 0;
  pelvisHeight = false;
  /** No gait steps: the feet move only by balance steps. */
  holdFeet = false;
  /** How much care the steps get (obstacle clearance), 0..1. */
  care = 1;
  /** No idle picks (postures, fidgets) of the plan's own. */
  busy = false;

  reset(): void {
    this.arms[0] = this.arms[1] = null;
    for (const v of [this.spine, this.chest, this.neck, this.head]) v[0] = v[1] = v[2] = 0;
    this.fold = 0;
    this.crouch = 0;
    this.shrug = 0;
    this.look = null;
    this.lookWeight = 0;
    this.pelvisPos = null;
    this.pelvisWeight = 0;
    this.pelvisHeight = false;
    this.holdFeet = false;
    this.care = 1;
    this.busy = false;
  }
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

function tuple(v: readonly number[] | undefined, fallback: V3): V3 {
  return v ? [v[0] ?? 0, v[1] ?? 0, v[2] ?? 0] : [fallback[0], fallback[1], fallback[2]];
}

export class MotionPlan {
  readonly skeleton: Skeleton;
  readonly pose: Pose;
  readonly fk: ModelFK;
  readonly world: WorldPose;
  readonly prevWorld: WorldPose;
  readonly input: MotionInput = {
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
  /** What the behaviours ask for (they fill it before every update). */
  readonly control = new PlanControl();
  collision: CollisionWorld;
  weapon: Prop | null = null;
  style: GaitStyle = { ...NEUTRAL_STYLE };
  /** Events of the last updates (take them with takeEvents). */
  readonly events: AnimEvent[] = [];
  /** Character root: ground position and facing (radians, 0 = +x, CCW; +y is yaw pi/2). */
  readonly rootPos: V3 = [0, 0, 0];
  rootYaw = 0;
  time = 0;
  /** The held prop's transform (world) after update, and how it is held. */
  readonly weaponPos: V3 = [0, 0, 0];
  weaponRot: Quat = [0, 0, 0, 1];
  /** The prop is in the right hand only (a knife, a lowered pistol, a long gun let go of). */
  weaponInHand = false;
  /**
   * How hard an action drives each limb this frame (0..1; left hand, right hand, left foot,
   * right foot), and whether it strikes with it: the body tenses those muscles (a punch is not
   * thrown with a relaxed arm).
   */
  readonly effort: [number, number, number, number] = [0, 0, 0, 0];
  readonly striking: [boolean, boolean, boolean, boolean] = [false, false, false, false];
  gait: GaitParams = gaitFor(0, 0);
  /** Smoothed world velocity. */
  readonly velocity: V3 = [0, 0, 0];
  /** The settled stance (while a transition runs: the one it comes from). */
  stance: Stance = 'stand';
  readonly feetPlanner: FootPlanner;
  readonly arms: ArmRig;
  readonly hold = new WeaponHold();
  readonly k: number;
  readonly legLen: number;
  readonly restPelvisZ: number;

  // proportions
  private readonly dims: Dims;

  // locomotion state
  private readonly lastPos: V3 = [0, 0, 0];
  private lastYaw = 0;
  private placed = false;
  private readonly velSpring = new Spring3(9, 1);
  private readonly accel: V3 = [0, 0, 0];
  private yawRate = 0;
  private readonly visZ = new Spring(16, 1);
  private readonly crouchS = new Spring(7, 1);
  private readonly lowerYaw = new Spring(5, 1);
  /** Where the hips point relative to the facing (with the planted feet when standing). */
  private readonly hipsYaw = new Spring(6, 1);
  private readonly pelvisZ = new Spring(26, 1);
  private readonly impact = new Spring(15, 0.45);
  private readonly trunkLean = new Spring(6, 0.55);
  private readonly bank = new Spring(6, 0.8);
  private readonly swingAmp = new Spring(5, 1);
  private readonly aimW = new Spring(8, 1);
  private readonly moodW = new Spring(6, 1);
  private readonly airW = new Spring(8, 1);
  private readonly leanS = new Spring(7, 1);
  private readonly aimYaw = new Spring(8, 1);
  private readonly aimPitch = new Spring(9, 1);
  private readonly headYaw = new Spring(5.5, 0.9);
  private readonly headPitch = new Spring(5.5, 0.9);
  private readonly shift = new Spring(1.5, 1);
  private readonly lookW = new Spring(9, 1);
  /** Footfall nod of the head, recoil of the chest (small secondary motion of the plan). */
  private readonly nod = new Spring(11, 0.42);
  private readonly recoil = new Spring(14, 0.5);
  private readonly taskW: [Spring, Spring] = [new Spring(16, 1), new Spring(16, 1)];
  private moodKind: Mood = 'normal';
  private readonly seed: number;
  private readonly rng: Rng;

  // stances
  private stanceTo: Stance = 'stand';
  private stanceP = 1;
  private stanceDur = 0.5;
  private readonly stanceQueue: Stance[] = [];
  /** Lying: on the back (else face down), and whether it stays down. */
  private downBack = true;
  private lying = false;
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
  private idleChoice: string | null = null;
  private armedIdle = 0;
  private readonly pendingEvents: { e: { name: string; limb?: Limb }; p: ActionPlayer }[] = [];

  constructor(skeleton: Skeleton, collision: CollisionWorld, seed = 1) {
    this.skeleton = skeleton;
    this.collision = collision;
    this.seed = seed;
    this.rng = new Rng(seed * 31 + 7);
    this.pose = new Pose(skeleton);
    this.fk = new ModelFK(skeleton);
    this.world = new WorldPose(skeleton);
    this.prevWorld = new WorldPose(skeleton);
    this.arms = new ArmRig(skeleton, this.pose, this.fk);
    const rh = skeleton.restHead;
    this.restPelvisZ = rh[H.pelvis]![2];
    this.k = this.restPelvisZ / 0.97;
    this.legLen = vdist(rh[H.thighL]!, rh[H.shinL]!) + vdist(rh[H.shinL]!, rh[H.footL]!);
    const ankleH = rh[H.footL]![2];
    const footX = Math.abs(rh[H.footL]![0]);
    this.dims = { k: this.k, ankleH, footX };
    this.feetPlanner = new FootPlanner(
      { k: this.k, legLen: this.legLen, ankleH, ballFwd: rh[H.toeL]![1] - rh[H.footL]![1], heelBack: 0.06 * this.k, footX },
      collision,
      [
        { thigh: H.thighL, shin: H.shinL, foot: H.footL, toe: H.toeL },
        { thigh: H.thighR, shin: H.shinR, foot: H.footR, toe: H.toeR },
      ],
    );
  }

  /** Forward unit vector (world) of a facing yaw. */
  static forward(yaw: number): V3 {
    return [Math.cos(yaw), Math.sin(yaw), 0];
  }

  rootRot(): Quat {
    return qz(this.rootYaw - Math.PI / 2);
  }

  get phase(): number {
    return this.feetPlanner.phase;
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
    this.feetPlanner.collision = this.collision;
    this.feetPlanner.reset(this.rootPos, yaw, this.crouchS.x, this.style);
    this.pelvisZ.x = this.restPelvisZ;
    this.placed = true;
    this.update(0);
    this.prevWorld.copyFrom(this.world);
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

  /**
   * Moves the root without the motion counting as walking (the body was carried there: a
   * shove, a stagger, a fall).
   */
  carryRoot(pos: Readonly<V3>, yaw: number): void {
    const dx = pos[0] - this.rootPos[0], dy = pos[1] - this.rootPos[1];
    vcopy(pos, this.rootPos);
    this.lastPos[0] += dx;
    this.lastPos[1] += dy;
    this.lastPos[2] = pos[2];
    this.lastYaw += wrapAngle(yaw - this.rootYaw);
    this.rootYaw = yaw;
    if (Math.abs(pos[2] - this.visZ.x) > 0.3 * this.k) this.visZ.x = pos[2];
  }

  /** Busy with a one-shot action. */
  get busy(): boolean {
    return this.act !== null && !this.act.done;
  }

  get actionName(): string | null {
    return this.act && !this.act.done ? this.act.def.name : null;
  }

  /** Mid stance transition (or lying): the host should not move the root. */
  get transitioning(): boolean {
    return this.stanceP < 1 || this.stanceQueue.length > 0 || this.stance === 'down';
  }

  /** Lying down, or getting up from it. */
  get down(): boolean {
    return this.lying || this.stance === 'down' || this.stanceTo === 'down' || (this.stanceQueue.length > 0 && this.getUpRun);
  }

  private getUpRun = false;

  /**
   * Starts a one-shot action (strike, block, reload, gesture, fidget), optionally aimed at a
   * world `target` (strikes). A held posture (guard, idle pose) goes on the pose layer. Returns
   * false if the body cannot (down, mid transition).
   */
  play(name: string, target: V3 | null = null, rate = 1): boolean {
    const def = actionDef(name);
    if (this.stance === 'down' || this.lying || this.stanceP < 1) return false;
    if (def.layer === 'pose') {
      this.setPoseAction(def);
      return true;
    }
    this.act?.stop();
    this.act = new ActionPlayer(def, target ? [target[0], target[1], target[2]] : null, rate);
    return true;
  }

  /** Stops the running one-shot action (a hit interrupts it). */
  interrupt(hard = true): void {
    if (hard && this.act && !this.act.def.name.startsWith('block')) this.act.stop();
    if (this.poseAct && this.poseAct.def.name !== 'guard' && this.poseAct.def.name !== 'knifeGuard') this.poseAct.stop();
    this.idleTime = 0;
    this.nextIdlePose = 4 + this.rng.next() * 4;
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
    if (!this.weapon) return;
    this.hold.fire(this.weapon, strength);
    this.recoil.kick((this.weapon.kind === 'pistol' ? 0.15 : 0.35) * strength);
  }

  /**
   * Lies on the ground where the body lies (root on the ground under the pelvis, `yaw` the way
   * the feet point from the head for a body on its back, the head's way face down): the plan
   * holds the lying pose until `getUp`.
   */
  lie(root: Readonly<V3>, yaw: number, back: boolean): void {
    this.stanceQueue.length = 0;
    this.act?.stop();
    this.poseAct?.stop();
    this.downBack = back;
    this.stance = 'down';
    this.stanceTo = 'down';
    this.stanceP = 1;
    this.lying = true;
    this.getUpRun = false;
    this.carryRoot(root, yaw);
    this.velSpring.reset();
    vcopy([0, 0, 0], this.velocity);
  }

  /** A fall to lying played by the plan alone (no physics: retro frames, a body far away). */
  fall(back: boolean): void {
    this.stanceQueue.length = 0;
    this.act?.stop();
    this.downBack = back;
    this.lying = true;
    this.getUpRun = false;
    this.beginTransition('down');
  }

  /** Gets up from lying: through sitting (or pushing up from the front), kneeling, to the host's stance. */
  getUp(): void {
    if (!this.lying) return;
    this.lying = false;
    this.getUpRun = true;
  }

  /** Progress of a stance transition 0..1 (1: settled). */
  get stanceProgress(): number {
    return this.stanceP;
  }

  /** The stance being blended to. */
  get stanceTarget(): Stance {
    return this.stanceTo;
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
        else this.getUpRun = false;
      }
      return;
    }
    if (this.lying) return;
    let want = this.input.stance;
    if (want === 'sit' && !this.input.seat) want = 'stand';
    if (want === 'down') want = 'stand';
    if (want !== this.stance) {
      // up from lying: sit up (on the back) or push up (face down), then kneel
      const route = this.stance === 'down' ? [this.downBack ? ('ground' as Stance) : ('prone' as Stance), ...stanceRoute(this.downBack ? 'ground' : 'prone', want)] : stanceRoute(this.stance, want);
      const first = route.shift();
      if (first) {
        this.stanceQueue.push(...route);
        this.beginTransition(first);
      }
    }
  }

  private weightOf(s: Stance): number {
    // (falling accelerates; the rest ease in and out)
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
        // (getting up from the back: sitting up with the knees drawn in)
        return groundSample(d, this.getUpRun ? 'kneesUp' : this.input.groundVariant, this.time + this.seed, out);
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
    const ctl = this.control;
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

    // ---- stance, actions --------------------------------------------------------------------
    this.updateStance(dt);
    this.updateActions(dt);
    const chP = this.poseAct && !this.poseAct.done ? this.chPose : null;
    const chA = this.act && !this.act.done ? this.chAct : null;
    const wP = chP ? this.poseAct!.weight : 0;
    const wA = chA ? this.act!.weight : 0;
    const add = (c: keyof ChannelFrame, i: number): number => (chP?.[c]?.[i] ?? 0) * wP + (chA?.[c]?.[i] ?? 0) * wA;
    // the limbs the actions drive, and those that strike
    for (let i = 0; i < 4; i++) {
      const side = i % 2 === 0 ? 'L' : 'R';
      const hand = i < 2;
      const w = (ch: ChannelFrame | null, wt: number): number => {
        if (!ch) return 0;
        if (hand) return ch[`hand${side}`] ? (ch[`hand${side}w`]?.[0] ?? 1) * wt : 0;
        return (ch[`foot${side}w`]?.[0] ?? 0) * wt;
      };
      this.effort[i] = clamp(Math.max(w(chP, wP), w(chA, wA)), 0, 1);
      const sc = hand ? chA?.[`strike${side}`] : chA?.[`strikeFoot${side}`];
      this.striking[i] = !!sc && wA > 0.1;
    }

    const crouchIn = inp.mood === 'cower' ? 1 : inp.crouch;
    this.crouchS.update(clamp(crouchIn + add('crouch', 0) + ctl.crouch, 0, 1), dt);
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
    const limpL = ctl.limp[0], limpR = ctl.limp[1];
    const pain = ctl.pain;
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
    const feet = this.feetPlanner;
    feet.collision = this.collision;
    const moving = speed > 0.12 && !inp.airborne && standW > 0.99 && !ctl.holdFeet;
    const bodyYaw = this.rootYaw + this.lowerYaw.x;
    let prevPhase = feet.phase;
    const hips: [V3, V3] = [this.world.p[H.thighL]!, this.world.p[H.thighR]!];
    const fctx = { root: this.rootPos, bodyYaw, vel, speed, gait: g, moving, airborne: inp.airborne, crouch, style: st, hips, care: ctl.care, groundZ: this.rootPos[2] };
    if (standW < 0.999) {
      // another stance: the feet wait at the standing stance's spots
      feet.reset(this.rootPos, bodyYaw, crouch, st);
      feet.stepping = false;
    } else {
      if (!ctl.holdFeet || feet.feet[0].forced || feet.feet[1].forced) prevPhase = feet.advanceClock(dt, fctx, ctl.limp);
      feet.update(dt, fctx, prevPhase);
      // footfall: the body settles onto the leg (heavier characters and faster gaits more), the
      // head nods with it unless it is held still
      for (let i = 0; i < 2; i++) {
        if (!feet.landed[i]) continue;
        const sp = moving ? Math.min(1.4, speed / 2.5) : 0.3;
        this.impact.kick(-(0.18 + 0.4 * st.heavy) * sp);
        this.nod.kick(-0.5 * sp * (1 - st.headStill));
      }
    }

    // crawling clock (prone)
    this.crawlPhase = fract(this.crawlPhase + dt * Math.min(1.2, speed / 0.35));

    // ---- the standing sample -----------------------------------------------------------------
    const origin: V3 = [this.rootPos[0], this.rootPos[1], this.visZ.x];
    const toModel = (w: Readonly<V3>): V3 => qrotate(inv, vsub(w, origin));
    const ph = feet.phase;
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
    const breathe = Math.sin(this.time * (1.9 + 1.2 * pain) + this.seed);
    // idle weight shift (contrapposto): the hips settle over one leg, the free knee bends
    const shiftTarget = idle > 0.5 && !aiming && !inp.guard ? (Math.floor((this.time + this.seed * 3.7) / (5 + 4 * (1 - st.fidget))) % 2 === 0 ? 1 : -1) : 0;
    // (a hurt leg carries no weight: the hips settle over the good one)
    const favour = limpL - limpR;
    this.shift.update(Math.abs(favour) > 0.15 ? Math.sign(favour) : shiftTarget, dt);
    const shiftX = this.shift.x * 0.035 * k * Math.max(idle, Math.min(1, Math.abs(favour) * 2));
    hipRoll += this.shift.x * 0.07 * idle;
    const crouchDrop = crouch * 0.4 * k;
    // limp: the pelvis dips over the wounded leg while it bears weight
    const limpDip = moving ? (fract(ph) < D ? limpL : 0) * 0.05 * k + (fract(ph + 0.5) < D ? limpR : 0) * 0.05 * k : 0;
    let pz = this.restPelvisZ - g.sink - crouchDrop + walkBob - 0.012 * k * idle - limpDip;
    // the hips: standing, they stay with the planted feet (the trunk and the head turn first,
    // the feet follow with steps); walking, they turn towards the motion
    let hipsWant = this.lowerYaw.x;
    const onFeet = !feet.feet[0].held && !feet.feet[1].held;
    if (!moving && onFeet && !inp.airborne && standW > 0.99) {
      let sx = 0, cy = 0;
      for (const f of feet.feet) {
        const rel = f.yaw + f.side * st.toeOut - this.rootYaw;
        sx += Math.sin(rel);
        cy += Math.cos(rel);
      }
      hipsWant = lerp(clamp(Math.atan2(sx, cy), -1.2, 1.2), this.lowerYaw.x, 0.3);
    }
    this.hipsYaw.update(hipsWant, dt);
    const pelvisYaw = this.hipsYaw.x + hipYawOsc;
    // banking into the turns of the path (its lateral acceleration, not the body's yaw) and a
    // spring-loaded lean into (de)acceleration
    this.bank.update(clamp(aLocal[0] * 0.025, -0.12, 0.12) * moveAmt, dt);
    this.trunkLean.omega = lerp(7.5, 5, st.heavy);
    this.trunkLean.update(clamp(aLocal[1] * (0.03 + 0.02 * st.heavy), -0.2, 0.24), dt);
    const pelvisRot = qmul(qz(pelvisYaw), qeuler(-(0.1 * crouch + 0.05 * g.lean), hipRoll + this.bank.x * 0.6, 0));
    const px = swayX + shiftX;
    const py = -0.07 * crouch * k;
    const lowest = this.restPelvisZ - 0.2 * k - crouchDrop;
    for (const f of feet.feet) {
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
    this.nod.update(0, dt);
    this.recoil.update(0, dt);
    const pzS = Math.min(this.pelvisZ.x, pz + 0.01 * k);
    const S0 = this.sA;
    S0.pelvisPos = [px, py, pzS];
    S0.pelvisRot = pelvisRot;
    const lean = g.lean + this.trunkLean.x + crouch * 0.38 + (this.moodKind === 'panic' ? this.moodW.x * 0.12 : 0) + (this.moodKind === 'cower' ? this.moodW.x * 0.25 : 0);
    // posture: slouched (chest and neck forward) .. upright (chest up); pain hunches
    const posture = st.posture * 0.07 - pain * 0.12;
    const counter = -hipYawOsc * 1.6 * (1 - 0.8 * tactical);
    S0.spine = qeuler(-lean * 0.5 + (0.1 * crouch + 0.05 * g.lean) + posture, -hipRoll * 0.6 - this.bank.x * 0.3, counter * 0.4);
    S0.chest = qeuler(-lean * 0.45 + 0.02 * breathe * (1 - 0.5 * moveAmt) + posture * 0.8, -hipRoll * 0.4 - this.bank.x * 0.2, counter * 0.6);
    S0.neck = qx(posture * 0.6);
    S0.head = qx(0);
    for (let i = 0; i < 2; i++) {
      const f = feet.feet[i]!;
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
        const so = this.sampleStance(other, this.sB, S0, toModel);
        S = blendSamples(S0, so, 1 - standW, this.sOut);
      } else {
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
    const impactZ = this.impact.x;
    // a strike at a target out of reach steps into it: the pelvis drives forward with the blow
    let lunge = 0;
    let lungeLean = 0;
    const lungeDir: V3 = [0, 0, 0];
    const act = this.act;
    if (chA && act?.target && act.def.reach) {
      const tm = toModel(act.target);
      const h = Math.hypot(tm[0], tm[1]);
      if (h > 1e-3) {
        const kick = chA.strikeFootR !== undefined || chA.strikeFootL !== undefined;
        const s = clamp(Math.max(chA.strikeR?.[0] ?? 0, chA.strikeL?.[0] ?? 0, chA.strikeFootR?.[0] ?? 0, chA.strikeFootL?.[0] ?? 0), 0, 1);
        const need = clamp(h - act.def.reach * k, 0, 0.36 * k) * s * wA;
        lunge = need * (kick ? 1 : 0.75);
        lungeLean = kick ? 0 : need * 1.4;
        lungeDir[0] = tm[0] / h;
        lungeDir[1] = tm[1] / h;
      }
    }
    vcopy(
      [
        S.pelvisPos[0] + add('pelvis', 0) * k + lungeDir[0] * lunge,
        S.pelvisPos[1] + add('pelvis', 1) * k + lungeDir[1] * lunge,
        S.pelvisPos[2] + add('pelvis', 2) * k + impactZ - lunge * 0.15,
      ],
      pose.t[H.pelvis],
    );
    const pr: V3 = [add('pelvisRot', 0), add('pelvisRot', 1), add('pelvisRot', 2)];
    pose.r[H.pelvis] = qmul(S.pelvisRot, qeuler(pr[0] * DEG, pr[1] * DEG, pr[2] * DEG));
    // the pelvis where the physics has it (the body reacting, falling, lying)
    if (ctl.pelvisPos && ctl.pelvisWeight > 0) {
      const w = clamp(ctl.pelvisWeight, 0, 1);
      const pm = toModel(ctl.pelvisPos);
      if (!ctl.pelvisHeight) pm[2] = pose.t[H.pelvis]![2];
      vlerp(pose.t[H.pelvis]!, pm, w, pose.t[H.pelvis]);
      pose.r[H.pelvis] = qnlerp(pose.r[H.pelvis]!, qmul(inv, ctl.pelvisRot), w);
    }

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
    // peeking round a corner is done standing (or crouched) still: walking, the body is upright
    this.leanS.update(clamp(inp.lean, -1, 1) * (1 - smoothstep(0.25, 0.7, speed)), dt);
    const aw = this.aimW.x;
    const rifle = this.weapon !== null && this.weapon.kind !== 'pistol' && this.weapon.kind !== 'knife';
    // a long gun bladed: the trunk turns off the target (from the hip more, so the support hand
    // reaches the handguard)
    const blade = aw * (rifle ? (inp.carry === 'aim' ? 0.3 : inp.carry === 'hip' ? 0.5 : 0) : 0);
    const turn = S.turn;
    const toward = (aiming || (tgt && inp.carry !== 'relaxed')) && tgt ? this.aimYaw.x - blade : 0;
    const trunkYaw = clamp(toward - this.hipsYaw.x * standW, -0.95, 0.95) * turn;
    const trunkPitch = (tgt && (aiming || inp.carry !== 'relaxed') ? this.aimPitch.x * lerp(0.55, 0.75, aw) : 0) * turn;
    const peek = this.leanS.x * 0.3;
    const fold = ctl.fold + pain * 0.12;
    const sp: V3 = [add('spine', 0) * DEG + ctl.spine[0], add('spine', 1) * DEG + ctl.spine[1], add('spine', 2) * DEG + ctl.spine[2]];
    const ch: V3 = [add('chest', 0) * DEG + ctl.chest[0] - this.recoil.x * 0.1, add('chest', 1) * DEG + ctl.chest[1], add('chest', 2) * DEG + ctl.chest[2]];
    pose.r[H.spine] = qmul(qmul(qeuler(trunkPitch * 0.3 - fold * 0.55 - lungeLean * 0.5 * lungeDir[1], peek * 0.55 + lungeLean * 0.5 * lungeDir[0], trunkYaw * 0.35), S.spine), qeuler(sp[0], sp[1], sp[2]));
    pose.r[H.chest] = qmul(qmul(qeuler(trunkPitch * 0.45 - fold * 0.45 - lungeLean * 0.5 * lungeDir[1], peek * 0.45 + lungeLean * 0.5 * lungeDir[0], trunkYaw * 0.45), S.chest), qeuler(ch[0], ch[1], ch[2]));
    if (inp.lean !== 0 || Math.abs(this.leanS.x) > 0.01) pose.t[H.pelvis]![0] += this.leanS.x * 0.07 * k;
    this.fk.update(pose, 0);

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
    // a behaviour's look (a wound, a threat) takes over
    this.lookW.update(ctl.look ? clamp(ctl.lookWeight, 0, 1) : 0, dt);
    if (ctl.look) {
      const m = toModel(ctl.look);
      const dd = vsub(m, [0, 0, chestRest[2] + 0.3 * k]);
      const w = this.lookW.x;
      lookYaw = lerp(lookYaw, clamp(Math.atan2(-dd[0], dd[1]), -1.9, 1.9), w);
      lookPitch = lerp(lookPitch, clamp(Math.atan2(dd[2], Math.hypot(dd[0], dd[1])), -1.2, 1.1), w);
    }
    // saccades: the head turns fast to a new target, then settles
    const far = Math.abs(lookYaw - this.headYaw.x) > 0.5;
    this.headYaw.omega = far ? 8 : 5;
    this.headYaw.update(lookYaw, dt);
    this.headPitch.update(lookPitch, dt);
    const chestQ = this.fk.q[H.chest]!;
    const chestFwd = qrotate(chestQ, [0, 1, 0]);
    const chestYawNow = Math.atan2(-chestFwd[0], chestFwd[1]);
    const chestPitchNow = Math.asin(clamp(chestFwd[2], -1, 1));
    const relYaw = clamp(wrapAngle(this.headYaw.x - chestYawNow), -1.3, 1.3) * lookW;
    const relPitch = clamp(this.headPitch.x - chestPitchNow, -0.9, 0.8) * lookW;
    const nk: V3 = [add('neck', 0) * DEG + ctl.neck[0], add('neck', 1) * DEG + ctl.neck[1], add('neck', 2) * DEG + ctl.neck[2]];
    const hd: V3 = [add('head', 0) * DEG + ctl.head[0] + this.nod.x * 0.12, add('head', 1) * DEG + ctl.head[1], add('head', 2) * DEG + ctl.head[2]];
    pose.r[H.neck] = qmul(qmul(qeuler(relPitch * 0.4 + this.nod.x * 0.06, -peek * 0.4, relYaw * 0.4), S.neck), qeuler(nk[0], nk[1], nk[2]));
    pose.r[H.head] = qmul(qmul(qeuler(relPitch * 0.6, -0.12 * aw * (rifle ? 1 : 0) - peek * 0.4, relYaw * 0.6), S.head), qeuler(hd[0], hd[1], hd[2]));
    this.fk.update(pose, H.neck);

    // ---- legs (last: every pelvis motion bends the knees, the feet stay) ---------------------
    for (let i = 0; i < 2; i++) {
      const f = feet.feet[i]!;
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
        // the foot's pitch in the air: pointed, or toes up for a push kick at the strike
        const kickRot = qx(lerp(-0.6, this.act?.def.kickPitch ?? -0.6, clamp(s, 0, 1)));
        if (s > 0 && this.act?.target) {
          // the striking part of the foot (the toes' end) meets the target
          const toeOff = qrotate(kickRot, vsub(this.skeleton.restTail[f.toe]!, this.skeleton.restHead[f.foot]!));
          a = vlerp(a, vsub(toModel(this.act.target), toeOff), clamp(s, 0, 1.2));
        }
        target = vlerp(target, a, fw);
        pole = vlerp(pole, [0.1 * f.side, 1, 0.4], fw);
        rot = qnlerp(rot, kickRot, fw);
      }
      solveTwoBone(pose, this.fk, f.thigh, f.shin, f.foot, target, pole, 0.02, KNEE_REST);
      setModelRotation(pose, this.fk, f.foot, rot);
      pose.r[f.toe] = qx(fp.toe);
      this.fk.updateBone(pose, f.toe);
    }

    // ---- arms ---------------------------------------------------------------------------------
    this.armSwing(dt, g, moving, speed, breathe, idle);
    this.fk.update(pose, H.clavicleL);
    // stance hands (seated on the thighs, prone on the elbows...)
    this.restHands(S, 1 - standW);
    // the support hand lets go of the weapon when a behaviour needs it
    this.taskW[0].update(ctl.arms[0] ? clamp(ctl.arms[0].weight, 0, 1) : 0, dt);
    this.taskW[1].update(ctl.arms[1] ? clamp(ctl.arms[1].weight, 0, 1) : 0, dt);
    const freeLeft = this.taskW[0].x > 0.35;
    let heldProp = false;
    if (this.weapon && this.weapon.kind !== 'knife') {
      heldProp = this.hold.hold(dt, this.arms, this.weapon, {
        carry: inp.carry,
        aimAt: inp.aimAt ? toModel(inp.aimAt) : null,
        aiming,
        moving,
        run,
        phase: feet.phase,
        aimW: aw,
        k,
        chP,
        wP,
        chA,
        wA,
        freeLeft,
      });
    }
    if (!heldProp && this.moodW.x > 0.01 && this.moodKind !== 'normal') this.moodArms(this.moodW.x, feet.phase);
    // action hands: the posture layer, then one-shots on top
    if (chP && wP > 0) this.actionHands(chP, wP, this.poseAct!, toModel, heldProp);
    if (chA && wA > 0) this.actionHands(chA, wA, this.act!, toModel, heldProp);
    // clavicle channels (shrugs)
    for (const [c, b, s] of [
      ['clavR', H.clavicleR, 1],
      ['clavL', H.clavicleL, -1],
    ] as const) {
      const e: V3 = [add(c, 0) * DEG, add(c, 1) * DEG - s * ctl.shrug * 0.35, add(c, 2) * DEG];
      if (e[0] !== 0 || e[1] !== 0 || e[2] !== 0) {
        pose.rotateLocal(b, qeuler(e[0], e[1], e[2]));
        this.fk.updateSubtree(pose, b);
      }
    }
    // behaviour tasks on top (a hand to a wound, on a wall, out to break a fall)
    for (let i = 0; i < 2; i++) {
      const task = ctl.arms[i];
      const w = this.taskW[i]!.x;
      if (!task || w < 0.01) continue;
      const side: Side = i === 0 ? 'L' : 'R';
      // (a right hand holding a long gun takes the gun with it: the gun is let go of meanwhile)
      const chestQn = this.fk.q[H.chest]!;
      const pole = task.pole ? qrotate(inv, task.pole) : qrotate(chestQn, vnorm([side === 'R' ? 0.8 : -0.8, -0.35, -0.5]));
      this.arms.handIK(side, toModel(task.target), task.rot ? qmul(inv, task.rot) : null, pole, clamp(w, 0, 1));
    }
    this.fk.update(pose, H.clavicleL);

    // ---- props, world, events ---------------------------------------------------------------------
    this.world.compute(pose, origin, rootRot);
    this.weaponInHand = false;
    if (this.weapon) {
      if (this.weapon.kind === 'knife' || !heldProp || this.taskW[1].x > 0.5) {
        this.propInHand(H.handR);
        this.weaponInHand = true;
      } else {
        vcopy(this.world.p[H.weapon]!, this.weaponPos);
        this.weaponRot = [...this.world.q[H.weapon]!] as Quat;
      }
    }
    this.flushEvents();
  }

  // ---- actions ------------------------------------------------------------------------------

  private updateActions(dt: number): void {
    const inp = this.input;
    const speed = Math.hypot(this.velocity[0], this.velocity[1]);
    const free = this.stance === 'stand' && this.stanceP >= 1 && inp.mood === 'normal' && !this.control.busy;
    const armed = this.weapon !== null && this.weapon.kind !== 'knife';
    // (a pause with the weapon down: standing still, not aiming)
    this.armedIdle = armed && free && speed < 0.15 && inp.carry !== 'aim' && inp.carry !== 'hip' ? this.armedIdle + dt : 0;
    // the posture layer: guard, talk, idle poses
    let want: string | null = null;
    if (inp.guard && free) want = this.weapon?.kind === 'knife' ? 'knifeGuard' : 'guard';
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
    // one-shots the plan starts itself: gestures and nods in conversation, fidgets
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
      } else if (inp.idle && armed && this.armedIdle > 2.5) {
        // an armed body in a pause: the helmet, the brow, the shoulders, the weapon, a look round
        this.nextFidget -= dt;
        if (this.nextFidget <= 0) {
          this.nextFidget = 6 + this.rng.next() * 9 * (1.3 - this.style.fidget);
          if (this.rng.chance(0.75)) this.play(this.rng.pick([...ARMED_FIDGETS]));
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

  /** An idle posture that suits the character (stable per character). */
  private idlePoseFor(listening: boolean): string {
    const r = new Rng(this.seed * 131 + (listening ? 7 : Math.floor(this.time / 15)));
    const pool = listening ? ['armsCrossed', 'pockets', 'handsFolded', 'handsBehind', 'handsOnHips'] : [...IDLE_POSES];
    return r.pick(pool);
  }

  private flushEvents(): void {
    for (const { e, p } of this.pendingEvents) {
      const pos = this.limbPos(e.limb);
      this.events.push({ name: e.name, action: p.def.name, ...(e.limb ? { limb: e.limb } : {}), pos, target: p.target ? ([...p.target] as V3) : null });
    }
    this.pendingEvents.length = 0;
  }

  /** World position of a limb's striking point (in the plan's pose). */
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
    const ph = this.feetPlanner.phase;
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
      this.arms.handIK(side, h, null, pole, w);
    }
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
      this.arms.handIK(side, palm, rot, pole, ww);
    }
  }

  /** Mood arm poses by IK (blended over the swing pose by weight w). */
  private moodArms(w: number, phase: number): void {
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
        const b = Math.sin(TAU * phase + (side < 0 ? 0 : Math.PI)) * 0.06 * k;
        target = vadd(headP, qrotate(chestQ, [side * 0.16 * k, 0.1 * k, 0.12 * k + b]));
        pole = qrotate(chestQ, vnorm([side, 0.2, -0.5]));
        rot = qmul(chestQ, qeuler(70 * DEG, side * -90 * DEG, 0));
      }
      this.arms.handIK(side < 0 ? 'L' : 'R', target, rot, pole, w);
    }
  }

  // ---- props ------------------------------------------------------------------------------

  /** A prop held in the hand (a knife, a lowered pistol): along the knuckles, grip in the palm. */
  private propInHand(b: number): void {
    const w = this.world;
    const side = b === H.handL ? 'L' : 'R';
    const q = qmul(w.q[b]!, qconj(this.arms.canonical(side)));
    const palm = vadd(w.p[b]!, qrotate(w.q[b]!, this.arms.palmOffset(side)));
    vcopy(palm, this.weaponPos);
    // a knife points out of the fist along the knuckles; a pistol hangs muzzle down-forward; a
    // long gun hangs from the hand
    this.weaponRot = this.weapon?.kind === 'pistol' ? qmul(q, qx(-0.3)) : this.weapon?.kind === 'knife' ? q : qmul(q, qx(-0.9));
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
  footState(): { planted: boolean; pos: V3; ankle: V3; yaw: number; forced: boolean }[] {
    return this.feetPlanner.state();
  }

  /** World position between the eyes (in the plan's pose). */
  eyes(out: V3 = [0, 0, 0]): V3 {
    const h = this.skeleton.restHead[H.head]!;
    return this.world.pointOf(H.head, [h[0], h[1] + 0.09 * this.k, h[2] + 0.12 * this.k], out);
  }

  /** The palm of a hand (world, in the plan's pose). */
  palm(side: Side, out: V3 = [0, 0, 0]): V3 {
    const b = side === 'L' ? H.handL : H.handR;
    return this.world.pointOf(b, vadd(this.skeleton.restHead[b]!, this.arms.palmOffset(side)), out);
  }
}
