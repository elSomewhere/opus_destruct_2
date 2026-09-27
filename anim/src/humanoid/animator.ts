/**
 * The humanoid animator: procedural animation of the humanoid rig from a handful of inputs.
 *
 * The host moves the character (root position on the ground and facing yaw, each frame, with
 * whatever collision it has) and says what it is doing (crouch, weapon carry, aim and look
 * targets, mood). The animator makes the body follow:
 *
 * - Locomotion: a gait clock (gait.ts) drives a foot planter. Feet are planted in world space
 *   while in stance (no sliding at any speed, direction or turn rate) and swing to predicted
 *   landing spots on the ground (CollisionWorld), with heel strike and toe-off roll. Standing
 *   still, feet stay put; turning or drifting away from them triggers corrective steps.
 * - Pelvis bob, sway, yaw and roll, trunk lean from speed and acceleration, counter-rotation,
 *   breathing; legs by two-bone IK to the foot targets (knees towards the feet).
 * - Upper body: aim yaw/pitch spread over spine, chest and neck; a bladed stance when
 *   shouldering a weapon. The weapon is placed in the carry pose (relaxed, low ready,
 *   shouldered, port arms when running) and both hands are put on it by IK; recoil and
 *   weapon sway are springs. Without a weapon the arms swing with the gait.
 * - Moods (civilians): panic (hands to the head while fleeing), cower (crouched, hands over the
 *   head), surrender (hands up).
 * - Secondary motion: hit flinches (angular springs on the trunk and head), a stagger of the
 *   pelvis, recoil.
 *
 * Output: `pose` (local), `world` (world transforms; `prevWorld` of the frame before, for
 * velocities when a ragdoll takes over), skin matrices, and the held prop's world transform.
 */
import { ModelFK, frameRotation, setModelRotation, solveTwoBone } from '../core/ik.ts';
import { Pose, WorldPose, type Skeleton } from '../core/skeleton.ts';
import { Spring, Spring3 } from '../core/spring.ts';
import { gaitFor, type GaitParams } from '../locomotion/gait.ts';
import { writeRigid } from '../math/mat4.ts';
import { valueNoise } from '../math/random.ts';
import { qconj, qeuler, qexp, qmirrorX, qmul, qnlerp, qrotate, qx, qz, type Quat } from '../math/quat.ts';
import { clamp, fract, lerp, smoothstep, vadd, vcopy, vcross, vdist, vlerp, vnorm, vscale, vsub, wrapAngle, type V3 } from '../math/vec.ts';
import type { CollisionWorld } from '../physics/collision.ts';
import type { Prop } from '../characters/props.ts';
import { H } from './rig.ts';

export type Carry = 'relaxed' | 'ready' | 'aim';
export type Mood = 'normal' | 'panic' | 'cower' | 'surrender';

export interface HumanoidInput {
  /** 0 standing .. 1 crouched. */
  crouch: number;
  /** How the weapon is held (ignored without one). */
  carry: Carry;
  /** World point to aim the weapon (and the eyes) at. */
  aimAt: V3 | null;
  /** World point to look at (when not aiming). */
  lookAt: V3 | null;
  mood: Mood;
  /** Off the ground (falling, jumping): legs tuck, feet are not planted. */
  airborne: boolean;
}

interface Foot {
  side: -1 | 1;
  thigh: number;
  shin: number;
  foot: number;
  toe: number;
  offset: number;
  planted: boolean;
  /** Ground point under the ankle (world) and the foot's heading. */
  pos: V3;
  yaw: number;
  lift: V3;
  liftYaw: number;
  /** Swing progress 0..1 and its rate (1/s). */
  swing: number;
  swingRate: number;
  target: V3;
  targetYaw: number;
  /** Ankle (world) and foot pitch after this frame's update. */
  ankle: V3;
  pitch: number;
}

const TAU = Math.PI * 2;

function angleLerp(a: number, b: number, t: number): number {
  return a + wrapAngle(b - a) * t;
}

export class HumanoidAnimator {
  readonly skeleton: Skeleton;
  readonly pose: Pose;
  readonly fk: ModelFK;
  readonly world: WorldPose;
  readonly prevWorld: WorldPose;
  readonly input: HumanoidInput = { crouch: 0, carry: 'relaxed', aimAt: null, lookAt: null, mood: 'normal', airborne: false };
  collision: CollisionWorld;
  weapon: Prop | null = null;
  /** Character root: ground position and facing (radians, 0 = +x, CCW; +y is yaw pi/2). */
  readonly rootPos: V3 = [0, 0, 0];
  rootYaw = 0;
  /** Seconds of simulated time. */
  time = 0;
  /** The held prop's transform (world) after update. */
  readonly weaponPos: V3 = [0, 0, 0];
  weaponRot: Quat = [0, 0, 0, 1];
  /** Gait state for inspection and retro baking. */
  phase = 0;
  gait: GaitParams = gaitFor(0, 0);
  /** Smoothed world velocity. */
  readonly velocity: V3 = [0, 0, 0];

