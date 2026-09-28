/**
 * The physical humanoid: the rig as 16 rigid bodies joined by 15 muscled joints (physics/
 * rigid.ts), the body a character is made of.
 *
 * - Bodies: pelvis, abdomen (spine), chest (with the clavicles), head (with the neck), upper
 *   arms, forearms, hands, thighs, shins, feet. Masses follow anthropometric segment fractions
 *   (Dempster/Winter) of the character's weight, with centres of mass along the segments, box
 *   and rod inertias, and collision spheres shaped like the voxel body.
 * - Joints have human ranges: the spine and neck bend further forward than back, hips flex far
 *   forward and little back, shoulders swing wide but not far behind, knees and elbows are
 *   hinges that bend one way, ankles and wrists give a little.
 * - Muscles: every joint drives towards the relative rotation of a target pose (the motion
 *   plan), with a stiffness set from the inertia it moves (so every joint answers at a chosen
 *   frequency), scaled by a per-joint `tone` a controller sets (a shot leg goes weak, a dying
 *   body goes limp), and gravity compensation (muscles hold the limbs' weight, so low tone
 *   still tracks the pose; without tone the limbs drop).
 * - Assists: the pelvis held up (vertical), steered (horizontal) and turned (the balance a
 *   controller grants the legs), planted feet pinned, hands pulled to targets (a grip, a wall).
 *
 * A body's frame is its primary bone's frame (every bone's rest rotation is the identity), its
 * position the centre of mass: bone head = x - q (com - restHead).
 */
import { Pose, WorldPose, type Skeleton } from '../core/skeleton.ts';
import { qconj, qmul, qnlerp, qrotate, type Quat } from '../math/quat.ts';
import { vcross, vlerp, vnorm, vsub, type V3 } from '../math/vec.ts';
import type { CollisionWorld } from '../physics/collision.ts';
import { Attachment, Joint, Orienter, RigidBody, RigidSystem, qerror, type Sphere, type SwingLimits } from '../physics/rigid.ts';
import { H } from '../humanoid/rig.ts';

/** Body indices. */
export const B = {
  pelvis: 0,
  spine: 1,
  chest: 2,
  head: 3,
  upperarmL: 4,
  forearmL: 5,
  handL: 6,
  upperarmR: 7,
  forearmR: 8,
  handR: 9,
  thighL: 10,
  shinL: 11,
  footL: 12,
  thighR: 13,
  shinR: 14,
  footR: 15,
} as const;
export const BODY_COUNT = 16;

/** The rig bone each body moves. */
export const BODY_BONE: readonly number[] = [
  H.pelvis, H.spine, H.chest, H.head,
  H.upperarmL, H.forearmL, H.handL, H.upperarmR, H.forearmR, H.handR,
  H.thighL, H.shinL, H.footL, H.thighR, H.shinR, H.footR,
];

/** Each body's parent body (-1: the pelvis). Joint i connects PARENT[i] and body i (i >= 1). */
export const BODY_PARENT: readonly number[] = [-1, B.pelvis, B.spine, B.chest, B.chest, B.upperarmL, B.forearmL, B.chest, B.upperarmR, B.forearmR, B.pelvis, B.thighL, B.shinL, B.pelvis, B.thighR, B.shinR];

/** Body regions, for tone. */
export type Region = 'trunk' | 'neck' | 'armL' | 'armR' | 'legL' | 'legR';
export const REGION: readonly Region[] = ['trunk', 'trunk', 'trunk', 'neck', 'armL', 'armL', 'armL', 'armR', 'armR', 'armR', 'legL', 'legL', 'legL', 'legR', 'legR', 'legR'];

/** Segment mass fractions (Winter): head+neck, trunk thirds, arm, leg segments. */
const MASS: readonly number[] = [0.142, 0.139, 0.216, 0.081, 0.028, 0.016, 0.006, 0.028, 0.016, 0.006, 0.1, 0.0465, 0.0145, 0.1, 0.0465, 0.0145];

/** Joint muscle frequency (rad/s) and damping ratio by child body. */
const MUSCLE: readonly [number, number][] = [
  [0, 0],
  [15, 0.95], // lower back
  [15, 0.95], // upper back
  [13, 0.8], // neck
  [12, 0.75], // shoulder
  [13, 0.75], // elbow
  [12, 0.8], // wrist
  [12, 0.75],
  [13, 0.75],
  [12, 0.8],
  [17, 0.95], // hip
  [18, 0.95], // knee
  [16, 0.95], // ankle
  [17, 0.95],
  [18, 0.95],
  [16, 0.95],
];

export interface HumanoidBodyOptions {
  /** Total mass (kg); default 75 kg scaled with the rig's height cubed. */
  mass?: number;
  /** Body thickness (collision radii). */
  girth?: number;
}

