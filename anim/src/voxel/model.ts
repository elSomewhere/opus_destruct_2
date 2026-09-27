/**
 * Voxel character models: voxels bound to the bones of a skeleton.
 *
 * A model lives on a lattice of pitch `voxelSize` in rest model space: voxel (i, j, k) is the
 * cube centred at ((i + 1/2) s, (j + 1/2) s, (k + 1/2) s), so the ground (z = 0) and the
 * sagittal plane (x = 0) are voxel boundaries. Each bone that carries voxels has one part: a
 * dense box of cells, each empty or holding a palette slot and a shade.
 *
 * Colours are not stored in the model: voxels hold a palette *slot* (skin, hair, top, ...;
 * see Slot) and a shade byte (per-voxel brightness), and a character instance supplies a
 * palette. One model therefore serves many colour variants (Doom's colour translation).
 */
import type { Skeleton } from '../core/skeleton.ts';
import type { V3 } from '../math/vec.ts';

/** Palette slots of the character palettes (16 in all). */
export const Slot = {
  Skin: 0,
  Hair: 1,
  Top: 2,
  Top2: 3,
  Bottom: 4,
  Bottom2: 5,
  Shoes: 6,
  Gear: 7,
  GearDark: 8,
  Metal: 9,
  Furniture: 10,
  Detail: 11,
  Accent: 12,
  Flesh: 13,
  Bone: 14,
  Blood: 15,
} as const;
export type SlotId = (typeof Slot)[keyof typeof Slot];
export const SLOT_COUNT = 16;

/** Linear RGB per slot (SLOT_COUNT entries). */
export type Palette = readonly (readonly [number, number, number])[];

export interface VoxelPart {
  bone: number;
  /** Lattice coordinates of cell (0, 0, 0). */
  origin: [number, number, number];
  dims: [number, number, number];
  /** Per cell (x fastest, then y, then z): 0 empty, else slot + 1. */
  cells: Uint8Array;
  /** Per cell brightness, 128 = 1.0 (shade / 128). */
  shade: Uint8Array;
  /** Solid cells now and when built (damage bookkeeping). */
  count: number;
  initialCount: number;
  /** Bumped on every change (meshes cache by it). */
  version: number;
}

export class VoxelModel {
  readonly skeleton: Skeleton;
  readonly voxelSize: number;
  readonly parts: VoxelPart[];
  /** Free-form name (for caches and debugging). */
  readonly name: string;
  /** Part index per bone (-1: the bone carries no voxels). */
  readonly partOfBone: Int16Array;

  constructor(skeleton: Skeleton, voxelSize: number, parts: VoxelPart[], name = 'model') {
    this.skeleton = skeleton;
    this.voxelSize = voxelSize;
    this.parts = parts;
    this.name = name;
    this.partOfBone = new Int16Array(skeleton.count).fill(-1);
    parts.forEach((p, i) => {
      if (this.partOfBone[p.bone]! >= 0) throw new Error(`two parts for bone ${skeleton.names[p.bone]}`);
      this.partOfBone[p.bone] = i;
    });
  }

  /** Deep copy (damage is applied to a character's own copy of a shared model). */
  clone(): VoxelModel {
    return new VoxelModel(
      this.skeleton,
      this.voxelSize,
      this.parts.map((p) => ({
        bone: p.bone,
        origin: [...p.origin],
        dims: [...p.dims],
        cells: p.cells.slice(),
        shade: p.shade.slice(),
        count: p.count,
        initialCount: p.initialCount,
        version: p.version,
      })),
      this.name,
    );
  }

  get voxelCount(): number {
    let n = 0;
    for (const p of this.parts) n += p.count;
    return n;
  }

  /** Rest model-space centre of lattice cell (i, j, k). */
  cellCentre(i: number, j: number, k: number, out: V3 = [0, 0, 0]): V3 {
    const s = this.voxelSize;
    out[0] = (i + 0.5) * s;
    out[1] = (j + 0.5) * s;
    out[2] = (k + 0.5) * s;
    return out;
  }
}

export function partIndex(p: VoxelPart, x: number, y: number, z: number): number {
  return x + p.dims[0] * (y + p.dims[1] * z);
}

/** Slot + 1 of the cell at lattice (i, j, k), 0 if empty or outside the part. */
export function partCell(p: VoxelPart, i: number, j: number, k: number): number {
  const x = i - p.origin[0];
  const y = j - p.origin[1];
  const z = k - p.origin[2];
  if (x < 0 || y < 0 || z < 0 || x >= p.dims[0] || y >= p.dims[1] || z >= p.dims[2]) return 0;
  return p.cells[x + p.dims[0] * (y + p.dims[1] * z)]!;
}

/** Rest-space bounding box of a part's cells (min, max corners). */
export function partBounds(p: VoxelPart, s: number): [V3, V3] {
  return [
    [p.origin[0] * s, p.origin[1] * s, p.origin[2] * s],
    [(p.origin[0] + p.dims[0]) * s, (p.origin[1] + p.dims[1]) * s, (p.origin[2] + p.dims[2]) * s],
  ];
}

/** A part that keeps only its occupied bounding box (after sculpting or damage). */
export function shrinkPart(p: VoxelPart): VoxelPart {
  const [nx, ny, nz] = p.dims;
  let x0 = nx, y0 = ny, z0 = nz, x1 = -1, y1 = -1, z1 = -1;
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++)
      for (let x = 0; x < nx; x++) {
        if (p.cells[x + nx * (y + ny * z)] === 0) continue;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (z < z0) z0 = z;
        if (x > x1) x1 = x;
        if (y > y1) y1 = y;
        if (z > z1) z1 = z;
      }
  if (x1 < 0) return { ...p, origin: [...p.origin], dims: [0, 0, 0], cells: new Uint8Array(0), shade: new Uint8Array(0), count: 0 };
  const d: [number, number, number] = [x1 - x0 + 1, y1 - y0 + 1, z1 - z0 + 1];
  const cells = new Uint8Array(d[0] * d[1] * d[2]);
  const shade = new Uint8Array(cells.length);
  let count = 0;
  for (let z = 0; z < d[2]; z++)
    for (let y = 0; y < d[1]; y++)
      for (let x = 0; x < d[0]; x++) {
        const src = x + x0 + nx * (y + y0 + ny * (z + z0));
        const dst = x + d[0] * (y + d[1] * z);
        cells[dst] = p.cells[src]!;
        shade[dst] = p.shade[src]!;
        if (cells[dst] !== 0) count++;
      }
  return { bone: p.bone, origin: [p.origin[0] + x0, p.origin[1] + y0, p.origin[2] + z0], dims: d, cells, shade, count, initialCount: p.initialCount, version: p.version + 1 };
}
