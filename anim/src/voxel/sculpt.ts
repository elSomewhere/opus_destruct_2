/**
 * Building voxel models from signed-distance shapes. A Sculptor holds a dense lattice over the
 * model's box; shapes are added (owned by a bone, in a palette slot), painted over, or carved
 * away, in order, at voxel centres. `finish` splits the lattice into one part per bone, turns
 * the inside of organic shapes into flesh (and bone around the limb axes, for gore), and
 * copies the cells of joint balls into every bone that shares the joint, so limbs rotating
 * about a joint overlap instead of opening gaps.
 */
import type { Skeleton } from '../core/skeleton.ts';
import { qconj, qrotate, type Quat } from '../math/quat.ts';
import type { V3 } from '../math/vec.ts';
import { Slot, VoxelModel, shrinkPart, type SlotId, type VoxelPart } from './model.ts';

export type Sdf = (x: number, y: number, z: number) => number;

/** A signed-distance shape (negative inside) with a conservative bounding box. */
export interface Shape {
  sdf: Sdf;
  min: V3;
  max: V3;
}

// ---- primitives ----------------------------------------------------------------------------

export function sphere(c: V3, r: number): Shape {
  return {
    sdf: (x, y, z) => Math.hypot(x - c[0], y - c[1], z - c[2]) - r,
    min: [c[0] - r, c[1] - r, c[2] - r],
    max: [c[0] + r, c[1] + r, c[2] + r],
  };
}

/** Ellipsoid with semi-axes r (approximate distance, exact sign). */
export function ellipsoid(c: V3, r: V3): Shape {
  return {
    sdf: (x, y, z) => {
      const px = (x - c[0]) / r[0], py = (y - c[1]) / r[1], pz = (z - c[2]) / r[2];
      const k0 = Math.hypot(px, py, pz);
      const k1 = Math.hypot(px / r[0], py / r[1], pz / r[2]);
      return k1 > 0 ? (k0 * (k0 - 1)) / k1 : -Math.min(r[0], r[1], r[2]);
    },
    min: [c[0] - r[0], c[1] - r[1], c[2] - r[2]],
    max: [c[0] + r[0], c[1] + r[1], c[2] + r[2]],
  };
}

/** Capsule from a (radius ra) to b (radius rb): a cone with round ends. */
export function capsule(a: V3, b: V3, ra: number, rb: number = ra): Shape {
  const ba: V3 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const l2 = ba[0] * ba[0] + ba[1] * ba[1] + ba[2] * ba[2];
  const r = Math.max(ra, rb);
  return {
    sdf: (x, y, z) => {
      const px = x - a[0], py = y - a[1], pz = z - a[2];
      const t = l2 > 0 ? Math.min(1, Math.max(0, (px * ba[0] + py * ba[1] + pz * ba[2]) / l2)) : 0;
      const dx = px - ba[0] * t, dy = py - ba[1] * t, dz = pz - ba[2] * t;
      return Math.sqrt(dx * dx + dy * dy + dz * dz) - (ra + (rb - ra) * t);
    },
    min: [Math.min(a[0], b[0]) - r, Math.min(a[1], b[1]) - r, Math.min(a[2], b[2]) - r],
    max: [Math.max(a[0], b[0]) + r, Math.max(a[1], b[1]) + r, Math.max(a[2], b[2]) + r],
  };
}

/**
 * Box with half extents h about c, edges rounded by `round` (inside the extents), optionally
 * rotated by q about c.
 */
export function box(c: V3, h: V3, round = 0, q?: Quat): Shape {
  const inv = q ? qconj(q) : null;
  const p: V3 = [0, 0, 0];
  const reach = Math.hypot(h[0], h[1], h[2]);
  return {
    sdf: (x, y, z) => {
      p[0] = x - c[0];
      p[1] = y - c[1];
      p[2] = z - c[2];
      if (inv) qrotate(inv, p, p);
      const qx = Math.abs(p[0]) - h[0] + round;
      const qy = Math.abs(p[1]) - h[1] + round;
      const qz = Math.abs(p[2]) - h[2] + round;
      const out = Math.hypot(Math.max(qx, 0), Math.max(qy, 0), Math.max(qz, 0));
      return out + Math.min(Math.max(qx, qy, qz), 0) - round;
    },
    min: q ? [c[0] - reach, c[1] - reach, c[2] - reach] : [c[0] - h[0], c[1] - h[1], c[2] - h[2]],
    max: q ? [c[0] + reach, c[1] + reach, c[2] + reach] : [c[0] + h[0], c[1] + h[1], c[2] + h[2]],
  };
}