/** Where a foot's sole rests relative to its ankle (rest model space, per unit height). */
const SOLE_DROP = 0.085;

export class HumanoidBody {
  readonly skeleton: Skeleton;
  readonly system: RigidSystem;
  readonly parts: RigidBody[] = [];
  /** joints[i] connects BODY_PARENT[i] and body i (joints[0] is null). */
  readonly joints: (Joint | null)[] = [null];
  /** Muscle tone per body's joint (0 limp .. 1 normal .. >1 tense). */
  readonly tone = new Float32Array(BODY_COUNT).fill(1);
  /** Gravity compensation per joint (0..1): the muscles hold the limb's weight. */
  readonly holdWeight = new Float32Array(BODY_COUNT).fill(1);
  /** Base stiffness (N m/rad) and damping (N m s/rad) per joint at tone 1. */
  readonly baseStiffness = new Float32Array(BODY_COUNT);
  readonly baseDamping = new Float32Array(BODY_COUNT);
  /** Inertia each joint moves (its subtree about the joint, kg m^2). */
  readonly inertiaAt = new Float32Array(BODY_COUNT);
  /** The plan's relative angular acceleration per joint (parent frame, rad/s^2). */
  private readonly targetAcc: V3[] = Array.from({ length: BODY_COUNT }, () => [0, 0, 0] as V3);
  /**
   * How much of the plan's acceleration the muscles supply ahead of the error (0..1). Off:
   * differentiated twice from frame to frame (and from a plan the physics feeds back into), it
   * is mostly noise, and the body trembles with it.
   */
  feedForward = 0;
  /** Centre of mass relative to the primary bone's head, per body (rest model space). */
  readonly comLocal: V3[] = [];
  readonly totalMass: number;
  readonly k: number;
  // assists
  readonly support: Attachment;
  readonly steer: Attachment;
  readonly upright: Orienter;
  readonly chestTurn: Orienter;
  readonly feet: [Attachment, Attachment];
  readonly feetTurn: [Orienter, Orienter];
  readonly hands: [Attachment, Attachment];
  readonly handsTurn: [Orienter, Orienter];
  /** Local rotations the physics does not have (neck share, clavicles, toes), from the plan. */
  private readonly extras: Pose;
  private readonly tmpQ: Quat = [0, 0, 0, 1];

