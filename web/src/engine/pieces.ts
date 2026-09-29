/**
 * The rigid pieces as the client's collision feels them: their voxels (a detached or remeshed
 * piece's occupancy: engine/protocol.ts DetachedEvent.occupancy, per shape its lattice at the
 * event's pose and a bit per cell of its box) and their poses (protocol `debris` messages),
 * placed one engine tick in the past and interpolated between the last two poses, as the
 * renderer draws them (render/islands.ts): the player stands on a lift's car, rides a
 * turntable, climbs rubble, where the eye sees it.
 */
import { gridToWorld, rotMatrix, type Lattice } from './gridframes.ts';
import { DEBRIS_STRIDE, type DetachedEvent, type Vec3 } from './protocol.ts';

type Quat = [number, number, number, number];

/** Engine tick (s): pieces are placed this far in the past. */
const TICK_S = 1 / 60;

/** A shape of a piece: its lattice at the pose it was sent at, its voxel box and a bit per cell. */
export interface PieceShape {
  origin: Vec3;
  R: Float64Array;
  h: number;
  lo: Vec3;
  dim: Vec3;
  bits: Uint8Array;
}

interface Sample {
  t: number;
  pos: Vec3;
  rot: Quat;
  vel: Vec3;
  ang: Vec3;
}

export interface PieceBody {
  id: number;
  /** Its centre when its voxels were sent (the pivot of its poses). */
  centroid: Vec3;
  shapes: PieceShape[];
  /** Radius of its voxels about the centroid (a bound). */
  radius: number;
  /** Now (interpolated): its centre, its rotation since its voxels were sent, its motion. */
  pos: Vec3;
  R: Float64Array;
  vel: Vec3;
  ang: Vec3;
  prev: Sample | null;
  cur: Sample | null;
  seen: number;
}

/** The shapes of a piece's occupancy (the layout of svx::piece_occupancy), or null if malformed. */
export function parseOccupancy(buf: ArrayBuffer): PieceShape[] | null {
  const v = new DataView(buf);
  let o = 0;
  const need = (n: number): boolean => o + n <= buf.byteLength;
  if (!need(4)) return null;
  const n = v.getUint32(o, true);
  o += 4;
  const shapes: PieceShape[] = [];
  for (let k = 0; k < n; k++) {
    if (!need(8 * 8 + 6 * 4)) return null;
    const f = (): number => {
      const x = v.getFloat64(o, true);
      o += 8;
      return x;
    };
    const i = (): number => {
      const x = v.getInt32(o, true);
      o += 4;
      return x;
    };
    const origin: Vec3 = [f(), f(), f()];
    const rot: Quat = [f(), f(), f(), f()];
    const h = f();
    const lo: Vec3 = [i(), i(), i()];
    const dim: Vec3 = [i(), i(), i()];
    const cells = dim[0] * dim[1] * dim[2];
    const bytes = (cells + 7) >> 3;
    if (!(cells > 0) || !need(bytes)) return null;
    shapes.push({ origin, R: rotMatrix(rot, new Float64Array(9)), h, lo, dim, bits: new Uint8Array(buf.slice(o, o + bytes)) });
    o += bytes;
  }
  return shapes;
}

/** Whether cell (x, y, z) of a shape's lattice is solid. */
export function shapeSolid(s: PieceShape, x: number, y: number, z: number): boolean {
  const i0 = x - s.lo[0];
  const i1 = y - s.lo[1];
  const i2 = z - s.lo[2];
  if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= s.dim[0] || i1 >= s.dim[1] || i2 >= s.dim[2]) return false;
  const i = (i0 * s.dim[1] + i1) * s.dim[2] + i2;
  return ((s.bits[i >> 3]! >> (i & 7)) & 1) === 1;
}

function mul3(A: Float64Array, B: Float64Array, out: Float64Array): Float64Array {
  for (let r = 0; r < 3; r++)
    for (let c = 0; c < 3; c++) out[3 * r + c] = A[3 * r]! * B[c]! + A[3 * r + 1]! * B[3 + c]! + A[3 * r + 2]! * B[6 + c]!;
  return out;
}

export class PieceBodies {
  private readonly bodies = new Map<number, PieceBody>();
  private stamp = 0;

  get size(): number {
    return this.bodies.size;
  }

  get(id: number): PieceBody | undefined {
    return this.bodies.get(id);
  }

  clear(): void {
    this.bodies.clear();
  }

  /** A piece's voxels (a detached piece, or a remeshed one: its poses from now on are relative to this). */
  add(ev: DetachedEvent): void {
    if (!ev.rigid || !ev.occupancy) return;
    const shapes = parseOccupancy(ev.occupancy);
    if (!shapes || shapes.length === 0) return;
    let radius = 0;
    for (const s of shapes)
      for (let k = 0; k < 8; k++) {
        const p = gridToWorld(s, [
          s.h * (k & 1 ? s.lo[0] + s.dim[0] - 0.5 : s.lo[0] - 0.5),
          s.h * (k & 2 ? s.lo[1] + s.dim[1] - 0.5 : s.lo[1] - 0.5),
          s.h * (k & 4 ? s.lo[2] + s.dim[2] - 0.5 : s.lo[2] - 0.5),
        ]);
        radius = Math.max(radius, Math.hypot(p[0] - ev.centroid[0], p[1] - ev.centroid[1], p[2] - ev.centroid[2]));
      }
    const old = this.bodies.get(ev.id);
    this.bodies.set(ev.id, {
      id: ev.id,
      centroid: [...ev.centroid],
      shapes,
      radius,
      pos: [...ev.centroid],
      R: rotMatrix([0, 0, 0, 1], new Float64Array(9)),
      vel: [...ev.velocity],
      ang: [...ev.angular],
      prev: null,
      cur: null,
      seen: old ? old.seen : 0,
    });
  }

