/**
 * Skeletons, poses and their world transforms.
 *
 * Conventions (all of svx_anim):
 * - Character (model) space: +x right, +y forward, +z up, metres; the origin is on the ground
 *   between the feet.
 * - Rest pose: every bone's rotation is the identity, so joint frames are aligned with model
 *   space in the rest pose, and a bone's rest local translation is its head minus its parent's
 *   head. Joint rotations are authored in that frame (see qeuler).
 * - A Pose holds each bone's local translation and rotation (relative to its parent; the root's
 *   are relative to the character origin). A WorldPose holds each bone's head position and
 *   rotation in world space.
 */
import { writeRigid } from '../math/mat4.ts';
import { qcopy, qidentity, qmul, qnlerp, qrotate, type Quat } from '../math/quat.ts';
import { vadd, vcopy, vlerp, vsub, type V3 } from '../math/vec.ts';

export interface BoneDef {
  name: string;
  /** Parent bone name; null for the root (exactly one). Parents come before their children. */
  parent: string | null;
  /** Rest position of the joint (bone head) in model space. */
  head: V3;
  /** Rest position of the bone's end (for ragdolls, debug drawing and hit capsules). */
  tail: V3;
}

export class Skeleton {
  readonly count: number;
  readonly names: readonly string[];
  /** Parent index per bone (-1 for the root). */
  readonly parents: Int16Array;
  /** Rest joint positions in model space. */
  readonly restHead: readonly V3[];
  readonly restTail: readonly V3[];
  /** Rest local translations (head - parent head; the root's is its head). */
  readonly restLocal: readonly V3[];
  /** Child indices per bone. */
  readonly children: readonly number[][];
  private readonly byName = new Map<string, number>();

  constructor(defs: readonly BoneDef[]) {
    if (defs.length === 0 || defs.length > 255) throw new Error('a skeleton has 1..255 bones');
    this.count = defs.length;
    this.names = defs.map((d) => d.name);
    this.parents = new Int16Array(defs.length);
    const heads: V3[] = [];
    const tails: V3[] = [];
    const local: V3[] = [];
    const children: number[][] = defs.map(() => []);
    defs.forEach((d, i) => {
      if (this.byName.has(d.name)) throw new Error(`duplicate bone ${d.name}`);
      this.byName.set(d.name, i);
      const p = d.parent === null ? -1 : this.byName.get(d.parent);
      if (p === undefined) throw new Error(`bone ${d.name}: parent ${d.parent} must come first`);
      if (p === -1 && i !== 0) throw new Error('only the first bone may be the root');
      this.parents[i] = p;
      heads.push(vcopy(d.head));
      tails.push(vcopy(d.tail));
      local.push(p < 0 ? vcopy(d.head) : vsub(d.head, defs[p]!.head));
      if (p >= 0) children[p]!.push(i);
    });
    this.restHead = heads;
    this.restTail = tails;
    this.restLocal = local;
    this.children = children;
  }

  /** Bone index by name (throws for an unknown name). */
  index(name: string): number {
    const i = this.byName.get(name);
    if (i === undefined) throw new Error(`no bone ${name}`);
    return i;
  }

  has(name: string): boolean {
    return this.byName.has(name);
  }

  /** Rest length of a bone (head to tail). */
  length(i: number): number {
    const h = this.restHead[i]!;
    const t = this.restTail[i]!;
    return Math.hypot(t[0] - h[0], t[1] - h[1], t[2] - h[2]);
  }

  /** Whether `i` is `ancestor` or below it. */
  isBelow(i: number, ancestor: number): boolean {
    for (let b = i; b >= 0; b = this.parents[b]!) if (b === ancestor) return true;
    return false;
  }
}

/** Local joint transforms (translation relative to the parent's head, rotation). */
export class Pose {
  readonly skeleton: Skeleton;
  readonly t: V3[];
  readonly r: Quat[];

