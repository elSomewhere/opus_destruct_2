/**
 * The body's behaviours: the character's motor intelligence, in the manner of NaturalMotion's
 * Euphoria. The motion plan says what the character means to do; the physical body is what it
 * is made of; the behaviours sit between them, read the body's senses (balance, contacts, what
 * is within reach, what hit it) and decide, every frame, how the muscles work: how tense each
 * region is, how much the legs hold the body up and where the feet step, where the hands go,
 * where the head turns.
 *
 * Modes:
 * - animated: the body tracks the plan (muscles and assists strong), with its own secondary
 *   motion; blows it can take in its stride are absorbed.
 * - reacting: knocked off the plan (a hit, a shove, a blast, a caught foot). Balance takes
 *   over: the legs hold the body up but no longer steer it; the ground pushes back through the
 *   feet's centre of pressure (the inverted pendulum), and the feet step where the capture
 *   point says (a stagger is a run of such steps), arms out; a hand reaches for a wall within
 *   reach and holds on. Back in balance and still, it hands back to the plan.
 * - falling: balance lost: the legs give, the hands go out to break the fall (on the ground or
 *   a wall), the head is kept off the ground.
 * - lying: on the ground, limp but alive (knocked down, knocked out), holding a wound.
 * - rising: gathering and getting up through sitting or pushing up, kneeling, standing, the
 *   legs taking the weight back.
 * - dying: the muscles fade (the legs first): the knees buckle, a hand goes to the wound, a
 *   last step or two, the arms half catch the fall; near a wall the body slumps against it.
 * - dead: no muscle; the body settles and sleeps.
 *
 * Reflexes on top of any mode: flinching from what lands close (the head turns away and
 * ducks, the shoulders come up, a hand comes up between the face and the danger), holding a
 * wound, the posture of injuries (a limp, a weak arm, a hunch), stuns (a struck limb goes limp
 * for a moment).
 */
import { B, BODY_BONE, BODY_COUNT, HumanoidBody, REGION, type Region } from '../body/humanoid.ts';
import { WorldPose } from '../core/skeleton.ts';
import { H } from '../humanoid/rig.ts';
import { qconj, qeuler, qfromBasis, qmul, qrotate, qz, type Quat } from '../math/quat.ts';
import { clamp, lerp, smoothstep, vadd, vcopy, vcross, vdot, vlen, vnorm, vscale, vsub, type V3 } from '../math/vec.ts';
import { Rng } from '../math/random.ts';
import type { MotionPlan } from '../motion/plan.ts';
import { Injuries, zoneOfPart, type HitInfo, type Injury, type Zone } from './injuries.ts';
import { SupportPolygon, Surroundings, type Surface } from './senses.ts';

export type BodyMode = 'animated' | 'reacting' | 'falling' | 'lying' | 'rising' | 'dying' | 'dead';

/** Something the body perceives close by: a round smacking in, a whiz past the head, a blast. */
export interface Perception {
  point: V3;
  /** 0..1 (and more for blasts). */
  strength: number;
  kind: 'impact' | 'whiz' | 'blast' | 'blow';
}

interface Threat {
  point: V3;
  amount: number;
  age: number;
  /** Hold time before it fades. */
  hold: number;
}

interface Brace {
  surface: Surface;
  /** 0 left hand, 1 right. */
  hand: number;
  /** The palm's target (world). */
  target: V3;
  normal: V3;
  why: 'balance' | 'lean' | 'fall' | 'slump';
  /** The hand has arrived and holds on. */
  holding: boolean;
  age: number;
}

const G = 9.81;
let ARMS_AT_EASE = 0.62;
/** (tuning) */
export function setArmsAtEase(t: number): void {
  ARMS_AT_EASE = t;
}
const PARENT_OF: readonly number[] = [-1, B.pelvis, B.spine, B.chest, B.chest, B.upperarmL, B.forearmL, B.chest, B.upperarmR, B.forearmR, B.pelvis, B.thighL, B.shinL, B.pelvis, B.thighR, B.shinR];
const REGIONS: readonly Region[] = ['trunk', 'neck', 'armL', 'armR', 'legL', 'legR'];

export class Behaviours {
  readonly plan: MotionPlan;
  readonly body: HumanoidBody;
  readonly injuries = new Injuries();
  readonly surroundings = new Surroundings();
  readonly support = new SupportPolygon();
  mode: BodyMode = 'animated';
  /** Seconds in the current mode. */
  modeTime = 0;
  /** The physical body is simulated (else the plan's pose is shown as is). */
  physical = false;
  alive = true;
  conscious = true;
  // balance
  readonly com: V3 = [0, 0, 0];
  readonly comVel: V3 = [0, 0, 0];
  readonly capture: V3 = [0, 0, 0];
  /** Distance of the capture point outside the support (negative: inside). */
  balanceError = 0;
  /** The pendulum's natural frequency (sqrt(g / h), 1/s). */
  private w0 = 3.5;
  groundZ = 0;
  /** Stagger steps taken in this reaction. */
  steps = 0;
  /** No foot on the ground. */
  airborne = false;
  /** Nerves 0..1: rattled by what lands close (a lower, hunched body; quicker flinches). */
  nerves = 0;
  /** Where the body's root is (ground under the body) when the body leads, and its heading. */
  readonly bodyRoot: V3 = [0, 0, 0];
  bodyYaw = 0;
  /** Root displacement the host still has to apply (the body moved it). */
  readonly rootMotion: V3 = [0, 0, 0];
  /** The last brace (for tests and debugging). */
  brace: Brace | null = null;
  /** Tone per region this frame (for tests and debugging). */
  readonly regionTone: Record<Region, number> = { trunk: 1, neck: 1, armL: 1, armR: 1, legL: 1, legR: 1 };

  private balancedFor = 0;
  private lowFor = 0;
  private threats: Threat[] = [];
  /** Per-body stun (0..1: tone lost), recovering. */
  private readonly stun = new Float32Array(BODY_COUNT);
  /** Whole-body daze (0..1). */
  private daze = 0;
  /** The shock of a hit (0..1): the whole body slack for a moment. */
  private shock = 0;
  private downUntil = 0;
  /** Where the last blow came from, and when (the head turns to look for it). */
  private hitFrom: V3 | null = null;
  private hitAt = -99;
  /** How long the body has been beyond saving (reacting). */
  private lostFor = 0;
  private dyingFor = 0.6;
  private dyingHead = false;
  private readonly writheSeed: number;
  /** The pelvis height the dying body sinks from. */
  private dieZ = 0;
  private tension = 0;
  private readonly rng: Rng;
  /** Hand tasks' physical grip (0..1) this frame, left and right. */
  private readonly grip: [number, number] = [0, 0];
  private catchT = [0, 0];
  private stepCooldown = 0;
  private upset = 0;
  private time = 0;
  private rootInit = false;
  private reactT = 0;

  constructor(plan: MotionPlan, body: HumanoidBody, seed = 1) {
    this.plan = plan;
    this.body = body;
    this.rng = new Rng(seed * 977 + 5);
    this.writheSeed = this.rng.next() * 100;
  }

  get k(): number {
    return this.plan.k;
  }

  /** The body leads the root (the host follows it). */
  get leading(): boolean {
    return this.mode !== 'animated';
  }

  /** Something is going on that needs the physics (not calm). */
  get needsPhysics(): boolean {
    if (this.mode !== 'animated') return true;
    if (this.brace) return true;
    for (let i = 0; i < BODY_COUNT; i++) if (this.stun[i]! > 0.05) return true;
    return this.upset > 0;
  }

  private setMode(m: BodyMode): void {
    if (m === this.mode) return;
    this.mode = m;
    this.modeTime = 0;
    if (m === 'reacting') {
      this.lostFor = 0;
      this.steps = 0;
      this.balancedFor = 0;
      this.reactT = 0;
    }
    if (m === 'animated') {
      this.plan.feetPlanner.stepping = false;
      this.brace = null;
    }
    if (m === 'falling') this.landed[0] = this.landed[1] = null;
    // (what knocked the body over is spent: it gets up from the ground afresh)
    if (m === 'falling' || m === 'lying' || m === 'rising') this.forceReact = false;
  }

  // ---- events --------------------------------------------------------------------------------

