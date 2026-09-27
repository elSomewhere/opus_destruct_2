/**
 * The humanoid ragdoll: the humanoid rig mapped onto a particle Ragdoll (physics/ragdoll.ts)
 * and back.
 *
 * It starts from the animated pose of the moment (positions from `world`, velocities from the
 * difference to `prev`), so a character shot mid-stride keeps its momentum. For a short while
 * the body keeps some muscle tone (soft targets relative to the pelvis that fade out), so it
 * slumps instead of dropping like a sack. Arms stay out of the torso and legs out of each
 * other. Every frame the bones' world transforms are rebuilt from the particles (pelvis, spine
 * and chest frames from the torso particles; limbs from their joints and bend planes), so the
 * rigid voxel parts follow. A straight limb has no bend plane: its roll carries over from the
 * frame before (limbs never flip about their length), and feet turn with their leg's plane.
 * While the body is nearly at rest, bone rotations are smoothed (no contact jitter shows).
 */
import { frameRotation } from '../core/ik.ts';
import { WorldPose, type Skeleton } from '../core/skeleton.ts';
import { qaxis, qmul, qnlerp, qrotate, type Quat } from '../math/quat.ts';
import { clamp, smoothstep, vadd, vcross, vdot, vlen, vlerp, vnorm, vscale, vsub, type V3 } from '../math/vec.ts';
import type { CollisionWorld } from '../physics/collision.ts';
import { Ragdoll } from '../physics/ragdoll.ts';
import { H } from './rig.ts';

/** Particle indices. */
const P = {
  pelvis: 0,
  hipL: 1,
  hipR: 2,
  belly: 3,
  chest: 4,
  neck: 5,
  head: 6,
  shoulderL: 7,
  elbowL: 8,
  wristL: 9,
  shoulderR: 10,
  elbowR: 11,
  wristR: 12,
  kneeL: 13,
  ankleL: 14,
  toeL: 15,
  kneeR: 16,
  ankleR: 17,
  toeR: 18,
  sternum: 19,
  pubis: 20,
} as const;

/** Particles held up by the collapse support (the hips). */
const SUPPORTED: readonly number[] = [P.pelvis, P.hipL, P.hipR, P.pubis];

export class HumanoidRagdoll {
  readonly body: Ragdoll;
  readonly skeleton: Skeleton;
  /** Bone transforms rebuilt from the particles after every update. */
  readonly world: WorldPose;
  private readonly k: number;
  private age = 0;
  private readonly tone: number;
  private readonly collapse: number;
  private readonly support0: number[];
  private readonly ground0: number;
  /** Bend-plane normals of the limbs (arm L, arm R, leg L, leg R), carried frame to frame. */
  private readonly bendN: V3[];
  /** Ankle pitch of each foot (rad, about the leg's plane normal). */
  private readonly footPitch: number[] = [0, 0];
  /** The same for the lower segments (forearms, shins). */
  private readonly lowN: V3[];
  /** Rotations shown the frame before (smoothing at rest). */
  private readonly shown: Quat[];

