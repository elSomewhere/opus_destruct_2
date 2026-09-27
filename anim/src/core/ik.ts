/**
 * Inverse kinematics on a Pose, in the character's model space (root at the origin, facing
 * +y): an analytic two-bone solver (legs, arms) with a pole direction for the middle joint,
 * and helpers that set a bone's model-space rotation or aim it.
 *
 * `ModelFK` keeps model-space transforms of a pose up to date for the bones the solvers touch
 * (a solver changes a chain and refreshes the chain's descendants on demand).
 */
import { qconj, qfromBasis, qmul, qnlerp, qrotate, type Quat } from '../math/quat.ts';
import { clamp, vadd, vcross, vdot, vlen, vmadd, vnorm, vscale, vsub, type V3 } from '../math/vec.ts';
import type { Pose, Skeleton } from './skeleton.ts';

/** Model-space bone transforms of a Pose (root at the origin with identity rotation). */
export class ModelFK {
  readonly skeleton: Skeleton;
  readonly p: V3[];
  readonly q: Quat[];
  private readonly tmp: V3 = [0, 0, 0];

  constructor(skeleton: Skeleton) {
    this.skeleton = skeleton;
    this.p = skeleton.restHead.map((v) => [v[0], v[1], v[2]] as V3);
    this.q = skeleton.restHead.map(() => [0, 0, 0, 1] as Quat);
  }

  /** Recomputes bones from `from` (index order: parents come first) to the end. */
  update(pose: Pose, from = 0): this {
    const s = this.skeleton;
    for (let i = from; i < s.count; i++) this.updateBone(pose, i);
    return this;
  }

  updateBone(pose: Pose, i: number): void {
    const par = this.skeleton.parents[i]!;
    if (par < 0) {
      const t = pose.t[i]!;
      this.p[i]![0] = t[0];
      this.p[i]![1] = t[1];
      this.p[i]![2] = t[2];
      const r = pose.r[i]!;
      this.q[i]![0] = r[0];
      this.q[i]![1] = r[1];
      this.q[i]![2] = r[2];
      this.q[i]![3] = r[3];
      return;
    }
    qrotate(this.q[par]!, pose.t[i]!, this.tmp);
    vadd(this.p[par]!, this.tmp, this.p[i]);
    qmul(this.q[par]!, pose.r[i]!, this.q[i]);
  }

  /** Recomputes a bone and everything below it. */
  updateSubtree(pose: Pose, i: number): void {
    this.updateBone(pose, i);
    for (const c of this.skeleton.children[i]!) this.updateSubtree(pose, c);
  }
}

/** Sets bone i's local rotation so that its model-space rotation becomes `q`. */
export function setModelRotation(pose: Pose, fk: ModelFK, i: number, q: Readonly<Quat>): void {
  const par = pose.skeleton.parents[i]!;
  if (par < 0) {
    pose.r[i]![0] = q[0];
    pose.r[i]![1] = q[1];
    pose.r[i]![2] = q[2];
    pose.r[i]![3] = q[3];
  } else qmul(qconj(fk.q[par]!), q, pose.r[i]);
  fk.updateBone(pose, i);
}

/**
 * Rotation that takes the frame (a, b) to the frame (a2, b2): a maps exactly to a2 and b to the
 * direction of b2 orthogonal to a2 (a and b need not be orthogonal, only independent).
 */
export function frameRotation(a: Readonly<V3>, b: Readonly<V3>, a2: Readonly<V3>, b2: Readonly<V3>, out: Quat = [0, 0, 0, 1]): Quat {
  const r0 = basis(a, b);
  const r1 = basis(a2, b2);
  return qmul(qfromBasis(r1[0], r1[1], r1[2]), qconj(qfromBasis(r0[0], r0[1], r0[2])), out);
}

