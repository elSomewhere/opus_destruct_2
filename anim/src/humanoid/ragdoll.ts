/**
 * The humanoid ragdoll: the humanoid rig mapped onto a particle Ragdoll (physics/ragdoll.ts)
 * and back.
 *
 * It starts from the animated pose of the moment (positions from `world`, velocities from the
 * difference to `prev`), so a character shot mid-stride keeps its momentum. For a short while
 * the body keeps some muscle tone (soft targets relative to the pelvis that fade out), so it
 * slumps instead of dropping like a sack. Every frame the bones' world transforms are rebuilt
 * from the particles (pelvis, spine and chest frames from the torso particles; limbs from their
 * joints and bend planes), so the rigid voxel parts follow.
 */
import { frameRotation } from '../core/ik.ts';
import { WorldPose, type Skeleton } from '../core/skeleton.ts';
import { qconj, qrotate, type Quat } from '../math/quat.ts';
import { vadd, vcross, vlen, vlerp, vnorm, vscale, vsub, type V3 } from '../math/vec.ts';
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

export class HumanoidRagdoll {
  readonly body: Ragdoll;
  readonly skeleton: Skeleton;
  /** Bone transforms rebuilt from the particles after every update. */
  readonly world: WorldPose;
  private readonly k: number;
  private readonly local: V3[];
  private age = 0;
  private readonly tone: number;

  /**
   * @param world the animated pose now; @param prev the one `dt` seconds earlier (velocities)
   * @param tone initial muscle tone 0..1 (0: limp at once)
   */
  constructor(skeleton: Skeleton, collision: CollisionWorld, world: WorldPose, prev: WorldPose, dt: number, tone = 0.7) {
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
    const radius = [0.11, 0.08, 0.08, 0.11, 0.12, 0.05, 0.1, 0.06, 0.045, 0.045, 0.06, 0.045, 0.045, 0.06, 0.05, 0.04, 0.06, 0.05, 0.04, 0.06, 0.06];
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
    // arms: elbows bend forward (the joint stays behind the shoulder-wrist line)
    for (const [s, e, w] of [
      [P.shoulderL, P.elbowL, P.wristL],
      [P.shoulderR, P.elbowR, P.wristR],
    ] as const) {
      body.link(s, e);
      body.link(e, w);
      limitAbs(body, s, w, 0.12 * k, restDist(skeleton, s === P.shoulderL ? H.upperarmL : H.upperarmR, s === P.shoulderL ? H.handL : H.handR) * 1.0);
      body.constraints.push({ kind: 'hinge', a: s, m: e, c: w, f0: P.sternum, f1: P.chest, sign: 1 });
      limitAbs(body, w, P.pelvis, 0.12 * k, 3);
    }
    // legs: knees bend backwards (the knee stays in front of the hip-ankle line)
    for (const [hp, kn, an, to] of [
      [P.hipL, P.kneeL, P.ankleL, P.toeL],
      [P.hipR, P.kneeR, P.ankleR, P.toeR],
    ] as const) {
      body.link(hp, kn);
      body.link(kn, an);
      body.link(an, to);
      body.link(kn, to);
      const left = hp === P.hipL;
      limitAbs(body, hp, an, 0.35 * k, restDist(skeleton, left ? H.thighL : H.thighR, left ? H.footL : H.footR));
      body.constraints.push({ kind: 'hinge', a: hp, m: kn, c: an, f0: P.pelvis, f1: P.pubis, sign: 1 });
      // the thigh does not fold back past the torso line much
      limitAbs(body, kn, P.chest, 0.22 * k, 2);
    }
    limitAbs(body, P.kneeL, P.kneeR, 0.09 * k, 1.2 * k);
    limitAbs(body, P.ankleL, P.ankleR, 0.08 * k, 1.4 * k);
    // muscle tone: targets relative to the pelvis frame
    const f = this.pelvisFrame();
    const inv = qconj(f.q);
    this.local = body.particles.map((q) => qrotate(inv, vsub(q.p, f.p)));
    body.targets = body.particles.map((q) => [q.p[0], q.p[1], q.p[2]] as V3);
    body.targetWeight = tone;
    this.rebuild();
  }

