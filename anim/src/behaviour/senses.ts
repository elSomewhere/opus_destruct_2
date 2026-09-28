/**
 * What a body senses: its balance (centre of mass, its velocity, the capture point and the
 * support under the feet) and what is around it (walls and ledges within reach, the ground).
 *
 * Balance follows the linear inverted pendulum: a body whose centre of mass c moves at v over
 * ground a height h below stops over the capture point xi = c + v / w0 (w0 = sqrt(g / h)). With
 * xi inside the support polygon (the planted feet, a braced hand) it can stand; outside, it has
 * to step there, or fall.
 */
import { clamp, type V3 } from '../math/vec.ts';
import type { CollisionWorld, SphereContact } from '../physics/collision.ts';

/** A convex polygon on the ground (x, y), counter-clockwise. */
export class SupportPolygon {
  readonly xs: number[] = [];
  readonly ys: number[] = [];

  get empty(): boolean {
    return this.xs.length === 0;
  }

  clear(): void {
    this.xs.length = 0;
    this.ys.length = 0;
  }

  /** The convex hull of points (x, y pairs). */
  hull(pts: readonly number[]): void {
    this.clear();
    const n = pts.length / 2;
    if (n === 0) return;
    // gift wrapping (a handful of points)
    let start = 0;
    for (let i = 1; i < n; i++) if (pts[2 * i]! < pts[2 * start]! || (pts[2 * i] === pts[2 * start] && pts[2 * i + 1]! < pts[2 * start + 1]!)) start = i;
    let p = start;
    for (let guard = 0; guard <= n; guard++) {
      this.xs.push(pts[2 * p]!);
      this.ys.push(pts[2 * p + 1]!);
      let q = (p + 1) % n;
      for (let r = 0; r < n; r++) {
        const cr = (pts[2 * q]! - pts[2 * p]!) * (pts[2 * r + 1]! - pts[2 * p + 1]!) - (pts[2 * q + 1]! - pts[2 * p + 1]!) * (pts[2 * r]! - pts[2 * p]!);
        if (cr < 0) q = r;
      }
      p = q;
      if (p === start) break;
    }
  }

  /** Signed distance from (x, y) to the polygon's boundary (negative inside); `out` gets the closest point inside. */
  distance(x: number, y: number, out: [number, number] | null = null): number {
    const n = this.xs.length;
    if (n === 0) {
      if (out) {
        out[0] = x;
        out[1] = y;
      }
      return Infinity;
    }
    if (n === 1) {
      if (out) {
        out[0] = this.xs[0]!;
        out[1] = this.ys[0]!;
      }
      return Math.hypot(x - this.xs[0]!, y - this.ys[0]!);
    }
    let inside = n >= 3;
    let best = Infinity, bx = x, by = y;
    for (let i = 0; i < n; i++) {
      const ax = this.xs[i]!, ay = this.ys[i]!;
      const cx = this.xs[(i + 1) % n]!, cy = this.ys[(i + 1) % n]!;
      const ex = cx - ax, ey = cy - ay;
      // (counter-clockwise: inside is to the left of every edge)
      if (ex * (y - ay) - ey * (x - ax) < 0) inside = false;
      const l2 = ex * ex + ey * ey;
      const t = l2 > 0 ? clamp(((x - ax) * ex + (y - ay) * ey) / l2, 0, 1) : 0;
      const px = ax + ex * t, py = ay + ey * t;
      const d = Math.hypot(x - px, y - py);
      if (d < best) {
        best = d;
        bx = px;
        by = py;
      }
    }
    if (out) {
      out[0] = inside ? x : bx;
      out[1] = inside ? y : by;
    }
    return inside ? -best : best;
  }

  centroid(out: [number, number]): [number, number] {
    const n = this.xs.length;
    let x = 0, y = 0;
    for (let i = 0; i < n; i++) {
      x += this.xs[i]!;
      y += this.ys[i]!;
    }
    out[0] = n > 0 ? x / n : 0;
    out[1] = n > 0 ? y / n : 0;
    return out;
  }
}

/** A surface within reach: a wall, a ledge, a table top. */
export interface Surface {
  /** The point touched (world) and the surface normal (towards the body). */
  point: V3;
  normal: V3;
  /** Horizontal distance from the probe origin. */
  dist: number;
  /** A horizontal top (a table, a railing, a wall's top) rather than a wall face. */
  top: boolean;
}

const DIRS = 12;

/** Probes of the surroundings: walls at chest and hip height in 12 directions, refreshed now and then. */
export class Surroundings {
  /** Nearest wall per direction at chest height and hip height (null: nothing within reach). */
  readonly chest: (Surface | null)[] = new Array(DIRS).fill(null);
  readonly hip: (Surface | null)[] = new Array(DIRS).fill(null);
  /** Seconds since the last probe. */
  age = 99;
  private readonly contact: SphereContact = { push: [0, 0, 0], normal: [0, 0, 1] };

  /**
   * Probes from a body standing at `feet` (ground point) with height scale k: rays out to
   * `reach` metres at chest and hip height.
   */
  probe(world: CollisionWorld, feet: Readonly<V3>, k: number, reach = 1.0): void {
    this.age = 0;
    for (const [list, h] of [
      [this.chest, 1.3],
      [this.hip, 0.95],
    ] as const) {
      const o: V3 = [feet[0], feet[1], feet[2] + h * k];
      for (let i = 0; i < DIRS; i++) {
        const a = (i / DIRS) * Math.PI * 2;
        const d: V3 = [Math.cos(a), Math.sin(a), 0];
        const t = world.raycast(o, d, reach * k);
        if (t < 0) {
          list[i] = null;
          continue;
        }
        const p: V3 = [o[0] + d[0] * t, o[1] + d[1] * t, o[2]];
        // the normal: push a small sphere just in front of the hit out of the wall
        let n: V3 = [-d[0], -d[1], 0];
        const c: V3 = [p[0] - d[0] * 0.02, p[1] - d[1] * 0.02, p[2]];
        if (world.sphere(c, 0.05, this.contact)) {
          const m = this.contact.normal;
          const l = Math.hypot(m[0], m[1]);
          if (l > 0.5) n = [m[0] / l, m[1] / l, 0];
        }
        list[i] = { point: p, normal: n, dist: t, top: false };
      }
    }
  }

  /**
   * The best wall to put a hand on: in direction `dir` (horizontal, unit) or within `spread`
   * radians of it, nearest first; at chest height (else hip height).
   */
  wallToward(dir: Readonly<V3>, spread = 1.2, maxDist = Infinity): Surface | null {
    let best: Surface | null = null;
    let score = Infinity;
    const heading = Math.atan2(dir[1], dir[0]);
    for (const list of [this.chest, this.hip]) {
      for (let i = 0; i < DIRS; i++) {
        const s = list[i];
        if (!s || s.dist > maxDist) continue;
        const a = (i / DIRS) * Math.PI * 2;
        let da = Math.abs(a - heading) % (Math.PI * 2);
        if (da > Math.PI) da = Math.PI * 2 - da;
        if (da > spread) continue;
        const sc = s.dist + da * 0.35 + (list === this.hip ? 0.15 : 0);
        if (sc < score) {
          score = sc;
          best = s;
        }
      }
      if (best) break;
    }
    return best;
  }

  /** The nearest wall in any direction (chest height, else hip), or null. */
  nearest(maxDist = Infinity): Surface | null {
    let best: Surface | null = null;
    for (const list of [this.chest, this.hip]) {
      for (const s of list) if (s && s.dist <= maxDist && (!best || s.dist < best.dist)) best = s;
      if (best) return best;
    }
    return best;
  }
}
