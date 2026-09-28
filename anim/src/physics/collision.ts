/**
 * What svx_anim needs to know about the world: where the ground is (feet), how to push a
 * sphere out of solid geometry (ragdolls, gibs, blood drops) and what a ray hits (optional).
 * A host implements CollisionWorld over its own world. VoxelCollision implements it over any
 * voxel occupancy (`solid(i, j, k)` with voxel p centred at h p, the structvox convention);
 * FlatGround is a plane for tests and empty scenes.
 */
import type { V3 } from '../math/vec.ts';

export interface SphereContact {
  /** Push-out that frees the sphere (add it to the centre). */
  push: V3;
  /** Unit contact normal (towards the sphere). */
  normal: V3;
}

export interface CollisionWorld {
  /**
   * Height of the walkable surface under (x, y): the top of the first solid found scanning
   * down from zTop, not below zBottom; null if none.
   */
  groundHeight(x: number, y: number, zTop: number, zBottom: number): number | null;
  /** Resolves a sphere against the static world: false if it touches nothing. */
  sphere(c: Readonly<V3>, r: number, out: SphereContact): boolean;
  /** Distance to the first solid along the unit direction d, or -1 within maxDist. */
  raycast(o: Readonly<V3>, d: Readonly<V3>, maxDist: number): number;
}

/** A horizontal plane at height z (default 0). */
export class FlatGround implements CollisionWorld {
  readonly z: number;
  constructor(z = 0) {
    this.z = z;
  }
  groundHeight(_x: number, _y: number, zTop: number, zBottom: number): number | null {
    return this.z <= zTop && this.z >= zBottom ? this.z : null;
  }
  sphere(c: Readonly<V3>, r: number, out: SphereContact): boolean {
    const d = c[2] - this.z;
    if (d >= r) return false;
    out.push[0] = 0;
    out.push[1] = 0;
    out.push[2] = r - d;
    out.normal[0] = 0;
    out.normal[1] = 0;
    out.normal[2] = 1;
    return true;
  }
  raycast(o: Readonly<V3>, d: Readonly<V3>, maxDist: number): number {
    if (d[2] >= 0 || o[2] < this.z) return -1;
    const t = (this.z - o[2]) / d[2];
    return t <= maxDist ? t : -1;
  }
}

/** CollisionWorld over voxel occupancy: voxel (i, j, k) is the cube of side h centred at h (i, j, k). */
export class VoxelCollision implements CollisionWorld {
  readonly h: number;
  readonly solid: (i: number, j: number, k: number) => boolean;

  constructor(h: number, solid: (i: number, j: number, k: number) => boolean) {
    this.h = h;
    this.solid = solid;
  }

  private idx(v: number): number {
    return Math.floor(v / this.h + 0.5);
  }

  groundHeight(x: number, y: number, zTop: number, zBottom: number): number | null {
    if (!Number.isFinite(x + y + zTop + zBottom) || zTop - zBottom > 64) return null;
    const i = this.idx(x);
    const j = this.idx(y);
    const k0 = this.idx(zTop);
    const k1 = this.idx(zBottom);
    let above = this.solid(i, j, k0 + 1);
    for (let k = k0; k >= k1; k--) {
      const s = this.solid(i, j, k);
      if (s && !above) return (k + 0.5) * this.h;
      above = s;
    }
    return null;
  }

