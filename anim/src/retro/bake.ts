/**
 * Retro baking: a posed voxel character re-voxelized into one axis-aligned voxel model, the way
 * Voxel Doom has one voxel model per sprite frame. The frame has no joints (nothing rotates
 * apart, no overlaps), sits on its own lattice (optionally coarser than the character's) and
 * is drawn with a single matrix at the character's root.
 *
 * Inverse mapping: every target cell centre (or a few sub-samples of it, when the target
 * lattice is coarser) is mapped back into each posed part's rest lattice and looked up there,
 * so rotated parts leave no holes.
 */
import type { WorldPose } from '../core/skeleton.ts';
import type { Quat } from '../math/quat.ts';
import type { V3 } from '../math/vec.ts';
import { propSkeleton } from '../humanoid/rig.ts';
import { Slot, VoxelModel, type VoxelPart } from '../voxel/model.ts';

export interface BakeOptions {
  /** Target voxel pitch (default: the model's; e.g. 1/16 for a chunkier look). */
  voxelSize?: number;
}

/** A one-bone prop model placed in model space: rest point x goes to rot x + pos. */
export interface BakeProp {
  model: VoxelModel;
  pos: V3;
  rot: Quat;
}

/** A posed part: rest point x = M w + c for a model-space point w. */
interface Source {
  part: VoxelPart;
  s: number;
  m: number[]; // 3x3 row-major inverse rotation
  c: V3;
  min: V3;
  max: V3;
}

const FLESH = Slot.Flesh + 1;
const BONE = Slot.Bone + 1;

function source(part: VoxelPart, s: number, restHead: Readonly<V3>, p: Readonly<V3>, q: Readonly<Quat>): Source {
  const [x, y, z, w] = q;
  // R (rest -> posed), rows
  const r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
  const r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
  const r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
  // inverse: rest = R^T (w - p) + restHead
  const m = [r00, r10, r20, r01, r11, r21, r02, r12, r22];
  const c: V3 = [
    restHead[0] - (m[0]! * p[0] + m[1]! * p[1] + m[2]! * p[2]),
    restHead[1] - (m[3]! * p[0] + m[4]! * p[1] + m[5]! * p[2]),
    restHead[2] - (m[6]! * p[0] + m[7]! * p[1] + m[8]! * p[2]),
  ];
  // posed AABB of the part's rest box
  const lo = [part.origin[0] * s, part.origin[1] * s, part.origin[2] * s];
  const hi = [(part.origin[0] + part.dims[0]) * s, (part.origin[1] + part.dims[1]) * s, (part.origin[2] + part.dims[2]) * s];
  const min: V3 = [Infinity, Infinity, Infinity];
  const max: V3 = [-Infinity, -Infinity, -Infinity];
  for (let k = 0; k < 8; k++) {
    const rx = (k & 1 ? hi[0]! : lo[0]!) - restHead[0];
    const ry = (k & 2 ? hi[1]! : lo[1]!) - restHead[1];
    const rz = (k & 4 ? hi[2]! : lo[2]!) - restHead[2];
    const wx = r00 * rx + r01 * ry + r02 * rz + p[0];
    const wy = r10 * rx + r11 * ry + r12 * rz + p[1];
    const wz = r20 * rx + r21 * ry + r22 * rz + p[2];
    if (wx < min[0]) min[0] = wx;
    if (wy < min[1]) min[1] = wy;
    if (wz < min[2]) min[2] = wz;
    if (wx > max[0]) max[0] = wx;
    if (wy > max[1]) max[1] = wy;
    if (wz > max[2]) max[2] = wz;
  }
  return { part, s, m, c, min, max };
}

/**
 * Re-voxelizes `model` posed by `world` (a WorldPose in model space: root at the origin, facing
 * +y; `world.compute(pose, [0, 0, 0], [0, 0, 0, 1])`) plus optional props into a single-part
 * model on an axis-aligned lattice. The result's skeleton is propSkeleton() (one bone at the
 * origin): draw it with the character root's transform.
 */