  // proportions
  private readonly k: number;
  private readonly legLen: number;
  private readonly ankleH: number;
  private readonly ballFwd: number;
  private readonly heelBack: number;
  private readonly footX: number;
  private readonly restPelvisZ: number;

  // state
  private readonly feet: [Foot, Foot];
  private readonly lastPos: V3 = [0, 0, 0];
  private placed = false;
  private readonly velSpring = new Spring3(9, 1);
  private readonly accel: V3 = [0, 0, 0];
  private readonly visZ = new Spring(16, 1);
  private readonly crouchS = new Spring(7, 1);
  private readonly lowerYaw = new Spring(7, 1);
  private readonly pelvisZ = new Spring(26, 1);
  private readonly aimW = new Spring(11, 1);
  private readonly readyW = new Spring(8, 1);
  private readonly sprintW = new Spring(6, 1);
  private readonly moodW = new Spring(6, 1);
  private readonly airW = new Spring(8, 1);
  private readonly aimYaw = new Spring(14, 1);
  private readonly aimPitch = new Spring(14, 1);
  private readonly headYaw = new Spring(6, 1);
  private readonly headPitch = new Spring(6, 1);
  private readonly flinch = new Spring3(13, 0.42);
  private readonly headFlinch = new Spring3(16, 0.4);
  private readonly stagger = new Spring3(9, 0.6);
  private readonly kickBack = new Spring(32, 0.55);
  private readonly kickPitch = new Spring(26, 0.5);
  private readonly sway = new Spring3(7, 0.7);
  private moodKind: Mood = 'normal';
  private stepping = false;
  private wasAirborne = false;
  private readonly seed: number;