  /**
   * A blow on the living body (world): an impulse where it lands (the physics answers it), a
   * limp moment of the struck part, a wound to hold, a flinch; the balance deals with the rest.
   * Returns the zone.
   */
  hit(info: HitInfo, part: number, pose: WorldPose): Zone {
    const zone = zoneOfPart(part);
    const f = clamp(info.force, 0, 8);
    const d = vnorm(info.dir);
    // how hard it shoves (N s): a round mostly stings, a kick or a blast moves the body
    let J = info.kind === 'bullet' ? 14 + 12 * Math.min(f, 2.5) : info.kind === 'blunt' ? 34 * f : info.kind === 'blade' ? 9 * f : 55 * f;
    // (a fist on the jaw snaps the head round on the neck: the head moves away from it, it
    // does not carry the body with it as a blow to the trunk does)
    if (part === B.head && info.kind === 'blunt') J = Math.min(J, (2.2 + 1.1 * f) * this.body.parts[B.head]!.mass);
    // a light part cannot take it all: what it cannot passes up the limb to its parent
    const dvMax = info.kind === 'blunt' ? 7 : info.kind === 'blast' ? 10 : 4;
    let left = J;
    let p = part;
    for (let hop = 0; hop < 3 && left > 0 && p >= 0; hop++) {
      const pb = this.body.parts[p]!;
      const take = Math.min(left, dvMax * pb.mass * (hop === 0 ? 1 : 1.5));
      const r = hop === 0 ? vsub(info.point, pb.x) : ([0, 0, 0] as V3);
      pb.applyImpulse(d[0] * take, d[1] * take, d[2] * take * 0.3, r[0], r[1], r[2]);
      left -= take;
      p = p === B.pelvis ? -1 : PARENT_OF[p]!;
    }
    // the struck part spins about the blow's lever arm (a shoulder hit turns the chest, a round
    // high in the chest rocks it back, a jaw hit snaps the head round), its parent with it:
    // what a hit looks like, more than its momentum alone would give
    {
      const hb = this.body.parts[part]!;
      // (about the joint it turns on: a round in the chest rocks it about the waist)
      const jt = part > 0 ? this.body.joints[part]! : null;
      const pivot = jt ? hb.point(jt.anchorB) : hb.x;
      const r = vsub(info.point, pivot);
      const ax = vcross(r, d);
      const al = vlen(ax);
      if (al > 1e-4) {
        const trunkHit = part === B.pelvis || part === B.spine || part === B.chest;
        // (a heavy blow rocks the trunk, it does not fold it in two)
        const rate = Math.min(trunkHit ? 4.2 : 9, (trunkHit ? 3.2 : part === B.head ? 3.6 : 4) * Math.min(f, 2.5) * (info.kind === 'blade' ? 0.4 : 1) * clamp(al / 0.12, 0.3, 1.2));
        for (let hop = 0, q = part; hop < 2 && q >= 0; hop++, q = PARENT_OF[q]!) {
          const w = this.body.parts[q]!.w;
          const k2 = (hop === 0 ? 1 : part === B.head ? 0.2 : 0.55) * rate / al;
          w[0] += ax[0] * k2;
          w[1] += ax[1] * k2;
          w[2] += ax[2] * k2;
        }
      }
    }
    // the rest of the body takes a share (it is connected)
    const share = info.kind === 'bullet' ? 0.15 : info.kind === 'blast' ? 0.6 : 0.3;
    const dv = (J * share + Math.max(0, left)) / this.body.totalMass;
    this.body.shove(d[0] * dv, d[1] * dv, 0);
    // the struck part and its neighbours go slack for a moment
    // (the trunk less: it carries the body)
    const trunkPart = part === B.pelvis || part === B.spine || part === B.chest;
    const s = clamp(0.35 + 0.3 * f, 0, trunkPart ? 0.65 : 0.95);
    this.stunPart(part, s);
    // the whole body goes slack for a moment with the shock of it (it answers the blow with
    // its own weight: the arms swing, the head lolls), then the muscles take over again
    this.shock = Math.max(this.shock, clamp(0.3 + 0.25 * f, 0, 0.8) * (info.kind === 'blade' ? 0.5 : part === B.head ? 0.6 : 1));
    if (part > 0) this.stunPart(PARENT_OF[part]!, s * 0.5);
    // a blow to the head dazes (hard ones knock out)
    if (zone === 'head' && info.kind === 'blunt') this.daze = Math.max(this.daze, clamp(0.25 * f, 0, 0.9));
    if (info.kind === 'blast') this.daze = Math.max(this.daze, clamp(0.4 * f, 0, 0.85));
    // a wound to hold
    if (info.kind === 'bullet' || info.kind === 'blade') {
      const q = pose.q[BODY_BONE[part]!]!;
      const bone = BODY_BONE[part]!;
      const local = vsub(qrotate(qconj(q), vsub(info.point, pose.p[bone]!)), this.body.comLocal[part]!);
      const nrm = vnorm(qrotate(qconj(q), vscale(d, -1)));
      const sev = clamp(0.25 + 0.35 * f, 0, 1);
      const inj: Injury = { part, local, normal: nrm, zone, kind: info.kind, severity: sev, lasting: sev * 0.45, age: 0, holdUntil: 1.6 + 2.6 * sev + (zone === 'gut' || zone === 'chest' ? 2 : 0) };
      this.injuries.add(inj);
    }
    // the body flinches from the blow (the eyes close, the shoulders come up), a beat after
    // the blow itself has shown
    this.threats.push({ point: vsub(info.point, vscale(d, 0.6)), amount: clamp(0.25 + 0.15 * f, 0, 0.7), age: -0.18, hold: 0.1 });
    this.plan.interrupt(f > 0.8);
    // then a look for where it came from
    this.hitFrom = vsub(info.point, vscale(d, 6));
    this.hitFrom[2] = Math.max(this.hitFrom[2], info.point[2]);
    this.hitAt = this.time;
    this.upset = 0.4;
    // knocked off the plan: a blow that moves the whole body (a blow to the head mostly snaps
    // the head), a leg that gives, a daze
    const bodyDv = (J * (zone === 'head' ? 0.35 : 1) * (1 + share)) / this.body.totalMass;
    if (bodyDv > 0.55 || ((zone === 'legL' || zone === 'legR') && f > 0.7 && info.kind !== 'blunt') || this.daze > 0.4) this.forceReact = true;
    return zone;
  }

  /** Another body bumped into this one (N s): hard enough, the balance has to answer it. */
  bumped(j: number): void {
    // (on the ground or getting up, bumps are part of it)
    if (!this.alive || (this.mode !== 'animated' && this.mode !== 'reacting')) return;
    // (a brush of hands or shoulders in passing is nothing; a body knocked into is)
    const dv = j / this.body.totalMass;
    if (dv > 0.1) this.upset = Math.max(this.upset, Math.min(0.5, 2 * dv));
    if (dv > 0.35) this.forceReact = true;
  }

  /** Something close by: a flinch, stronger the closer and the bigger it is. */
  perceive(p: Perception): void {
    if (!this.alive || !this.conscious) return;
    const eyes = this.plan.eyes();
    const d = Math.hypot(p.point[0] - eyes[0], p.point[1] - eyes[1], p.point[2] - eyes[2]);
    const reach = p.kind === 'blast' ? 12 : p.kind === 'whiz' ? 1.6 : 3.2;
    const amount = clamp(p.strength * (1 - d / reach) * (1 + 0.6 * this.nerves), 0, 1.3);
    if (amount < 0.08) return;
    this.threats.push({ point: [...p.point], amount, age: 0, hold: 0.1 + 0.25 * amount });
    if (this.threats.length > 4) this.threats.shift();
    this.nerves = Math.min(1, this.nerves + 0.12 * amount);
    this.upset = Math.max(this.upset, 0.25 * amount);
  }

  /**
   * A shove of the whole body (a blast's push, a collision): `dv` is the velocity change of the
   * trunk (m/s, world); limbs flail after it.
   */
  push(dv: Readonly<V3>, stunAll = 0): void {
    const share = [0.8, 1, 1.1, 1.15, 1.05, 1, 0.9, 1.05, 1, 0.9, 0.7, 0.55, 0.45, 0.7, 0.55, 0.45];
    this.body.shove(dv[0], dv[1], dv[2], share);
    if (stunAll > 0) this.daze = Math.max(this.daze, stunAll);
    this.upset = 0.5;
    if (Math.hypot(dv[0], dv[1]) > 0.3) this.forceReact = true;
  }

  /**
   * A foot catches (the host's say, or an obstacle): the swinging foot stops dead (or the next
   * one to swing, within half a second).
   */
  trip(): void {
    if (!this.alive || this.mode === 'falling' || this.mode === 'lying' || this.mode === 'rising') return;
    const feet = this.plan.feetPlanner.feet;
    const i: number = !feet[0].planted && !feet[0].held ? 0 : !feet[1].planted && !feet[1].held ? 1 : -1;
    if (i >= 0 && feet[i]!.swing < 0.8) this.catchFoot(i);
    else this.tripPending = 0.6;
  }

  /** A trip waiting for the next swing (s left). */
  private tripPending = 0;
  /** A caught foot bears no weight and cannot step until then (time). */
  private readonly snagUntil: [number, number] = [0, 0];

  private catchFoot(i: number): void {
    const f = this.plan.feetPlanner.feet[i]!;
    const at = this.physical ? this.solePos(i) : [...f.pos] as V3;
    this.plan.feetPlanner.plantNow(i, at);
    this.snagUntil[i] = this.time + 0.24;
    this.tripPending = 0;
    // the legs stop, the rest carries on: the trunk pitches forward over the stance foot
    const v = this.physical ? this.comVel : this.plan.velocity;
    const sp = Math.hypot(v[0], v[1]);
    if (sp > 0.2 && this.physical) {
      const ax: V3 = [-v[1] / sp, v[0] / sp, 0];
      const pitch = 1.1 + 0.5 * sp;
      for (const [b, k] of [[B.chest, 1], [B.spine, 0.8], [B.head, 1.1]] as const) {
        const w = this.body.parts[b]!.w;
        w[0] -= ax[0] * pitch * k;
        w[1] -= ax[1] * pitch * k;
      }
      const chest = this.body.parts[B.chest]!;
      chest.v[0] += (v[0] / sp) * 0.3;
      chest.v[1] += (v[1] / sp) * 0.3;
    }
    this.upset = 0.6;
    this.forceReact = true;
  }

  private forceReact = false;
  /** How long each swinging foot has been caught on something (s). */
  private readonly blockedFor: [number, number] = [0, 0];
  /** Why the last reaction ended in a fall (debugging). */
  lostWhy = '';

  /**
   * Too badly hurt to stand: the legs give, the body goes down and writhes (clutching the
   * wound, curling up, rocking) for `seconds`, then struggles back up.
   */
  collapse(seconds: number): void {
    if (!this.alive || this.writhing) return;
    this.writhing = true;
    this.downUntil = this.time + seconds;
    this.daze = Math.max(this.daze, 0.8);
    this.upset = 1;
    this.forceReact = true;
  }

  /** Down and writhing in pain (see collapse). */
  writhing = false;

  /** Knocked out: the body drops and stays down `seconds`. */
  knockOut(seconds: number): void {
    this.conscious = false;
    this.daze = 1;
    this.downUntil = this.time + seconds;
    this.upset = 1;
    this.forceReact = true;
  }

  /**
   * Death: the muscles fade over `collapse` seconds (0: at once, a head shot or a blast), the
   * last wound held while they last.
   */
  die(collapse: number): void {
    if (!this.alive) return;
    this.alive = false;
    this.dyingFor = Math.max(0.05, collapse);
    this.dyingHead = collapse < 0.2;
    this.threats.length = 0;
    this.dieZ = this.plan.world.p[H.pelvis]![2];
    this.setMode('dying');
    // the legs give way at once in a quick death: standing takes muscle, and without it the
    // knees fold (a body does not topple like a plank)
    if (this.physical && collapse < 0.7) {
      const pq = this.body.parts[B.pelvis]!.q;
      const f = qrotate(pq, [0, 1, 0]);
      const give = 1.4 * (1 - collapse);
      for (const s of [B.shinL, B.shinR]) {
        const b = this.body.parts[s]!;
        b.v[0] += f[0] * give;
        b.v[1] += f[1] * give;
      }
      this.body.parts[B.pelvis]!.v[2] -= 0.6 * give;
    }
  }

  private stunPart(part: number, s: number): void {
    this.stun[part] = Math.max(this.stun[part]!, s);
  }

  // ---- sensing -------------------------------------------------------------------------------

  /** Where the body is: centre of mass and velocity, the ground under it, the capture point, the support. */
  private senseDt = 0;
  private hadCom = false;
  /** The plan's pelvis velocity (smoothed like the body's). */
  private readonly planPelvisVel: V3 = [0, 0, 0];
  private readonly planPelvisPrev: V3 = [0, 0, 0];

