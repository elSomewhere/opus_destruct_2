/**
 * Client-side solid occupancy (protocol `occupancy` messages): one bit per voxel for every
 * resident chunk of the world grid and of the oriented grids (in their lattices, placed by their
 * frames: engine/gridframes.ts), so the player's collision sweeps run on the main thread with
 * exactly the engine's `collide` rules and never wait for a busy worker (structure solves and a
 * collapse's pieces can hold the worker for tens of milliseconds per tick).
 *
 * The world grid's voxels are swept layer by layer; an oriented grid's as the turned cubes they
 * are (the engine's box-cube sweep, separating axes). What the box stands on is reported with
 * its velocity there: a lift, a turntable carries its rider.
 */
import { gridToWorld, gridVelocity, worldToGrid, type GridFrame, type GridFrames } from '../engine/gridframes.ts';
import type { OccupancyMessage, Vec3 } from '../engine/protocol.ts';
import type { SweepResult } from './stepmove.ts';

const OFF = 1 << 15;

function chunkKey(cx: number, cy: number, cz: number): number {
  // < 2^48: exact in a double
  return ((cx + OFF) * 65536 + (cy + OFF)) * 65536 + (cz + OFF);
}

interface ChunkOcc {
  full: boolean;
  bits: Uint8Array | null;
}

/** An oriented grid's chunks (its lattice) and the lattice box they span (voxels). */
interface GridOcc {
  chunks: Map<number, ChunkOcc>;
  lo: Vec3;
  hi: Vec3;
}

function solidIn(chunks: Map<number, ChunkOcc>, x: number, y: number, z: number): boolean {
  const c = chunks.get(chunkKey(Math.floor(x / 32), Math.floor(y / 32), Math.floor(z / 32)));
  if (!c) return false;
  if (c.full) return true;
  if (!c.bits) return false;
  const v = ((x & 31) * 32 + (y & 31)) * 32 + (z & 31);
  return ((c.bits[v >> 3]! >> (v & 7)) & 1) === 1;
}

/** A sweep's hit on an oriented grid: the free fraction, the surface's normal, the grid. */
export interface GridHit {
  t: number;
  normal: Vec3;
  grid: number;
}

/**
 * The engine's sweep_box_cube: the box (centre ca, half extents ea) moving by V against a cube
 * (centre cb, unit axes u, half side hb), by separating axes. Returns the entry fraction and
 * the normal there (towards the box), or null.
 */
function sweepBoxCube(ca: Vec3, ea: Vec3, V: Vec3, cb: Vec3, u: readonly Vec3[], hb: number): { t: number; n: Vec3 } | null {
  const axes: Vec3[] = [[1, 0, 0], [0, 1, 0], [0, 0, 1], u[0]!, u[1]!, u[2]!];
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 3; j++) {
      const e: Vec3 = i === 0 ? [1, 0, 0] : i === 1 ? [0, 1, 0] : [0, 0, 1];
      const w = u[j]!;
      const c: Vec3 = [e[1] * w[2] - e[2] * w[1], e[2] * w[0] - e[0] * w[2], e[0] * w[1] - e[1] * w[0]];
      if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] > 1e-12) axes.push(c);
    }
  let tin = -Infinity;
  let tout = Infinity;
  let nin: Vec3 = [0, 0, 0];
  const D: Vec3 = [ca[0] - cb[0], ca[1] - cb[1], ca[2] - cb[2]];
  for (const L of axes) {
    const R =
      ea[0] * Math.abs(L[0]) +
      ea[1] * Math.abs(L[1]) +
      ea[2] * Math.abs(L[2]) +
      hb * (Math.abs(dot(L, u[0]!)) + Math.abs(dot(L, u[1]!)) + Math.abs(dot(L, u[2]!)));
    const d0 = dot(L, D);
    const vl = dot(L, V);
    if (Math.abs(vl) < 1e-15) {
      if (Math.abs(d0) > R) return null;
      continue;
    }
    let a = (-R - d0) / vl;
    let b = (R - d0) / vl;
    if (a > b) [a, b] = [b, a];
    if (a > tin) {
      tin = a;
      const ln = Math.hypot(L[0], L[1], L[2]);
      const s = d0 >= 0 ? 1 / ln : -1 / ln;
      nin = [L[0] * s, L[1] * s, L[2] * s];
    }
    tout = Math.min(tout, b);
    if (tin > tout) return null;
  }
  if (tin > 1 || tout < 0 || tin < -1e-9) return null;
  return { t: Math.max(0, tin), n: nin };
}