  /**
   * @param world the animated pose now; @param prev the one `dt` seconds earlier (velocities)
   * @param tone initial muscle tone 0..1 (0: limp at once)
   * @param collapse seconds the legs take to give way (0: the body drops at once, as from a
   *   head shot or a blast)
   */
  constructor(skeleton: Skeleton, collision: CollisionWorld, world: WorldPose, prev: WorldPose, dt: number, tone = 0.7, collapse = 0.6) {
    this.skeleton = skeleton;
    this.world = new WorldPose(skeleton);
    this.world.copyFrom(world);
    this.tone = tone;
    const rh = skeleton.restHead;
    this.k = rh[H.pelvis]![2] / 0.97;
    const k = this.k;
    const body = new Ragdoll(collision);
    this.body = body;
    const pt = (w: WorldPose, i: number): V3 => {
      switch (i) {
        case P.pelvis: return vcopy3(w.p[H.pelvis]!);
        case P.hipL: return vcopy3(w.p[H.thighL]!);
        case P.hipR: return vcopy3(w.p[H.thighR]!);
        case P.belly: return vcopy3(w.p[H.chest]!);
        case P.chest: return w.pointOf(H.chest, [0, rh[H.chest]![1], rh[H.chest]![2] + 0.13 * k]);
        case P.neck: return vcopy3(w.p[H.neck]!);
        case P.head: return w.pointOf(H.head, [0, rh[H.head]![1] + 0.015 * k, rh[H.head]![2] + 0.1 * k]);
        case P.shoulderL: return vcopy3(w.p[H.upperarmL]!);
        case P.elbowL: return vcopy3(w.p[H.forearmL]!);
        case P.wristL: return vcopy3(w.p[H.handL]!);
        case P.shoulderR: return vcopy3(w.p[H.upperarmR]!);
        case P.elbowR: return vcopy3(w.p[H.forearmR]!);
        case P.wristR: return vcopy3(w.p[H.handR]!);
        case P.kneeL: return vcopy3(w.p[H.shinL]!);
        case P.ankleL: return vcopy3(w.p[H.footL]!);
        case P.toeL: return vcopy3(w.p[H.toeL]!);
        case P.kneeR: return vcopy3(w.p[H.shinR]!);
        case P.ankleR: return vcopy3(w.p[H.footR]!);
        case P.toeR: return vcopy3(w.p[H.toeR]!);
        case P.sternum: return w.pointOf(H.chest, [0, rh[H.chest]![1] + 0.12 * k, rh[H.chest]![2] + 0.1 * k]);
        default: return w.pointOf(H.pelvis, [0, rh[H.pelvis]![1] + 0.12 * k, rh[H.pelvis]![2] - 0.04 * k]);
      }
    };
    // (the ankles reach the ground: they are the heels a standing body rests on)
    const radius = [0.11, 0.08, 0.08, 0.11, 0.12, 0.05, 0.1, 0.06, 0.045, 0.045, 0.06, 0.045, 0.045, 0.06, 0.085, 0.04, 0.06, 0.085, 0.04, 0.06, 0.06];
    const mass = [12, 5, 5, 10, 14, 3, 5, 3, 2, 1, 3, 2, 1, 4, 2, 0.7, 4, 2, 0.7, 2, 2];
    for (let i = 0; i <= P.pubis; i++) {
      body.addParticle(pt(world, i), radius[i]! * k, mass[i]!);
      const was = pt(prev, i);
      const now = body.particles[i]!.p;
      const v = dt > 0 ? vscale(vsub(now, was), 1 / dt) : [0, 0, 0];
      body.setVelocity(i, [clampV(v[0]!), clampV(v[1]!), clampV(v[2]!)]);
    }
    // torso blocks
    body.link(P.pelvis, P.hipL);
    body.link(P.pelvis, P.hipR);
    body.link(P.hipL, P.hipR);
    body.link(P.pubis, P.pelvis);
    body.link(P.pubis, P.hipL);
    body.link(P.pubis, P.hipR);
    body.link(P.belly, P.hipL, 0.6);
    body.link(P.belly, P.hipR, 0.6);
    body.link(P.pelvis, P.belly);
    body.link(P.belly, P.chest);
    body.limit(P.pelvis, P.chest, 0.8, 1.03);
    body.limit(P.pubis, P.chest, 0.75, 1.08);
    body.link(P.chest, P.neck);
    body.link(P.chest, P.shoulderL);
    body.link(P.chest, P.shoulderR);
    body.link(P.shoulderL, P.shoulderR);
    body.link(P.neck, P.shoulderL);
    body.link(P.neck, P.shoulderR);
    body.link(P.sternum, P.chest);
    body.link(P.sternum, P.shoulderL);
    body.link(P.sternum, P.shoulderR);
    body.link(P.sternum, P.neck);
    body.link(P.belly, P.shoulderL, 0.4);
    body.link(P.belly, P.shoulderR, 0.4);
    // twisting and folding limits between hips and shoulders
    body.limit(P.hipL, P.shoulderL, 0.7, 1.05);
    body.limit(P.hipR, P.shoulderR, 0.7, 1.05);
    body.limit(P.hipL, P.shoulderR, 0.75, 1.08);
    body.limit(P.hipR, P.shoulderL, 0.75, 1.08);
    // head
    body.link(P.neck, P.head);
    body.limit(P.head, P.shoulderL, 0.75, 1.12);
    body.limit(P.head, P.shoulderR, 0.75, 1.12);
    body.limit(P.head, P.chest, 0.8, 1.06);
    body.limit(P.head, P.sternum, 0.7, 1.15);
    // arms: elbows bend one way (the forearm comes forward and up, never back past straight)
    for (const [s, e, w] of [
      [P.shoulderL, P.elbowL, P.wristL],
      [P.shoulderR, P.elbowR, P.wristR],
    ] as const) {
      body.link(s, e);
      body.link(e, w);
      limitAbs(body, s, w, 0.12 * k, restDist(skeleton, s === P.shoulderL ? H.upperarmL : H.upperarmR, s === P.shoulderL ? H.handL : H.handR) * 1.0);
      body.constraints.push({ kind: 'hinge', a: s, m: e, c: w, f0: P.shoulderL, f1: P.shoulderR, sign: 1 });
      limitAbs(body, w, P.pelvis, 0.12 * k, 3);
    }
    // legs: knees bend one way (the shin goes back, never forward past straight)
    for (const [hp, kn, an, to] of [
      [P.hipL, P.kneeL, P.ankleL, P.toeL],
      [P.hipR, P.kneeR, P.ankleR, P.toeR],
    ] as const) {
      body.link(hp, kn);
      body.link(kn, an);
      body.link(an, to);
      body.link(kn, to);
      const left = hp === P.hipL;
      // (a fully bent knee brings the heel to the buttock)
      limitAbs(body, hp, an, 0.18 * k, restDist(skeleton, left ? H.thighL : H.thighR, left ? H.footL : H.footR));
      body.constraints.push({ kind: 'hinge', a: hp, m: kn, c: an, f0: P.hipL, f1: P.hipR, sign: -1 });
      // the thigh does not fold back past the torso line much
      limitAbs(body, kn, P.chest, 0.22 * k, 2);
    }
    limitAbs(body, P.kneeL, P.kneeR, 0.09 * k, 1.2 * k);
    limitAbs(body, P.ankleL, P.ankleR, 0.08 * k, 1.4 * k);
    // the body does not pass through itself: arms stay out of the torso, legs out of each
    // other (never tighter than they start, so the first frame does not pop)
    const apart = (a: number, b: number, min: number): void => {
      const pa = body.particles[a]!.p, pb = body.particles[b]!.p;
      limitAbs(body, a, b, Math.min(min, 0.9 * Math.hypot(pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2])), 1e3);
    };
    for (const [e, w] of [
      [P.elbowL, P.wristL],
      [P.elbowR, P.wristR],
    ] as const) {
      for (const t of [P.belly, P.chest, P.pelvis, P.sternum]) {
        apart(e, t, 0.15 * k);
        apart(w, t, 0.14 * k);
      }
    }
    apart(P.kneeL, P.ankleR, 0.1 * k);
    apart(P.kneeR, P.ankleL, 0.1 * k);
    apart(P.toeL, P.toeR, 0.07 * k);
    apart(P.head, P.pelvis, 0.3 * k);
    // muscle tone (fading): soft targets that keep the body's shape
    body.targets = body.particles.map((q) => [q.p[0], q.p[1], q.p[2]] as V3);
    body.targetWeight = tone;
    // tone by shape matching: every substep the targets are the body's shape at death, fitted
    // (centroid and rotation) to where the toned particles are now, so tone keeps the body's
    // shape without pushing it anywhere (no momentum from nowhere)
    const toned = body.particles.map((_, i) => i).filter((i) => (body.targetShare?.[i] ?? 1) > 0);
    const massOf = (i: number): number => (body.particles[i]!.w > 0 ? 1 / body.particles[i]!.w : 0);
    const centroid = (): V3 => {
      let m = 0;
      const c: V3 = [0, 0, 0];
      for (const i of toned) {
        const mi = massOf(i), p = body.particles[i]!.p;
        c[0] += p[0] * mi;
        c[1] += p[1] * mi;
        c[2] += p[2] * mi;
        m += mi;
      }
      return vscale(c, 1 / (m || 1));
    };
    const c0 = centroid();
    const shape = body.particles.map((q) => vsub(q.p, c0));
    let fit: Quat = [0, 0, 0, 1];
    body.beforeSubstep = (): void => {
      if (body.targetWeight <= 0 || !body.targets) return;
      const c = centroid();
      // A = sum m (x - c) r^T, columns a0..a2; its rotational part by iteration from the last fit
      const a: [V3, V3, V3] = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
      for (const i of toned) {
        const mi = massOf(i), p = body.particles[i]!.p, r = shape[i]!;
        const d: V3 = [(p[0] - c[0]) * mi, (p[1] - c[1]) * mi, (p[2] - c[2]) * mi];
        for (let j = 0; j < 3; j++) {
          a[j]![0] += d[0] * r[j]!;
          a[j]![1] += d[1] * r[j]!;
          a[j]![2] += d[2] * r[j]!;
        }
      }
      for (let it = 0; it < 6; it++) {
        const r0 = qrotate(fit, [1, 0, 0]), r1 = qrotate(fit, [0, 1, 0]), r2 = qrotate(fit, [0, 0, 1]);
        const num = vadd(vadd(vcross(r0, a[0]), vcross(r1, a[1])), vcross(r2, a[2]));
        const den = Math.abs(vdot(r0, a[0]) + vdot(r1, a[1]) + vdot(r2, a[2])) + 1e-9;
        const omega = vscale(num, 1 / den);
        const w = vlen(omega);
        if (w < 1e-9) break;
        fit = qnorm4(qmul(qaxis(vscale(omega, 1 / w), w), fit));
      }
      for (const i of toned) {
        const t = body.targets![i]!;
        const g = vadd(c, qrotate(fit, shape[i]!));
        t[0] = g[0];
        t[1] = g[1];
        t[2] = g[2];
      }
    };
    // the legs give way first (they carry the weight), the arms go limp, the trunk and the neck
    // hold their shape a moment longer
    const legs: number[] = [P.kneeL, P.ankleL, P.toeL, P.kneeR, P.ankleR, P.toeR];
    const arms: number[] = [P.elbowL, P.wristL, P.elbowR, P.wristR];
    const neck: number[] = [P.neck, P.head];
    body.targetShare = body.particles.map((_, i) => (legs.includes(i) ? 0 : arms.includes(i) ? 0.3 : neck.includes(i) ? 0.6 : 1));
    // the legs give way: a support under the hips sinks from where they are to a crouch
    this.collapse = collapse;
    this.support0 = body.particles.map((q, i) => (SUPPORTED.includes(i) ? q.p[2] : NaN));
    this.ground0 = Math.min(...body.particles.map((q) => q.p[2] - q.r));
    if (collapse > 0) body.support = this.support0.slice();
    // the knees give (a straight leg would stand): forwards, the faster the quicker the collapse
    const fwd = qrotate(this.pelvisFrame().q, [0, 1, 0]);
    const give = collapse > 0 ? clamp(0.5 / collapse, 0.6, 2.2) : 1.5;
    for (const kn of [P.kneeL, P.kneeR]) body.impulse(kn, [fwd[0] * give, fwd[1] * give, -0.2 * give]);
    // the limbs' bend planes as the animation left them
    this.bendN = [
      [H.upperarmL, H.forearmL, true],
      [H.upperarmR, H.forearmR, true],
      [H.thighL, H.shinL, false],
      [H.thighR, H.shinR, false],
    ].map(([b0, b1, fwd]) => qrotate(world.q[b0 as number]!, restBendNormal(skeleton, b0 as number, b1 as number, fwd as boolean)));
    this.lowN = [
      [H.upperarmL, H.forearmL, true],
      [H.upperarmR, H.forearmR, true],
      [H.thighL, H.shinL, false],
      [H.thighR, H.shinR, false],
    ].map(([b0, b1, fwd]) => qrotate(world.q[b1 as number]!, restBendNormal(skeleton, b0 as number, b1 as number, fwd as boolean)));
    this.shown = world.q.map((q) => [q[0], q[1], q[2], q[3]] as Quat);
    this.rebuild();
  }

  get asleep(): boolean {
    return this.body.asleep;
  }

  /**
   * A hit on the body: velocity change `dv` (m/s) at the particle nearest to `point`; with
   * `carry` the rest of the upper body takes that share of it (a killing shot sends the trunk
   * the way it came from).
   */
  hit(point: Readonly<V3>, dv: Readonly<V3>, carry = 0): void {
    const i = this.body.nearest(point);
    if (carry > 0)
      for (const j of [P.belly, P.chest, P.sternum, P.neck, P.head, P.shoulderL, P.shoulderR]) if (j !== i) this.body.impulse(j, vscale(dv, carry));
    this.body.impulse(i, dv);
    // neighbours share some of it
    for (const c of this.body.constraints) {
      if (c.kind !== 'dist') continue;
      if (c.a === i) this.body.impulse(c.b, vscale(dv, 0.35));
      else if (c.b === i) this.body.impulse(c.a, vscale(dv, 0.35));
    }
  }

  blast(center: Readonly<V3>, radius: number, speed: number): void {
    this.body.blast(center, radius, speed);
  }

  update(dt: number): void {
    this.age += dt;
    if (this.body.asleep) return;
    const sp = this.body.support;
    if (sp) {
      // the support sinks faster and faster (the knees buckle), then lets go
      const u = this.age / this.collapse;
      if (u >= 1) this.body.support = null;
      else {
        const e = u * u * (3 - 2 * u);
        const low = this.ground0 + 0.3 * this.k;
        this.support0.forEach((z0, i) => (sp[i] = z0 === z0 ? z0 + (Math.min(low, z0) - z0) * e : NaN));
      }
    }
    const tone = this.tone * Math.exp(-this.age / 0.28);
    this.body.targetWeight = tone > 0.02 ? tone : 0;
    this.body.update(dt);
    this.rebuild();
  }

  /**
   * Pelvis frame from the rigid pelvis triangle (the hips and the pubis in front of them):
   * origin at the hips' midpoint.
   */
  private pelvisFrame(): { p: V3; q: Quat } {
    const sk = this.skeleton;
    const rh = sk.restHead;
    const pp = this.body.particles;
    const mid = vlerp(pp[P.hipL]!.p, pp[P.hipR]!.p, 0.5);
    const right = vsub(pp[P.hipR]!.p, pp[P.hipL]!.p);
    const fwd = vsub(pp[P.pubis]!.p, mid);
    const restMid = vlerp(rh[H.thighL]!, rh[H.thighR]!, 0.5);
    const restPubis: V3 = [0, rh[H.pelvis]![1] + 0.12 * this.k, rh[H.pelvis]![2] - 0.04 * this.k];
    const q = frameRotation(vsub(rh[H.thighR]!, rh[H.thighL]!), vsub(restPubis, restMid), right, fwd);
    return { p: mid, q };
  }

  /** Bone transforms from the particles. */
  private rebuild(): void {
    const sk = this.skeleton;
    const rh = sk.restHead;
    const rt = sk.restTail;
    const w = this.world;
    const pp = this.body.particles;
    const X = (i: number): V3 => pp[i]!.p;
    const q = w.q;
    // torso
    const pf = this.pelvisFrame();
    q[H.pelvis] = pf.q;
    q[H.root] = pf.q;
    const restShoulders = vsub(rh[H.upperarmR]!, rh[H.upperarmL]!);
    const restHips = vsub(rh[H.thighR]!, rh[H.thighL]!);
    const shoulders = vsub(X(P.shoulderR), X(P.shoulderL));
    const hips = vsub(X(P.hipR), X(P.hipL));
    const midRight = vadd(vnorm(shoulders), vnorm(hips));
    q[H.spine] = frameRotation(vsub(rh[H.chest]!, rh[H.spine]!), restHips, vsub(X(P.belly), X(P.pelvis)), midRight);
    q[H.chest] = frameRotation(vsub(rh[H.neck]!, rh[H.chest]!), restShoulders, vsub(X(P.neck), X(P.belly)), shoulders);
    q[H.neck] = frameRotation(vsub(rt[H.neck]!, rh[H.neck]!), restShoulders, vsub(X(P.head), X(P.neck)), shoulders);
    q[H.head] = q[H.neck]!;
    q[H.clavicleL] = q[H.chest]!;
    q[H.clavicleR] = q[H.chest]!;
    // limbs from their joints and bend planes. The plane's normal turns about the limb towards
    // the joint's own bend plane (the more the joint is bent, the more it follows it) at a
    // limited rate: a straight or hyperextended limb keeps its roll, and no limb ever flips
    // about its length from one frame to the next
    const maxTurn = 0.2;
    const limb = (li: number, b0: number, b1: number, j0: number, j1: number, j2: number, bendsForward: boolean, parentQ: Quat): V3 => {
      const u = vsub(X(j1), X(j0));
      const f = vsub(X(j2), X(j1));
      const restN = restBendNormal(sk, b0, b1, bendsForward);
      const ul = vlen(u) || 1;
      const uh = vscale(u, 1 / ul);
      const perp = (v: V3): V3 => vnorm(vsub(v, vscale(uh, vdot(v, uh))), [0, 0, 0], restN);
      const e1 = perp(this.bendN[li]!);
      const e2 = vcross(uh, e1);
      const angleTo = (v: V3): number => Math.atan2(vdot(v, e2), vdot(v, e1));
      // a slow pull towards where the parent would hold the limb, and the joint's own plane
      let turn = 0.02 * angleTo(qrotate(parentQ, restN));
      const c = vcross(u, f);
      const bend = vlen(c) / (ul * (vlen(f) || 1));
      if (bend > 1e-4) turn += smoothstep(0.06, 0.35, bend) * (angleTo(vscale(c, 1 / vlen(c))) - turn);
      turn = clamp(turn, -maxTurn, maxTurn);
      const n = vadd(vscale(e1, Math.cos(turn)), vscale(e2, Math.sin(turn)));
      this.bendN[li] = n;
      q[b0] = frameRotation(vsub(rh[b1]!, rh[b0]!), restN, u, n);
      // the lower segment's roll: the same plane taken across that segment, carried and turned
      // at the same limited rate (a forearm or shin swinging through the plane's normal would
      // otherwise flip)
      const fh = vnorm(f, [0, 0, 0], uh);
      const across = (v: V3): V3 => vnorm(vsub(v, vscale(fh, vdot(v, fh))), [0, 0, 0], n);
      const g1 = across(this.lowN[li]!);
      const g2 = vcross(fh, g1);
      const want = vsub(n, vscale(fh, vdot(n, fh)));
      let t2 = 0;
      if (vlen(want) > 1e-3) t2 = clamp(Math.atan2(vdot(want, g2), vdot(want, g1)), -maxTurn, maxTurn);
      const nl = vadd(vscale(g1, Math.cos(t2)), vscale(g2, Math.sin(t2)));
      this.lowN[li] = nl;
      q[b1] = frameRotation(vsub(sk.restTail[b1]!, rh[b1]!), restN, f, nl);
      return n;
    };
    limb(0, H.upperarmL, H.forearmL, P.shoulderL, P.elbowL, P.wristL, true, q[H.chest]!);
    limb(1, H.upperarmR, H.forearmR, P.shoulderR, P.elbowR, P.wristR, true, q[H.chest]!);
    q[H.handL] = q[H.forearmL]!;
    q[H.handR] = q[H.forearmR]!;
    const legN = [
      limb(2, H.thighL, H.shinL, P.hipL, P.kneeL, P.ankleL, false, pf.q),
      limb(3, H.thighR, H.shinR, P.hipR, P.kneeR, P.ankleR, false, pf.q),
    ];
    // feet: the shin's frame at the natural ankle angle, pitched in the leg's plane towards the
    // toe particle within the ankle's range (the foot turns with the leg, never about it)
    for (const [i, foot, toe, shin, an, tp] of [
      [0, H.footL, H.toeL, H.shinL, P.ankleL, P.toeL],
      [1, H.footR, H.toeR, H.shinR, P.ankleR, P.toeR],
    ] as const) {
      const n = legN[i]!;
      const rest = qrotate(q[shin]!, vsub(rh[toe]!, rh[foot]!));
      const t = vsub(X(tp), X(an));
      const inPlane = vsub(t, vscale(n, vdot(t, n)));
      let pitch = this.footPitch[i]!;
      if (vlen(inPlane) > 1e-4 && vlen(rest) > 1e-6) {
        const a = Math.atan2(vdot(vcross(rest, inPlane), n), vdot(rest, inPlane));
        // (a toe swung round behind the ankle says nothing: the foot keeps its angle)
        if (Math.abs(a) < 1.6) pitch += clamp(clamp(a, -0.5, 0.7) - pitch, -maxTurn, maxTurn);
      }
      this.footPitch[i] = pitch;
      q[foot] = qmul(qaxis(n, pitch), q[shin]!);
      q[toe] = q[foot]!;
    }
    q[H.weapon] = pf.q;
    // nearly at rest, the shown rotations follow smoothly (contacts leave a little jitter)
    let fast = 0;
    for (const p of pp) fast = Math.max(fast, Math.hypot(p.p[0] - p.prev[0], p.p[1] - p.prev[1], p.p[2] - p.prev[2]) / this.body.substep);
    const follow = clamp(0.3 + fast / 1.2, 0.3, 1);
    for (let i = 0; i < sk.count; i++) {
      this.shown[i] = qnlerp(this.shown[i]!, q[i]!, follow);
      q[i] = this.shown[i]!;
    }
    // positions by chaining the rest offsets from the pelvis (joints stay connected)
    const restMid = vlerp(rh[H.thighL]!, rh[H.thighR]!, 0.5);
    w.p[H.pelvis] = vadd(pf.p, qrotate(pf.q, vsub(rh[H.pelvis]!, restMid)));
    w.p[H.root] = vsub(w.p[H.pelvis]!, qrotate(pf.q, rh[H.pelvis]!));
    for (let i = 0; i < sk.count; i++) {
      if (i === H.root || i === H.pelvis) continue;
      const par = sk.parents[i]!;
      w.p[i] = vadd(w.p[par]!, qrotate(q[par]!, vsub(rh[i]!, rh[par]!)));
    }
  }
}

function vcopy3(v: Readonly<V3>): V3 {
  return [v[0], v[1], v[2]];
}

function clampV(v: number): number {
  return Math.max(-25, Math.min(25, v));
}

function restDist(sk: Skeleton, a: number, b: number): number {
  const x = sk.restHead[a]!, y = sk.restHead[b]!;
  return Math.hypot(x[0] - y[0], x[1] - y[1], x[2] - y[2]);
}

function limitAbs(body: Ragdoll, a: number, b: number, min: number, max: number): void {
  body.constraints.push({ kind: 'limit', a, b, min, max });
}

/** The rest bend-plane normal of a limb (upper bone b0, lower b1): elbows bend forward, knees back. */
function restBendNormal(sk: Skeleton, b0: number, b1: number, bendsForward: boolean): V3 {
  return vnorm(vcross(vsub(sk.restHead[b1]!, sk.restHead[b0]!), bendsForward ? [0, 1, 0] : [0, -1, 0]));
}

function qnorm4(q: Quat): Quat {
  const l = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
  return [q[0] / l, q[1] / l, q[2] / l, q[3] / l];
}