  private sense(pose: WorldPose): void {
    const k = this.k;
    if (this.physical) {
      const px = this.com[0], py = this.com[1], pz = this.com[2];
      this.body.com(this.com);
      if (this.senseDt > 0 && this.hadCom && this.upset < 0.35) {
        // (from the frame's motion: the last substep's velocities carry the contacts' jitter)
        const a = 1 - Math.exp(-this.senseDt * 30);
        this.comVel[0] += ((this.com[0] - px) / this.senseDt - this.comVel[0]) * a;
        this.comVel[1] += ((this.com[1] - py) / this.senseDt - this.comVel[1]) * a;
        this.comVel[2] += ((this.com[2] - pz) / this.senseDt - this.comVel[2]) * a;
      } else this.body.comVelocity(this.comVel);
      this.hadCom = true;
    } else {
      this.hadCom = false;
      // (animated only: the plan's pelvis stands for the centre of mass)
      const p = this.plan.world.p[H.pelvis]!;
      this.com[0] = p[0];
      this.com[1] = p[1];
      this.com[2] = p[2] + 0.08 * k;
      vcopy(this.plan.velocity, this.comVel);
    }
    const g = this.plan.collision.groundHeight(this.com[0], this.com[1], this.com[2], this.com[2] - 2.2 * k);
    this.groundZ = g ?? this.plan.rootPos[2];
    const h = Math.max(0.45 * k, this.com[2] - this.groundZ);
    const w0 = Math.sqrt(G / h);
    this.w0 = w0;
    this.capture[0] = this.com[0] + this.comVel[0] / w0;
    this.capture[1] = this.com[1] + this.comVel[1] / w0;
    this.capture[2] = this.groundZ;
    // the support: planted feet (their soles), a hand holding on
    const pts: number[] = [];
    const feet = this.plan.feetPlanner.feet;
    for (let fi = 0; fi < 2; fi++) {
      const f = feet[fi]!;
      if (!f.planted || this.time < this.snagUntil[fi]!) continue;
      const c = Math.cos(f.yaw), s = Math.sin(f.yaw);
      for (const [a, b] of [
        [-0.07, -0.045],
        [-0.07, 0.045],
        [0.18, -0.04],
        [0.18, 0.04],
      ] as const) {
        const fx = a * k, sx = b * k;
        pts.push(f.pos[0] + c * fx + s * sx, f.pos[1] + s * fx - c * sx);
      }
    }
    if (this.brace?.holding) {
      const t = this.brace.target;
      // a hand on a wall props the body up towards it
      pts.push(t[0] - this.brace.normal[0] * 0.05, t[1] - this.brace.normal[1] * 0.05);
    }
    if (this.physical && pts.length > 0) {
      // so does the trunk or a shoulder leaning on a wall
      for (let i = 0; i < 10; i++) {
        const b = this.body.parts[i]!;
        if (!b.contact || Math.abs(b.contactNormal[2]) > 0.5 || b.contactPoint[2] - this.groundZ < 0.4 * k) continue;
        pts.push(b.contactPoint[0], b.contactPoint[1]);
      }
    }
    this.support.hull(pts);
    // (both feet off the ground: a running stride, a jump; judged by where the feet will land)
    this.airborne = this.support.empty;
    if (this.airborne) {
      for (const f of feet) pts.push(f.target[0], f.target[1]);
      this.support.hull(pts);
    }
    this.balanceError = this.support.empty ? 0 : this.support.distance(this.capture[0], this.capture[1]);
    // the root under the body, and its heading (the pelvis's forward)
    const pq = pose.q[H.pelvis]!;
    const fwd = qrotate(pq, [0, 1, 0]);
    const up = qrotate(pq, [0, 0, 1]);
    // (lying, the pelvis's forward points up or down: its up axis says where the head is)
    const hx = Math.abs(fwd[2]) > 0.7 ? up[0] * Math.sign(fwd[2]) * -1 : fwd[0];
    const hy = Math.abs(fwd[2]) > 0.7 ? up[1] * Math.sign(fwd[2]) * -1 : fwd[1];
    if (Math.hypot(hx, hy) > 0.2) this.bodyYaw = Math.atan2(hy, hx);
    this.bodyRoot[0] = this.com[0];
    this.bodyRoot[1] = this.com[1];
    this.bodyRoot[2] = this.groundZ;
    void up;
  }

  /** Sole (world) of the physical foot i. */
  private solePos(i: number, out: V3 = [0, 0, 0]): V3 {
    const f = this.body.parts[i === 0 ? B.footL : B.footR]!;
    const a = this.body.feet[i]!;
    f.point(a.local, out);
    out[2] -= this.plan.feetPlanner.dims.ankleH;
    return out;
  }

  // ---- the frame, before the plan -------------------------------------------------------------

  /**
   * Senses, changes mode, and says what the plan should do this frame. `pose` is the body's
   * pose of the last frame (physical or planned).
   */
  prepare(dt: number, pose: WorldPose): void {
    this.time += dt;
    this.modeTime += dt;
    const k = this.k;
    const plan = this.plan;
    const ctl = plan.control;
    ctl.reset();
    this.grip[0] = this.grip[1] = 0;
    this.injuries.update(dt);
    for (let i = 0; i < BODY_COUNT; i++) this.stun[i] = Math.max(0, this.stun[i]! - dt * (0.9 + 0.8 * this.stun[i]!));
    this.daze = Math.max(0, this.daze - dt * (this.conscious ? 0.9 : 0));
    this.shock = Math.max(0, this.shock - dt * 2.2);
    this.nerves = Math.max(0, this.nerves - dt * 0.05);
    this.upset = Math.max(0, this.upset - dt);
    this.stepCooldown = Math.max(0, this.stepCooldown - dt);
    if (this.tripPending > 0) {
      this.tripPending -= dt;
      const fs = plan.feetPlanner.feet;
      for (let i = 0; i < 2; i++) if (!fs[i]!.planted && !fs[i]!.held && fs[i]!.swing > 0.15) {
        this.catchFoot(i);
        break;
      }
    }
    for (const t of this.threats) t.age += dt;
    this.threats = this.threats.filter((t) => t.age < t.hold + 1.2);
    this.senseDt = dt;
    if (dt > 0) {
      const pp = plan.world.p[H.pelvis]!;
      const a = 1 - Math.exp(-dt * 30);
      const v = this.planPelvisVel, q = this.planPelvisPrev;
      v[0] += ((pp[0] - q[0]) / dt - v[0]) * a;
      v[1] += ((pp[1] - q[1]) / dt - v[1]) * a;
      v[2] += ((pp[2] - q[2]) / dt - v[2]) * a;
      vcopy(pp, this.planPelvisPrev);
    }
    this.sense(pose);
    if (!this.rootInit) {
      this.rootInit = true;
      vcopy(plan.rootPos, this.bodyRoot);
    }
    // what is around (walls to hold on to), now and then, more often when it may matter
    const wants = this.mode !== 'animated' || this.injuries.pain > 0.3 || this.injuries.legL + this.injuries.legR > 0.3;
    this.surroundings.age += dt;
    if (wants && this.surroundings.age > (this.mode === 'animated' ? 0.4 : 0.12)) this.surroundings.probe(plan.collision, this.bodyRootOrPlan(), k, 1.05);

    // ---- modes ----
    this.modes(dt, pose);

    // ---- the plan's controls ----
    const inj = this.injuries;
    const alive = this.alive;
    // injuries: a limp, pain, a hunch
    ctl.limp[0] = clamp(inj.legL * 1.1, 0, 1);
    ctl.limp[1] = clamp(inj.legR * 1.1, 0, 1);
    ctl.pain = inj.pain;
    ctl.fold += inj.trunk * 0.22 + (this.mode === 'dying' ? 0.25 : 0);
    ctl.crouch += this.nerves * 0.25;
    ctl.care = clamp(1 - 0.6 * smoothstep(2.2, 5, Math.hypot(plan.velocity[0], plan.velocity[1])) - 0.4 * (plan.input.mood === 'panic' ? 1 : 0) - 0.3 * inj.pain, 0.1, 1);
    if (this.mode !== 'animated') ctl.busy = true;
    // the body leads: the plan's pelvis is where the physics has it, its root under the body
    if (this.physical && (this.mode === 'reacting' || this.mode === 'falling' || this.mode === 'dying' || this.mode === 'dead')) {
      const pp = pose.p[H.pelvis]!;
      ctl.pelvisPos = [pp[0], pp[1], pp[2]];
      ctl.pelvisRot[0] = pose.q[H.pelvis]![0];
      ctl.pelvisRot[1] = pose.q[H.pelvis]![1];
      ctl.pelvisRot[2] = pose.q[H.pelvis]![2];
      ctl.pelvisRot[3] = pose.q[H.pelvis]![3];
      ctl.pelvisWeight = this.mode === 'reacting' ? 0.85 : 1;
      ctl.pelvisHeight = this.mode !== 'reacting' && this.mode !== 'dying';
      ctl.holdFeet = true;
      this.carryRoot();
    }
    if (this.mode === 'lying' || this.mode === 'rising') ctl.holdFeet = true;
    if (this.mode === 'falling') {
      // the legs carry nothing now: loose, bent; the trunk curls a little
      ctl.relaxLegs = smoothstep(0.05, 0.35, this.modeTime);
      ctl.fold += 0.15;
    }

    // writhing on the ground: curling up and stretching out, rocking, the head coming up
    if (this.mode === 'lying' && this.writhing && this.alive) {
      const t = this.time + this.writheSeed;
      // spasms of pain come and go: curling up, rocking, the head coming up, easing off
      const spasm = Math.pow(0.5 + 0.5 * Math.sin(t * 0.9) * Math.sin(t * 0.31 + 1), 1.5);
      const a = 0.5 + 0.5 * Math.sin(t * 1.7);
      ctl.relaxLegs = 0.25 + 0.75 * spasm;
      ctl.spine[2] += (0.25 + 0.4 * spasm) * Math.sin(t * 1.3);
      ctl.spine[1] += 0.35 * spasm * Math.sin(t * 0.8 + 2);
      ctl.chest[0] -= 0.35 * spasm * a;
      ctl.neck[0] -= 0.45 * spasm * Math.max(0, Math.sin(t * 1.1));
      ctl.fold += 0.45 * spasm;
    }

    // ---- reflexes and behaviours ----
    if (alive || this.mode === 'dying') {
      if (this.mode === 'reacting') this.balance(dt);
      if (this.mode === 'falling' || (this.mode === 'dying' && this.modeTime > this.dyingFor * 0.4)) this.catchFall(dt, pose);
      this.bracing(dt, pose);
      if (this.conscious) this.flinch(dt, pose);
      this.holdWound(dt, pose);
      // after the flinch and a glance at the wound: where did that come from?
      const since = this.time - this.hitAt;
      if (this.hitFrom && this.conscious && this.alive && !ctl.look && since > 0.45 && since < 3) {
        ctl.look = this.hitFrom;
        ctl.lookWeight = 0.85 * smoothstep(0.45, 0.8, since) * (1 - smoothstep(2.2, 3, since));
      }
    }
  }

