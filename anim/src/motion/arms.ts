/**
 * Arms and hands of the motion plan: two-bone arm IK to a palm target with a hand orientation
 * (the canonical fist frame), and the held weapon: the prop at the rig's weapon socket in its
 * carry (relaxed, low ready, shouldered, from the hip, port arms; a pistol lowered, at low
 * ready, two-handed or one-handed), recoil and sway, with both hands put on it.
 */
import { ModelFK, frameRotation, setModelRotation, solveTwoBone } from '../core/ik.ts';
import type { Pose, Skeleton } from '../core/skeleton.ts';
import { Spring, Spring3 } from '../core/spring.ts';
import type { Prop } from '../characters/props.ts';
import { qeuler, qmul, qnlerp, qrotate, qx, type Quat } from '../math/quat.ts';
import { DEG, clamp, vadd, vlerp, vnorm, vscale, vsub, type V3 } from '../math/vec.ts';
import { H } from '../humanoid/rig.ts';
import type { ChannelFrame } from './actions.ts';

export type Carry = 'relaxed' | 'ready' | 'aim' | 'hip';
export type Side = 'L' | 'R';

const TAU = Math.PI * 2;
/**
 * Where an elbow points in the rest pose (back: the forearm bends forward). The arm IK twists
 * the upper arm with it, so the elbow always bends as a hinge (the physical body's elbow can
 * follow every planned arm).
 */
export const ELBOW_REST: V3 = [0, -1, 0];

export class ArmRig {
  readonly skeleton: Skeleton;
  readonly pose: Pose;
  readonly fk: ModelFK;
  private readonly canon: Record<Side, Quat>;
  private readonly palm: Record<Side, V3>;

  constructor(skeleton: Skeleton, pose: Pose, fk: ModelFK) {
    this.skeleton = skeleton;
    this.pose = pose;
    this.fk = fk;
    const sk = skeleton;
    const frame = (side: Side): Quat => {
      const b = side === 'L' ? H.handL : H.handR;
      const dir = vsub(sk.restTail[b]!, sk.restHead[b]!);
      return frameRotation(dir, [side === 'L' ? 1 : -1, 0, 0], [0, 1, 0], [0, 0, -1]);
    };
    this.canon = { L: frame('L'), R: frame('R') };
    const palm = (side: Side): V3 => {
      const b = side === 'L' ? H.handL : H.handR;
      return vscale(vsub(sk.restTail[b]!, sk.restHead[b]!), 0.42);
    };
    this.palm = { L: palm('L'), R: palm('R') };
  }

  /** Rotation from a hand's rest frame to the canonical fist frame (knuckles +y, palm -z). */
  canonical(side: Side): Quat {
    return this.canon[side];
  }

  /** Palm centre relative to the wrist in rest space. */
  palmOffset(side: Side): V3 {
    return this.palm[side];
  }

  /**
   * Puts a hand's palm at `target` (model space), oriented `rot` (model, the canonical fist
   * frame) or left as is, elbow towards `pole`, blending the arm by w.
   */
  handIK(side: Side, target: Readonly<V3>, rot: Quat | null, pole: Readonly<V3>, w: number, soft = 0.03): void {
    const pose = this.pose;
    const fk = this.fk;
    const ua = side === 'L' ? H.upperarmL : H.upperarmR;
    const fa = side === 'L' ? H.forearmL : H.forearmR;
    const hand = side === 'L' ? H.handL : H.handR;
    const saved = w < 0.999 ? ([[...pose.r[ua]!], [...pose.r[fa]!], [...pose.r[hand]!]] as Quat[]) : null;
    const handQ = rot ? qmul(rot, this.canonical(side)) : fk.q[hand]!;
    const palm = qrotate(handQ, this.palmOffset(side));
    solveTwoBone(pose, fk, ua, fa, hand, vsub(target, palm), pole, soft, ELBOW_REST);
    if (rot) setModelRotation(pose, fk, hand, handQ);
    if (saved) {
      pose.r[ua] = qnlerp(saved[0]!, pose.r[ua]!, w);
      pose.r[fa] = qnlerp(saved[1]!, pose.r[fa]!, w);
      pose.r[hand] = qnlerp(saved[2]!, pose.r[hand]!, w);
    }
    fk.updateSubtree(pose, ua);
  }