/** Flat-capped cylinder of radius r from a to b. */
export function cylinder(a: V3, b: V3, r: number): Shape {
  const ba: V3 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const l = Math.hypot(ba[0], ba[1], ba[2]);
  const n: V3 = [ba[0] / l, ba[1] / l, ba[2] / l];
  return {
    sdf: (x, y, z) => {
      const px = x - a[0], py = y - a[1], pz = z - a[2];
      const t = px * n[0] + py * n[1] + pz * n[2];
      const rx = px - n[0] * t, ry = py - n[1] * t, rz = pz - n[2] * t;
      const dr = Math.sqrt(rx * rx + ry * ry + rz * rz) - r;
      const da = Math.abs(t - l / 2) - l / 2;
      return Math.min(Math.max(dr, da), 0) + Math.hypot(Math.max(dr, 0), Math.max(da, 0));
    },
    min: [Math.min(a[0], b[0]) - r, Math.min(a[1], b[1]) - r, Math.min(a[2], b[2]) - r],
    max: [Math.max(a[0], b[0]) + r, Math.max(a[1], b[1]) + r, Math.max(a[2], b[2]) + r],
  };
}

// ---- combinators ---------------------------------------------------------------------------

/** Points of s on the side of the plane (through p, normal n) that n points to. */
export function clip(s: Shape, p: V3, n: V3): Shape {
  return {
    sdf: (x, y, z) => Math.max(s.sdf(x, y, z), -((x - p[0]) * n[0] + (y - p[1]) * n[1] + (z - p[2]) * n[2])),
    min: s.min,
    max: s.max,
  };
}

export function intersect(a: Shape, b: Shape): Shape {
  return {
    sdf: (x, y, z) => Math.max(a.sdf(x, y, z), b.sdf(x, y, z)),
    min: [Math.max(a.min[0], b.min[0]), Math.max(a.min[1], b.min[1]), Math.max(a.min[2], b.min[2])],
    max: [Math.min(a.max[0], b.max[0]), Math.min(a.max[1], b.max[1]), Math.min(a.max[2], b.max[2])],
  };
}

export function subtract(a: Shape, b: Shape): Shape {
  return { sdf: (x, y, z) => Math.max(a.sdf(x, y, z), -b.sdf(x, y, z)), min: a.min, max: a.max };
}

/** Smooth union (polynomial smooth minimum over `k` metres): organic blends of shapes. */
export function smoothUnion(shapes: readonly Shape[], k: number): Shape {
  const smin = (a: number, b: number): number => {
    const h = Math.max(k - Math.abs(a - b), 0) / k;
    return Math.min(a, b) - (h * h * k) / 4;
  };
  return {
    sdf: (x, y, z) => {
      let d = shapes[0]!.sdf(x, y, z);
      for (let i = 1; i < shapes.length; i++) d = smin(d, shapes[i]!.sdf(x, y, z));
      return d;
    },
    min: [Math.min(...shapes.map((s) => s.min[0])) - k, Math.min(...shapes.map((s) => s.min[1])) - k, Math.min(...shapes.map((s) => s.min[2])) - k],
    max: [Math.max(...shapes.map((s) => s.max[0])) + k, Math.max(...shapes.map((s) => s.max[1])) + k, Math.max(...shapes.map((s) => s.max[2])) + k],
  };
}

/** A hollow shell of thickness t on the inside of s. */
export function shell(s: Shape, t: number): Shape {
  return { sdf: (x, y, z) => Math.abs(s.sdf(x, y, z) + t / 2) - t / 2, min: s.min, max: s.max };
}

/** s grown by d (negative shrinks). */
export function inflate(s: Shape, d: number): Shape {
  return {
    sdf: (x, y, z) => s.sdf(x, y, z) - d,
    min: [s.min[0] - d, s.min[1] - d, s.min[2] - d],
    max: [s.max[0] + d, s.max[1] + d, s.max[2] + d],
  };
}

