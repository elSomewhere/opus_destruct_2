/**
 * The oriented grids' places (protocol `grids` messages, docs/GRIDS.md), shared by the renderer
 * (a grid's chunks are drawn with its frame) and the client's collision (the player sweeps
 * against its voxels in its lattice). Grids stand still: one placed anew is there at once. (What
 * moves is the rigid pieces - a lift's car, a turntable: engine/pieces.ts.)
 */
import { GRID_STRIDE, type Vec3 } from './protocol.ts';

type Quat = [number, number, number, number];

/** A lattice in the world: its point p (metres) is at origin + R p (R row-major). */
export interface Lattice {
  origin: Vec3;
  R: Float64Array;
  h: number;
}

export interface GridFrame extends Lattice {
  id: number;
  rot: Quat;
  /** Bumped when origin / rot change (the renderer uploads its model then). */
  version: number;
}

export function rotMatrix(q: Quat, out: Float64Array): Float64Array {
  const [x, y, z, w] = q;
  out[0] = 1 - 2 * (y * y + z * z);
  out[1] = 2 * (x * y - w * z);
  out[2] = 2 * (x * z + w * y);
  out[3] = 2 * (x * y + w * z);
  out[4] = 1 - 2 * (x * x + z * z);
  out[5] = 2 * (y * z - w * x);
  out[6] = 2 * (x * z - w * y);
  out[7] = 2 * (y * z + w * x);
  out[8] = 1 - 2 * (x * x + y * y);
  return out;
}

export class GridFrames {
  private readonly grids = new Map<number, GridFrame>();
  /** Bumped when a grid comes or goes. */
  version = 0;

  get size(): number {
    return this.grids.size;
  }

  get(id: number): GridFrame | undefined {
    return this.grids.get(id);
  }

  values(): MapIterator<GridFrame> {
    return this.grids.values();
  }

  clear(): void {
    this.grids.clear();
    this.version++;
  }

  apply(frames: Float64Array, removed: readonly number[]): void {
    for (const id of removed) if (this.grids.delete(id)) this.version++;
    for (let o = 0; o + GRID_STRIDE <= frames.length; o += GRID_STRIDE) {
      const id = frames[o]!;
      const origin: Vec3 = [frames[o + 1]!, frames[o + 2]!, frames[o + 3]!];
      const rot: Quat = [frames[o + 4]!, frames[o + 5]!, frames[o + 6]!, frames[o + 7]!];
      const h = frames[o + 8]!;
      const g = this.grids.get(id);
      if (!g) {
        this.grids.set(id, { id, origin, rot, R: rotMatrix(rot, new Float64Array(9)), h, version: 1 });
        this.version++;
        continue;
      }
      g.h = h;
      g.origin = origin;
      g.rot = rot;
      rotMatrix(rot, g.R);
      g.version++;
    }
  }
}

/** Lattice point p in the world. */
export function gridToWorld(g: Lattice, p: Vec3): Vec3 {
  const R = g.R;
  return [
    g.origin[0] + R[0]! * p[0] + R[1]! * p[1] + R[2]! * p[2],
    g.origin[1] + R[3]! * p[0] + R[4]! * p[1] + R[5]! * p[2],
    g.origin[2] + R[6]! * p[0] + R[7]! * p[1] + R[8]! * p[2],
  ];
}

/** World point X in the lattice (metres). */
export function worldToGrid(g: Lattice, X: Vec3): Vec3 {
  const R = g.R;
  const d0 = X[0] - g.origin[0];
  const d1 = X[1] - g.origin[1];
  const d2 = X[2] - g.origin[2];
  return [R[0]! * d0 + R[3]! * d1 + R[6]! * d2, R[1]! * d0 + R[4]! * d1 + R[7]! * d2, R[2]! * d0 + R[5]! * d1 + R[8]! * d2];
}