function dot(a: Vec3, b: Vec3): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/** Whether the box (centre ca, half extents ea) and the cube (centre cb, axes u, half side hb) overlap. */
function boxCubeOverlap(ca: Vec3, ea: Vec3, cb: Vec3, u: readonly Vec3[], hb: number): boolean {
  const D: Vec3 = [ca[0] - cb[0], ca[1] - cb[1], ca[2] - cb[2]];
  const test = (L: Vec3): boolean => {
    const R =
      ea[0] * Math.abs(L[0]) +
      ea[1] * Math.abs(L[1]) +
      ea[2] * Math.abs(L[2]) +
      hb * (Math.abs(dot(L, u[0]!)) + Math.abs(dot(L, u[1]!)) + Math.abs(dot(L, u[2]!)));
    return Math.abs(dot(L, D)) <= R;
  };
  const e: Vec3[] = [
    [1, 0, 0],
    [0, 1, 0],
    [0, 0, 1],
  ];
  for (const L of e) if (!test(L)) return false;
  for (const L of u) if (!test(L)) return false;
  for (const a of e)
    for (const w of u) {
      const c: Vec3 = [a[1] * w[2] - a[2] * w[1], a[2] * w[0] - a[0] * w[2], a[0] * w[1] - a[1] * w[0]];
      if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] > 1e-12 && !test(c)) return false;
    }
  return true;
}

export class OccupancyStore {
  private readonly chunks = new Map<number, ChunkOcc>();
  private readonly grids = new Map<number, GridOcc>();
  private frames: GridFrames | null = null;
  private h = 0.125;

  /** True once the engine has sent occupancy for the current world. */
  get ready(): boolean {
    return this.chunks.size > 0 || this.grids.size > 0;
  }

  get chunkCount(): number {
    let n = this.chunks.size;
    for (const g of this.grids.values()) n += g.chunks.size;
    return n;
  }

  /** The grids' frames the oriented grids' occupancy is placed with. */
  setFrames(frames: GridFrames): void {
    this.frames = frames;
  }

  clear(): void {
    this.chunks.clear();
    this.grids.clear();
  }

  removeGrid(id: number): void {
    this.grids.delete(id);
  }

  apply(msg: OccupancyMessage): void {
    this.h = msg.voxelSize;
    for (const c of msg.chunks) {
      const key = chunkKey(c.chunk[0], c.chunk[1], c.chunk[2]);
      const occ: ChunkOcc = { full: c.state === 1, bits: c.state === 2 && c.bits ? new Uint8Array(c.bits) : null };
      if (c.grid === undefined || c.grid === 0) {
        if (c.state === 0) this.chunks.delete(key);
        else this.chunks.set(key, occ);
        continue;
      }
      let g = this.grids.get(c.grid);
      if (!g) {
        if (c.state === 0) continue;
        g = { chunks: new Map(), lo: [Infinity, Infinity, Infinity], hi: [-Infinity, -Infinity, -Infinity] };
        this.grids.set(c.grid, g);
      }
      if (c.state === 0) g.chunks.delete(key);
      else g.chunks.set(key, occ);
      // (its box: the chunks it holds; a chunk emptied leaves it as large - a bound, not tight)
      if (c.state !== 0)
        for (let a = 0; a < 3; a++) {
          g.lo[a] = Math.min(g.lo[a]!, c.chunk[a]! * 32);
          g.hi[a] = Math.max(g.hi[a]!, c.chunk[a]! * 32 + 31);
        }
    }
  }

  /** Whether world voxel (x, y, z) is solid (integer voxel coordinates of the world grid). */
  solid(x: number, y: number, z: number): boolean {
    return solidIn(this.chunks, x, y, z);
  }

