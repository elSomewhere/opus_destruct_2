/**
 * Hit reactions: what a blow does to a body, by where it lands and how hard.
 *
 * The impulse of a hit (point, direction, force, kind) becomes angular velocity on damped
 * springs of the struck bone and, weaker, the bones it hangs from (the blow travels through the
 * body), so the body answers the physics of the hit: a round in the right shoulder spins the
 * chest back to the right, a punch on the jaw snaps the head round and the neck follows, a
 * chest hit rocks the trunk back while the head lags forward. Reflexes are layered on top:
 * a gut hit doubles the body over and the hands go to the wound; a leg hit drops the pelvis
 * over the buckling knee and leaves a limp; hard blows knock the body back (a knockback
 * velocity the host applies, so the feet stumble real recovery steps) or down.
 *
 * Offsets are model-space rotation vectors, applied by the animator on top of the pose.
 */
import { Spring, Spring3 } from '../core/spring.ts';
import type { Skeleton } from '../core/skeleton.ts';
import { clamp, vcross, vdot, vlen, vnorm, vscale, vsub, type V3 } from '../math/vec.ts';
import { H } from './rig.ts';

export type HitKind = 'bullet' | 'blunt' | 'blade' | 'blast';
export type Zone = 'head' | 'chest' | 'gut' | 'pelvis' | 'armL' | 'armR' | 'legL' | 'legR';

export interface HitInfo {
  /** Where the blow lands and the direction it travels (world). */
  point: V3;
  dir: V3;
  /** ~1 a rifle round or a punch; 0.6 a pistol round; 1.8 a kick; 2.5 a shotgun; blasts up to 6. */
  force: number;
  kind: HitKind;
  /** The bone struck, if known (else the nearest to the point). */
  bone?: number;
}

/** Spring frequency and damping per bone: heavy segments answer slower. */
const TUNING: Partial<Record<number, [number, number]>> = {
  [H.pelvis]: [7.5, 0.55],
  [H.spine]: [8, 0.5],
  [H.chest]: [8.5, 0.48],
  [H.neck]: [10, 0.45],
  [H.head]: [11.5, 0.42],
  [H.clavicleL]: [11, 0.5],
  [H.clavicleR]: [11, 0.5],
  [H.upperarmL]: [8.5, 0.42],
  [H.upperarmR]: [8.5, 0.42],
  [H.forearmL]: [10, 0.4],
  [H.forearmR]: [10, 0.4],
};

/** Bones whose offsets are applied, top-down. */
export const REACTION_BONES: readonly number[] = [H.pelvis, H.spine, H.chest, H.neck, H.head, H.clavicleL, H.clavicleR, H.upperarmL, H.upperarmR, H.forearmL, H.forearmR];

export function zoneOf(bone: number): Zone {
  if (bone === H.head || bone === H.neck) return 'head';
  if (bone === H.chest) return 'chest';
  if (bone === H.spine) return 'gut';
  if (bone === H.pelvis || bone === H.root || bone === H.weapon) return 'pelvis';
  if (bone >= H.clavicleL && bone <= H.handL) return 'armL';
  if (bone >= H.clavicleR && bone <= H.handR) return 'armR';
  if (bone >= H.thighL && bone <= H.toeL) return 'legL';
  return 'legR';
}

export class Reactions {
  readonly springs = new Map<number, Spring3>();
  /** Visual pelvis displacement (m, model space) and the knockback velocity the host applies (m/s, world). */
  readonly shift = new Spring3(9, 0.55);
  readonly knockback: V3 = [0, 0, 0];
  /** 0..1: pain (hunched posture, slower), decays over seconds. */
  pain = 0;
  /** Forward fold of the trunk (gut hits, pain). */
  readonly fold = new Spring(9, 0.6);
  /** Limp per leg (0..1): recovers slowly. */
  readonly limp: [number, number] = [0, 0];
  /** The weapon arm is knocked off target (0..1 spring). */
  readonly aimOff = new Spring(10, 0.5);
  /** A hand held to a wound: the bone and the rest-space point, until `until` (seconds). */
  clutch: { bone: number; rest: V3; until: number; hand: 'L' | 'R' } | null = null;
  /** A knockdown was triggered (fall backwards if `back`). */
  down: { back: boolean } | null = null;
  /** Seconds since the last hit (reactions scale the idle behaviour down meanwhile). */
  sinceHit = 99;
  time = 0;

  constructor() {
    for (const b of REACTION_BONES) {
      const [w, z] = TUNING[b] ?? [12, 0.45];
      this.springs.set(b, new Spring3(w, z));
    }
  }

  /** Current rotation offset of a bone (model space rotation vector). */
  offset(b: number): V3 | null {
    return this.springs.get(b)?.x ?? null;
  }

