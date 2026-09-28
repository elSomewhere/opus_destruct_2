/**
 * The oriented grids' places (protocol `grids` messages, docs/GRIDS.md), shared by the renderer
 * (a grid's chunks are drawn with its frame) and the client's collision (the player sweeps
 * against its voxels in its lattice).
 *
 * A grid that moves every tick (a kinematic body's: a lift, a door, a turntable) is drawn and
 * felt one engine tick in the past, interpolated between its last two frames, as rigid pieces
 * are: smooth motion, and the collision where the eye sees it. A grid placed anew (moved once)
 * snaps there.
 */
import { GRID_STRIDE, type Vec3 } from './protocol.ts';

type Quat = [number, number, number, number];

/** Engine tick (s): moving grids are placed this far in the past. */
const TICK_S = 1 / 60;

interface Sample {
  t: number;
  origin: Vec3;
  rot: Quat;
}

export interface GridFrame {
  id: number;
  /** Where it is now (interpolated): lattice point p (metres) is at origin + R p. */
  origin: Vec3;
  rot: Quat;
  /** Rotation matrix (row-major) and its voxel size. */
  R: Float64Array;
  h: number;
  /** Its kinematic body (0: the static world) and the velocity field it moves with. */
  body: number;
  vel: Vec3;
  ang: Vec3;
  centre: Vec3;
  /** Bumped when origin / rot change (the renderer uploads its model then). */
  version: number;
  prev: Sample | null;
  cur: Sample;
}

function rotMatrix(q: Quat, out: Float64Array): void {
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

  apply(frames: Float64Array, removed: readonly number[], nowS: number): void {
    for (const id of removed) if (this.grids.delete(id)) this.version++;
    for (let o = 0; o + GRID_STRIDE <= frames.length; o += GRID_STRIDE) {
      const id = frames[o]!;
      const origin: Vec3 = [frames[o + 1]!, frames[o + 2]!, frames[o + 3]!];
      const rot: Quat = [frames[o + 4]!, frames[o + 5]!, frames[o + 6]!, frames[o + 7]!];
      const vel: Vec3 = [frames[o + 10]!, frames[o + 11]!, frames[o + 12]!];
      const ang: Vec3 = [frames[o + 13]!, frames[o + 14]!, frames[o + 15]!];
      const moving = vel.some((v) => v !== 0) || ang.some((v) => v !== 0);
      let g = this.grids.get(id);
      if (!g) {
        g = {
          id,
          origin,
          rot,
          R: new Float64Array(9),
          h: frames[o + 8]!,
          body: frames[o + 9]!,
          vel,
          ang,
          centre: [frames[o + 16]!, frames[o + 17]!, frames[o + 18]!],
          version: 1,
          prev: null,
          cur: { t: nowS, origin, rot },
        };
        rotMatrix(rot, g.R);
        this.grids.set(id, g);
        this.version++;
        continue;
      }
      g.h = frames[o + 8]!;
      g.body = frames[o + 9]!;
      g.vel = vel;
      g.ang = ang;
      g.centre = [frames[o + 16]!, frames[o + 17]!, frames[o + 18]!];
      // (a grid in motion: from where it was a tick ago; placed anew, or after a pause: there now)
      const last = g.cur;
      g.prev = moving || nowS - last.t <= 2 * TICK_S ? last : null;
      if (g.prev && nowS - g.prev.t > 2 * TICK_S) g.prev = { t: nowS - TICK_S, origin: g.prev.origin, rot: g.prev.rot };
      g.cur = { t: nowS, origin, rot };
      if (!g.prev) this.place(g, origin, rot);
    }
  }

  /** Places the moving grids where they were one tick before nowS. */
  advance(nowS: number): void {
    for (const g of this.grids.values()) {
      const a = g.prev;
      const b = g.cur;
      if (!a) continue;
      const s = Math.min(1, Math.max(0, (nowS - TICK_S - a.t) / Math.max(1e-6, b.t - a.t)));
      const origin: Vec3 = [
        a.origin[0] + (b.origin[0] - a.origin[0]) * s,
        a.origin[1] + (b.origin[1] - a.origin[1]) * s,
        a.origin[2] + (b.origin[2] - a.origin[2]) * s,
      ];
      const sign = a.rot[0] * b.rot[0] + a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3] < 0 ? -1 : 1;
      const q: Quat = [0, 0, 0, 0];
      let n = 0;
      for (let k = 0; k < 4; k++) {
        q[k] = a.rot[k]! * (1 - s) + sign * b.rot[k]! * s;
        n += q[k]! * q[k]!;
      }
      n = Math.sqrt(n) || 1;
      for (let k = 0; k < 4; k++) q[k] = q[k]! / n;
      this.place(g, origin, q);
      if (s >= 1) g.prev = null; // (arrived: until the next frame comes)
    }
  }

  private place(g: GridFrame, origin: Vec3, rot: Quat): void {
    const same =
      origin[0] === g.origin[0] &&
      origin[1] === g.origin[1] &&
      origin[2] === g.origin[2] &&
      rot[0] === g.rot[0] &&
      rot[1] === g.rot[1] &&
      rot[2] === g.rot[2] &&
      rot[3] === g.rot[3];
    if (same) return;
    g.origin = origin;
    g.rot = rot;
    rotMatrix(rot, g.R);
    g.version++;
  }
}

/** Lattice point p of the grid in the world. */
export function gridToWorld(g: GridFrame, p: Vec3): Vec3 {
  const R = g.R;
  return [
    g.origin[0] + R[0]! * p[0] + R[1]! * p[1] + R[2]! * p[2],
    g.origin[1] + R[3]! * p[0] + R[4]! * p[1] + R[5]! * p[2],
    g.origin[2] + R[6]! * p[0] + R[7]! * p[1] + R[8]! * p[2],
  ];
}

/** World point X in the grid's lattice (metres). */
export function worldToGrid(g: GridFrame, X: Vec3): Vec3 {
  const R = g.R;
  const d0 = X[0] - g.origin[0];
  const d1 = X[1] - g.origin[1];
  const d2 = X[2] - g.origin[2];
  return [R[0]! * d0 + R[3]! * d1 + R[6]! * d2, R[1]! * d0 + R[4]! * d1 + R[7]! * d2, R[2]! * d0 + R[5]! * d1 + R[8]! * d2];
}

/** The grid's velocity at world point X (its kinematic body's field; zero for a static grid). */
export function gridVelocity(g: GridFrame, X: Vec3): Vec3 {
  const w = g.ang;
  const r: Vec3 = [X[0] - g.centre[0], X[1] - g.centre[1], X[2] - g.centre[2]];
  return [g.vel[0] + w[1] * r[2] - w[2] * r[1], g.vel[1] + w[2] * r[0] - w[0] * r[2], g.vel[2] + w[0] * r[1] - w[1] * r[0]];
}