  constructor(skeleton: Skeleton) {
    this.skeleton = skeleton;
    this.t = skeleton.restLocal.map((v) => vcopy(v));
    this.r = skeleton.restLocal.map(() => qidentity());
  }

  /** Back to the rest pose. */
  reset(): this {
    const s = this.skeleton;
    for (let i = 0; i < s.count; i++) {
      vcopy(s.restLocal[i]!, this.t[i]);
      qidentity(this.r[i]);
    }
    return this;
  }

  copyFrom(p: Pose): this {
    for (let i = 0; i < this.t.length; i++) {
      vcopy(p.t[i]!, this.t[i]);
      qcopy(p.r[i]!, this.r[i]);
    }
    return this;
  }

  /** this = lerp(this, p, w) per bone (w in 0..1), optionally weighted per bone. */
  blend(p: Pose, w: number, mask?: Float32Array): this {
    for (let i = 0; i < this.t.length; i++) {
      const k = mask ? w * mask[i]! : w;
      if (k <= 0) continue;
      if (k >= 1) {
        vcopy(p.t[i]!, this.t[i]);
        qcopy(p.r[i]!, this.r[i]);
        continue;
      }
      vlerp(this.t[i]!, p.t[i]!, k, this.t[i]);
      qnlerp(this.r[i]!, p.r[i]!, k, this.r[i]);
    }
    return this;
  }

  /** Post-multiplies bone i's local rotation by q (a rotation in the joint's own frame). */
  rotateLocal(i: number, q: Readonly<Quat>): this {
    qmul(this.r[i]!, q, this.r[i]);
    return this;
  }

  /** Pre-multiplies bone i's local rotation by q (a rotation in the parent's frame). */
  rotateParent(i: number, q: Readonly<Quat>): this {
    qmul(q, this.r[i]!, this.r[i]);
    return this;
  }
}

/** Bone heads and rotations in world space. */
export class WorldPose {
  readonly skeleton: Skeleton;
  readonly p: V3[];
  readonly q: Quat[];

  constructor(skeleton: Skeleton) {
    this.skeleton = skeleton;
    this.p = skeleton.restHead.map((v) => vcopy(v));
    this.q = skeleton.restHead.map(() => qidentity());
  }

  /**
   * Forward kinematics: world transforms of `pose` for a character placed at (`rootPos`,
   * `rootRot`) (model space -> world).
   */
  compute(pose: Pose, rootPos: Readonly<V3>, rootRot: Readonly<Quat>): this {
    const s = this.skeleton;
    const tmp: V3 = [0, 0, 0];
    for (let i = 0; i < s.count; i++) {
      const par = s.parents[i]!;
      const pp = par < 0 ? rootPos : this.p[par]!;
      const pq = par < 0 ? rootRot : this.q[par]!;
      qrotate(pq, pose.t[i]!, tmp);
      vadd(pp, tmp, this.p[i]);
      qmul(pq, pose.r[i]!, this.q[i]);
    }
    return this;
  }

  copyFrom(w: WorldPose): this {
    for (let i = 0; i < this.p.length; i++) {
      vcopy(w.p[i]!, this.p[i]);
      qcopy(w.q[i]!, this.q[i]);
    }
    return this;
  }

  /** World position of a point given in bone i's rest model space (e.g. its rest tail). */
  pointOf(i: number, restPoint: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
    const d = vsub(restPoint, this.skeleton.restHead[i]!, out);
    qrotate(this.q[i]!, d, d);
    return vadd(this.p[i]!, d, out);
  }

  tail(i: number, out: V3 = [0, 0, 0]): V3 {
    return this.pointOf(i, this.skeleton.restTail[i]!, out);
  }

  /**
   * Skin matrices (16 floats per bone, column-major) mapping rest model-space points to world:
   * x -> q_i (x - restHead_i) + p_i.
   */
  writeSkin(out: Float32Array, offset = 0): void {
    const s = this.skeleton;
    for (let i = 0; i < s.count; i++) writeRigid(out, offset + i * 16, this.p[i]!, this.q[i]!, s.restHead[i]!);
  }
}