/** Mirror image across x = 0. */
export function mirrorX(s: Shape): Shape {
  return { sdf: (x, y, z) => s.sdf(-x, y, z), min: [-s.max[0], s.min[1], s.min[2]], max: [-s.min[0], s.max[1], s.max[2]] };
}

// ---- sculptor ------------------------------------------------------------------------------

export interface AddOptions {
  /** The inside of organic shapes turns to flesh (and bone) in `finish`. Default true. */
  organic?: boolean;
  /** Brightness multiplier of the voxels (default 1). */
  shade?: number;
  /** Per-voxel brightness jitter amplitude (default 0.035). */
  jitter?: number;
}

export interface PaintOptions {
  /** Only recolour cells in these slots. */
  only?: readonly SlotId[];
  /** Only recolour cells owned by these bones. */
  bones?: readonly number[];
  shade?: number;
  jitter?: number;
}

export interface FinishOptions {
  /** Organic cells at least this many cells deep (6-connected) become flesh (default 2). */
  fleshDepth?: number;
  /** Flesh cells this close (m) to their bone's axis become bone (default 0.018). */
  boneRadius?: number;
  name?: string;
}

/** Deterministic hash of lattice coordinates to [0, 1). */
export function hash3(i: number, j: number, k: number, seed = 0): number {
  let h = (Math.imul(i, 0x27d4eb2d) ^ Math.imul(j, 0x165667b1) ^ Math.imul(k, 0x9e3779b1) ^ Math.imul(seed, 0x85ebca6b)) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x2c1b3c6d) >>> 0;
  h = Math.imul(h ^ (h >>> 12), 0x297a2d39) >>> 0;
  return ((h ^ (h >>> 15)) >>> 0) / 4294967296;
}

export class Sculptor {
  readonly lo: [number, number, number];
  readonly dims: [number, number, number];
  /** slot + 1 per cell (0 empty). */
  readonly cells: Uint8Array;
  readonly bone: Uint8Array;
  readonly shade: Uint8Array;
  readonly organic: Uint8Array;
  private readonly joints: { c: V3; r: number; bones: number[] }[] = [];
  private readonly seed: number;
  readonly skeleton: Skeleton;
  /** Voxel pitch (m). */
  readonly s: number;

  constructor(skeleton: Skeleton, s: number, min: V3, max: V3, seed = 1) {
    this.skeleton = skeleton;
    this.s = s;
    this.lo = [Math.floor(min[0] / s), Math.floor(min[1] / s), Math.floor(min[2] / s)];
    const hi = [Math.ceil(max[0] / s), Math.ceil(max[1] / s), Math.ceil(max[2] / s)];
    this.dims = [hi[0]! - this.lo[0], hi[1]! - this.lo[1], hi[2]! - this.lo[2]];
    const n = this.dims[0] * this.dims[1] * this.dims[2];
    this.cells = new Uint8Array(n);
    this.bone = new Uint8Array(n);
    this.shade = new Uint8Array(n);
    this.organic = new Uint8Array(n);
    this.seed = seed;
  }

  private each(shape: Shape, fn: (idx: number, i: number, j: number, k: number) => void): void {
    const s = this.s;
    const [nx, ny, nz] = this.dims;
    const i0 = Math.max(0, Math.floor(shape.min[0] / s) - this.lo[0]);
    const j0 = Math.max(0, Math.floor(shape.min[1] / s) - this.lo[1]);
    const k0 = Math.max(0, Math.floor(shape.min[2] / s) - this.lo[2]);
    const i1 = Math.min(nx - 1, Math.ceil(shape.max[0] / s) - this.lo[0]);
    const j1 = Math.min(ny - 1, Math.ceil(shape.max[1] / s) - this.lo[1]);
    const k1 = Math.min(nz - 1, Math.ceil(shape.max[2] / s) - this.lo[2]);
    for (let k = k0; k <= k1; k++) {
      const z = (k + this.lo[2] + 0.5) * s;
      for (let j = j0; j <= j1; j++) {
        const y = (j + this.lo[1] + 0.5) * s;
        for (let i = i0; i <= i1; i++) {
          const x = (i + this.lo[0] + 0.5) * s;
          if (shape.sdf(x, y, z) <= 0) fn(i + nx * (j + ny * k), i + this.lo[0], j + this.lo[1], k + this.lo[2]);
        }
      }
    }
  }