  /** The grids near a world box (their frames and occupancy). */
  private *gridsNear(lo: Vec3, hi: Vec3): Generator<[GridFrame, GridOcc]> {
    if (!this.frames) return;
    for (const [id, g] of this.grids) {
      const f = this.frames.get(id);
      if (!f || g.chunks.size === 0) continue;
      // (its lattice box in the world)
      const h = f.h;
      const wlo: Vec3 = [Infinity, Infinity, Infinity];
      const whi: Vec3 = [-Infinity, -Infinity, -Infinity];
      for (let k = 0; k < 8; k++) {
        const p = gridToWorld(f, [h * ((k & 1 ? g.hi[0] : g.lo[0]) + (k & 1 ? 0.5 : -0.5)), h * ((k & 2 ? g.hi[1] : g.lo[1]) + (k & 2 ? 0.5 : -0.5)), h * ((k & 4 ? g.hi[2] : g.lo[2]) + (k & 4 ? 0.5 : -0.5))]);
        for (let a = 0; a < 3; a++) {
          wlo[a] = Math.min(wlo[a]!, p[a]!);
          whi[a] = Math.max(whi[a]!, p[a]!);
        }
      }
      if (hi[0] < wlo[0] - h || lo[0] > whi[0] + h || hi[1] < wlo[1] - h || lo[1] > whi[1] + h || hi[2] < wlo[2] - h || lo[2] > whi[2] + h) continue;
      yield [f, g];
    }
  }