  private bodyRootOrPlan(): V3 {
    return this.mode === 'animated' ? this.plan.rootPos : this.bodyRoot;
  }

  /** The plan's root follows the body (the host is told how far it moved). */
  private carryRoot(): void {
    const plan = this.plan;
    const r = this.bodyRoot;
    const dx = r[0] - plan.rootPos[0], dy = r[1] - plan.rootPos[1];
    this.rootMotion[0] += dx;
    this.rootMotion[1] += dy;
    const z = this.mode === 'reacting' ? plan.rootPos[2] + clamp(this.groundZ - plan.rootPos[2], -0.05, 0.05) : this.groundZ;
    this.rootMotion[2] += z - plan.rootPos[2];
    // (the heading follows the pelvis while reacting; lying, the plan's own)
    plan.carryRoot([r[0], r[1], z], this.mode === 'reacting' ? plan.rootYaw + clamp(wrapPi(this.bodyYaw - plan.rootYaw), -0.05, 0.05) : plan.rootYaw);
  }

  /** Takes the root displacement the body made (hosts move their character by it). */
  takeRootMotion(): V3 {
    const out: V3 = [this.rootMotion[0], this.rootMotion[1], this.rootMotion[2]];
    this.rootMotion[0] = this.rootMotion[1] = this.rootMotion[2] = 0;
    return out;
  }

  private modes(dt: number, pose: WorldPose): void {
    const k = this.k;
    const plan = this.plan;
    switch (this.mode) {
      case 'animated': {
        if (!this.physical) {
          if (this.forceReact) this.forceReact = false;
          break;
        }
        // knocked off the plan: too far off its pelvis, or moving other than it means to
        const pp = plan.world.p[H.pelvis]!;
        const bp = pose.p[H.pelvis]!;
        const ex = bp[0] - pp[0], ey = bp[1] - pp[1];
        const dev = Math.hypot(ex, ey);
        // (a host that moves its character faster than a body can follow: the body is carried
        // after the plan rather than left behind)
        if (this.upset <= 0 && !this.forceReact && dev > 0.3 * k) this.carryBody(ex, ey, pp[2] - bp[2], dev - 0.3 * k);
        // (lagging behind a hurried start is not a fall; being pushed sideways is)
        const vs = Math.hypot(plan.velocity[0], plan.velocity[1]);
        const lateral = vs > 0.3 ? Math.abs(ex * plan.velocity[1] - ey * plan.velocity[0]) / vs : dev;
        // (against the pelvis target's own motion, smoothed: the plan's velocity lags a start)
        const pv = this.planPelvisVel;
        const dv = Math.hypot(this.comVel[0] - pv[0], this.comVel[1] - pv[1]);
        const standing = plan.stance === 'stand' && plan.stanceProgress >= 1 && !plan.down;
        // (out of balance beyond what the plan means: a lunge into a punch is not a fall)
        const px = pp[0] + this.com[0] - bp[0] + pv[0] / this.w0, py = pp[1] + this.com[1] - bp[1] + pv[1] / this.w0;
        const planned = this.support.empty ? 0 : Math.max(0, this.support.distance(px, py));
        // (only after something happened to the body: lagging a hurried plan is not a fall)
        const knocked = this.upset > 0 && (lateral > 0.1 * k || dev > 0.22 * k || dv > 1.4 || (vs < 0.3 && this.balanceError > 0.06 * k + planned));
        if (standing && (this.forceReact || knocked)) {
          this.forceReact = false;
          this.setMode('reacting');
        } else if (!standing && this.upset > 0 && (dev > 0.25 * k || this.daze > 0.6)) {
          // knocked over from a kneel, a seat, the ground
          this.setMode('falling');
        }
        this.forceReact = false;
        if (this.daze > 0.6 && standing) this.setMode('reacting');
        break;
      }
      case 'reacting': {
        // (already knocked: another knock is dealt with here)
        this.forceReact = false;
        this.reactT += dt;
        const legs = this.legStrength();
        const tilt = this.tilt(pose);
        const maxStep = 1.1 * plan.legLen * clamp(legs, 0.45, 1);
        // (a step in the air still has its landing to catch the body with)
        const fp = plan.feetPlanner.feet;
        const stepping = (!fp[0].planted && fp[0].forced) || (!fp[1].planted && fp[1].forced);
        const reach = maxStep * (stepping ? 1.75 : 1.3) + 0.1 * k;
        // (lost for a moment, not a passing jolt - unless far gone)
        const beyond = tilt > 0.9 || (!this.airborne && this.balanceError > reach);
        this.lostFor = beyond ? this.lostFor + dt : 0;
        const gone = beyond && (this.lostFor > 0.14 || tilt > 1.3 || this.balanceError > 1.8 * reach);
        // (thrown up off the feet: no step catches that)
        const thrown = this.airborne && this.comVel[2] - Math.max(0, this.planPelvisVel[2]) > 1.3;
        const lost = gone || thrown || legs < 0.25 || this.steps > 12 || this.reactT > 6 || this.daze > 0.75 || !this.conscious;
        if (lost) {
          this.lostWhy = thrown ? 'thrown' : tilt > 0.9 ? 'tilt' : this.balanceError > reach ? 'reach' : legs < 0.25 ? 'legs' : this.steps > 12 ? 'steps' : this.reactT > 6 ? 'time' : 'daze';
          this.setMode('falling');
          break;
        }
        const slow = Math.hypot(this.comVel[0], this.comVel[1]) < 0.3;
        const still = !plan.feetPlanner.feet[0].forced && !plan.feetPlanner.feet[1].forced;
        // (in balance, or at its edge and still: leaning on something)
        // (a sway about the edge of the support, as a body collects itself, is settled too)
        const settled = this.balanceError < 0.02 * k || (this.balanceError < 0.06 * k && Math.hypot(this.comVel[0], this.comVel[1]) < 0.2);
        this.balancedFor = settled && slow && still ? this.balancedFor + dt : Math.max(0, this.balancedFor - 2 * dt);
        if (this.balancedFor > 0.3 && this.modeTime > 0.35) this.setMode('animated');
        break;
      }
      case 'falling': {
        const pelvisH = pose.p[H.pelvis]![2] - this.groundZ;
        const slow = Math.hypot(this.comVel[0], this.comVel[1], this.comVel[2]) < 0.7;
        this.lowFor = pelvisH < 0.42 * k && slow ? this.lowFor + dt : 0;
        // (down - the hips and the chest on the ground - is lying, whatever still slides)
        const chestH = pose.p[H.chest]![2] - this.groundZ;
        const down = pelvisH < 0.38 * k && chestH < 0.42 * k;
        if (this.lowFor > 0.3 || (this.modeTime > 0.3 && down) || (this.modeTime > 1.4 && pelvisH < 0.45 * k) || this.modeTime > 3) this.enterLying(pose);
        break;
      }
      case 'lying': {
        this.alignLying(pose, true);
        if (!this.conscious && this.time >= this.downUntil) this.conscious = true;
        // (face down and hurting, a host that wants it away has it crawl off: the struggle of it)
        const crawl = this.writhing && !plan.lyingOnBack && plan.input.stance === 'prone' && this.modeTime > 1.5;
        if (this.alive && this.conscious && this.modeTime > 0.8 && (this.time >= this.downUntil || crawl)) this.setMode('rising');
        break;
      }
      case 'rising': {
        this.writhing = false;
        // gather for a moment, then get up through the stances: briskly unhurt, slowly hurt
        const inj = this.injuries;
        const hurt = clamp(inj.pain + 0.6 * Math.max(inj.legL, inj.legR) + 0.5 * this.daze, 0, 1);
        if (this.modeTime > 0.3 + 0.5 * hurt && plan.down && plan.stance === 'down' && plan.stanceProgress >= 1) plan.getUp(lerp(1.45, 0.7, hurt));
        // (up to the host's stance: standing, or prone to crawl away)
        if (this.modeTime > 0.6 && !plan.down && plan.stance !== 'down' && plan.stance === plan.stanceTarget && plan.stanceProgress >= 1) this.setMode('animated');
        break;
      }
      case 'dying': {
        if (this.modeTime > this.dyingFor + 0.4) this.setMode('dead');
        break;
      }
      case 'dead':
        break;
    }
  }

  /** Moves every body towards the plan by `amount` along the deviation (ex, ey, ez), keeping velocities. */
  private carryBody(ex: number, ey: number, ez: number, amount: number): void {
    const d = Math.hypot(ex, ey) || 1;
    const k = amount / d;
    for (const b of this.body.parts) {
      b.x[0] -= ex * k;
      b.x[1] -= ey * k;
      b.x[2] += ez * k * 0.5;
    }
    this.hadCom = false;
  }

  private enterLying(pose: WorldPose): void {
    this.setMode('lying');
    this.alignLying(pose);
    if (this.downUntil < this.time) this.downUntil = this.time + 1.2 + this.rng.next() * 1.4 + 2 * this.injuries.pain;
  }

  /**
   * The plan lies as the body lies (on the back or face down, the head's way); a body that
   * rolled over or slid on after lying down is followed.
   */
  private alignLying(pose: WorldPose, onlyIfChanged = false): void {
    const pq = pose.q[H.pelvis]!;
    const fwd = qrotate(pq, [0, 1, 0]);
    if (onlyIfChanged) {
      const flipped = this.plan.lyingOnBack ? fwd[2] < -0.5 : fwd[2] > 0.5;
      const pp = pose.p[H.pelvis]!, r = this.plan.rootPos;
      if (!flipped && Math.hypot(pp[0] - r[0], pp[1] - r[1]) < 0.35 * this.k) return;
    }
    const back = fwd[2] > 0;
    // the plan's lying pose: on the back the feet point along +y from the head, face down the head does
    const head = vsub(pose.p[H.chest]!, pose.p[H.pelvis]!);
    const yaw = back ? Math.atan2(-head[1], -head[0]) : Math.atan2(head[1], head[0]);
    const pp = pose.p[H.pelvis]!;
    const before: V3 = [...this.plan.rootPos];
    this.plan.lie([pp[0], pp[1], this.groundZ], yaw, back);
    // the host is told where the body ended up
    const r = this.plan.rootPos;
    this.rootMotion[0] += r[0] - before[0];
    this.rootMotion[1] += r[1] - before[1];
    this.rootMotion[2] += r[2] - before[2];
  }