  private shadeOf(i: number, j: number, k: number, shade: number, jitter: number): number {
    const n = (hash3(i, j, k, this.seed) + hash3(i >> 1, j >> 1, k >> 1, this.seed + 1)) - 1;
    return Math.max(1, Math.min(255, Math.round(128 * shade * (1 + jitter * n))));
  }

  /** Fills the shape's voxels, owned by `bone`, in `slot` (over whatever was there). */
  add(shape: Shape, bone: number | string, slot: SlotId, opts: AddOptions = {}): this {
    const b = typeof bone === 'string' ? this.skeleton.index(bone) : bone;
    const org = opts.organic ?? true ? 1 : 0;
    const sh = opts.shade ?? 1;
    const jit = opts.jitter ?? 0.035;
    this.each(shape, (idx, i, j, k) => {
      this.cells[idx] = slot + 1;
      this.bone[idx] = b;
      this.organic[idx] = org;
      this.shade[idx] = this.shadeOf(i, j, k, sh, jit);
    });
    return this;
  }

  /** Recolours existing voxels inside the shape. */
  paint(shape: Shape, slot: SlotId, opts: PaintOptions = {}): this {
    const sh = opts.shade ?? 1;
    const jit = opts.jitter ?? 0.035;
    this.each(shape, (idx, i, j, k) => {
      const c = this.cells[idx]!;
      if (c === 0) return;
      if (opts.only && !opts.only.includes((c - 1) as SlotId)) return;
      if (opts.bones && !opts.bones.includes(this.bone[idx]!)) return;
      this.cells[idx] = slot + 1;
      this.shade[idx] = this.shadeOf(i, j, k, sh, jit);
    });
    return this;
  }

  /**
   * Procedural recolouring of every voxel: `fn` gets the voxel centre (m), its lattice
   * coordinates and slot, and returns a new slot (or -1 to keep it).
   */
  paintFn(fn: (x: number, y: number, z: number, slot: SlotId, i: number, j: number, k: number) => number, shade = 1, jitter = 0.035): this {
    const s = this.s;
    const [nx, ny, nz] = this.dims;
    for (let k = 0; k < nz; k++)
      for (let j = 0; j < ny; j++)
        for (let i = 0; i < nx; i++) {
          const idx = i + nx * (j + ny * k);
          const c = this.cells[idx]!;
          if (c === 0) continue;
          const gi = i + this.lo[0], gj = j + this.lo[1], gk = k + this.lo[2];
          const r = fn((gi + 0.5) * s, (gj + 0.5) * s, (gk + 0.5) * s, (c - 1) as SlotId, gi, gj, gk);
          if (r < 0 || r === c - 1) continue;
          this.cells[idx] = r + 1;
          this.shade[idx] = this.shadeOf(gi, gj, gk, shade, jitter);
        }
    return this;
  }

  /** Removes the voxels inside the shape. */
  carve(shape: Shape): this {
    this.each(shape, (idx) => {
      this.cells[idx] = 0;
    });
    return this;
  }

  /**
   * A joint ball: at `finish`, the solid voxels inside it are copied into the parts of all the
   * given bones (so the limbs meeting there overlap when they rotate about it).
   */
  joint(c: V3, r: number, bones: readonly (number | string)[]): this {
    this.joints.push({ c, r, bones: bones.map((b) => (typeof b === 'string' ? this.skeleton.index(b) : b)) });
    return this;
  }