  /** Grip frames: the hand's rotation relative to the prop. */
  gripR(pistol: boolean): Quat {
    const sk = this.skeleton;
    const dir = vsub(sk.restTail[H.handR]!, sk.restHead[H.handR]!);
    return frameRotation(dir, [-1, 0, 0], vnorm(pistol ? [0, -0.2, -1] : [0, -0.3, -1]), [-1, 0, 0]);
  }

  gripL(pistol: boolean): Quat {
    const sk = this.skeleton;
    const dir = vsub(sk.restTail[H.handL]!, sk.restHead[H.handL]!);
    // a pistol's support hand wraps the gun hand from the left
    if (pistol) return frameRotation(dir, [1, 0, 0], vnorm([0.4, -0.1, -1]), vnorm([1, 0.2, 0.1]));
    return frameRotation(dir, [1, 0, 0], vnorm([0.35, 0.8, 0.1]), vnorm([0.3, 0, 1]));
  }
}

/** What the weapon hold needs to know each frame. */
export interface HoldContext {
  carry: Carry;
  /** Aim target in model space, or null. */
  aimAt: V3 | null;
  aiming: boolean;
  moving: boolean;
  run: number;
  phase: number;
  /** Aim weight 0..1 (eased). */
  aimW: number;
  k: number;
  /** Channels of the posture and one-shot action layers, with their weights. */
  chP: ChannelFrame | null;
  wP: number;
  chA: ChannelFrame | null;
  wA: number;
  /** Let go with the support hand (it has something else to do). */
  freeLeft: boolean;
}

/**
 * The held weapon: its carry pose at the socket, recoil and sway, and both hands on it.
 * Knives (and lowered pistols) sit in the right hand instead.
 */
export class WeaponHold {
  readonly readyW = new Spring(6.5, 1);
  readonly hipW = new Spring(9, 1);
  readonly sprintW = new Spring(6, 1);
  readonly kickBack = new Spring(32, 0.55);
  readonly kickPitch = new Spring(26, 0.5);
  private readonly sway = new Spring3(7, 0.7);

  /** Recoil of one shot. */
  fire(prop: Prop, strength: number): void {
    const pistol = prop.kind === 'pistol';
    this.kickBack.kick((pistol ? 0.8 : 1.1) * strength);
    this.kickPitch.kick((pistol ? 12 : 7) * strength);
  }