  /** The solid voxels of a grid whose cubes may meet a world box (in its lattice). */
  private *voxelsNear(f: GridFrame, g: GridOcc, lo: Vec3, hi: Vec3): Generator<Vec3> {
    const h = f.h;
    const llo: Vec3 = [Infinity, Infinity, Infinity];
    const lhi: Vec3 = [-Infinity, -Infinity, -Infinity];
    for (let k = 0; k < 8; k++) {
      const l = worldToGrid(f, [k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]]);
      for (let a = 0; a < 3; a++) {
        llo[a] = Math.min(llo[a]!, l[a]!);
        lhi[a] = Math.max(lhi[a]!, l[a]!);
      }
    }
    const v0 = llo.map((x) => Math.floor(x / h + 0.5) - 1);
    const v1 = lhi.map((x) => Math.floor(x / h + 0.5) + 1);
    for (let x = v0[0]!; x <= v1[0]!; x++)
      for (let y = v0[1]!; y <= v1[1]!; y++)
        for (let z = v0[2]!; z <= v1[2]!; z++) if (solidIn(g.chunks, x, y, z)) yield [x, y, z];
  }

  private static axes(f: GridFrame): Vec3[] {
    const R = f.R;
    return [
      [R[0]!, R[3]!, R[6]!],
      [R[1]!, R[4]!, R[7]!],
      [R[2]!, R[5]!, R[8]!],
    ];
  }

  /**
   * The engine's sweep of the box [mn, mx] by V against the oriented grids' voxels: the first
   * touch (free fraction, normal, grid), or null.
   */
  sweepGrids(mn: Vec3, mx: Vec3, V: Vec3): GridHit | null {
    const ca: Vec3 = [(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2];
    const ea: Vec3 = [(mx[0] - mn[0]) / 2, (mx[1] - mn[1]) / 2, (mx[2] - mn[2]) / 2];
    const slo: Vec3 = [Math.min(mn[0], mn[0] + V[0]), Math.min(mn[1], mn[1] + V[1]), Math.min(mn[2], mn[2] + V[2])];
    const shi: Vec3 = [Math.max(mx[0], mx[0] + V[0]), Math.max(mx[1], mx[1] + V[1]), Math.max(mx[2], mx[2] + V[2])];
    let best: GridHit | null = null;
    for (const [f, g] of this.gridsNear(slo, shi)) {
      const u = OccupancyStore.axes(f);
      const h = f.h;
      for (const v of this.voxelsNear(f, g, slo, shi)) {
        const cb = gridToWorld(f, [h * v[0], h * v[1], h * v[2]]);
        const hit = sweepBoxCube(ca, ea, V, cb, u, 0.5 * h);
        if (hit && (!best || hit.t < best.t)) best = { t: hit.t, normal: hit.n, grid: f.id };
      }
    }
    return best;
  }

  /** Whether the box overlaps a solid voxel (voxel v spans [v - 1/2, v + 1/2] h), of any grid. */
  overlaps(mn: Vec3, mx: Vec3): boolean {
    const h = this.h;
    const eps = 1e-4;
    const lo = mn.map((v) => Math.floor((v + eps) / h + 0.5));
    const hi = mx.map((v) => Math.floor((v - eps) / h + 0.5));
    for (let x = lo[0]!; x <= hi[0]!; x++)
      for (let y = lo[1]!; y <= hi[1]!; y++) for (let z = lo[2]!; z <= hi[2]!; z++) if (this.solid(x, y, z)) return true;
    // (the oriented grids' turned cubes)
    const smn: Vec3 = [mn[0] + eps, mn[1] + eps, mn[2] + eps];
    const smx: Vec3 = [mx[0] - eps, mx[1] - eps, mx[2] - eps];
    const ca: Vec3 = [(smn[0] + smx[0]) / 2, (smn[1] + smx[1]) / 2, (smn[2] + smx[2]) / 2];
    const ea: Vec3 = [(smx[0] - smn[0]) / 2, (smx[1] - smn[1]) / 2, (smx[2] - smn[2]) / 2];
    for (const [f, g] of this.gridsNear(smn, smx)) {
      const u = OccupancyStore.axes(f);
      for (const v of this.voxelsNear(f, g, smn, smx)) {
        const cb = gridToWorld(f, [f.h * v[0], f.h * v[1], f.h * v[2]]);
        if (boxCubeOverlap(ca, ea, cb, u, 0.5 * f.h)) return true;
      }
    }
    return false;
  }

  /**
   * Smallest upward lift (<= maxRise, in steps of half a voxel) that frees a box stuck in solid
   * voxels (a lift rising under the player, a door closing on them); 0 if it is free, null if
   * nothing within maxRise helps.
   */
  depenetrate(mn: Vec3, mx: Vec3, maxRise: number): number | null {
    if (!this.overlaps(mn, mx)) return 0;
    for (let d = this.h / 2; d <= maxRise + 1e-9; d += this.h / 2) {
      if (!this.overlaps([mn[0], mn[1], mn[2] + d], [mx[0], mx[1], mx[2] + d])) return d;
    }
    return null;
  }

  /**
   * The engine's `collide`: an axis-by-axis (x, y, z) sweep of the box against the world grid's
   * voxels (layer by layer) and the oriented grids' (turned cubes); what it lands on, with its
   * velocity there.
   */
  collide(mn: Vec3, mx: Vec3, move: Vec3): SweepResult {
    const h = this.h;
    const eps = 1e-4;
    const lo: Vec3 = [...mn];
    const hi: Vec3 = [...mx];
    const out: SweepResult = { move: [0, 0, 0], onGround: false };
    const vidx = (x: number): number => Math.floor(x / h + 0.5);
    const p = [0, 0, 0];
    let ground = -1;
    for (let a = 0; a < 3; a++) {
      let dm = move[a]!;
      if (dm === 0) continue;
      const b = (a + 1) % 3;
      const c = (a + 2) % 3;
      const b0 = vidx(lo[b]! + eps);
      const b1 = vidx(hi[b]! - eps);
      const c0 = vidx(lo[c]! + eps);
      const c1 = vidx(hi[c]! - eps);
      const lead = dm > 0 ? hi[a]! : lo[a]!;
      const target = lead + dm;
      let layer = dm > 0 ? vidx(lead - eps) + 1 : vidx(lead + eps) - 1;
      const last = dm > 0 ? vidx(target - eps) : vidx(target + eps);
      let blocked = false;
      for (; dm > 0 ? layer <= last : layer >= last; layer += dm > 0 ? 1 : -1) {
        for (let ib = b0; ib <= b1 && !blocked; ib++)
          for (let ic = c0; ic <= c1 && !blocked; ic++) {
            p[a] = layer;
            p[b] = ib;
            p[c] = ic;
            if (this.solid(p[0]!, p[1]!, p[2]!)) blocked = true;
          }
        if (blocked) break;
      }
      if (blocked) {
        const face = dm > 0 ? (layer - 0.5) * h : (layer + 0.5) * h;
        dm = dm > 0 ? Math.max(0, face - lead - eps) : Math.min(0, face - lead + eps);
        if (a === 2 && move[2] < 0) {
          out.onGround = true;
          ground = 0;
        }
      }
      // (the oriented grids' voxels: cubes at an angle)
      if (dm !== 0 && this.grids.size > 0) {
        const L: Vec3 = [0, 0, 0];
        L[a] = dm;
        const hit = this.sweepGrids(lo, hi, L);
        if (hit) {
          const free = hit.t * dm;
          dm = dm > 0 ? Math.max(0, free - eps) : Math.min(0, free + eps);
          if (a === 2 && move[2] < 0) {
            out.onGround = true;
            ground = hit.grid;
          }
        }
      }
      lo[a] = lo[a]! + dm;
      hi[a] = hi[a]! + dm;
      out.move[a] = dm;
    }
    if (ground >= 0) {
      out.ground = ground;
      const f = ground > 0 ? this.frames?.get(ground) : undefined;
      if (f) {
        out.groundVelocity = gridVelocity(f, [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]]);
        out.groundAngular = [...f.ang];
      }
    }
    return out;
  }
}