  sphere(c: Readonly<V3>, r: number, out: SphereContact): boolean {
    const h = this.h;
    // (a runaway query must not scan the world)
    if (!(r < 4 * 1024 * h) || !Number.isFinite(c[0] + c[1] + c[2]) || r > 2) return false;
    const i0 = this.idx(c[0] - r), i1 = this.idx(c[0] + r);
    const j0 = this.idx(c[1] - r), j1 = this.idx(c[1] + r);
    const k0 = this.idx(c[2] - r), k1 = this.idx(c[2] + r);
    let px = 0, py = 0, pz = 0;
    let hit = false;
    let deepest = 0;
    let dnx = 0, dny = 0, dnz = 1;
    for (let k = k0; k <= k1; k++)
      for (let j = j0; j <= j1; j++)
        for (let i = i0; i <= i1; i++) {
          if (!this.solid(i, j, k)) continue;
          const bx0 = (i - 0.5) * h, by0 = (j - 0.5) * h, bz0 = (k - 0.5) * h;
          const qx = Math.max(bx0, Math.min(c[0], bx0 + h));
          const qy = Math.max(by0, Math.min(c[1], by0 + h));
          const qz = Math.max(bz0, Math.min(c[2], bz0 + h));
          let dx = c[0] - qx, dy = c[1] - qy, dz = c[2] - qz;
          let dist = Math.sqrt(dx * dx + dy * dy + dz * dz);
          let pen: number;
          if (dist > 1e-9) {
            if (dist >= r) continue;
            pen = r - dist;
            dx /= dist;
            dy /= dist;
            dz /= dist;
          } else {
            // centre inside the voxel: leave through the nearest face whose neighbour is open
            const faces: [number, number, number, number][] = [
              [c[0] - bx0, -1, 0, 0],
              [bx0 + h - c[0], 1, 0, 0],
              [c[1] - by0, 0, -1, 0],
              [by0 + h - c[1], 0, 1, 0],
              [c[2] - bz0, 0, 0, -1],
              [bz0 + h - c[2], 0, 0, 1],
            ];
            let best = Infinity;
            dx = 0;
            dy = 0;
            dz = 1;
            for (const [d, fx, fy, fz] of faces) {
              const open = !this.solid(i + fx, j + fy, k + fz);
              const cost = d + (open ? 0 : h * 4);
              if (cost < best) {
                best = cost;
                dx = fx;
                dy = fy;
                dz = fz;
              }
            }
            // (buried - no open face at hand: out through the top, the way it came in)
            if (best >= h * 4) {
              for (let up = 1; up <= 12; up++) {
                if (this.solid(i, j, k + up)) continue;
                best = (k + up - 0.5) * h - c[2];
                dx = 0;
                dy = 0;
                dz = 1;
                break;
              }
            }
            pen = best + r;
            dist = 0;
          }
          hit = true;
          // accumulate the largest push per axis direction (overlapping voxels share faces)
          const ax = dx * pen, ay = dy * pen, az = dz * pen;
          if (Math.abs(ax) > Math.abs(px)) px = ax;
          if (Math.abs(ay) > Math.abs(py)) py = ay;
          if (Math.abs(az) > Math.abs(pz)) pz = az;
          if (pen > deepest) {
            deepest = pen;
            dnx = dx;
            dny = dy;
            dnz = dz;
          }
        }
    if (!hit) return false;
    // resolve along the dominant contact normal first: pushing out on every axis at once
    // would throw a sphere sitting on a floor sideways at voxel edges
    const along = px * dnx + py * dny + pz * dnz;
    out.push[0] = dnx * Math.max(along, deepest);
    out.push[1] = dny * Math.max(along, deepest);
    out.push[2] = dnz * Math.max(along, deepest);
    out.normal[0] = dnx;
    out.normal[1] = dny;
    out.normal[2] = dnz;
    return true;
  }

  raycast(o: Readonly<V3>, d: Readonly<V3>, maxDist: number): number {
    const h = this.h;
    let i = this.idx(o[0]), j = this.idx(o[1]), k = this.idx(o[2]);
    if (this.solid(i, j, k)) return 0;
    const si = d[0] > 0 ? 1 : -1, sj = d[1] > 0 ? 1 : -1, sk = d[2] > 0 ? 1 : -1;
    const next = (p: number, v: number, s: number): number => ((v + s * 0.5) * h - p);
    let tx = d[0] !== 0 ? next(o[0], i, si) / d[0] : Infinity;
    let ty = d[1] !== 0 ? next(o[1], j, sj) / d[1] : Infinity;
    let tz = d[2] !== 0 ? next(o[2], k, sk) / d[2] : Infinity;
    const dx = d[0] !== 0 ? h / Math.abs(d[0]) : Infinity;
    const dy = d[1] !== 0 ? h / Math.abs(d[1]) : Infinity;
    const dz = d[2] !== 0 ? h / Math.abs(d[2]) : Infinity;
    for (let n = 0; n < 4096; n++) {
      let t: number;
      if (tx < ty && tx < tz) {
        t = tx;
        i += si;
        tx += dx;
      } else if (ty < tz) {
        t = ty;
        j += sj;
        ty += dy;
      } else {
        t = tz;
        k += sk;
        tz += dz;
      }
      if (t > maxDist) return -1;
      if (this.solid(i, j, k)) return t;
    }
    return -1;
  }
}