  /**
   * Places the socket and the hands. Returns true when the prop is held at the socket (both
   * hands, or the gun hand, busy with it).
   */
  hold(dt: number, arms: ArmRig, prop: Prop, c: HoldContext): boolean {
    const pose = arms.pose;
    const fk = arms.fk;
    const k = c.k;
    const pistol = prop.kind === 'pistol';
    this.readyW.update(c.carry === 'relaxed' ? 0 : 1, dt);
    this.hipW.update(c.carry === 'hip' ? 1 : 0, dt);
    this.sprintW.update(c.moving && !c.aiming ? c.run : 0, dt);
    this.kickBack.update(0, dt);
    this.kickPitch.update(0, dt);
    // a lowered pistol hangs in the hand
    if (pistol && this.readyW.x < 0.05 && !c.aiming) return false;
    const chestQ = fk.q[H.chest]!;
    const chestP = fk.p[H.chest]!;
    const shoulder = fk.p[H.upperarmR]!;
    const pocket = vadd(shoulder, qrotate(chestQ, [-0.06 * k, 0.07 * k, -0.035 * k]));
    const cf = qrotate(chestQ, [0, 1, 0]);
    const chestYawQ = frameRotation([0, 1, 0], [0, 0, 1], vnorm([cf[0], cf[1], 0]), [0, 0, 1]);
    const eyes = vadd(fk.p[H.head]!, qrotate(fk.q[H.head]!, [0, 0.09 * k, 0.1 * k]));
    let aimDir: V3 = vnorm([cf[0], cf[1], 0]);
    if (c.aimAt) aimDir = vnorm(vsub(c.aimAt, pistol ? eyes : pocket));
    const aimRot = frameRotation([0, 1, 0], [0, 0, 1], aimDir, [0, 0, 1]);
    const rw = clamp(this.readyW.x, 0, 1);
    const aw = clamp(c.aimW, 0, 1);
    const hw = clamp(this.hipW.x, 0, 1);
    let rot: Quat;
    let pos: V3;
    let twoHanded = !c.freeLeft;
    if (pistol) {
      // lowered (in the hand) -> low ready (two hands, muzzle down) -> aimed (two hands at eye
      // level) or one-handed (arm out at the target)
      const readyRot = qmul(aimRot, qeuler(-0.7, 0, 0));
      const readyGrip = vadd(chestP, qrotate(chestYawQ, [0.03 * k, 0.34 * k, 0.02 * k]));
      const aimGrip = vadd(eyes, vadd(vscale(aimDir, 0.47 * k), [0, 0, -0.07 * k]));
      const oneGrip = vadd(shoulder, vscale(aimDir, 0.56 * k));
      rot = qnlerp(readyRot, aimRot, aw);
      pos = vlerp(readyGrip, vlerp(aimGrip, oneGrip, c.freeLeft ? 1 : hw), aw);
      twoHanded = twoHanded && hw < 0.5;
    } else {
      const relaxedRot = qmul(chestYawQ, qeuler(-0.95, 0.15, 0.55));
      const relaxedGrip = vadd(chestP, qrotate(chestYawQ, [0.13 * k, 0.2 * k, -0.2 * k]));
      // (a heavy machine gun is carried across the body, lower)
      const heavy = prop.kind === 'lmg';
      const readyRot = qmul(aimRot, qeuler(heavy ? -0.32 : -0.5, 0, heavy ? 0.45 : 0.3));
      const readyStock = vadd(pocket, qrotate(chestQ, heavy ? [0.01 * k, -0.12 * k, -0.1 * k] : [0.01 * k, 0.0, -0.05 * k]));
      const readyGrip = vsub(readyStock, qrotate(readyRot, prop.stock));
      const aimGrip = vsub(pocket, qrotate(aimRot, prop.stock));
      // hip fire: stock under the arm, level at the target (machine guns on the move)
      const hipRot = qnlerp(aimRot, frameRotation([0, 1, 0], [0, 0, 1], vnorm([aimDir[0], aimDir[1], aimDir[2] * 0.5]), [0, 0, 1]), 0.5);
      const hipGrip = vadd(chestP, qrotate(chestYawQ, [0.13 * k, 0.2 * k, -0.2 * k]));
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
      if (c.freeLeft) {
        // one hand on a long gun: it hangs lower, muzzle down
        rot = qnlerp(rot, qmul(chestYawQ, qeuler(-1.15, 0.1, 0.35)), 0.7);
        pos = vlerp(pos, vadd(chestP, qrotate(chestYawQ, [0.2 * k, 0.12 * k, -0.28 * k])), 0.7);
      }
    }
    // recoil, weapon sway
    const fwd = qrotate(rot, [0, 1, 0]);
    pos = vadd(pos, vscale(fwd, -(pistol ? 0.03 : 0.035) * this.kickBack.x));
    rot = qmul(rot, qx((pistol ? 0.07 : 0.05) * this.kickPitch.x));
    const bob = c.moving ? Math.sin(TAU * 2 * c.phase) * 0.008 * k * (1 - aw * 0.7) : 0;
    pos = vadd(pos, this.sway.update([0, 0, bob], dt));
    // action offsets of the weapon (reloads, a rifle jab)
    for (const [ch, w] of [
      [c.chP, c.wP],
      [c.chA, c.wA],
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
    const handR = qmul(rot, arms.gripR(pistol));
    const wristR = vsub(pos, qrotate(handR, arms.palmOffset('R')));
    const poleR = qrotate(chestQ, vnorm(pistol ? [0.5, -0.2, -0.9] : [0.7, -0.3, -0.75]));
    pose.r[H.clavicleR] = qeuler(0, 0, (pistol ? 0.08 : 0.12) * rw);
    pose.r[H.clavicleL] = qeuler(0, 0, -(pistol ? 0.08 : 0.18) * rw);
    fk.update(pose, H.clavicleL);
    solveTwoBone(pose, fk, H.upperarmR, H.forearmR, H.handR, wristR, poleR, 0.02, ELBOW_REST);
    setModelRotation(pose, fk, H.handR, handR);
    if (twoHanded) {
      const support = vadd(pos, qrotate(rot, prop.support));
      const handL = qmul(rot, arms.gripL(pistol));
      const wristL = vsub(support, qrotate(handL, arms.palmOffset('L')));
      const poleL = qrotate(chestQ, vnorm(pistol ? [-0.5, -0.2, -0.9] : [-0.7, 0.1, -0.8]));
      solveTwoBone(pose, fk, H.upperarmL, H.forearmL, H.handL, wristL, poleL, 0.04, ELBOW_REST);
      setModelRotation(pose, fk, H.handL, handL);
    }
    return true;
  }
}