  private legStrength(): number {
    const inj = this.injuries;
    const legs = 1 - 0.5 * Math.max(inj.legL, inj.legR) - 0.2 * Math.min(inj.legL, inj.legR);
    const stun = Math.max(this.stun[B.thighL]!, this.stun[B.shinL]!, this.stun[B.thighR]!, this.stun[B.shinR]!);
    return clamp(legs * (1 - 0.6 * stun) * (1 - this.daze), 0, 1);
  }

  /** How far the trunk leans from upright (rad). */
  private tilt(pose: WorldPose): number {
    const up = qrotate(pose.q[H.chest]!, [0, 0, 1]);
    return Math.acos(clamp(up[2], -1, 1));
  }

  // ---- balance ---------------------------------------------------------------------------------

  /**
   * Stepping to stay up: where the capture point runs out of the support, a foot goes there
   * (the one it suits, on its own side of the other), quicker the worse it is; a step already
   * swinging is re-aimed where the capture point will be when it lands. Arms go out.
   */
  private balance(dt: number): void {
    const k = this.k;
    const plan = this.plan;
    const fp = plan.feetPlanner;
    const feet = fp.feet;
    const err = this.balanceError;
    const h = Math.max(0.45 * k, this.com[2] - this.groundZ);
    const w0 = Math.sqrt(G / h);
    const legs = this.legStrength();
    const maxStep = 1.1 * plan.legLen * clamp(legs, 0.45, 1);
    // (a stride the gait had begun becomes the balance's step)
    const swinging: number = !feet[0].planted && !feet[0].held ? 0 : !feet[1].planted && !feet[1].held ? 1 : -1;
    // (just outside, a weight shift does it: no step)
    const margin = 0.05 * k;
    const right: V3 = [Math.sin(plan.rootYaw), -Math.cos(plan.rootYaw), 0];
    const plannedStep = (i: number, T: number): V3 => {
      const stance = feet[1 - i]!;
      // where the capture point will be when the foot lands (it runs away from the stance
      // foot's centre of pressure)
      const px = stance.planted ? stance.pos[0] : this.com[0], py = stance.planted ? stance.pos[1] : this.com[1];
      const grow = Math.exp(w0 * T);
      let tx = px + (this.capture[0] - px) * grow;
      let ty = py + (this.capture[1] - py) * grow;
      // a little past it, to catch the body rather than just stop
      const dx = tx - this.com[0], dy = ty - this.com[1];
      const dl = Math.hypot(dx, dy) || 1;
      tx += (dx / dl) * 0.06 * k;
      ty += (dy / dl) * 0.06 * k;
      // on its own side of the other foot (a cross-over step at most a little)
      if (stance.planted) {
        const lat = (tx - stance.pos[0]) * right[0] + (ty - stance.pos[1]) * right[1];
        const want = feet[i]!.side * 0.13 * k;
        const need = feet[i]!.side > 0 ? Math.max(0, want - lat) : Math.min(0, want - lat);
        tx += right[0] * need * 0.8;
        ty += right[1] * need * 0.8;
        // within reach of the stance foot
        const sx = tx - stance.pos[0], sy = ty - stance.pos[1];
        const sl = Math.hypot(sx, sy);
        if (sl > maxStep) {
          tx = stance.pos[0] + (sx / sl) * maxStep;
          ty = stance.pos[1] + (sy / sl) * maxStep;
        }
      }
      return [tx, ty, this.groundZ];
    };
    if (swinging >= 0) {
      // re-aim the swing (and hurry it if things got worse)
      const f = feet[swinging]!;
      const urgency = clamp(err / (0.3 * k), 0, 1);
      const left = Math.min((1 - f.swing) / Math.max(1e-3, f.swingRate), (1 - f.swing) * lerp(0.3, 0.14, urgency) / Math.max(0.6, legs));
      fp.step(swinging, plannedStep(swinging, left), left);
    } else if (err > margin && this.stepCooldown <= 0 && this.time >= this.snagUntil[0] && this.time >= this.snagUntil[1]) {
      // (a snagged foot: the other one has the weight and cannot go; the snag frees first)
      // which foot: the one behind the way the body goes (the other has the weight), unless
      // that would cross the legs
      const dir = vnorm([this.capture[0] - this.com[0], this.capture[1] - this.com[1], 0], [0, 0, 0], [Math.cos(plan.rootYaw), Math.sin(plan.rootYaw), 0]);
      let best = -1;
      let score = -Infinity;
      for (let i = 0; i < 2; i++) {
        const f = feet[i]!;
        if (!f.planted || f.held || this.time < this.snagUntil[i]!) continue;
        const behind = -((f.pos[0] - this.com[0]) * dir[0] + (f.pos[1] - this.com[1]) * dir[1]);
        const sideFit = f.side * vdot(dir, right);
        const s = behind + 0.12 * sideFit - (f.since < 0.12 ? 1 : 0);
        if (s > score) {
          score = s;
          best = i;
        }
      }
      if (best >= 0) {
        const urgency = clamp(err / (0.3 * k), 0, 1);
        const T = lerp(0.3, 0.15, urgency) / Math.max(0.6, legs);
        fp.step(best, plannedStep(best, T), T, plan.rootYaw - feet[best]!.side * plan.style.toeOut);
        this.steps++;
        this.stepCooldown = 0.05;
      }
    }
    // arms out for balance (a windmill when it is bad)
    const flail = smoothstep(0.04 * k, 0.4 * k, err + (swinging >= 0 ? 0.08 * k : 0));
    if (flail > 0.02) this.armsOut(flail, dt);
  }

  private armsOut(w: number, _dt: number): void {
    const plan = this.plan;
    const k = this.k;
    const wp = plan.world;
    const chestQ = wp.q[H.chest]!;
    for (let i = 0; i < 2; i++) {
      if (this.plan.control.arms[i]) continue;
      // a long gun stays in the right hand
      if (i === 1 && plan.weapon && plan.weapon.kind !== 'knife' && plan.weapon.kind !== 'pistol') continue;
      const side = i === 0 ? -1 : 1;
      const sh = wp.p[i === 0 ? H.upperarmL : H.upperarmR]!;
      // out to the side, a little forward, circling (no higher than the shoulder)
      const ph = this.time * 7 + i * 1.7;
      const circle: V3 = [0, Math.cos(ph) * 0.1 * k * w, Math.sin(ph) * 0.08 * k * w];
      const off = qrotate(chestQ, [side * 0.44 * k, 0.12 * k + circle[1], -0.08 * k + circle[2]]);
      this.plan.control.arms[i] = { target: vadd(sh, off), rot: qmul(chestQ, qeuler(0, side * 1.4, 0)), pole: qrotate(chestQ, [side * 0.3, -0.5, -0.8]), weight: w * 0.8 };
    }
  }

  // ---- catching a fall -----------------------------------------------------------------------------

  /**
   * Hands out where the body will land (the ground, or a wall in the way), the head kept up.
   * A hand that lands stays where it landed and gives, so the arms fold as the body comes down
   * on them; once the body is down the arms let go.
   */
  private catchFall(dt: number, pose: WorldPose): void {
    const k = this.k;
    const ctl = this.plan.control;
    const tone = this.mode === 'dying' ? clamp(1 - this.modeTime / (this.dyingFor + 0.2), 0, 1) * (this.dyingHead ? 0 : 0.8) : 1;
    const low = pose.p[H.pelvis]![2] - this.groundZ < 0.38 * k && pose.p[H.chest]![2] - this.groundZ < 0.45 * k;
    if (tone < 0.05 || low) {
      this.landed[0] = this.landed[1] = null;
      return;
    }
    const v = this.comVel;
    let dir: V3 = [v[0], v[1], 0];
    if (vlen(dir) < 0.3) {
      // slow: the way the trunk leans
      const up = qrotate(pose.q[H.chest]!, [0, 0, 1]);
      dir = [up[0], up[1], 0];
    }
    dir = vnorm(dir, [0, 0, 0], [Math.cos(this.bodyYaw), Math.sin(this.bodyYaw), 0]);
    const fwd = qrotate(pose.q[H.pelvis]!, [0, 1, 0]);
    const back = dir[0] * fwd[0] + dir[1] * fwd[1] < -0.3;
    const wall = this.surroundings.wallToward(dir, 0.7, 1.0 * k);
    const chest = pose.p[H.chest]!;
    for (let i = 0; i < 2; i++) {
      if (ctl.arms[i] && ctl.arms[i]!.weight > 0.5) continue;
      const side = i === 0 ? -1 : 1;
      const sh = pose.p[i === 0 ? H.upperarmL : H.upperarmR]!;
      const across: V3 = [dir[1] * side, -dir[0] * side, 0];
      const hand = this.body.parts[i === 0 ? B.handL : B.handR]!;
      let target: V3;
      const landed = this.landed[i];
      if (landed) target = landed;
      else if (wall) {
        target = vadd(wall.point, vadd(vscale(wall.normal, 0.04 * k), vscale(across, 0.18 * k)));
        target[2] = chest[2] + 0.05 * k;
      } else {
        target = vadd(sh, vadd(vscale(dir, back ? 0.3 * k : 0.5 * k), vscale(across, 0.12 * k)));
        target[2] = this.groundZ + 0.06 * k;
      }
      // a hand that touches down stays there and gives
      if (!landed && hand.contact && this.physical) {
        const at = hand.point(this.body.hands[i]!.local);
        this.landed[i] = at;
        target = at;
      }
      this.catchT[i] = this.landed[i] ? Math.min(1, this.catchT[i]! + dt * 2.5) : Math.max(0, this.catchT[i]! - dt);
      const palmRot = palmFrame([0, 0, -1], dir);
      ctl.arms[i] = { target, rot: back ? null : palmRot, pole: vadd(vscale(dir, back ? 0.5 : -0.6), [0, 0, -0.6]), weight: tone * (1 - 0.6 * this.catchT[i]!) };
      // (guided out while in the air; landed, only the arm's own muscles hold)
      this.grip[i] = this.landed[i] ? 0 : 0.2 * tone;
    }
    // the head: up off the ground falling forward, chin tucked falling back
    if (back) ctl.neck[0] += 0.45 * tone;
    else ctl.neck[0] -= 0.35 * tone;
    ctl.crouch += 0.5 * tone;
  }

  /** Where each hand landed breaking a fall (null: still in the air). */
  private readonly landed: [V3 | null, V3 | null] = [null, null];