  /**
   * A hit in MODEL space (point, dir of travel, the bone). `joint` gives bone head positions in
   * model space (for lever arms). Returns the zone.
   */
  hit(sk: Skeleton, joint: (b: number) => V3, point: V3, dir: V3, force: number, kind: HitKind, bone: number): Zone {
    this.sinceHit = 0;
    const zone = zoneOf(bone);
    const d = vnorm(dir);
    const f = clamp(force, 0, 8);
    const kick = (b: number, axis: V3, speed: number): void => {
      const s = this.springs.get(b);
      if (s) s.kick(axis, speed);
    };
    // angular impulse about the struck bone's joint: the torque r x dir (a blow through the
    // joint turns nothing; the further off it lands, the more it turns the bone)
    const torque = vcross(vsub(point, joint(bone)), d);
    const lever = clamp(vlen(torque) * 4, 0, 1.5);
    const axis = vnorm(torque, [1, 0, 0]);
    // the trunk is always pushed the way the blow travels (about its base)
    const tilt = vnorm(vcross([0, 0, 1], d), [1, 0, 0]);
    const kindGain = kind === 'blunt' ? 1.15 : kind === 'blade' ? 0.6 : kind === 'blast' ? 1.4 : 1;
    const g = f * kindGain;
    // propagate up the chain: the struck bone fully, parents less
    let b = bone;
    let w = 1;
    for (let k = 0; k < 5 && b >= 0; k++) {
      if (this.springs.has(b)) kick(b, axis, 3.5 * g * lever * w);
      b = sk.parents[b]!;
      w *= 0.5;
    }
    let kb = kind === 'bullet' ? 1.0 : kind === 'blunt' ? 1.2 : kind === 'blade' ? 0.35 : 2.6;
    switch (zone) {
      case 'head':
        kick(H.head, tilt, 6 * g);
        kick(H.neck, tilt, 2.2 * g);
        kick(H.chest, tilt, 1.2 * g);
        kick(H.spine, tilt, 0.5 * g);
        kb *= 0.8;
        break;
      case 'chest':
        kick(H.chest, tilt, 2 * g);
        kick(H.spine, tilt, 1.2 * g);
        kick(H.pelvis, tilt, 0.4 * g);
        // whiplash: the head lags
        kick(H.neck, tilt, -1.2 * g);
        kick(H.head, tilt, -1 * g);
        break;
      case 'gut':
        // the body folds over the blow, whatever its direction
        this.fold.kick(11 * g);
        kick(H.spine, tilt, 1.5 * g);
        kick(H.head, tilt, -1.5 * g);
        this.pain = Math.min(1, this.pain + 0.25 * g);
        break;
      case 'pelvis':
        kick(H.pelvis, tilt, 2.5 * g);
        kick(H.spine, tilt, -1 * g);
        break;
      case 'armL':
      case 'armR': {
        const side = zone === 'armL' ? 'L' : 'R';
        const ua = side === 'L' ? H.upperarmL : H.upperarmR;
        const fa = side === 'L' ? H.forearmL : H.forearmR;
        kick(ua, axis, 11 * g);
        kick(fa, axis, 12 * g);
        // the shoulder is spun back by the round
        kick(side === 'L' ? H.clavicleL : H.clavicleR, axis, 8 * g);
        kick(H.chest, [0, 0, side === 'L' ? -1 : 1], 4.5 * g * (d[1] < 0 ? 1 : -1));
        kick(H.spine, [0, 0, side === 'L' ? -1 : 1], 2 * g * (d[1] < 0 ? 1 : -1));
        if (side === 'R') this.aimOff.kick(6 * g);
        kb *= 0.6;
        break;
      }
      case 'legL':
      case 'legR': {
        const leg = zone === 'legL' ? 0 : 1;
        // the leg gives way: the pelvis drops over the buckling knee, rolling to that side
        this.shift.kick([0, 0, -2 * g]);
        kick(H.pelvis, [0, leg === 0 ? -1 : 1, 0], 4 * g);
        kick(H.spine, [0, leg === 0 ? 1 : -1, 0], 2 * g);
        kick(H.chest, tilt, -1.5 * g);
        this.limp[leg] = Math.min(1, this.limp[leg] + (kind === 'blunt' ? 0.1 : 0.35) * f);
        kb *= 0.5;
        break;
      }
    }
    // hands go to a wound (not to a punch on the arm)
    if ((kind === 'bullet' || kind === 'blade') && (zone === 'gut' || zone === 'chest' || zone === 'pelvis' || zone.startsWith('leg') || zone === 'armL' || zone === 'armR')) {
      const hand: 'L' | 'R' = zone === 'armL' ? 'R' : zone === 'armR' ? 'L' : 'L';
      this.clutch = { bone, rest: [0, 0, 0], until: this.time + 1.4 + 0.6 * f, hand };
    }
    // the body is shoved (knockback: the host moves the root, the feet stumble after it)
    const hx = d[0], hy = d[1];
    const hl = Math.hypot(hx, hy) || 1;
    const push = kb * f;
    this.knockback[0] += (hx / hl) * push;
    this.knockback[1] += (hy / hl) * push;
    this.shift.kick([(hx / hl) * push * 0.25, (hy / hl) * push * 0.25, 0]);
    this.pain = Math.min(1, this.pain + 0.12 * f);
    // hard blows knock the body down
    const hard = (kind === 'blunt' && f >= 2.1 && (zone === 'head' || zone === 'chest')) || (kind === 'blast' && f >= 2.5) || (kind === 'bullet' && f >= 2.4 && zone.startsWith('leg'));
    if (hard) this.down = { back: vdot(d, [0, 1, 0]) < 0.3 };
    return zone;
  }

  update(dt: number): void {
    this.time += dt;
    this.sinceHit += dt;
    for (const s of this.springs.values()) s.update([0, 0, 0], dt);
    this.shift.update([0, 0, 0], dt);
    this.fold.update(this.pain * 0.5, dt);
    this.aimOff.update(0, dt);
    const decay = Math.exp(-dt * 3.5);
    this.knockback[0] *= decay;
    this.knockback[1] *= decay;
    this.pain = Math.max(0, this.pain - dt * 0.06);
    this.limp[0] = Math.max(0, this.limp[0] - dt * 0.004);
    this.limp[1] = Math.max(0, this.limp[1] - dt * 0.004);
    if (this.clutch && this.time > this.clutch.until) this.clutch = null;
  }

  /** Knockback displacement for this frame (m, world) — hosts move the root by it. */
  takeKnockback(dt: number): V3 {
    return vscale(this.knockback, dt);
  }
}
