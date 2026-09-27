/**
 * The humanoid rig: a 23-bone skeleton (fixed bone indices, see H) scaled by a build.
 *
 * Rest pose (model space, x right, y forward, z up, origin on the ground between the feet): a
 * relaxed A-pose, arms ~17 degrees out from the body and feet a hand apart, so sculpted limbs
 * stay clear of the torso. `weapon` is a socket (no voxels): props such as rifles are separate
 * models drawn at the socket's transform, and the hands are placed on them with IK.
 */
import { Skeleton, type BoneDef } from '../core/skeleton.ts';
import type { V3 } from '../math/vec.ts';

export const H = {
  root: 0,
  pelvis: 1,
  spine: 2,
  chest: 3,
  neck: 4,
  head: 5,
  clavicleL: 6,
  upperarmL: 7,
  forearmL: 8,
  handL: 9,
  clavicleR: 10,
  upperarmR: 11,
  forearmR: 12,
  handR: 13,
  thighL: 14,
  shinL: 15,
  footL: 16,
  toeL: 17,
  thighR: 18,
  shinR: 19,
  footR: 20,
  toeR: 21,
  weapon: 22,
} as const;
export type HumanoidBone = (typeof H)[keyof typeof H];

/** Left/right bone pairs (for mirroring). */
export const H_MIRROR: readonly (readonly [number, number])[] = [
  [H.clavicleL, H.clavicleR],
  [H.upperarmL, H.upperarmR],
  [H.forearmL, H.forearmR],
  [H.handL, H.handR],
  [H.thighL, H.thighR],
  [H.shinL, H.shinR],
  [H.footL, H.footR],
  [H.toeL, H.toeR],
];

export interface HumanoidBuild {
  /** Overall scale; 1 = 1.78 m. */
  height: number;
  /** Shoulder width scale. */
  shoulders: number;
  /** Hip width scale. */
  hips: number;
  /** Body thickness (sculpting only). */
  girth: number;
}

export const DEFAULT_BUILD: Readonly<HumanoidBuild> = { height: 1, shoulders: 1, hips: 1, girth: 1 };

/** Bone heads and tails of the unscaled rig (left side; the right side is mirrored). */
const REST: Record<string, { parent: string | null; head: V3; tail: V3; side?: 'shoulder' | 'hip' }> = {
  root: { parent: null, head: [0, 0, 0], tail: [0, 0.15, 0] },
  pelvis: { parent: 'root', head: [0, 0, 0.97], tail: [0, 0, 1.07] },
  spine: { parent: 'pelvis', head: [0, -0.01, 1.07], tail: [0, -0.015, 1.22] },
  chest: { parent: 'spine', head: [0, -0.015, 1.22], tail: [0, -0.005, 1.45] },
  neck: { parent: 'chest', head: [0, -0.005, 1.455], tail: [0, 0.01, 1.545] },
  head: { parent: 'neck', head: [0, 0.01, 1.545], tail: [0, 0.02, 1.765] },
  clavicleL: { parent: 'chest', head: [-0.025, 0.005, 1.42], tail: [-0.175, -0.01, 1.425], side: 'shoulder' },
  upperarmL: { parent: 'clavicleL', head: [-0.185, -0.01, 1.42], tail: [-0.27, -0.01, 1.145], side: 'shoulder' },
  forearmL: { parent: 'upperarmL', head: [-0.27, -0.01, 1.145], tail: [-0.345, 0.005, 0.895], side: 'shoulder' },
  handL: { parent: 'forearmL', head: [-0.345, 0.005, 0.895], tail: [-0.39, 0.01, 0.73], side: 'shoulder' },
  thighL: { parent: 'pelvis', head: [-0.1, 0, 0.925], tail: [-0.1, 0.01, 0.51], side: 'hip' },
  shinL: { parent: 'thighL', head: [-0.1, 0.01, 0.51], tail: [-0.1, -0.015, 0.085], side: 'hip' },
  footL: { parent: 'shinL', head: [-0.1, -0.015, 0.085], tail: [-0.1, 0.115, 0.022], side: 'hip' },
  toeL: { parent: 'footL', head: [-0.1, 0.115, 0.022], tail: [-0.1, 0.175, 0.015], side: 'hip' },
};

const ORDER = [
  'root', 'pelvis', 'spine', 'chest', 'neck', 'head',
  'clavicleL', 'upperarmL', 'forearmL', 'handL',
  'clavicleR', 'upperarmR', 'forearmR', 'handR',
  'thighL', 'shinL', 'footL', 'toeL',
  'thighR', 'shinR', 'footR', 'toeR',
  'weapon',
];

/** The humanoid skeleton for a build (bone i is H[name]). */
export function humanoidSkeleton(build: Partial<HumanoidBuild> = {}): Skeleton {
  const b = { ...DEFAULT_BUILD, ...build };
  const defs: BoneDef[] = [];
  for (const name of ORDER) {
    if (name === 'weapon') {
      const k = b.height;
      defs.push({ name, parent: 'root', head: [0.25 * k, 0.35 * k, 1.25 * k], tail: [0.25 * k, 0.8 * k, 1.25 * k] });
      continue;
    }
    const right = name.endsWith('R') && name !== 'root';
    const src = REST[right ? name.slice(0, -1) + 'L' : name]!;
    const scaleX = src.side === 'shoulder' ? b.shoulders : src.side === 'hip' ? b.hips : 1;
    const map = (p: V3): V3 => [(right ? -1 : 1) * p[0] * scaleX * b.height, p[1] * b.height, p[2] * b.height];
    const parent = src.parent === null ? null : right && src.parent.endsWith('L') ? src.parent.slice(0, -1) + 'R' : src.parent;
    defs.push({ name, parent, head: map(src.head), tail: map(src.tail) });
  }
  return new Skeleton(defs);
}

/** A one-bone skeleton for props (weapons): the prop's origin is its grip. */
export function propSkeleton(): Skeleton {
  return new Skeleton([{ name: 'prop', parent: null, head: [0, 0, 0], tail: [0, 0.3, 0] }]);
}