  // ---- holding on to a wall ------------------------------------------------------------------------

  private bracing(dt: number, pose: WorldPose): void {
    const k = this.k;
    const plan = this.plan;
    const ctl = plan.control;
    const inj = this.injuries;
    // why a hand would go to a wall now (and a reason once found holds a while: a hand does
    // not come and go with every wobble)
    let why: Brace['why'] | null = null;
    let dir: V3 | null = null;
    const prev = this.brace;
    if (this.mode === 'reacting' && (this.balanceError > 0.03 * k || (prev?.why === 'balance' && (prev.age < 0.9 || this.balancedFor < 0.25)))) {
      why = 'balance';
      dir = vnorm([this.capture[0] - this.com[0], this.capture[1] - this.com[1], 0], [0, 0, 0], [0, 0, 0]);
    } else if (this.mode === 'dying' && this.modeTime < this.dyingFor * 0.9 && !this.dyingHead) {
      why = 'slump';
      dir = vnorm([this.comVel[0], this.comVel[1], 0], [0, 0, 0], [Math.cos(this.bodyYaw), Math.sin(this.bodyYaw), 0]);
    } else if (this.mode === 'animated' && this.alive && (inj.pain > 0.45 || Math.max(inj.legL, inj.legR) > 0.4) && Math.hypot(plan.velocity[0], plan.velocity[1]) < 0.25 && plan.stance === 'stand' && !plan.busy) {
      why = 'lean';
    }
    if (!why) {
      this.brace = null;
      return;
    }
    // find (or keep) the wall
    let b = this.brace;
    if (b && b.why !== why) b = null;
    if (!b) {
      const s = dir && vlen(dir) > 0.1 ? this.surroundings.wallToward(dir, 1.1, 0.95 * k) : this.surroundings.nearest(0.75 * k);
      if (!s) {
        this.brace = null;
        return;
      }
      const wp = pose;
      const toWall = vnorm([s.point[0] - wp.p[H.pelvis]![0], s.point[1] - wp.p[H.pelvis]![1], 0]);
      const right: V3 = [Math.sin(this.bodyYaw), -Math.cos(this.bodyYaw), 0];
      let hand = vdot(toWall, right) > 0 ? 1 : 0;
      // (a right hand on a long gun: the left one goes)
      if (hand === 1 && plan.weapon && plan.weapon.kind !== 'knife' && plan.weapon.kind !== 'pistol') hand = 0;
      const height = why === 'lean' ? 1.25 : why === 'slump' ? 1.05 : 1.2;
      const t: V3 = [s.point[0] + s.normal[0] * 0.035 * k, s.point[1] + s.normal[1] * 0.035 * k, this.groundZ + height * k];
      // (along the wall, towards the hand's side)
      const along: V3 = [-s.normal[1], s.normal[0], 0];
      const sgn = (vdot(along, right) >= 0 ? 1 : -1) * (hand === 1 ? 1 : -1);
      t[0] += along[0] * sgn * 0.12 * k;
      t[1] += along[1] * sgn * 0.12 * k;
      b = { surface: s, hand, target: t, normal: [...s.normal], why, holding: false, age: 0 };
    }
    b.age += dt;
    this.brace = b;
    const handBody = this.body.parts[b.hand === 0 ? B.handL : B.handR]!;
    const palm = this.physical ? handBody.point(this.body.hands[b.hand]!.local) : this.plan.palm(b.hand === 0 ? 'L' : 'R');
    if (!b.holding) {
      // it holds on where it gets there, or wherever the palm meets the wall first
      const n0 = handBody.contactNormal;
      if (vlen(vsub(palm, b.target)) < 0.12 * k) b.holding = true;
      else if (this.physical && handBody.contact && n0[0] * b.normal[0] + n0[1] * b.normal[1] > 0.6) {
        b.holding = true;
        b.target = vadd(palm, vscale(b.normal, 0.01 * k));
      }
    }
    // a palm flat on the wall, fingers up
    const n = b.normal;
    const side: V3 = vnorm(vcross([0, 0, 1], n));
    const rot = palmFrame(vscale(n, -1), [0, 0, 1]);
    const w = smoothstep(0, 0.25, b.age);
    ctl.arms[b.hand] = { target: b.target, rot, pole: [-n[0] * 0.3 + (b.hand === 0 ? 0.3 : -0.3) * side[0], -n[1] * 0.3, -1], weight: w };
    this.grip[b.hand] = b.holding ? 1 : 0.4;
    if (why === 'lean' || why === 'slump') {
      // the weight goes towards the wall: the trunk leans onto the arm
      const toWall = vnorm([-n[0], -n[1], 0]);
      const local = qrotate(qconj(qz(plan.rootYaw - Math.PI / 2)), toWall);
      ctl.spine[1] += -local[0] * 0.12 * w;
      ctl.chest[1] += -local[0] * 0.1 * w;
      ctl.look = why === 'lean' ? null : ctl.look;
    }
  }

  // ---- flinching -----------------------------------------------------------------------------------

  private flinch(dt: number, pose: WorldPose): void {
    if (this.threats.length === 0) {
      this.tension = Math.max(0, this.tension - dt * 2);
      return;
    }
    const k = this.k;
    const plan = this.plan;
    const ctl = plan.control;
    // the strongest current threat
    let best: Threat | null = null;
    let bw = 0;
    for (const t of this.threats) {
      const env = t.age < 0 ? 0 : t.age < 0.07 ? t.age / 0.07 : t.age < 0.07 + t.hold ? 1 : Math.exp(-(t.age - 0.07 - t.hold) / 0.3);
      const w = env * t.amount;
      if (w > bw) {
        bw = w;
        best = t;
      }
    }
    // once the flinch has passed: a look back at what it was
    for (const t of this.threats) {
      const after = t.age - 0.07 - t.hold - 0.2;
      if (after <= 0 || after > 1.1 || t.amount < 0.2) continue;
      if (!ctl.look || ctl.lookWeight < 0.3) {
        ctl.look = t.point;
        ctl.lookWeight = 0.75 * smoothstep(0, 0.25, after) * (1 - smoothstep(0.7, 1.1, after));
      }
      break;
    }
    if (!best || bw < 0.02) return;
    const w = clamp(bw, 0, 1.2);
    this.tension = Math.max(this.tension, w);
    const head = pose.p[H.head]!;
    const away = vnorm(vsub(head, best.point), [0, 0, 0], [0, 0, 1]);
    // in the body's frame: where the danger is
    const inv = qconj(qz(plan.rootYaw - Math.PI / 2));
    const lm = qrotate(inv, vscale(away, -1));
    const sideOf = lm[0] >= 0 ? 1 : -1;
    // duck and turn away
    ctl.look = vadd(head, vadd(vscale(away, 2), [0, 0, -1.2]));
    ctl.lookWeight = Math.max(ctl.lookWeight, 0.8 * clamp(w, 0, 1));
    ctl.neck[0] += 0.35 * w;
    ctl.head[1] += sideOf * 0.2 * w;
    // (the face turns from it at once, before the eyes have found anything to look at)
    ctl.neck[2] += sideOf * 0.35 * w;
    ctl.head[2] += sideOf * 0.45 * w;
    ctl.shrug = Math.max(ctl.shrug, clamp(w, 0, 1));
    ctl.spine[0] -= 0.14 * w;
    ctl.chest[0] -= 0.1 * w;
    ctl.crouch += 0.3 * w;
    // a hand up between the face and the danger (a two-handed gun: the body hunches over it)
    const longGun = plan.weapon && plan.weapon.kind !== 'knife' && plan.weapon.kind !== 'pistol';
    if (!longGun || w > 0.9) {
      const hand = longGun ? 0 : sideOf > 0 ? 1 : 0;
      if (!ctl.arms[hand] || ctl.arms[hand]!.weight < 0.5) {
        const shield = vadd(head, vadd(vscale(away, -0.16 * k), [0, 0, 0.02 * k]));
        const rot = palmFrame(vscale(away, -1), [0, 0, 1]);
        ctl.arms[hand] = { target: shield, rot, pole: [0, 0, -1], weight: clamp(w * 1.1, 0, 1) };
      }
      if (w > 0.7 && !longGun) {
        const other = 1 - hand;
        if (!ctl.arms[other]) {
          const shield = vadd(head, vadd(vscale(away, -0.14 * k), [0, 0, -0.06 * k]));
          ctl.arms[other] = { target: shield, rot: null, pole: [0, 0, -1], weight: clamp((w - 0.7) * 3, 0, 1) };
        }
      }
    }
  }

  // ---- holding a wound ------------------------------------------------------------------------------

  private holdWound(_dt: number, pose: WorldPose): void {
    const inj = this.injuries.toHold();
    const ctl = this.plan.control;
    if (!inj || (!this.conscious && this.mode !== 'dying')) return;
    const k = this.k;
    const tone = this.mode === 'dying' ? clamp(1 - this.modeTime / Math.max(0.3, this.dyingFor), 0, 1) : 1;
    if (tone < 0.1 || this.dyingHead) return;
    // the free hand, or the other one for an arm
    const plan = this.plan;
    const longGun = plan.weapon && plan.weapon.kind !== 'knife' && plan.weapon.kind !== 'pistol';
    const bone = BODY_BONE[inj.part]!;
    const q = pose.q[bone]!;
    const wound = vadd(pose.p[bone]!, qrotate(q, vadd(this.body.comLocal[inj.part]!, inj.local)));
    const n = qrotate(q, inj.normal);
    let hand: number;
    if (inj.zone === 'armL') hand = 1;
    else if (inj.zone === 'armR') hand = 0;
    else {
      const right: V3 = [Math.sin(this.bodyYaw), -Math.cos(this.bodyYaw), 0];
      hand = vdot(vsub(wound, pose.p[H.pelvis]!), right) > 0 ? 1 : 0;
      if (longGun && hand === 1) hand = 0;
    }
    if (hand === 1 && longGun && inj.zone === 'armL') {
      // the gun hand lets go of nothing: the wounded arm just hangs
      return;
    }
    if (ctl.arms[hand] && ctl.arms[hand]!.weight > 0.6) return;
    // a moment to react, then the hand presses on it
    const w = smoothstep(0.12, 0.4, inj.age) * (1 - smoothstep(inj.holdUntil - 0.6, inj.holdUntil, inj.age)) * tone;
    if (w < 0.02) return;
    const target = vadd(wound, vscale(n, 0.03 * k));
    const rot = palmFrame(vscale(n, -1), [0, 0, 1]);
    ctl.arms[hand] = { target, rot, pole: null, weight: w };
    this.grip[hand] = 0.35 * w;
    // reaching a leg: the body bends to it
    if (inj.zone === 'legL' || inj.zone === 'legR') {
      const reach = clamp((pose.p[H.chest]![2] - target[2] - 0.5 * k) / (0.5 * k), 0, 1);
      ctl.fold += 0.5 * reach * w;
      ctl.crouch += 0.3 * reach * w;
    }
    // a glance at it at first
    if (inj.age < 1.2 && !ctl.look) {
      ctl.look = wound;
      ctl.lookWeight = 0.7 * w * (1 - smoothstep(0.6, 1.2, inj.age));
    }
  }