  /**
   * The engine's poses (DEBRIS_STRIDE doubles each). Pieces that had a pose before and are
   * missing now were removed by the engine; a piece whose first pose is on the way is kept.
   */
  applyDebris(poses: Float64Array, nowS: number): void {
    const stamp = ++this.stamp;
    for (let o = 0; o + DEBRIS_STRIDE <= poses.length; o += DEBRIS_STRIDE) {
      const b = this.bodies.get(poses[o]!);
      if (!b) continue;
      const s: Sample = {
        t: nowS,
        pos: [poses[o + 1]!, poses[o + 2]!, poses[o + 3]!],
        rot: [poses[o + 4]!, poses[o + 5]!, poses[o + 6]!, poses[o + 7]!],
        vel: [poses[o + 9]!, poses[o + 10]!, poses[o + 11]!],
        ang: [poses[o + 12]!, poses[o + 13]!, poses[o + 14]!],
      };
      const last = b.cur;
      // (resting pieces are not re-sent every tick: after a gap, from the old pose)
      b.prev =
        last === null
          ? { t: nowS - TICK_S, pos: [...b.centroid], rot: [0, 0, 0, 1], vel: s.vel, ang: s.ang }
          : nowS - last.t > 2 * TICK_S
            ? { ...last, t: nowS - TICK_S }
            : last;
      b.cur = s;
      b.seen = stamp;
    }
    for (const [id, b] of this.bodies) if (b.cur !== null && b.seen !== stamp) this.bodies.delete(id);
  }

  /** Places the pieces where they were one tick before nowS (as drawn). */
  advance(nowS: number): void {
    for (const b of this.bodies.values()) {
      const a = b.prev;
      const c = b.cur;
      if (!a || !c) continue;
      const s = Math.min(1, Math.max(0, (nowS - TICK_S - a.t) / Math.max(1e-6, c.t - a.t)));
      b.pos = [a.pos[0] + (c.pos[0] - a.pos[0]) * s, a.pos[1] + (c.pos[1] - a.pos[1]) * s, a.pos[2] + (c.pos[2] - a.pos[2]) * s];
      const sign = a.rot[0] * c.rot[0] + a.rot[1] * c.rot[1] + a.rot[2] * c.rot[2] + a.rot[3] * c.rot[3] < 0 ? -1 : 1;
      const q: Quat = [0, 0, 0, 0];
      let n = 0;
      for (let k = 0; k < 4; k++) {
        q[k] = a.rot[k]! * (1 - s) + sign * c.rot[k]! * s;
        n += q[k]! * q[k]!;
      }
      n = Math.sqrt(n) || 1;
      for (let k = 0; k < 4; k++) q[k] = q[k]! / n;
      rotMatrix(q, b.R);
      b.vel = c.vel;
      b.ang = c.ang;
    }
  }

  /** The pieces whose voxels may meet a world box. */
  *near(lo: Vec3, hi: Vec3): Generator<PieceBody> {
    for (const b of this.bodies.values()) {
      const r = b.radius;
      if (hi[0] < b.pos[0] - r || lo[0] > b.pos[0] + r || hi[1] < b.pos[1] - r || lo[1] > b.pos[1] + r || hi[2] < b.pos[2] - r || lo[2] > b.pos[2] + r)
        continue;
      yield b;
    }
  }

  /** A shape's lattice where its piece is now. */
  static lattice(b: PieceBody, s: PieceShape): Lattice {
    const d: Vec3 = [s.origin[0] - b.centroid[0], s.origin[1] - b.centroid[1], s.origin[2] - b.centroid[2]];
    const R = b.R;
    return {
      origin: [
        b.pos[0] + R[0]! * d[0] + R[1]! * d[1] + R[2]! * d[2],
        b.pos[1] + R[3]! * d[0] + R[4]! * d[1] + R[5]! * d[2],
        b.pos[2] + R[6]! * d[0] + R[7]! * d[1] + R[8]! * d[2],
      ],
      R: mul3(R, s.R, new Float64Array(9)),
      h: s.h,
    };
  }

  /** A piece's velocity at a world point (its centre's, and its turn). */
  static velocity(b: PieceBody, X: Vec3): Vec3 {
    const w = b.ang;
    const r: Vec3 = [X[0] - b.pos[0], X[1] - b.pos[1], X[2] - b.pos[2]];
    return [b.vel[0] + w[1] * r[2] - w[2] * r[1], b.vel[1] + w[2] * r[0] - w[0] * r[2], b.vel[2] + w[0] * r[1] - w[1] * r[0]];
  }
}