function basis(a: Readonly<V3>, b: Readonly<V3>): [V3, V3, V3] {
  const x = vnorm(a);
  let z = vcross(x, b);
  if (vlen(z) < 1e-9) z = vcross(x, Math.abs(x[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0]);
  vnorm(z, z);
  const y = vcross(z, x);
  return [x, y, z];
}

export interface TwoBoneResult {
  /** Model-space positions of the middle and end joints after solving. */
  mid: V3;
  end: V3;
  /** Target distance / chain length (>= 1: out of reach, the chain is straight). */
  reach: number;
}

/**
 * Two-bone IK: rotates `upper` and `lower` (child of upper) so that `end` (child of lower)
 * reaches `target`, bending the middle joint towards `pole` (a model-space direction). The rest
 * pose's twist about each bone is kept relative to the pole plane. `soft` (0..1) eases the last
 * part of the reach so a nearly straight limb does not snap.
 */
export function solveTwoBone(
  pose: Pose,
  fk: ModelFK,
  upper: number,
  lower: number,
  end: number,
  target: Readonly<V3>,
  pole: Readonly<V3>,
  soft = 0.03,
  /**
   * The direction (model space) the joint points in the rest pose (a knee: forward). With it the
   * whole limb twists towards the pole (the leg turns with its knee); without, the pole serves
   * as its own rest reference and the limb keeps the model frame's twist.
   */
  restPole: Readonly<V3> | null = null,
): TwoBoneResult {
  const sk = pose.skeleton;
  const a = vlen(vsub(sk.restHead[lower]!, sk.restHead[upper]!));
  const b = vlen(vsub(sk.restHead[end]!, sk.restHead[lower]!));
  const root = fk.p[upper]!;
  let dir = vsub(target, root);
  let d = vlen(dir);
  const reach = d / (a + b);
  const maxD = a + b;
  // soft IK: approach full extension asymptotically (no knee pop)
  const ds = maxD - soft * maxD;
  if (soft > 0 && d > ds) d = ds + soft * maxD * (1 - Math.exp(-(d - ds) / (soft * maxD)));
  d = clamp(d, Math.abs(a - b) + 1e-4, maxD - 1e-5);
  vnorm(dir, dir, [0, 0, -1]);
  // the bend plane: the pole's component orthogonal to the chain
  let w = vsub(pole, vscale(dir, vdot(pole, dir)));
  if (vlen(w) < 1e-6) w = vcross(dir, [1, 0, 0]);
  vnorm(w, w);
  const cosA = clamp((a * a + d * d - b * b) / (2 * a * d), -1, 1);
  const sinA = Math.sqrt(1 - cosA * cosA);
  const mid = vadd(root, vadd(vscale(dir, a * cosA), vscale(w, a * sinA)));
  const endP = vmadd(root, dir, d);

  // upper: rest direction and rest pole reference -> new direction and pole
  const restUp = vsub(sk.restHead[lower]!, sk.restHead[upper]!);
  const restLo = vsub(sk.restHead[end]!, sk.restHead[lower]!);
  // rest bend plane normal: derived from the rest limb (nearly straight: use the pole's
  // rest-space equivalent, the model forward/backward axis the solver bends towards)
  const ref = restPole ?? pole;
  const qUp = frameRotation(restUp, restPoleFor(restUp, ref), vsub(mid, root), w);
  setModelRotation(pose, fk, upper, qUp);
  fk.updateBone(pose, lower);
  const qLo = frameRotation(restLo, restPoleFor(restLo, ref), vsub(endP, mid), w);
  setModelRotation(pose, fk, lower, qLo);
  fk.updateBone(pose, end);
  return { mid, end: endP, reach };
}

/**
 * The rest-space reference that maps to the pole: the component of the pole direction
 * orthogonal to the rest bone (the rest pose's bones are nearly straight, so the pole given in
 * model space serves as its own rest reference, keeping the limb's twist stable).
 */
function restPoleFor(restBone: Readonly<V3>, pole: Readonly<V3>): V3 {
  const n = vnorm(restBone);
  const w = vsub(pole, vscale(n, vdot(pole, n)));
  if (vlen(w) < 1e-6) return vcross(n, [1, 0, 0]);
  return vnorm(w);
}

/**
 * Turns bone i (model space) so that its rest `axis` points along `dir`, keeping its rest `up`
 * as close as possible to `up`; weight 0..1 blends from the current rotation.
 */
export function aimBone(pose: Pose, fk: ModelFK, i: number, axis: Readonly<V3>, up: Readonly<V3>, dir: Readonly<V3>, upWanted: Readonly<V3>, weight = 1): void {
  const q = frameRotation(axis, up, dir, upWanted);
  const cur = fk.q[i]!;
  setModelRotation(pose, fk, i, weight >= 1 ? q : qnlerp(cur, q, weight));
}