  // ---- the frame, after the plan: the muscles and the body -------------------------------------------

  /** Muscle targets, tone and assists from the plan, then the physics step; writes `out`. */
  drive(dt: number, out: WorldPose): void {
    const body = this.body;
    const plan = this.plan;
    const k = this.k;
    const m = body.totalMass;
    const target = plan.world;
    const prevT = plan.prevWorld;
    body.track(target, plan.pose, prevT, dt);
    const idt = dt > 0 ? 1 / dt : 0;

    // ---- tone ----
    const mode = this.mode;
    const inj = this.injuries;
    let base = 1;
    let legs = 1, arms = 1, neck = 1, trunk = 1;
    switch (mode) {
      case 'animated':
        // at ease the arms and the head are carried loosely (they swing and settle with the
        // body's motion); what an action moves is firmer (see limbT)
        arms = ARMS_AT_EASE;
        neck = 0.8;
        break;
      case 'reacting':
        arms = 0.8;
        break;
      case 'falling':
        // (braced for the ground, not fighting it)
        legs = 0.35;
        trunk = 0.55;
        neck = 0.8;
        arms = 0.9;
        break;
      case 'lying':
        base = !this.conscious ? 0.06 : this.writhing ? 0.75 : 0.22;
        break;
      case 'rising': {
        base = lerp(0.25, 1, smoothstep(0.1, 0.9, this.modeTime));
        break;
      }
      case 'dying': {
        // the knees go first, the arms drop, the trunk and the neck keep their shape longest
        const u = clamp(this.modeTime / this.dyingFor, 0, 1);
        base = this.dyingHead ? 0.02 : 0.9 * Math.pow(1 - u, 0.8) + 0.02;
        legs = Math.pow(1 - u, 1.2) * 0.75;
        arms = Math.pow(1 - u, 0.7) * 0.8;
        break;
      }
      case 'dead':
        base = 0;
        break;
    }
    const dz = (1 - 0.85 * this.daze) * (1 - 0.6 * this.shock);
    const tense = 1 + 0.35 * this.tension;
    const regionT: Record<Region, number> = {
      trunk: base * trunk * dz * tense * (1 - 0.3 * inj.trunk),
      neck: base * neck * dz * tense * (1 - 0.3 * inj.head),
      armL: base * arms * dz * (1 - 0.65 * inj.armL) * (1 + 0.8 * this.tension),
      armR: base * arms * dz * (1 - 0.65 * inj.armR) * (1 + 0.8 * this.tension),
      legL: base * legs * dz * (1 - 0.45 * inj.legL),
      legR: base * legs * dz * (1 - 0.45 * inj.legR),
    };
    for (const r of REGIONS) this.regionTone[r] = regionT[r];
    // a limb that strikes is thrown hard (tensed), one an action moves is firmer
    const limbT = (r: Region): number => {
      const i: number = r === 'armL' ? 0 : r === 'armR' ? 1 : r === 'legL' ? 2 : r === 'legR' ? 3 : -1;
      if (i < 0) return 1;
      return 1 + plan.effort[i]! * (plan.striking[i] ? 2.5 : 0.6);
    };
    // (a limb that strikes passes through the body it strikes: the host deals the blow)
    for (let i = 0; i < 4; i++) {
      const g = plan.striking[i]! && this.alive;
      const [a, b] = i === 0 ? [B.forearmL, B.handL] : i === 1 ? [B.forearmR, B.handR] : i === 2 ? [B.shinL, B.footL] : [B.shinR, B.footR];
      body.parts[a]!.ghost = g;
      body.parts[b]!.ghost = g;
    }
    const feet = plan.feetPlanner.feet;
    for (let i = 1; i < BODY_COUNT; i++) {
      const t = regionT[REGION[i]!] * limbT(REGION[i]!) * (1 - 0.85 * this.stun[i]!);
      body.tone[i] = t;
      // standing legs are held by the ground, hanging limbs by the muscles
      const leg = i >= B.thighL;
      const planted = leg && feet[i < B.thighR ? 0 : 1]!.planted && (mode === 'animated' || mode === 'reacting' || mode === 'rising');
      body.holdWeight[i] = planted || mode === 'lying' ? 0 : 1;
    }
    body.applyTone();
    body.compensateGravity();

    // ---- assists: how much the legs hold the body up and where ----
    const pel = target.p[H.pelvis]!;
    const velT = plan.velocity;
    const sup = body.support, steer = body.steer, up = body.upright;
    const standing = plan.stance === 'stand' && plan.stanceProgress >= 1 && !plan.down;
    const legsS = this.legStrength();
    sup.enabled = steer.enabled = up.enabled = false;
    body.chestTurn.enabled = false;
    sup.target[2] = pel[2];
    sup.targetVel[2] = (pel[2] - prevT.p[H.pelvis]![2]) * idt;
    steer.target[0] = pel[0];
    steer.target[1] = pel[1];
    // (the pelvis target's own velocity: the plan's smoothed velocity lags a hurried start)
    const pelPrev = prevT.p[H.pelvis]!;
    steer.targetVel[0] = (pel[0] - pelPrev[0]) * idt;
    steer.targetVel[1] = (pel[1] - pelPrev[1]) * idt;
    void velT;
    const pq = target.q[H.pelvis]!;
    for (let c = 0; c < 4; c++) up.target[c] = pq[c]!;
    up.tiltOnly = false;
    const rise = mode === 'rising' ? smoothstep(0.15, 1.0, this.modeTime) : 1;
    if (mode === 'animated' || mode === 'rising') {
      sup.enabled = true;
      sup.stiffness = m * 900 * rise;
      sup.damping = m * 55 * rise;
      sup.maxForce = m * G * 2.2 * rise;
      steer.enabled = true;
      steer.stiffness = m * 700 * rise;
      steer.damping = m * 50 * rise;
      // (legs push harder on the move: starts, stops, turns)
      const sp = Math.hypot(velT[0], velT[1]);
      steer.maxForce = m * G * (standing ? 0.8 + 0.7 * Math.min(1, sp / 3) : 1.2) * rise;
      up.enabled = true;
      up.stiffness = 5000 * rise;
      up.damping = 450 * rise;
      up.maxTorque = (standing ? 380 : 900) * rise;
    } else if (mode === 'reacting') {
      // the legs hold the body up (as strong as they are) but no longer steer it
      sup.enabled = true;
      sup.stiffness = m * 700;
      sup.damping = m * 45;
      sup.maxForce = m * G * lerp(0.7, 1.9, legsS);
      up.enabled = true;
      up.tiltOnly = true;
      up.stiffness = 1500 * legsS;
      up.damping = 220;
      up.maxTorque = 200 * legsS;
      this.centreOfPressure(legsS);
    } else if (mode === 'dying') {
      const u = clamp(this.modeTime / this.dyingFor, 0, 1);
      // (the legs hold at first, then give all at once)
      const hold = this.dyingHead ? 0 : 1 - u * u;
      if (hold > 0.02) {
        // the knees buckle: the legs hold less and less, lower and lower
        sup.enabled = true;
        sup.target[2] = this.dieZ - 0.35 * k * u;
        sup.stiffness = m * 400 * hold;
        sup.damping = m * 30;
        sup.maxForce = m * G * 1.05 * hold;
        // the ground's push through failing legs: the body topples as one over its feet
        // (it does not fold over hips held up in the air)
        if (!this.support.empty) this.centreOfPressure(0.5 * hold);
      }
      // a seat holds a dying body where it sits
      if (plan.stance === 'sit') {
        sup.enabled = true;
        sup.target[2] = pel[2];
        sup.stiffness = m * 600;
        sup.damping = m * 40;
        sup.maxForce = m * G * 1.5;
        steer.enabled = true;
        steer.stiffness = m * 200;
        steer.damping = m * 30;
        steer.maxForce = m * G * 0.5;
      }
    }

    // ---- feet: planted ones pinned, swinging ones led (an obstacle can stop them) ----
    for (let i = 0; i < 2; i++) {
      const f = feet[i]!;
      const pin = body.feet[i]!, turn = body.feetTurn[i]!;
      const footBone = i === 0 ? H.footL : H.footR;
      const onFeet = mode === 'animated' || mode === 'reacting' || (mode === 'rising' && standing) || (mode === 'dying' && this.modeTime < this.dyingFor * 0.55 && !this.dyingHead);
      pin.enabled = turn.enabled = false;
      if (!onFeet || !(plan.stance === 'stand' || plan.stanceTarget === 'stand')) continue;
      const a = target.p[footBone]!;
      pin.target[0] = a[0];
      pin.target[1] = a[1];
      pin.target[2] = a[2];
      const fq = target.q[footBone]!;
      for (let c = 0; c < 4; c++) turn.target[c] = fq[c]!;
      pin.enabled = turn.enabled = true;
      const fm = body.parts[i === 0 ? B.footL : B.footR]!.mass;
      if (f.planted) {
        // (stiff, not rigid: a foot that lands a little off its spot is drawn in, not snapped)
        pin.stiffness = fm * 16000;
        pin.maxForce = m * G * 2.5;
        pin.damping = fm * 250;
        pin.targetVel[0] = pin.targetVel[1] = pin.targetVel[2] = 0;
        turn.stiffness = 2500;
        turn.maxTorque = 400;
        turn.damping = 60;
      } else if (f.held) {
        // a kick: the foot is driven to where the action puts it
        const e = plan.effort[2 + i]!;
        pin.stiffness = fm * 3000 * e;
        pin.maxForce = 60 + 400 * e * (plan.striking[2 + i] ? 1 : 0.4);
        pin.damping = fm * 60;
        const pa = prevT.p[footBone]!;
        pin.targetVel[0] = (a[0] - pa[0]) * idt;
        pin.targetVel[1] = (a[1] - pa[1]) * idt;
        pin.targetVel[2] = (a[2] - pa[2]) * idt;
        turn.stiffness = 200;
        turn.maxTorque = 60;
        turn.damping = 6;
      } else {
        // (the leg's muscles swing it; this only guides the foot to its spot, gently, so the
        // whole body is not dragged by it and an obstacle can stop it; up a stair the knee is
        // lifted with a will)
        const up = clamp(Math.max(f.target[2] - f.lift[2], f.clear) / (0.25 * k), 0, 1);
        pin.stiffness = fm * (500 + 1500 * up);
        pin.maxForce = (mode === 'reacting' ? 90 : 45) + 250 * up;
        pin.damping = fm * 40;
        const pa = prevT.p[footBone]!;
        pin.targetVel[0] = (a[0] - pa[0]) * idt;
        pin.targetVel[1] = (a[1] - pa[1]) * idt;
        pin.targetVel[2] = (a[2] - pa[2]) * idt;
        turn.stiffness = 120;
        turn.maxTorque = 40;
        turn.damping = 4;
      }
    }

    // ---- hands: on the weapon, or where a behaviour sends them ----
    for (let i = 0; i < 2; i++) {
      const att = body.hands[i]!, turn = body.handsTurn[i]!;
      att.enabled = turn.enabled = false;
      const handBone = i === 0 ? H.handL : H.handR;
      const tone = regionT[i === 0 ? 'armL' : 'armR'];
      const task = plan.control.arms[i];
      let grip = this.grip[i]!;
      // the weapon: the gun hand keeps its aim, the support hand its hold
      const onWeapon = plan.weapon && plan.weapon.kind !== 'knife' && !plan.weaponInHand && (i === 1 || !task || task.weight < 0.35);
      if (onWeapon && tone > 0.3 && (mode === 'animated' || mode === 'reacting' || mode === 'rising')) grip = Math.max(grip, i === 1 ? 0.8 : 0.6);
      // (an action's hand: a fist thrown at a jaw, a hand on a magazine)
      if (mode === 'animated' && plan.effort[i]! > 0.05) grip = Math.max(grip, plan.effort[i]! * (plan.striking[i] ? 1 : 0.5));
      if (grip <= 0.01 || tone < 0.05) continue;
      const w = target.p[handBone]!;
      att.target[0] = w[0];
      att.target[1] = w[1];
      att.target[2] = w[2];
      const wp = prevT.p[handBone]!;
      att.targetVel[0] = (w[0] - wp[0]) * idt;
      att.targetVel[1] = (w[1] - wp[1]) * idt;
      att.targetVel[2] = (w[2] - wp[2]) * idt;
      const hq = target.q[handBone]!;
      for (let c = 0; c < 4; c++) turn.target[c] = hq[c]!;
      const hm = body.parts[i === 0 ? B.handL : B.handR]!.mass;
      att.enabled = true;
      att.stiffness = hm * 2500 * grip * Math.min(1, tone);
      att.damping = hm * 60 * Math.min(1, tone);
      att.maxForce = 30 + 420 * grip * Math.min(1, tone);
      turn.enabled = true;
      turn.stiffness = 30 * grip;
      turn.maxTorque = 12 * grip;
      turn.damping = 1.5 * grip;
    }

    // writhing on the back: the pain rolls the body from side to side about its length
    if (mode === 'lying' && this.writhing && this.alive && this.conscious) {
      const t = this.time + this.writheSeed;
      const spasm = Math.pow(0.5 + 0.5 * Math.sin(t * 0.9) * Math.sin(t * 0.31 + 1), 1.5);
      const pel = body.parts[B.pelvis]!, ch = body.parts[B.chest]!;
      const ax = vnorm(vsub(ch.x, pel.x));
      const roll = 55 * spasm * Math.sin(t * 0.75 + 0.6);
      for (const b of [pel, ch]) {
        b.torque[0] += ax[0] * roll;
        b.torque[1] += ax[1] * roll;
        b.torque[2] += ax[2] * roll;
      }
    }

    // ---- step ----
    body.system.step(dt);
    body.writePose(out);
    // bumped into (by) someone, something: the balance has to answer it (a brush in passing
    // is nothing; being barged moves the body)
    let bump = 0;
    for (const p of body.parts) bump += p.bumped;
    const bdv = bump / body.totalMass;
    if (bdv > 0.12 && this.alive && (mode === 'animated' || mode === 'reacting')) {
      this.upset = Math.max(this.upset, Math.min(0.5, 2.5 * bdv));
      if (bdv > 0.3) this.forceReact = true;
    }
    this.detectTrips(out, dt);
    // the dead settle (a body at rest does not keep rocking on its contacts) and sleep
    if (mode === 'dead') {
      const slow = body.system.lastSpeed < 0.25 || this.modeTime > 2.5;
      body.system.linearDrag = slow ? 0.05 : 0.98;
      body.system.angularDrag = slow ? 0.02 : 0.9;
      // (once down, the limbs lie where they lie: keeping them out of the trunk would only
      // squeeze a trapped arm out from under the body)
      body.system.pairsEnabled = !(this.modeTime > 1.2 && body.system.lastSpeed < 0.6);
      if (this.modeTime > 1) body.system.trySleep(0.5);
    }
  }