  constructor(skeleton: Skeleton, collision: CollisionWorld | null, o: HumanoidBodyOptions = {}) {
    this.skeleton = skeleton;
    const rh = skeleton.restHead;
    const rt = skeleton.restTail;
    const k = rh[H.pelvis]![2] / 0.97;
    this.k = k;
    const girth = o.girth ?? 1;
    const total = o.mass ?? 75 * k * k * k * girth;
    this.totalMass = total;
    this.system = new RigidSystem(collision, { maxSubstep: 1 / 480, margin: 0.02 * k });
    this.extras = new Pose(skeleton);
    const mid = (a: Readonly<V3>, b: Readonly<V3>, t: number): V3 => vlerp(a, b, t);

    // ---- bodies: centre of mass, inertia, spheres (rest model space) ----
    const coms: V3[] = [];
    const inert: V3[] = [];
    const sph: Sphere[][] = [];
    const rod = (m: number, a: Readonly<V3>, b: Readonly<V3>, r: number): V3 => {
      // a solid rod's inertia about its centre, the diagonal in the model frame
      const d = vsub(b, a);
      const L = Math.hypot(d[0], d[1], d[2]);
      const u = vnorm(d);
      const perp = (m * (3 * r * r + L * L)) / 12;
      const along = 0.5 * m * r * r;
      return [perp + (along - perp) * u[0] * u[0], perp + (along - perp) * u[1] * u[1], perp + (along - perp) * u[2] * u[2]];
    };
    const box = (m: number, w: number, d: number, h: number): V3 => [(m * (d * d + h * h)) / 12, (m * (w * w + h * h)) / 12, (m * (w * w + d * d)) / 12];
    const g = girth;
    for (let i = 0; i < BODY_COUNT; i++) {
      const m = MASS[i]! * total;
      let com: V3;
      let I: V3;
      let spheres: { at: V3; r: number }[];
      switch (i) {
        case B.pelvis: {
          com = [0, 0, rh[H.pelvis]![2] + 0.01 * k];
          I = box(m, 0.32 * k * g, 0.2 * k * g, 0.2 * k);
          spheres = [
            { at: [rh[H.thighL]![0] * 0.8, 0, rh[H.thighL]![2] + 0.03 * k], r: 0.1 * k * g },
            { at: [rh[H.thighR]![0] * 0.8, 0, rh[H.thighR]![2] + 0.03 * k], r: 0.1 * k * g },
          ];
          break;
        }
        case B.spine: {
          com = mid(rh[H.spine]!, rh[H.chest]!, 0.5);
          I = box(m, 0.3 * k * g, 0.19 * k * g, 0.15 * k);
          spheres = [{ at: [0, 0, com[2]], r: 0.115 * k * g }];
          break;
        }
        case B.chest: {
          com = mid(rh[H.chest]!, rh[H.neck]!, 0.45);
          I = box(m, 0.36 * k * g, 0.21 * k * g, 0.25 * k);
          spheres = [
            { at: [0, 0.005 * k, rh[H.chest]![2] + 0.07 * k], r: 0.125 * k * g },
            { at: [rh[H.upperarmL]![0] * 0.55, -0.01 * k, rh[H.upperarmL]![2] - 0.03 * k], r: 0.085 * k * g },
            { at: [rh[H.upperarmR]![0] * 0.55, -0.01 * k, rh[H.upperarmR]![2] - 0.03 * k], r: 0.085 * k * g },
          ];
          break;
        }
        case B.head: {
          com = mid(rh[H.neck]!, rt[H.head]!, 0.6);
          I = box(m, 0.17 * k, 0.2 * k, 0.24 * k);
          spheres = [{ at: [0, 0.02 * k, rh[H.head]![2] + 0.1 * k], r: 0.1 * k }];
          break;
        }
        default: {
          const bone = BODY_BONE[i]!;
          const a = rh[bone]!;
          const isFoot = i === B.footL || i === B.footR;
          const isHand = i === B.handL || i === B.handR;
          const end = isFoot ? rt[bone === H.footL ? H.toeL : H.toeR]! : bone === H.handL || bone === H.handR ? rt[bone]! : rt[bone]!;
          if (isFoot) {
            const x = a[0];
            com = [x, (a[1] + end[1]) * 0.5 - 0.01 * k, 0.045 * k];
            I = box(m, 0.09 * k, 0.24 * k, 0.08 * k);
            spheres = [
              { at: [x, a[1] - 0.035 * k, 0.04 * k], r: 0.04 * k },
              { at: [x, end[1] - 0.075 * k, 0.034 * k], r: 0.034 * k },
              { at: [x, end[1] - 0.02 * k, 0.022 * k], r: 0.022 * k },
            ];
          } else if (isHand) {
            com = mid(a, end, 0.4);
            I = rod(m, a, end, 0.04 * k);
            spheres = [{ at: mid(a, end, 0.45), r: 0.045 * k }];
          } else {
            const upper = i === B.upperarmL || i === B.upperarmR || i === B.thighL || i === B.thighR;
            const leg = i >= B.thighL;
            const r = (leg ? (upper ? 0.075 : 0.052) : upper ? 0.048 : 0.042) * k * g;
            com = mid(a, end, 0.43);
            I = rod(m, a, end, r);
            spheres = [
              { at: mid(a, end, upper ? 0.3 : 0.25), r },
              { at: mid(a, end, upper ? 0.72 : 0.7), r: r * (leg ? 0.85 : 0.92) },
            ];
          }
        }
      }
      coms.push(com);
      inert.push(I);
      sph.push(spheres.map((s) => ({ c: vsub(s.at, com), r: s.r })));
    }
    for (let i = 0; i < BODY_COUNT; i++) {
      const body = new RigidBody(MASS[i]! * total, inert[i]!, coms[i]!, [0, 0, 0, 1], sph[i]!);
      body.friction = i === B.footL || i === B.footR ? 1.0 : 0.75;
      if (i >= B.upperarmL) {
        // limbs resist spinning about their length
        const bone = BODY_BONE[i]!;
        const ax = vnorm(vsub(i === B.footL || i === B.footR ? rh[bone === H.footL ? H.toeL : H.toeR]! : rt[bone]!, rh[bone]!));
        body.longAxis[0] = ax[0];
        body.longAxis[1] = ax[1];
        body.longAxis[2] = ax[2];
        body.twistDamping = 25;
      }
      this.system.add(body);
      this.parts.push(body);
      this.comLocal.push(vsub(coms[i]!, rh[BODY_BONE[i]!]!));
    }

    // ---- joints ----
    const fwd: V3 = [0, 1, 0];
    const up: V3 = [0, 0, 1];
    for (let i = 1; i < BODY_COUNT; i++) {
      const p = BODY_PARENT[i]!;
      const bone = BODY_BONE[i]!;
      // the joint: at the child bone's head (the head body turns at the neck's base)
      const at = i === B.head ? rh[H.neck]! : rh[bone]!;
      const tail = i === B.head ? rt[H.head]! : i === B.footL ? rh[H.toeL]! : i === B.footR ? rh[H.toeR]! : rt[bone]!;
      const left = rh[bone]![0] < 0;
      const side = left ? -1 : 1;
      let z: V3 = vnorm(vsub(tail, at));
      let frame: Quat;
      let kind: 'ball' | 'hinge' = 'ball';
      let swing: SwingLimits | undefined;
      let twist: [number, number] | undefined;
      let hinge: [number, number] | undefined;
      const D = Math.PI / 180;
      /** Anatomical swing limits for a frame whose y is `yDir` (forward) and x = y x z. */
      const anat = (xAxis: V3, o: { fwd: number; back: number; out: number; in: number }): SwingLimits => {
        // +x of the frame is outward if it points to the body's side
        const outPos = xAxis[0] * side > 0;
        return { yPos: o.fwd * D, yNeg: o.back * D, xPos: (outPos ? o.out : o.in) * D, xNeg: (outPos ? o.in : o.out) * D };
      };
      const frameOf = (zAxis: V3, yHint: V3): { q: Quat; x: V3 } => {
        const x = vnorm(vcross(yHint, zAxis));
        const y = vcross(zAxis, x);
        return { q: basisQuat(x, y, zAxis), x };
      };
      switch (i) {
        case B.spine:
        case B.chest:
        case B.head: {
          z = up;
          const f = frameOf(up, fwd);
          frame = f.q;
          if (i === B.head) {
            swing = { yPos: 55 * D, yNeg: 50 * D, xPos: 40 * D, xNeg: 40 * D };
            twist = [-70 * D, 70 * D];
          } else {
            swing = i === B.spine ? { yPos: 45 * D, yNeg: 25 * D, xPos: 28 * D, xNeg: 28 * D } : { yPos: 35 * D, yNeg: 25 * D, xPos: 22 * D, xNeg: 22 * D };
            twist = i === B.spine ? [-25 * D, 25 * D] : [-30 * D, 30 * D];
          }
          break;
        }
        case B.upperarmL:
        case B.upperarmR: {
          const f = frameOf(z, fwd);
          frame = f.q;
          swing = anat(f.x, { fwd: 160, back: 55, out: 150, in: 45 });
          twist = [-85 * D, 85 * D];
          break;
        }
        case B.forearmL:
        case B.forearmR: {
          // a hinge: the forearm swings forward (positive about x)
          kind = 'hinge';
          const upper = vsub(rh[bone]!, rh[BODY_BONE[p]!]!);
          const x = vnorm(vcross(upper, fwd));
          const zz = vnorm(vsub(z, vscaleL(x, x[0] * z[0] + x[1] * z[1] + x[2] * z[2])));
          frame = basisQuat(x, vcross(zz, x), zz);
          hinge = [-3 * D, 150 * D];
          break;
        }
        case B.handL:
        case B.handR: {
          const f = frameOf(z, fwd);
          frame = f.q;
          swing = { yPos: 70 * D, yNeg: 70 * D, xPos: 35 * D, xNeg: 35 * D };
          twist = [-80 * D, 80 * D];
          break;
        }
        case B.thighL:
        case B.thighR: {
          const f = frameOf(z, fwd);
          frame = f.q;
          swing = anat(f.x, { fwd: 125, back: 40, out: 55, in: 30 });
          twist = [-45 * D, 45 * D];
          break;
        }
        case B.shinL:
        case B.shinR: {
          // a hinge: the shin swings back (negative about +x)
          kind = 'hinge';
          const x: V3 = [1, 0, 0];
          const zz = vnorm(vsub(z, vscaleL(x, z[0])));
          frame = basisQuat(x, vcross(zz, x), zz);
          hinge = [-150 * D, 3 * D];
          break;
        }
        default: {
          // ankle: z along the foot, x across it
          const x: V3 = [1, 0, 0];
          const zz = vnorm(vsub(z, vscaleL(x, z[0])));
          frame = basisQuat(x, vcross(zz, x), zz);
          // +y of this frame points down and back: toes down is a tilt towards +y
          swing = { yPos: 50 * D, yNeg: 30 * D, xPos: 28 * D, xNeg: 28 * D };
          twist = [-30 * D, 30 * D];
        }
      }
      const pc = coms[p]!, cc = coms[i]!;
      const j = new Joint(this.parts[p]!, this.parts[i]!, {
        kind,
        anchorA: vsub(at, pc),
        anchorB: vsub(at, cc),
        frameA: frame,
        frameB: frame,
        ...(swing ? { swing } : {}),
        ...(twist ? { twist } : {}),
        ...(hinge ? { hinge } : {}),
      });
      this.system.addJoint(j);
      this.joints.push(j);
    }
    // muscle stiffness from the inertia each joint moves
    for (let i = 1; i < BODY_COUNT; i++) {
      const jp = this.jointPoint(i);
      let I = 0;
      for (const c of this.subtree(i)) {
        const d = vsub(coms[c]!, jp);
        const ii = inert[c]!;
        I += MASS[c]! * total * (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + (ii[0] + ii[1] + ii[2]) / 3;
      }
      const [w, z] = MUSCLE[i]!;
      this.inertiaAt[i] = I;
      this.baseStiffness[i] = I * w * w;
      this.baseDamping[i] = 2 * z * w * I;
      this.joints[i]!.effInertia = I;
    }
    // bodies that must not pass through each other
    const pair = (a: number, b: number): void => {
      const A = this.parts[a]!, Bb = this.parts[b]!;
      for (let sa = 0; sa < A.spheres.length; sa++) for (let sb = 0; sb < Bb.spheres.length; sb++) this.system.pairs.push({ a: A, sa, b: Bb, sb });
    };
    for (const arm of [B.forearmL, B.handL, B.forearmR, B.handR, B.upperarmL, B.upperarmR]) {
      for (const t of [B.pelvis, B.spine, B.chest, B.head]) {
        if ((arm === B.upperarmL || arm === B.upperarmR) && t === B.chest) continue;
        pair(arm, t);
      }
    }
    for (const a of [B.forearmL, B.handL]) for (const b of [B.forearmR, B.handR]) pair(a, b);
    for (const a of [B.thighL, B.shinL, B.footL]) for (const b of [B.thighR, B.shinR, B.footR]) pair(a, b);
    for (const a of [B.handL, B.handR, B.forearmL, B.forearmR]) for (const b of [B.thighL, B.thighR, B.shinL, B.shinR]) pair(a, b);

    // ---- assists ----
    const pel = this.parts[B.pelvis]!;
    const pelvisHead = vsub(rh[H.pelvis]!, coms[B.pelvis]!);
    this.support = this.system.attach(pel, pelvisHead);
    this.support.axes[0] = false;
    this.support.axes[1] = false;
    this.steer = this.system.attach(pel, pelvisHead);
    this.steer.axes[2] = false;
    this.upright = this.system.orienter(pel);
    this.chestTurn = this.system.orienter(this.parts[B.chest]!);
    const footLocal = (b: number): V3 => vsub(rh[BODY_BONE[b]!]!, coms[b]!);
    this.feet = [this.system.attach(this.parts[B.footL]!, footLocal(B.footL)), this.system.attach(this.parts[B.footR]!, footLocal(B.footR))];
    this.feetTurn = [this.system.orienter(this.parts[B.footL]!), this.system.orienter(this.parts[B.footR]!)];
    this.hands = [this.system.attach(this.parts[B.handL]!, footLocal(B.handL)), this.system.attach(this.parts[B.handR]!, footLocal(B.handR))];
    this.handsTurn = [this.system.orienter(this.parts[B.handL]!), this.system.orienter(this.parts[B.handR]!)];
  }

  /** Bodies below (and including) body i. */
  subtree(i: number): number[] {
    const out = [i];
    for (let c = i + 1; c < BODY_COUNT; c++) if (out.includes(BODY_PARENT[c]!)) out.push(c);
    return out;
  }

  /** The rest position of joint i (model space). */
  jointPoint(i: number): V3 {
    const rh = this.skeleton.restHead;
    return i === B.head ? [...rh[H.neck]!] : [...rh[BODY_BONE[i]!]!];
  }

  /** Sole height below the ankle at rest. */
  get soleDrop(): number {
    return SOLE_DROP * this.k;
  }

  // ---- pose <-> bodies --------------------------------------------------------------------------

  /**
   * Places the bodies on a pose (`world`), with velocities from the pose `dt` seconds earlier
   * (`prev`), so a body taking over from animation keeps its momentum.
   */
  setFromPose(world: WorldPose, prev: WorldPose | null, dt: number): void {
    const x: V3 = [0, 0, 0];
    for (let i = 0; i < BODY_COUNT; i++) {
      const b = this.parts[i]!;
      const bone = BODY_BONE[i]!;
      this.bodyAt(world, i, x);
      b.x[0] = x[0];
      b.x[1] = x[1];
      b.x[2] = x[2];
      const q = world.q[bone]!;
      b.q[0] = q[0];
      b.q[1] = q[1];
      b.q[2] = q[2];
      b.q[3] = q[3];
      if (prev && dt > 0) {
        const xp = this.bodyAt(prev, i, [0, 0, 0]);
        b.v[0] = clampV((x[0] - xp[0]) / dt);
        b.v[1] = clampV((x[1] - xp[1]) / dt);
        b.v[2] = clampV((x[2] - xp[2]) / dt);
        const e = qerror(q, prev.q[bone]!);
        b.w[0] = clampV(e[0] / dt, 30);
        b.w[1] = clampV(e[1] / dt, 30);
        b.w[2] = clampV(e[2] / dt, 30);
      } else {
        b.v[0] = b.v[1] = b.v[2] = 0;
        b.w[0] = b.w[1] = b.w[2] = 0;
      }
    }
    this.system.wake();
  }

  /** Where body i's centre of mass is in a pose. */
  bodyAt(world: WorldPose, i: number, out: V3): V3 {
    const bone = BODY_BONE[i]!;
    qrotate(world.q[bone]!, this.comLocal[i]!, out);
    const p = world.p[bone]!;
    out[0] += p[0];
    out[1] += p[1];
    out[2] += p[2];
    return out;
  }

  /**
   * Muscle targets from a target pose (world rotations of the bones; only relative rotations
   * matter) and, from the target `dt` seconds before, how fast they move; and the local
   * rotations the physics lacks (clavicles, the neck's share, toes).
   */
  track(target: WorldPose, local: Pose, prev: WorldPose | null = null, dt = 0): void {
    const q = target.q;
    const prevRel: Quat = [0, 0, 0, 1];
    for (let i = 1; i < BODY_COUNT; i++) {
      const j = this.joints[i]!;
      const pa = q[BODY_BONE[BODY_PARENT[i]!]!]!;
      const ch = q[BODY_BONE[i]!]!;
      if (prev && dt > 0) {
        // the relative rotation's rate, in the parent's frame
        qmul(qconj(prev.q[BODY_BONE[BODY_PARENT[i]!]!]!, this.tmpQ), prev.q[BODY_BONE[i]!]!, prevRel);
        qmul(qconj(pa, this.tmpQ), ch, j.target);
        // R = qA^-1 qB turns at w (in A's frame) when R(t + dt) = exp(w dt) R(t)
        const d = qerror(j.target, prevRel);
        const vx = clampV(d[0] / dt, 30), vy = clampV(d[1] / dt, 30), vz = clampV(d[2] / dt, 30);
        // the plan's angular acceleration at the joint (smoothed: frame differences are noisy)
        const acc = this.targetAcc[i]!;
        const a = 1 - Math.exp(-dt * 25);
        acc[0] += (clampV((vx - j.targetVel[0]) / dt, 200) - acc[0]) * a;
        acc[1] += (clampV((vy - j.targetVel[1]) / dt, 200) - acc[1]) * a;
        acc[2] += (clampV((vz - j.targetVel[2]) / dt, 200) - acc[2]) * a;
        j.targetVel[0] = vx;
        j.targetVel[1] = vy;
        j.targetVel[2] = vz;
      } else {
        qmul(qconj(pa, this.tmpQ), ch, j.target);
        j.targetVel[0] = j.targetVel[1] = j.targetVel[2] = 0;
        this.targetAcc[i]!.fill(0);
      }
    }
    // shoulders move with the clavicles
    const rh = this.skeleton.restHead;
    const chestQ = q[H.chest]!, chestP = target.p[H.chest]!;
    for (const [bi, bone] of [
      [B.upperarmL, H.upperarmL],
      [B.upperarmR, H.upperarmR],
    ] as const) {
      const j = this.joints[bi]!;
      const w = target.p[bone]!;
      const d = qrotate(qconj(chestQ), [w[0] - chestP[0], w[1] - chestP[1], w[2] - chestP[2]]);
      const com = this.comLocal[B.chest]!;
      // anchor = (head offset in the chest's rest frame) - (com - chest head)
      j.anchorA[0] = d[0] - com[0];
      j.anchorA[1] = d[1] - com[1];
      j.anchorA[2] = d[2] - com[2];
      void rh;
    }
    this.extras.copyFrom(local);
  }

  /** Sets muscle stiffness and damping from tone. */
  applyTone(): void {
    for (let i = 1; i < BODY_COUNT; i++) {
      const j = this.joints[i]!;
      const t = Math.max(0, this.tone[i]!);
      j.stiffness = this.baseStiffness[i]! * t;
      // a limp joint keeps some damping (tissue), a tense one more
      j.damping = this.baseDamping[i]! * (0.5 + 0.5 * Math.min(1.5, t));
    }
  }

  /** Feed-forward muscle torques that hold each limb's weight (scaled by holdWeight). */
  compensateGravity(): void {
    const g = this.system.gravity;
    const parts = this.parts;
    // subtree mass and weighted centre, children before parents
    const m = SUB_M, cx = SUB_X, cy = SUB_Y, cz = SUB_Z;
    for (let i = BODY_COUNT - 1; i >= 0; i--) {
      const b = parts[i]!;
      m[i] = b.mass;
      cx[i] = b.x[0] * b.mass;
      cy[i] = b.x[1] * b.mass;
      cz[i] = b.x[2] * b.mass;
    }
    for (let i = BODY_COUNT - 1; i >= 1; i--) {
      const p = BODY_PARENT[i]!;
      m[p]! += m[i]!;
      cx[p]! += cx[i]!;
      cy[p]! += cy[i]!;
      cz[p]! += cz[i]!;
    }
    const jp: V3 = [0, 0, 0];
    const acc: V3 = [0, 0, 0];
    for (let i = 1; i < BODY_COUNT; i++) {
      const j = this.joints[i]!;
      const tone = Math.min(1, Math.max(0, this.tone[i]!));
      const hw = this.holdWeight[i]! * tone;
      j.feed[0] = j.feed[1] = j.feed[2] = 0;
      // the torque that swings the limb along the plan: I alpha, in the world
      const ff = this.feedForward * tone * this.inertiaAt[i]!;
      if (ff > 0) {
        const a = this.targetAcc[i]!;
        qrotate(j.a.q, a, acc);
        j.feed[0] = acc[0] * ff;
        j.feed[1] = acc[1] * ff;
        j.feed[2] = acc[2] * ff;
      }
      if (hw <= 0) continue;
      parts[i]!.point(j.anchorB, jp);
      const mm = m[i]!;
      const rx = cx[i]! / mm - jp[0], ry = cy[i]! / mm - jp[1];
      // torque on B that cancels gravity's: r x (0, 0, m g)
      const f = mm * g * hw;
      j.feed[0] += ry * f;
      j.feed[1] += -rx * f;
    }
  }

  /**
   * Writes the bodies' pose into `out` (all 23 bones): bodies give their bones; the neck
   * takes a share of the head's turn; clavicles, toes and the root follow their parents with
   * the plan's local rotations.
   */
  writePose(out: WorldPose): void {
    const sk = this.skeleton;
    const rh = sk.restHead;
    const tmp: V3 = [0, 0, 0];
    for (let i = 0; i < BODY_COUNT; i++) {
      const b = this.parts[i]!;
      const bone = BODY_BONE[i]!;
      const q = out.q[bone]!;
      q[0] = b.q[0];
      q[1] = b.q[1];
      q[2] = b.q[2];
      q[3] = b.q[3];
      qrotate(b.q, this.comLocal[i]!, tmp);
      out.p[bone] = [b.x[0] - tmp[0], b.x[1] - tmp[1], b.x[2] - tmp[2]];
    }
    // root: under the pelvis
    const pq = out.q[H.pelvis]!;
    const rootOff = qrotate(pq, rh[H.pelvis]!);
    out.p[H.root] = vsub(out.p[H.pelvis]!, rootOff);
    out.q[H.root] = [...pq];
    // neck: from the chest, turned part of the way to the head
    const cq = out.q[H.chest]!;
    const neckQ = qnlerp(cq, out.q[H.head]!, 0.45);
    out.q[H.neck] = neckQ;
    out.p[H.neck] = vadd3(out.p[H.chest]!, qrotate(cq, vsub(rh[H.neck]!, rh[H.chest]!)));
    // the head hangs from the physical neck base (keeps it on the neck)
    const headBase = vadd3(out.p[H.neck]!, qrotate(neckQ, vsub(rh[H.head]!, rh[H.neck]!)));
    out.p[H.head] = headBase;
    // clavicles: the plan's local rotation on the chest
    for (const c of [H.clavicleL, H.clavicleR]) {
      out.q[c] = qmul(cq, this.extras.r[c]!);
      out.p[c] = vadd3(out.p[H.chest]!, qrotate(cq, vsub(rh[c]!, rh[H.chest]!)));
    }
    // toes on the feet
    for (const [t, f] of [
      [H.toeL, H.footL],
      [H.toeR, H.footR],
    ] as const) {
      const fq = out.q[f]!;
      out.q[t] = qmul(fq, this.extras.r[t]!);
      out.p[t] = vadd3(out.p[f]!, qrotate(fq, vsub(rh[t]!, rh[f]!)));
    }
  }

  // ---- sensing ----------------------------------------------------------------------------------

  /** Centre of mass (world). */
  com(out: V3 = [0, 0, 0]): V3 {
    let x = 0, y = 0, z = 0;
    for (const b of this.parts) {
      x += b.x[0] * b.mass;
      y += b.x[1] * b.mass;
      z += b.x[2] * b.mass;
    }
    out[0] = x / this.totalMass;
    out[1] = y / this.totalMass;
    out[2] = z / this.totalMass;
    return out;
  }

  /** Velocity of the centre of mass (world). */
  comVelocity(out: V3 = [0, 0, 0]): V3 {
    let x = 0, y = 0, z = 0;
    for (const b of this.parts) {
      x += b.v[0] * b.mass;
      y += b.v[1] * b.mass;
      z += b.v[2] * b.mass;
    }
    out[0] = x / this.totalMass;
    out[1] = y / this.totalMass;
    out[2] = z / this.totalMass;
    return out;
  }

  /** Adds a velocity change `dv` to every body (a shove that moves the whole body). */
  shove(dvx: number, dvy: number, dvz: number, share: readonly number[] | null = null): void {
    for (let i = 0; i < BODY_COUNT; i++) {
      const s = share ? share[i]! : 1;
      const b = this.parts[i]!;
      b.v[0] += dvx * s;
      b.v[1] += dvy * s;
      b.v[2] += dvz * s;
    }
    this.system.wake();
  }

  /**
   * The body's collision spheres in the world (appended to `out`), from the bodies or, when
   * given, from a pose (a body resting on its plan): what others bump into and trip over.
   */
  spheresOf(pose: WorldPose | null, out: { c: V3; r: number; body?: RigidBody }[]): void {
    const com: V3 = [0, 0, 0];
    for (let i = 0; i < BODY_COUNT; i++) {
      const b = this.parts[i]!;
      if (b.ghost) continue;
      let q: Readonly<Quat> = b.q;
      if (pose) {
        const bone = BODY_BONE[i]!;
        q = pose.q[bone]!;
        this.bodyAt(pose, i, com);
      } else {
        com[0] = b.x[0];
        com[1] = b.x[1];
        com[2] = b.x[2];
      }
      for (const s of b.spheres) {
        const c = qrotate(q, s.c);
        out.push({ c: [com[0] + c[0], com[1] + c[1], com[2] + c[2]], r: s.r, body: b });
      }
    }
  }

  /** The body whose collision spheres are nearest to a world point. */
  nearestPart(p: Readonly<V3>): number {
    let best: number = B.chest, bd = Infinity;
    const c: V3 = [0, 0, 0];
    for (let i = 0; i < BODY_COUNT; i++) {
      const b = this.parts[i]!;
      for (const s of b.spheres) {
        b.point(s.c, c);
        const d = Math.hypot(c[0] - p[0], c[1] - p[1], c[2] - p[2]) - s.r;
        if (d < bd) {
          bd = d;
          best = i;
        }
      }
    }
    return best;
  }

  /** The body that moves a rig bone (the neck and clavicles belong to their neighbours). */
  static bodyOfBone(bone: number): number {
    const i = BODY_BONE.indexOf(bone);
    if (i >= 0) return i;
    if (bone === H.neck) return B.head;
    if (bone === H.clavicleL || bone === H.clavicleR) return B.chest;
    if (bone === H.toeL) return B.footL;
    if (bone === H.toeR) return B.footR;
    return B.pelvis;
  }
}

const SUB_M = new Float64Array(BODY_COUNT);
const SUB_X = new Float64Array(BODY_COUNT);
const SUB_Y = new Float64Array(BODY_COUNT);
const SUB_Z = new Float64Array(BODY_COUNT);

function clampV(v: number, max = 25): number {
  return v > max ? max : v < -max ? -max : v;
}

function vscaleL(a: Readonly<V3>, k: number): V3 {
  return [a[0] * k, a[1] * k, a[2] * k];
}

function vadd3(a: Readonly<V3>, b: Readonly<V3>): V3 {
  return [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
}

/** The rotation whose columns are the right-handed orthonormal basis (x, y, z). */
function basisQuat(x: Readonly<V3>, y: Readonly<V3>, z: Readonly<V3>): Quat {
  const m00 = x[0], m10 = x[1], m20 = x[2];
  const m01 = y[0], m11 = y[1], m21 = y[2];
  const m02 = z[0], m12 = z[1], m22 = z[2];
  const tr = m00 + m11 + m22;
  let q: Quat;
  if (tr > 0) {
    const s = Math.sqrt(tr + 1) * 2;
    q = [(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s];
  } else if (m00 > m11 && m00 > m22) {
    const s = Math.sqrt(1 + m00 - m11 - m22) * 2;
    q = [0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s];
  } else if (m11 > m22) {
    const s = Math.sqrt(1 + m11 - m00 - m22) * 2;
    q = [(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s];
  } else {
    const s = Math.sqrt(1 + m22 - m00 - m11) * 2;
    q = [(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s];
  }
  const l = Math.hypot(q[0], q[1], q[2], q[3]);
  return [q[0] / l, q[1] / l, q[2] / l, q[3] / l];
}