export function bakePose(model: VoxelModel, world: WorldPose, opts: BakeOptions & { props?: readonly BakeProp[]; name?: string } = {}): VoxelModel {
  const sk = model.skeleton;
  const st = opts.voxelSize ?? model.voxelSize;
  const sources: Source[] = [];
  for (const part of model.parts) {
    if (part.count === 0) continue;
    sources.push(source(part, model.voxelSize, sk.restHead[part.bone]!, world.p[part.bone]!, world.q[part.bone]!));
  }
  for (const pr of opts.props ?? []) {
    for (const part of pr.model.parts) {
      if (part.count === 0) continue;
      sources.push(source(part, pr.model.voxelSize, pr.model.skeleton.restHead[part.bone]!, pr.pos, pr.rot));
    }
  }
  const skel = propSkeleton();
  if (sources.length === 0) {
    return new VoxelModel(skel, st, [{ bone: 0, origin: [0, 0, 0], dims: [0, 0, 0], cells: new Uint8Array(0), shade: new Uint8Array(0), count: 0, initialCount: 0, version: 0 }], opts.name ?? `${model.name}-baked`);
  }
  const min: V3 = [Infinity, Infinity, Infinity];
  const max: V3 = [-Infinity, -Infinity, -Infinity];
  for (const src of sources)
    for (let a = 0; a < 3; a++) {
      if (src.min[a]! < min[a]!) min[a] = src.min[a]!;
      if (src.max[a]! > max[a]!) max[a] = src.max[a]!;
    }
  const o: [number, number, number] = [Math.floor(min[0] / st), Math.floor(min[1] / st), Math.floor(min[2] / st)];
  const dims: [number, number, number] = [
    Math.max(1, Math.ceil(max[0] / st) - o[0]),
    Math.max(1, Math.ceil(max[1] / st) - o[1]),
    Math.max(1, Math.ceil(max[2] / st) - o[2]),
  ];
  const [nx, ny, nz] = dims;
  const n = nx * ny * nz;
  const cells = new Uint8Array(n);
  const shade = new Uint8Array(n);
  // coarser targets: n^3 sub-samples per cell, solid when enough of them hit
  const sub = Math.max(1, Math.min(3, Math.round(st / model.voxelSize)));
  const need = sub === 1 ? 1 : Math.max(1, Math.ceil(sub * sub * sub * 0.25)); // (thin limbs survive coarse frames)
  // which sub-samples hit (a bit each: overlapping parts at joints never count twice)
  const hits = sub === 1 ? null : new Uint32Array(n);
  const best = sub === 1 ? null : new Uint8Array(n); // best slot seen (surface slots win)
  const bestShade = sub === 1 ? null : new Uint8Array(n);
  const offs: number[] = [];
  for (let a = 0; a < sub; a++) offs.push(((a + 0.5) / sub - 0.5) * st);

  for (const src of sources) {
    const p = src.part;
    const [px, py, pz] = p.dims;
    const [ox, oy, oz] = p.origin;
    const inv = 1 / src.s;
    const m = src.m;
    const i0 = Math.max(0, Math.floor(src.min[0] / st) - o[0]);
    const j0 = Math.max(0, Math.floor(src.min[1] / st) - o[1]);
    const k0 = Math.max(0, Math.floor(src.min[2] / st) - o[2]);
    const i1 = Math.min(nx - 1, Math.ceil(src.max[0] / st) - o[0]);
    const j1 = Math.min(ny - 1, Math.ceil(src.max[1] / st) - o[1]);
    const k1 = Math.min(nz - 1, Math.ceil(src.max[2] / st) - o[2]);
    for (let k = k0; k <= k1; k++)
      for (let j = j0; j <= j1; j++)
        for (let i = i0; i <= i1; i++) {
          const idx = i + nx * (j + ny * k);
          const cx = (i + o[0] + 0.5) * st, cy = (j + o[1] + 0.5) * st, cz = (k + o[2] + 0.5) * st;
          for (let sz = 0; sz < sub; sz++)
            for (let sy = 0; sy < sub; sy++)
              for (let sx = 0; sx < sub; sx++) {
                const wx = cx + offs[sx]!, wy = cy + offs[sy]!, wz = cz + offs[sz]!;
                const rx = m[0]! * wx + m[1]! * wy + m[2]! * wz + src.c[0];
                const ry = m[3]! * wx + m[4]! * wy + m[5]! * wz + src.c[1];
                const rz = m[6]! * wx + m[7]! * wy + m[8]! * wz + src.c[2];
                const lx = Math.floor(rx * inv) - ox, ly = Math.floor(ry * inv) - oy, lz = Math.floor(rz * inv) - oz;
                if (lx < 0 || ly < 0 || lz < 0 || lx >= px || ly >= py || lz >= pz) continue;
                const si = lx + px * (ly + py * lz);
                const v = p.cells[si]!;
                if (v === 0) continue;
                const interior = v === FLESH || v === BONE;
                if (sub === 1) {
                  const cur = cells[idx]!;
                  // surface colours win over flesh where parts overlap (joints)
                  if (cur === 0 || (!interior && (cur === FLESH || cur === BONE))) {
                    cells[idx] = v;
                    shade[idx] = p.shade[si]!;
                  }
                } else {
                  hits![idx] = hits![idx]! | (1 << (sx + sub * (sy + sub * sz)));
                  const cur = best![idx]!;
                  if (cur === 0 || (!interior && (cur === FLESH || cur === BONE))) {
                    best![idx] = v;
                    bestShade![idx] = p.shade[si]!;
                  }
                }
              }
        }
  }
  let count = 0;
  if (sub === 1) {
    for (let idx = 0; idx < n; idx++) if (cells[idx] !== 0) count++;
  } else {
    for (let idx = 0; idx < n; idx++) {
      if (popcount(hits![idx]!) >= need) {
        cells[idx] = best![idx]!;
        shade[idx] = bestShade![idx]!;
        count++;
      }
    }
  }
  const part: VoxelPart = { bone: 0, origin: o, dims, cells, shade, count, initialCount: count, version: 0 };
  return new VoxelModel(skel, st, [part], opts.name ?? `${model.name}-baked`);
}

function popcount(v: number): number {
  let x = v - ((v >>> 1) & 0x55555555);
  x = (x & 0x33333333) + ((x >>> 2) & 0x33333333);
  return (Math.imul((x + (x >>> 4)) & 0x0f0f0f0f, 0x01010101) >>> 24) & 0xff;
}