  finish(opts: FinishOptions = {}): VoxelModel {
    const [nx, ny, nz] = this.dims;
    const n = nx * ny * nz;
    // depth of organic cells: 6-connected distance to the nearest empty cell (BFS)
    const fleshDepth = opts.fleshDepth ?? 2;
    const depth = new Uint8Array(n).fill(255);
    const queue = new Int32Array(n);
    let qh = 0, qt = 0;
    for (let k = 0; k < nz; k++)
      for (let j = 0; j < ny; j++)
        for (let i = 0; i < nx; i++) {
          const idx = i + nx * (j + ny * k);
          if (this.cells[idx] === 0) continue;
          const border = i === 0 || j === 0 || k === 0 || i === nx - 1 || j === ny - 1 || k === nz - 1;
          if (
            border ||
            this.cells[idx - 1] === 0 ||
            this.cells[idx + 1] === 0 ||
            this.cells[idx - nx] === 0 ||
            this.cells[idx + nx] === 0 ||
            this.cells[idx - nx * ny] === 0 ||
            this.cells[idx + nx * ny] === 0
          ) {
            depth[idx] = 0;
            queue[qt++] = idx;
          }
        }
    while (qh < qt) {
      const idx = queue[qh++]!;
      const d = depth[idx]!;
      if (d >= fleshDepth) continue;
      const i = idx % nx, j = Math.floor(idx / nx) % ny, k = Math.floor(idx / (nx * ny));
      const nb = [i > 0 ? idx - 1 : -1, i < nx - 1 ? idx + 1 : -1, j > 0 ? idx - nx : -1, j < ny - 1 ? idx + nx : -1, k > 0 ? idx - nx * ny : -1, k < nz - 1 ? idx + nx * ny : -1];
      for (const m of nb) {
        if (m < 0 || this.cells[m] === 0 || depth[m]! <= d + 1) continue;
        depth[m] = d + 1;
        queue[qt++] = m;
      }
    }
    const sk = this.skeleton;
    const boneR = opts.boneRadius ?? 0.018;
    const s = this.s;
    // joints keep their surface colour inside: bending a limb shows the joint's interior
    const inJoint = (x: number, y: number, z: number): boolean => {
      for (const jt of this.joints) if (Math.hypot(x - jt.c[0], y - jt.c[1], z - jt.c[2]) < jt.r + 1.5 * s) return true;
      return false;
    };
    for (let k = 0; k < nz; k++)
      for (let j = 0; j < ny; j++)
        for (let i = 0; i < nx; i++) {
          const idx = i + nx * (j + ny * k);
          if (this.cells[idx] === 0 || !this.organic[idx] || depth[idx]! < fleshDepth) continue;
          const b = this.bone[idx]!;
          const x = (i + this.lo[0] + 0.5) * s, y = (j + this.lo[1] + 0.5) * s, z = (k + this.lo[2] + 0.5) * s;
          if (inJoint(x, y, z)) continue;
          const h = sk.restHead[b]!, t = sk.restTail[b]!;
          const d = distToSegment(x, y, z, h, t);
          const isBone = d < boneR && sk.parents[b]! > 0;
          this.cells[idx] = (isBone ? Slot.Bone : Slot.Flesh) + 1;
          this.shade[idx] = this.shadeOf(i, j, k, isBone ? 1 : 0.9 + 0.2 * hash3(i >> 1, j >> 1, k >> 1, 7), 0.08);
        }

    // cells per bone (owned, plus joint-ball copies)
    const lists: number[][] = sk.names.map(() => []);
    for (let idx = 0; idx < n; idx++) if (this.cells[idx] !== 0) lists[this.bone[idx]!]!.push(idx);
    for (const jt of this.joints) {
      const inside: number[] = [];
      this.each(sphere(jt.c, jt.r), (idx) => {
        if (this.cells[idx] !== 0) inside.push(idx);
      });
      for (const b of jt.bones) {
        const have = new Set(lists[b]);
        for (const idx of inside) if (!have.has(idx)) lists[b]!.push(idx);
      }
    }
    const parts: VoxelPart[] = [];
    lists.forEach((list, b) => {
      if (list.length === 0) return;
      const full: VoxelPart = {
        bone: b,
        origin: [...this.lo],
        dims: [nx, ny, nz],
        cells: new Uint8Array(n),
        shade: new Uint8Array(n),
        count: list.length,
        initialCount: list.length,
        version: 0,
      };
      for (const idx of list) {
        full.cells[idx] = this.cells[idx]!;
        full.shade[idx] = this.shade[idx]!;
      }
      const part = shrinkPart(full);
      part.initialCount = part.count;
      part.version = 0;
      parts.push(part);
    });
    return new VoxelModel(sk, s, parts, opts.name ?? 'model');
  }
}

function distToSegment(x: number, y: number, z: number, a: V3, b: V3): number {
  const bx = b[0] - a[0], by = b[1] - a[1], bz = b[2] - a[2];
  const px = x - a[0], py = y - a[1], pz = z - a[2];
  const l2 = bx * bx + by * by + bz * bz;
  const t = l2 > 0 ? Math.min(1, Math.max(0, (px * bx + py * by + pz * bz) / l2)) : 0;
  return Math.hypot(px - bx * t, py - by * t, pz - bz * t);
}