  get asleep(): boolean {
    return this.body.asleep;
  }

  /** A hit on the body: velocity change `dv` (m/s) at the particle nearest to `point`. */
  hit(point: Readonly<V3>, dv: Readonly<V3>): void {
    const i = this.body.nearest(point);
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
    const tone = this.tone * Math.exp(-this.age / 0.35);
    this.body.targetWeight = tone > 0.02 ? tone : 0;
    if (this.body.targetWeight > 0 && this.body.targets) {
      const f = this.pelvisFrame();
      this.local.forEach((l, i) => {
        const t = vadd(f.p, qrotate(f.q, l));
        // knees buckle and the trunk folds: targets sag while the tone fades
        t[2] -= 0.25 * this.k * (1 - Math.exp(-this.age / 0.25)) * (i === P.pelvis || i === P.hipL || i === P.hipR ? 0 : 0.5);
        this.body.targets![i] = t;
      });
    }
    this.body.update(dt);
    this.rebuild();
  }

  /** Pelvis frame from the pelvis particles: origin at the hips' midpoint. */
  private pelvisFrame(): { p: V3; q: Quat } {
    const sk = this.skeleton;
    const rh = sk.restHead;
    const pp = this.body.particles;
    const mid = vlerp(pp[P.hipL]!.p, pp[P.hipR]!.p, 0.5);
    const right = vsub(pp[P.hipR]!.p, pp[P.hipL]!.p);
    const up = vsub(pp[P.belly]!.p, mid);
    const restMid = vlerp(rh[H.thighL]!, rh[H.thighR]!, 0.5);
    const q = frameRotation(vsub(rh[H.chest]!, restMid), vsub(rh[H.thighR]!, rh[H.thighL]!), up, right);
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
    // limbs from their joints and bend planes
    const limb = (b0: number, b1: number, j0: number, j1: number, j2: number, bendsForward: boolean, fallback: V3): void => {
      const u = vsub(X(j1), X(j0));
      const f = vsub(X(j2), X(j1));
      const ru = vsub(rh[b1]!, rh[b0]!);
      const restN = vnorm(vcross(ru, bendsForward ? [0, 1, 0] : [0, -1, 0]));
      let n = vcross(u, f);
      if (vlen(n) < 1e-3 * vlen(u) * vlen(f)) n = fallback;
      q[b0] = frameRotation(ru, restN, u, n);
      q[b1] = frameRotation(vsub(sk.restTail[b1]!, rh[b1]!), restN, f, n);
    };
    const chestRight = vnorm(shoulders);
    const pelvisRight = vnorm(hips);
    limb(H.upperarmL, H.forearmL, P.shoulderL, P.elbowL, P.wristL, true, chestRight);
    limb(H.upperarmR, H.forearmR, P.shoulderR, P.elbowR, P.wristR, true, chestRight);
    q[H.handL] = q[H.forearmL]!;
    q[H.handR] = q[H.forearmR]!;
    limb(H.thighL, H.shinL, P.hipL, P.kneeL, P.ankleL, false, vscale(pelvisRight, -1));
    limb(H.thighR, H.shinR, P.hipR, P.kneeR, P.ankleR, false, vscale(pelvisRight, -1));
    for (const [foot, toe, an, tp, kn] of [
      [H.footL, H.toeL, P.ankleL, P.toeL, P.kneeL],
      [H.footR, H.toeR, P.ankleR, P.toeR, P.kneeR],
    ] as const) {
      q[foot] = frameRotation(vsub(rh[toe]!, rh[foot]!), vsub(rh[foot === H.footL ? H.shinL : H.shinR]!, rh[foot]!), vsub(X(tp), X(an)), vsub(X(kn), X(an)));
      q[toe] = q[foot]!;
    }
    q[H.weapon] = pf.q;
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