  constructor(skeleton: Skeleton, collision: CollisionWorld, seed = 1) {
    this.skeleton = skeleton;
    this.collision = collision;
    this.seed = seed;
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
    const foot = (side: -1 | 1): Foot => ({
      side,
      thigh: side < 0 ? H.thighL : H.thighR,
      shin: side < 0 ? H.shinL : H.shinR,
      foot: side < 0 ? H.footL : H.footR,
      toe: side < 0 ? H.toeL : H.toeR,
      offset: side < 0 ? 0 : 0.5,
      planted: true,
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

  /** Model rotation of the facing: the model's +y along the facing yaw. */
  private rootRot(): Quat {
    return qz(this.rootYaw - Math.PI / 2);
  }

  /** Puts the character at `pos` facing `yaw`, feet planted in the default stance. */
  place(pos: Readonly<V3>, yaw: number): void {
    vcopy(pos, this.rootPos);
    vcopy(pos, this.lastPos);
    this.rootYaw = yaw;
    this.visZ.x = pos[2];
    this.visZ.v = 0;
    this.velSpring.reset();
    vcopy([0, 0, 0], this.velocity);
    for (const f of this.feet) {
      this.nominalFoot(f, pos, yaw, f.pos);
      f.yaw = yaw + f.side * -0.12;
      f.planted = true;
      f.swing = 0;
      vcopy(f.pos, f.target);
    }
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

  /** Recoil of one shot. */
  fire(strength = 1): void {
    this.kickBack.kick(1.1 * strength);
    this.kickPitch.kick(7 * strength);
    this.flinch.kick([0.35 * strength, 0, 0]);
  }

  /**
   * A hit: `dir` is the direction the blow travels (world), `strength` ~1 for a rifle round;
   * `height` 0 (legs) .. 1 (head) picks where the body gives.
   */
  hit(dir: Readonly<V3>, strength = 1, height = 0.7): void {
    const m = qrotate(qconj(this.rootRot()), vnorm(dir));
    const axis = vcross([0, 0, 1], m);
    const s = 4.5 * strength;
    this.flinch.kick(axis, s * (0.6 + 0.4 * height));
    this.headFlinch.kick(axis, s * 1.2 * smoothstep(0.5, 1, height));
    this.stagger.kick([m[0], m[1], 0], 0.9 * strength);
  }

  private nominalFoot(f: Foot, root: Readonly<V3>, yaw: number, out: V3): V3 {
    const c = this.crouchS.x;
    const x = f.side * this.footX * (1 + 0.45 * c);
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

  update(dtIn: number): void {
    const dt = Math.min(0.05, Math.max(0, dtIn));
    this.prevWorld.copyFrom(this.world);
    this.time += dt;
    const inp = this.input;
    const k = this.k;

    // ---- motion estimate --------------------------------------------------------------------
    if (dt > 0) {
      const raw: V3 = vscale(vsub(this.rootPos, this.lastPos), 1 / dt);
      raw[2] = 0;
      const before = vcopy(this.velSpring.x);
      this.velSpring.update(raw, dt);
      const a = vscale(vsub(this.velSpring.x, before), 1 / dt);
      for (let i = 0; i < 3; i++) this.accel[i] = lerp(this.accel[i]!, a[i]!, 1 - Math.exp(-dt * 6));
    }
    vcopy(this.velSpring.x, this.velocity);
    vcopy(this.rootPos, this.lastPos);
    if (Math.abs(this.rootPos[2] - this.visZ.x) > 0.6 * k) this.visZ.x = this.rootPos[2];
    this.visZ.update(this.rootPos[2], dt);
    this.crouchS.update(clamp(inp.mood === 'cower' ? 1 : inp.crouch, 0, 1), dt);
    const crouch = clamp(this.crouchS.x, 0, 1);
    const vel = this.velocity;
    const speed = Math.hypot(vel[0], vel[1]);
    const rootRot = this.rootRot();
    const inv = qconj(rootRot);
    const vLocal = qrotate(inv, vel);
    const aLocal = qrotate(inv, this.accel);
    const armed = this.weapon !== null;
    const aiming = armed && inp.carry === 'aim' && inp.aimAt !== null;

    // lower body heading relative to the facing: bladed when shouldering, towards the motion
    // when strafing
    let lower = 0;
    if (speed > 0.3) {
      const th = Math.atan2(-vLocal[0], vLocal[1]);
      if (Math.abs(th) < 1.75) lower = clamp(th, -1.0, 1.0) * 0.65;
      else lower = clamp(wrapAngle(th - Math.PI), -1.0, 1.0) * 0.65;
    } else if (aiming) lower = -0.42;
    this.lowerYaw.update(lower, dt);

    // ---- gait clock ---------------------------------------------------------------------------
    const g = gaitFor(speed, crouch, k);
    this.gait = g;
    const moving = speed > 0.12 && !inp.airborne;
    let prevPhase = this.phase;
    const bodyYaw = this.rootYaw + this.lowerYaw.x;
    if (moving) {
      if (!this.stepping) {
        // start with the foot that is further behind the motion
        const dir = vnorm([vel[0], vel[1], 0]);
        const d0 = (this.feet[0].pos[0] - this.rootPos[0]) * dir[0] + (this.feet[0].pos[1] - this.rootPos[1]) * dir[1];
        const d1 = (this.feet[1].pos[0] - this.rootPos[0]) * dir[0] + (this.feet[1].pos[1] - this.rootPos[1]) * dir[1];
        const lead = d0 <= d1 ? this.feet[0] : this.feet[1];
        if (this.feet[0].planted && this.feet[1].planted) {
          this.phase = fract(g.duty - 0.02 - lead.offset);
          prevPhase = this.phase; // (a jump, not a crossing)
        }
        this.stepping = true;
      }
      this.phase = fract(this.phase + g.freq * dt);
    } else if (!inp.airborne) {
      // standing: step only to correct the stance (after stopping, turning, being pushed)
      let need = false;
      for (const f of this.feet) {
        if (!f.planted) need = true;
        else {
          const nom = this.nominalFoot(f, this.rootPos, bodyYaw, [0, 0, 0]);
          const err = Math.hypot(nom[0] - f.pos[0], nom[1] - f.pos[1]);
          const yawErr = Math.abs(wrapAngle(bodyYaw + f.side * -0.12 - f.yaw));
          if (err > 0.16 * k || yawErr > 0.6 || (this.stepping && err > 0.07 * k)) need = true;
        }
      }
      if (need) this.phase = fract(this.phase + 1.5 * dt);
      else this.stepping = false;
    }

    // ---- feet ---------------------------------------------------------------------------------
    if (this.wasAirborne && !inp.airborne) {
      for (const f of this.feet) {
        this.nominalFoot(f, this.rootPos, bodyYaw, f.pos);
        f.pos[2] = this.ground(f.pos[0], f.pos[1], this.rootPos[2], this.rootPos[2]);
        f.yaw = bodyYaw + f.side * -0.12;
        f.planted = true;
      }
      this.stagger.kick([0, 0, -1]);
    }
    this.wasAirborne = inp.airborne;
    const D = g.duty;
    const freq = moving ? g.freq : 1.5;
    const stanceT = D / freq;
    const moveAmt = smoothstep(0.1, 0.9, speed);
    for (const f of this.feet) {
      const p0 = fract(prevPhase + f.offset);
      const p1 = fract(this.phase + f.offset);
      const advanced = this.phase !== prevPhase;
      if (inp.airborne) f.planted = false;
      else if (f.planted) {
        const wrapped = p1 < p0;
        const crossedLift = advanced && ((!wrapped && p0 < D && p1 >= D) || (wrapped && p0 < D));
        // a planted foot left too far behind (sudden start, shove) steps quickly to catch up
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
        // landing target: under the hip at mid-stance of the coming step
        const remain = (1 - f.swing) / f.swingRate;
        const pred: V3 = [this.rootPos[0] + vel[0] * remain, this.rootPos[1] + vel[1] * remain, this.rootPos[2]];
        const tgt = this.nominalFoot(f, pred, bodyYaw, [0, 0, 0]);
        // walking lands half a stance ahead of the hip; running lands closer under the body
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
        f.targetYaw = bodyYaw + f.side * -0.12;
        if (f.swing >= 1) {
          f.planted = true;
          vcopy(f.target, f.pos);
          f.yaw = f.targetYaw;
        }
      }
      if (inp.airborne) {
        // dangling: under the hips, knees up a little
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
        // the ground under a planted foot may be blasted away
        const gz = this.ground(f.pos[0], f.pos[1], f.pos[2] + 0.2 * k, f.pos[2]);
        if (gz < f.pos[2] - 0.01) f.pos[2] = Math.max(gz, f.pos[2] - 3 * dt);
        const u = p1 < D ? p1 / D : 0.3; // (a foot planted off-cycle stands flat)
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
        const e = s * s * (3 - 2 * s);
        // running: the foot stays back (heel kick) and comes through late with the knee drive
        const sh = lerp(s, Math.pow(s, 1.6), g.run);
        const eh = sh * sh * (3 - 2 * sh);
        const hz = vlerp(f.lift, f.target, eh);
        const peak = Math.sin(Math.PI * Math.pow(s, lerp(1, 0.62, g.run)));
        const lift = (moving ? g.lift : 0.06 * k) * peak + Math.max(0, f.target[2] - f.lift[2]) * 0.3 * peak;
        hz[2] += lift;
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

    // ---- pelvis --------------------------------------------------------------------------------
    const pose = this.pose;
    pose.reset();
    const origin: V3 = [this.rootPos[0], this.rootPos[1], this.visZ.x];
    const toModel = (w: Readonly<V3>): V3 => qrotate(inv, vsub(w, origin));
    const ph = this.phase;
    const run = g.run;
    const walkBob = moving ? g.bob * (1 - 2 * run) * Math.cos(TAU * 2 * (ph - D / 2)) : 0;
    const swayX = moving ? -g.sway * Math.cos(TAU * (ph - D / 2)) : 0;
    const hipYawOsc = moving ? -g.hipYaw * Math.cos(TAU * ph) : 0;
    const hipRoll = moving ? -g.hipRoll * Math.cos(TAU * (ph - (1 + D) / 2)) : 0;
    this.moodKind = inp.mood;
    this.moodW.update(inp.mood === 'normal' ? 0 : 1, dt);
    this.airW.update(inp.airborne ? 1 : 0, dt);
    const idle = 1 - moveAmt;
    const breathe = Math.sin(this.time * 1.9 + this.seed);
    // idle weight shift
    const shift = idle * 0.018 * k * (valueNoise(this.time * 0.25, this.seed, 0, 1, this.seed) * 2 - 1);
    const crouchDrop = crouch * 0.4 * k;
    let pz = this.restPelvisZ - g.sink - crouchDrop + walkBob - 0.012 * k * idle;
    // stay low enough for both feet to be reached
    const pelvisYaw = this.lowerYaw.x + hipYawOsc;
    // (a positive x rotation tips an upright bone backwards: forward tilt is negative)
    const pelvisRot = qmul(qz(pelvisYaw), qeuler(-(0.1 * crouch + 0.05 * g.lean), hipRoll, 0));
    const stag = this.stagger.update([0, 0, 0], dt);
    const px = swayX + shift + stag[0] * 0.12;
    const py = -0.07 * crouch * k + stag[1] * 0.12;
    const lowest = this.restPelvisZ - 0.2 * k - crouchDrop;
    for (const f of this.feet) {
      // planted feet must be reached; a swinging foot mostly follows the hips
      const a = toModel(f.ankle);
      const hipOff = qrotate(pelvisRot, vsub(this.skeleton.restHead[f.thigh]!, this.skeleton.restHead[H.pelvis]!));
      const dx = px + hipOff[0] - a[0];
      const dy = py + hipOff[1] - a[1];
      const reach2 = (0.995 * this.legLen) ** 2 - dx * dx - dy * dy;
      let maxZ = (reach2 > 0 ? Math.sqrt(reach2) : 0) + a[2] - hipOff[2];
      if (!f.planted) maxZ = lerp(pz, maxZ, smoothstep(0.45, 0.9, f.swing));
      if (pz > maxZ) pz = Math.max(maxZ, lowest);
    }
    if (inp.airborne) pz = this.restPelvisZ - 0.04 * k;
    this.pelvisZ.update(pz, dt);
    const pzS = Math.min(this.pelvisZ.x, pz + 0.01 * k) + stag[2] * 0.12;
    vcopy([px, py, pzS], pose.t[H.pelvis]);
    pose.r[H.pelvis] = pelvisRot;

    // ---- trunk ----------------------------------------------------------------------------------
    const accelLean = clamp(aLocal[1] * 0.035, -0.18, 0.22);
    const panic = this.moodKind === 'panic' ? this.moodW.x : 0;
    const cower = this.moodKind === 'cower' ? this.moodW.x : 0;
    const lean = g.lean + accelLean + crouch * 0.38 + panic * 0.12 + cower * 0.25 + 0.012 * breathe * idle;
    // aim: yaw and pitch (model space) of the aim direction seen from the chest
    let wantYaw = 0;
    let wantPitch = 0;
    const tgt = aiming ? inp.aimAt : (inp.lookAt ?? (armed && inp.carry !== 'relaxed' ? inp.aimAt : null));
    const chestRest = this.skeleton.restHead[H.chest]!;
    if (tgt) {
      const m = toModel(tgt);
      const d = vsub(m, [0, 0, chestRest[2] + 0.2 * k]);
      wantYaw = clamp(Math.atan2(-d[0], d[1]), -1.9, 1.9);
      wantPitch = clamp(Math.atan2(d[2], Math.hypot(d[0], d[1])), -1.1, 1.1);
    }
    this.aimYaw.update(wantYaw, dt);
    this.aimPitch.update(wantPitch, dt);
    this.aimW.update(aiming ? 1 : 0, dt);
    const aw = this.aimW.x;
    // the upper body turns to the aim (relative to the pelvis); bladed while shouldered
    const trunkYaw = (tgt ? this.aimYaw.x : 0) - pelvisYaw - aw * 0.3;
    const trunkPitch = tgt ? this.aimPitch.x * lerp(0.55, 0.75, aw) : 0;
    const counter = -hipYawOsc * 1.6;
    const fl = this.flinch.update([0, 0, 0], dt);
    // lean forward = negative pitch; aiming up bends the trunk back
    const spinePitch = -lean * 0.5 + trunkPitch * 0.3 + (0.1 * crouch + 0.05 * g.lean);
    const chestPitch = -lean * 0.45 + trunkPitch * 0.45 + 0.02 * breathe;
    pose.r[H.spine] = qmul(qeuler(spinePitch, -hipRoll * 0.6, trunkYaw * 0.35 + counter * 0.4), qexp(vscale(fl, 0.4)));
    pose.r[H.chest] = qmul(qeuler(chestPitch, -hipRoll * 0.4, trunkYaw * 0.45 + counter * 0.6), qexp(vscale(fl, 0.6)));
    this.fk.update(pose, 0);

    // ---- head -------------------------------------------------------------------------------
    let lookYaw = 0;
    let lookPitch = -0.06 - lean * 0.3;
    if (tgt) {
      lookYaw = this.aimYaw.x;
      lookPitch = this.aimPitch.x;
    } else if (idle > 0.5 && this.moodKind === 'normal') {
      lookYaw = (valueNoise(this.time * 0.18, 3.1, this.seed, 1, this.seed + 9) * 2 - 1) * 1.0;
      lookPitch = (valueNoise(this.time * 0.13, 7.7, this.seed, 1, this.seed + 3) * 2 - 1) * 0.25;
    } else if (panic > 0) {
      lookYaw = Math.sin(this.time * 2.7 + this.seed) * 0.7;
    }
    if (cower > 0) lookPitch = lerp(lookPitch, -0.7, cower);
    this.headYaw.update(lookYaw, dt);
    this.headPitch.update(lookPitch, dt);
    // express the look in the chest's frame: neck and head share it
    const chestQ = this.fk.q[H.chest]!;
    const chestYawNow = Math.atan2(-qrotate(chestQ, [0, 1, 0])[0], qrotate(chestQ, [0, 1, 0])[1]);
    const chestFwd = qrotate(chestQ, [0, 1, 0]);
    const chestPitchNow = Math.asin(clamp(chestFwd[2], -1, 1));
    const relYaw = clamp(wrapAngle(this.headYaw.x - chestYawNow), -1.3, 1.3);
    const relPitch = clamp(this.headPitch.x - chestPitchNow, -0.9, 0.8);
    const hf = this.headFlinch.update([0, 0, 0], dt);
    pose.r[H.neck] = qmul(qeuler(relPitch * 0.4, 0, relYaw * 0.4), qexp(vscale(hf, 0.4)));
    pose.r[H.head] = qmul(qeuler(relPitch * 0.6, -0.12 * aw, relYaw * 0.6), qexp(vscale(hf, 0.6)));
    this.fk.update(pose, H.neck);

    // ---- legs -----------------------------------------------------------------------------------
    for (const f of this.feet) {
      const target = toModel(f.ankle);
      const fy = wrapAngle(f.yaw - this.rootYaw); // foot yaw in model space (0 = +y)
      const footFwd: V3 = [-Math.sin(fy), Math.cos(fy), 0];
      const pole = vnorm(vadd(footFwd, [f.side * (0.15 + 0.22 * crouch), 0, 0.1]));
      solveTwoBone(pose, this.fk, f.thigh, f.shin, f.foot, target, pole, 0.02);
      const footQ = qmul(qz(fy), qx(f.pitch));
      setModelRotation(pose, this.fk, f.foot, footQ);
      pose.r[f.toe] = qx(f.planted && f.pitch < 0 ? -f.pitch * 0.9 : this.airW.x * 0.2);
      this.fk.updateBone(pose, f.toe);
    }

    // ---- arms -----------------------------------------------------------------------------------
    this.armSwing(g, moving, speed, breathe, idle);
    this.fk.update(pose, H.clavicleL);
    if (armed) this.holdWeapon(dt, aiming, run, moving, toModel);
    else this.weaponRot = qmul(rootRot, [0, 0, 0, 1]);
    if (this.moodW.x > 0.01 && this.moodKind !== 'normal') this.moodArms(this.moodW.x);

    // ---- world ----------------------------------------------------------------------------------
    this.world.compute(pose, origin, rootRot);
    if (armed) {
      // the prop follows the weapon socket
      vcopy(this.world.p[H.weapon]!, this.weaponPos);
      this.weaponRot = [...this.world.q[H.weapon]!] as Quat;
    }
  }

  /** Ankle position (world) of a foot planted at `plant` with heading `yaw` and pitch. */
  private ankleFromPlant(plant: Readonly<V3>, yaw: number, pitch: number, out: V3): V3 {
    const fwd: V3 = [Math.cos(yaw), Math.sin(yaw), 0];
    let dy = 0;
    let dz = this.ankleH;
    if (pitch < 0) {
      // heel off: roll over the ball of the foot
      const c = Math.cos(pitch), s = Math.sin(pitch);
      const by = -this.ballFwd, bz = this.ankleH;
      dy = this.ballFwd + by * c - bz * s;
      dz = by * s + bz * c;
    } else if (pitch > 0) {
      // heel strike: toes up, pivoting on the heel
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

  /** Arms with the gait (or relaxed), as local joint rotations; mirrored for the right side. */
  private armSwing(g: GaitParams, moving: boolean, speed: number, breathe: number, idle: number): void {
    const pose = this.pose;
    const ph = this.phase;
    const run = g.run;
    const swing = moving ? g.armSwing : 0;
    const elbow = lerp(0.18 + 0.03 * breathe, g.elbow, smoothstep(0.1, 1, speed));
    for (const side of [-1, 1] as const) {
      // left arm forward when the right leg is (phase 0.5)
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

  /** Places the weapon socket in the carry pose and puts both hands on the prop. */
  private holdWeapon(dt: number, aiming: boolean, run: number, moving: boolean, toModel: (w: Readonly<V3>) => V3): void {
    const prop = this.weapon!;
    const pose = this.pose;
    const fk = this.fk;
    const k = this.k;
    const inp = this.input;
    const chestQ = fk.q[H.chest]!;
    const chestP = fk.p[H.chest]!;
    const shoulder = fk.p[H.upperarmR]!;
    const pocket = vadd(shoulder, qrotate(chestQ, [-0.06 * k, 0.07 * k, -0.035 * k]));
    // aim direction in model space
    let aimDir: V3 = qrotate(chestQ, [0, 1, 0]);
    if (inp.aimAt) {
      aimDir = vnorm(vsub(toModel(inp.aimAt), pocket));
    } else {
      aimDir = vnorm([aimDir[0], aimDir[1], 0]);
    }
    const aimRot = frameRotation([0, 1, 0], [0, 0, 1], aimDir, [0, 0, 1]);
    this.readyW.update(inp.carry === 'relaxed' ? 0 : 1, dt);
    this.sprintW.update(moving && !aiming ? run : 0, dt);
    // relaxed: muzzle down and across, held low
    const chestYawQ = frameRotation([0, 1, 0], [0, 0, 1], vnorm([qrotate(chestQ, [0, 1, 0])[0], qrotate(chestQ, [0, 1, 0])[1], 0]), [0, 0, 1]);
    const relaxedRot = qmul(chestYawQ, qeuler(-0.95, 0.15, 0.55));
    const relaxedGrip = vadd(chestP, qrotate(chestYawQ, [0.13 * k, 0.2 * k, -0.2 * k]));
    // low ready: shouldered, muzzle lowered
    const readyRot = qmul(aimRot, qeuler(-0.5, 0, 0.3));
    const readyStock = vadd(pocket, qrotate(chestQ, [0.01 * k, 0.0, -0.05 * k]));
    const readyGrip = vsub(readyStock, qrotate(readyRot, prop.stock));
    // shouldered: stock in the pocket, barrel on the aim line
    const aimGrip = vsub(pocket, qrotate(aimRot, prop.stock));
    // port arms (running): across the chest, muzzle up and left
    const portRot = qmul(chestYawQ, frameRotation([0, 1, 0], [0, 0, 1], vnorm([-0.55, 0.3, 0.78]), vnorm([0.1, 1, 0.1])));
    const portGrip = vadd(chestP, qrotate(chestYawQ, [0.12 * k, 0.2 * k, -0.08 * k]));
    let rot = relaxedRot;
    let pos = relaxedGrip;
    const rw = clamp(this.readyW.x, 0, 1);
    rot = qnlerp(rot, readyRot, rw);
    pos = vlerp(pos, readyGrip, rw);
    const sw = clamp(this.sprintW.x, 0, 1) * (1 - clamp(this.aimW.x, 0, 1));
    rot = qnlerp(rot, portRot, sw);
    pos = vlerp(pos, portGrip, sw);
    const aw = clamp(this.aimW.x, 0, 1);
    rot = qnlerp(rot, aimRot, aw);
    pos = vlerp(pos, aimGrip, aw);
    // recoil and sway
    this.kickBack.update(0, dt);
    this.kickPitch.update(0, dt);
    const fwd = qrotate(rot, [0, 1, 0]);
    pos = vadd(pos, vscale(fwd, -0.035 * this.kickBack.x));
    rot = qmul(rot, qx(0.05 * this.kickPitch.x));
    const bob = moving ? Math.sin(TAU * 2 * this.phase) * 0.008 * k * (1 - aw * 0.7) : 0;
    const sway = this.sway.update([0, 0, bob], dt);
    pos = vadd(pos, sway);
    // the socket bone carries the prop (grip at its head; its parent is the root)
    pose.t[H.weapon] = [pos[0], pos[1], pos[2]];
    pose.r[H.weapon] = rot;
    fk.updateBone(pose, H.weapon);

    // hands on the prop
    const grip = pos;
    const support = vadd(pos, qrotate(rot, prop.support));
    const handR = qmul(rot, this.gripR);
    const handL = qmul(rot, this.gripL);
    const wristR = vsub(grip, qrotate(handR, this.palmR));
    const wristL = vsub(support, qrotate(handL, this.palmL));
    const poleR = qrotate(chestQ, vnorm([0.7, -0.3, -0.75]));
    const poleL = qrotate(chestQ, vnorm([-0.7, 0.1, -0.8]));
    // clavicles shrug towards the weapon when it is up
    pose.r[H.clavicleR] = qeuler(0, 0, 0.12 * rw);
    pose.r[H.clavicleL] = qeuler(0, 0, -0.18 * rw);
    fk.update(pose, H.clavicleL);
    solveTwoBone(pose, fk, H.upperarmR, H.forearmR, H.handR, wristR, poleR, 0.02);
    setModelRotation(pose, fk, H.handR, handR);
    solveTwoBone(pose, fk, H.upperarmL, H.forearmL, H.handL, wristL, poleL, 0.04);
    setModelRotation(pose, fk, H.handL, handL);
  }

  // grip frames (hand rotation relative to the prop) and palm offsets, from the rest pose
  private get gripR(): Quat {
    const sk = this.skeleton;
    const dir = vsub(sk.restTail[H.handR]!, sk.restHead[H.handR]!);
    return frameRotation(dir, [-1, 0, 0], vnorm([0, -0.3, -1]), [-1, 0, 0]);
  }

  private get gripL(): Quat {
    const sk = this.skeleton;
    const dir = vsub(sk.restTail[H.handL]!, sk.restHead[H.handL]!);
    return frameRotation(dir, [1, 0, 0], vnorm([0.35, 0.8, 0.1]), vnorm([0.3, 0, 1]));
  }

  /** Palm centre relative to the wrist in rest space. */
  private get palmR(): V3 {
    const sk = this.skeleton;
    return vscale(vsub(sk.restTail[H.handR]!, sk.restHead[H.handR]!), 0.42);
  }

  private get palmL(): V3 {
    const sk = this.skeleton;
    return vscale(vsub(sk.restTail[H.handL]!, sk.restHead[H.handL]!), 0.42);
  }

  /** Mood arm poses by IK (blended over the swing pose by weight w). */
  private moodArms(w: number): void {
    const pose = this.pose;
    const fk = this.fk;
    const k = this.k;
    const bones = [H.clavicleL, H.upperarmL, H.forearmL, H.handL, H.clavicleR, H.upperarmR, H.forearmR, H.handR];
    const saved = bones.map((b) => [...pose.r[b]!] as Quat);
    const headP = fk.p[H.head]!;
    const headQ = fk.q[H.head]!;
    const chestQ = fk.q[H.chest]!;
    const t = this.time;
    for (const side of [-1, 1] as const) {
      const ua = side < 0 ? H.upperarmL : H.upperarmR;
      const fa = side < 0 ? H.forearmL : H.forearmR;
      const hand = side < 0 ? H.handL : H.handR;
      let target: V3;
      let pole: V3;
      if (this.moodKind === 'cower') {
        target = vadd(headP, qrotate(headQ, [side * 0.06 * k, -0.05 * k, 0.16 * k]));
        pole = qrotate(chestQ, vnorm([side * 0.6, 0.8, 0.1]));
      } else if (this.moodKind === 'surrender') {
        const sh = fk.p[ua]!;
        target = vadd(sh, qrotate(chestQ, [side * 0.2 * k, 0.06 * k, 0.5 * k + 0.02 * Math.sin(t * 3 + side)]));
        pole = qrotate(chestQ, vnorm([side, -0.2, -0.4]));
      } else {
        // panic: hands up by the head, bouncing with the stride
        const b = Math.sin(TAU * this.phase + (side < 0 ? 0 : Math.PI)) * 0.06 * k;
        target = vadd(headP, qrotate(chestQ, [side * 0.16 * k, 0.1 * k, 0.12 * k + b]));
        pole = qrotate(chestQ, vnorm([side, 0.2, -0.5]));
      }
      solveTwoBone(pose, fk, ua, fa, hand, target, pole, 0.03);
      pose.r[hand] = qeuler(this.moodKind === 'surrender' ? -0.3 : 0.4, 0, 0);
      fk.updateBone(pose, hand);
    }
    if (w < 0.999) {
      bones.forEach((b, i) => {
        pose.r[b] = qnlerp(saved[i]!, pose.r[b]!, w);
      });
      fk.update(pose, H.clavicleL);
    }
  }

  /** World position of a point on the held prop (prop space), e.g. its muzzle. */
  propPoint(p: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
    return vadd(this.weaponPos, qrotate(this.weaponRot, p), out);
  }

  /** Skin matrix of the held prop (its model's single bone). */
  writePropSkin(out: Float32Array, offset = 0): void {
    writeRigid(out, offset, this.weaponPos, this.weaponRot, [0, 0, 0]);
  }

  /** The planted feet (world), for debugging and tests. */
  footState(): { planted: boolean; pos: V3; ankle: V3 }[] {
    return this.feet.map((f) => ({ planted: f.planted, pos: [...f.pos] as V3, ankle: [...f.ankle] as V3 }));
  }

  /** World position between the eyes (line of sight, muzzle-less aiming). */
  eyes(out: V3 = [0, 0, 0]): V3 {
    const h = this.skeleton.restHead[H.head]!;
    return this.world.pointOf(H.head, [h[0], h[1] + 0.09 * this.k, h[2] + 0.12 * this.k], out);
  }
}
