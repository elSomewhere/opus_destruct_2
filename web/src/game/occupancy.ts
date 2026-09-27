/**
 * Client-side solid occupancy (protocol `occupancy` messages): one bit per voxel for every
 * resident chunk, so the player's collision sweeps run on the main thread with exactly the
 * engine's `collide` rules and never wait for a busy worker (a structural bubble can hold the
 * worker for tens of milliseconds per tick).
 */
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

export class OccupancyStore {
  private readonly chunks = new Map<number, ChunkOcc>();
  private h = 0.125;

  /** True once the engine has sent occupancy for the current world. */
  get ready(): boolean {
    return this.chunks.size > 0;
  }

  get chunkCount(): number {
    return this.chunks.size;
  }

  clear(): void {
    this.chunks.clear();
  }

  apply(msg: OccupancyMessage): void {
    this.h = msg.voxelSize;
    for (const c of msg.chunks) {
      const key = chunkKey(c.chunk[0], c.chunk[1], c.chunk[2]);
      if (c.state === 0) this.chunks.delete(key);
      else this.chunks.set(key, { full: c.state === 1, bits: c.state === 2 && c.bits ? new Uint8Array(c.bits) : null });
    }
  }

  /** Whether voxel (x, y, z) is solid (integer voxel coordinates). */
  solid(x: number, y: number, z: number): boolean {
    const c = this.chunks.get(chunkKey(Math.floor(x / 32), Math.floor(y / 32), Math.floor(z / 32)));
    if (!c) return false;
    if (c.full) return true;
    if (!c.bits) return false;
    const v = ((x & 31) * 32 + (y & 31)) * 32 + (z & 31);
    return ((c.bits[v >> 3]! >> (v & 7)) & 1) === 1;
  }

  /** Whether the box overlaps a solid voxel (voxel v spans [v - 1/2, v + 1/2] h). */
  overlaps(mn: Vec3, mx: Vec3): boolean {
    const h = this.h;
    const eps = 1e-4;
    const lo = mn.map((v) => Math.floor((v + eps) / h + 0.5));
    const hi = mx.map((v) => Math.floor((v - eps) / h + 0.5));
    for (let x = lo[0]!; x <= hi[0]!; x++)
      for (let y = lo[1]!; y <= hi[1]!; y++) for (let z = lo[2]!; z <= hi[2]!; z++) if (this.solid(x, y, z)) return true;
    return false;
  }

  /**
   * Smallest upward lift (<= maxRise, in voxel steps) that frees a box stuck in solid voxels
   * (a lift rising under the player, a door closing on them); 0 if it is free, null if nothing
   * within maxRise helps.
   */
  depenetrate(mn: Vec3, mx: Vec3, maxRise: number): number | null {
    if (!this.overlaps(mn, mx)) return 0;
    for (let d = this.h / 2; d <= maxRise + 1e-9; d += this.h / 2) {
      if (!this.overlaps([mn[0], mn[1], mn[2] + d], [mx[0], mx[1], mx[2] + d])) return d;
    }
    return null;
  }

  /** The engine's `collide`: an axis-by-axis (x, y, z) sweep of the box against solid voxels. */
  collide(mn: Vec3, mx: Vec3, move: Vec3): SweepResult {
    const h = this.h;
    const eps = 1e-4;
    const lo: Vec3 = [...mn];
    const hi: Vec3 = [...mx];
    const out: SweepResult = { move: [0, 0, 0], onGround: false };
    const vidx = (x: number): number => Math.floor(x / h + 0.5);
    const p = [0, 0, 0];
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
        if (a === 2 && move[2] < 0) out.onGround = true;
      }
      lo[a] = lo[a]! + dm;
      hi[a] = hi[a]! + dm;
      out.move[a] = dm;
    }
    return out;
  }
}