  /**
   * The ground's push through the feet (reacting): the centre of pressure is kept under the
   * feet; the force m w0^2 (c - p) brakes the body when the capture point is inside the support
   * and lets it topple when it is not.
   */
  private centreOfPressure(legs: number): void {
    const k = this.k;
    const m = this.body.totalMass;
    const h = Math.max(0.45 * k, this.com[2] - this.groundZ);
    const w2 = G / h;
    const w0 = Math.sqrt(w2);
    if (this.support.empty) return;
    const cen: [number, number] = [0, 0];
    this.support.centroid(cen);
    // aim the centre of pressure so the capture point comes back to the middle of the feet
    const gain = 2.2 * legs;
    const p: [number, number] = [this.capture[0] + ((this.capture[0] - cen[0]) * gain) / w0, this.capture[1] + ((this.capture[1] - cen[1]) * gain) / w0];
    const want: [number, number] = [p[0], p[1]];
    const out = this.support.distance(p[0], p[1], p);
    // the hip strategy (a swing of the trunk, the arms) puts the effective centre of pressure a
    // little past the feet's edge
    if (out > 0) {
      const ext = Math.min(out, 0.07 * k * legs);
      const dx = want[0] - p[0], dy = want[1] - p[1];
      const dl = Math.hypot(dx, dy) || 1;
      p[0] += (dx / dl) * ext;
      p[1] += (dy / dl) * ext;
    }
    let fx = m * w2 * (this.com[0] - p[0]);
    let fy = m * w2 * (this.com[1] - p[1]);
    // friction and leg strength bound it
    const max = m * G * 0.9 * clamp(legs, 0.3, 1);
    const f = Math.hypot(fx, fy);
    if (f > max) {
      fx *= max / f;
      fy *= max / f;
    }
    const pel = this.body.parts[B.pelvis]!;
    pel.force[0] += fx;
    pel.force[1] += fy;
  }

  /** A swinging foot that hits something on its way stops there: a trip. */
  private detectTrips(pose: WorldPose, dt: number): void {
    if (this.mode !== 'animated' && this.mode !== 'reacting') return;
    const feet = this.plan.feetPlanner.feet;
    const k = this.k;
    // (a hurried body trips at a touch; one picking its way lifts the foot over and goes on)
    const hurry = clamp((Math.hypot(this.plan.velocity[0], this.plan.velocity[1]) - 1.2) / 2.3, 0, 1);
    for (let i = 0; i < 2; i++) {
      const f = feet[i]!;
      // (still rolling off its toes, a foot against a riser is not caught: it lifts)
      if (f.planted || f.held || f.swing < 0.2 || f.swing > 0.85) {
        this.blockedFor[i] = 0;
        continue;
      }
      const fb = this.body.parts[i === 0 ? B.footL : B.footR]!;
      const planned = this.plan.world.p[i === 0 ? H.footL : H.footR]!;
      const actual = pose.p[i === 0 ? H.footL : H.footR]!;
      const lag = Math.hypot(planned[0] - actual[0], planned[1] - actual[1]);
      const sx = f.target[0] - f.lift[0], sy = f.target[1] - f.lift[1];
      const sl = Math.hypot(sx, sy) || 1;
      const n = fb.contactNormal;
      // (the foot, or the shin: a body lying in the way catches a runner at the knee)
      const sb = this.body.parts[i === 0 ? B.shinL : B.shinR]!;
      const sn = sb.contactNormal;
      const blocked = (fb.contact && (n[0] * sx + n[1] * sy) / sl < -0.35) || (sb.bumped > 1.5 && (sn[0] * sx + sn[1] * sy) / sl < -0.35);
      // (coming down onto something - a body, a lump of rubble - late in the swing: the foot
      // lands on it, a step sooner than meant, and the gait goes on)
      if (!blocked && fb.contact && n[2] > 0.6 && f.swing > 0.5 && lag > 0.08 * k) {
        this.plan.feetPlanner.plantNow(i, this.solePos(i));
        this.blockedFor[i] = 0;
        continue;
      }
      // (blocked by something in the way; merely scuffing the ground is not a trip unless the
      // foot is hopelessly behind)
      if ((blocked && lag > 0.05 * k) || (fb.contact && lag > 0.4 * k)) {
        // the stumble reflex: the first touch lifts the foot higher; caught for longer than a
        // careful step allows (or hopelessly behind), it trips
        if (this.blockedFor[i]! === 0) f.clear = Math.max(0, f.clear) + 0.1 * k * (1 - 0.7 * hurry);
        this.blockedFor[i]! += dt;
        if (this.blockedFor[i]! > lerp(0.16, 0.02, hurry) || lag > 0.5 * k) {
          this.catchFoot(i);
          if (this.mode === 'animated') this.setMode('reacting');
          return;
        }
      } else this.blockedFor[i] = Math.max(0, this.blockedFor[i]! - dt);
    }
  }
}

/**
 * A hand rotation (the canonical fist frame: knuckles +y, palm -z) with the palm facing
 * `palm` and the fingers towards `fingers` (as far as it allows).
 */
function palmFrame(palm: Readonly<V3>, fingers: Readonly<V3>): Quat {
  const z = vnorm(vscale(palm, -1));
  let y = vsub(fingers, vscale(z, vdot(fingers, z)));
  if (vlen(y) < 1e-4) y = vsub([0, 1, 0], vscale(z, z[1]));
  y = vnorm(y);
  const x = vcross(y, z);
  return qfromBasis(x, y, z);
}

function wrapPi(a: number): number {
  let x = a % (Math.PI * 2);
  if (x > Math.PI) x -= Math.PI * 2;
  else if (x <= -Math.PI) x += Math.PI * 2;
  return x;
}
