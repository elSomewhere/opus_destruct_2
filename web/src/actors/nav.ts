/**
 * Navigation for actors on the voxel world: A* over a grid of standable cells (0.25 m), found
 * on the fly from the chunk occupancy, so paths follow the world as it is destroyed. A cell is
 * standable where there is ground within a step up / a drop down of the previous one and room
 * for the actor above it. Paths are smoothed by string pulling (straight walkable segments).
 */
import type { V3 } from 'svx-anim';
import type { WorldAccess } from './env.ts';

export interface NavOptions {
  cell: number;
  /** Actor radius and height checked for room. */
  radius: number;
  height: number;
  /** Largest step up and drop down between cells (m). */
  step: number;
  drop: number;
  maxNodes: number;
}

export interface NavPath {
  points: V3[];
  /** The goal was not reached: the path ends at the reachable cell closest to it. */
  partial: boolean;
}

const DEFAULTS: NavOptions = { cell: 0.25, radius: 0.22, height: 1.7, step: 0.55, drop: 1.1, maxNodes: 3500 };
const DIRS = [
  [1, 0, 1],
  [-1, 0, 1],
  [0, 1, 1],
  [0, -1, 1],
  [1, 1, Math.SQRT2],
  [1, -1, Math.SQRT2],
  [-1, 1, Math.SQRT2],
  [-1, -1, Math.SQRT2],
] as const;

interface Node {
  ix: number;
  iy: number;
  z: number;
  g: number;
  f: number;
  parent: Node | null;
  closed: boolean;
  heap: number;
}

class Heap {
  readonly items: Node[] = [];
  push(n: Node): void {
    this.items.push(n);
    n.heap = this.items.length - 1;
    this.up(n.heap);
  }
  pop(): Node | undefined {
    const a = this.items;
    if (a.length === 0) return undefined;
    const top = a[0]!;
    const last = a.pop()!;
    if (a.length > 0) {
      a[0] = last;
      last.heap = 0;
      this.down(0);
    }
    top.heap = -1;
    return top;
  }
  update(n: Node): void {
    this.up(n.heap);
  }
  get size(): number {
    return this.items.length;
  }
  private up(i: number): void {
    const a = this.items;
    const n = a[i]!;
    while (i > 0) {
      const p = (i - 1) >> 1;
      if (a[p]!.f <= n.f) break;
      a[i] = a[p]!;
      a[i]!.heap = i;
      i = p;
    }
    a[i] = n;
    n.heap = i;
  }
  private down(i: number): void {
    const a = this.items;
    const n = a[i]!;
    for (;;) {
      const l = 2 * i + 1;
      if (l >= a.length) break;
      const r = l + 1;
      const c = r < a.length && a[r]!.f < a[l]!.f ? r : l;
      if (a[c]!.f >= n.f) break;
      a[i] = a[c]!;
      a[i]!.heap = i;
      i = c;
    }
    a[i] = n;
    n.heap = i;
  }
}

export class Navigator {
  readonly world: WorldAccess;
  readonly opts: NavOptions;
  /** Searches run and nodes expanded (for the HUD). */
  searches = 0;
  expanded = 0;

  constructor(world: WorldAccess, opts: Partial<NavOptions> = {}) {
    this.world = world;
    this.opts = { ...DEFAULTS, ...opts };
  }

  /** Ground height of cell (ix, iy) reachable from height z, or null. */
  private stand(ix: number, iy: number, z: number, height: number): number | null {
    const c = this.opts.cell;
    const p = this.world.standAt((ix + 0.5) * c, (iy + 0.5) * c, z, this.opts.step, this.opts.drop, this.opts.radius, height);
    return p ? p[2] : null;
  }

  findPath(from: Readonly<V3>, to: Readonly<V3>, maxNodes = this.opts.maxNodes, height = this.opts.height): NavPath | null {
    this.searches++;
    const c = this.opts.cell;
    const sx = Math.floor(from[0] / c), sy = Math.floor(from[1] / c);
    const gx = Math.floor(to[0] / c), gy = Math.floor(to[1] / c);
    const nodes = new Map<number, Node>();
    const key = (ix: number, iy: number, z: number): number => ((ix + 32768) * 65536 + (iy + 32768)) * 512 + ((Math.round(z / 0.125) + 256) & 511);
    const z0 = this.stand(sx, sy, from[2] + 0.3, height) ?? from[2];
    const start: Node = { ix: sx, iy: sy, z: z0, g: 0, f: 0, parent: null, closed: false, heap: -1 };
    const hdist = (n: { ix: number; iy: number; z: number }): number => Math.hypot(n.ix - gx, n.iy - gy) * c + Math.abs(n.z - to[2]) * 0.5;
    start.f = hdist(start);
    nodes.set(key(sx, sy, z0), start);
    const open = new Heap();
    open.push(start);
    let best = start;
    let bestH = start.f;
    let expanded = 0;
    const standCache = new Map<number, number | null>();
    const standC = (ix: number, iy: number, z: number): number | null => {
      const k = key(ix, iy, z);
      if (standCache.has(k)) return standCache.get(k)!;
      const v = this.stand(ix, iy, z, height);
      standCache.set(k, v);
      return v;
    };
    let goal: Node | null = null;
    while (open.size > 0 && expanded < maxNodes) {
      const n = open.pop()!;
      n.closed = true;
      expanded++;
      if (n.ix === gx && n.iy === gy && Math.abs(n.z - to[2]) < 1.0) {
        goal = n;
        break;
      }
      const hn = hdist(n);
      if (hn < bestH) {
        bestH = hn;
        best = n;
      }
      for (const [dx, dy, cost] of DIRS) {
        const ix = n.ix + dx, iy = n.iy + dy;
        const z = standC(ix, iy, n.z);
        if (z === null) continue;
        if (dx !== 0 && dy !== 0) {
          // no corner cutting
          const za = standC(n.ix + dx, n.iy, n.z);
          const zb = standC(n.ix, n.iy + dy, n.z);
          if (za === null || zb === null) continue;
        }
        const k = key(ix, iy, z);
        let m = nodes.get(k);
        const g = n.g + cost * c + Math.max(0, z - n.z) * 1.5;
        if (m) {
          if (m.closed || g >= m.g) continue;
          m.g = g;
          m.f = g + hdist(m);
          m.parent = n;
          open.update(m);
        } else {
          m = { ix, iy, z, g, f: 0, parent: n, closed: false, heap: -1 };
          m.f = g + hdist(m);
          nodes.set(k, m);
          open.push(m);
        }
      }
    }
    this.expanded += expanded;
    const end = goal ?? best;
    if (end === start && !goal) return null;
    const cells: V3[] = [];
    for (let n: Node | null = end; n; n = n.parent) cells.push([(n.ix + 0.5) * c, (n.iy + 0.5) * c, n.z]);
    cells.reverse();
    if (goal) cells[cells.length - 1] = [to[0], to[1], goal.z];
    return { points: this.smooth([from[0], from[1], z0], cells, height), partial: goal === null };
  }

  /** A walkable straight segment from a to b (ground all along, steps within reach). */
  walkable(a: Readonly<V3>, b: Readonly<V3>, height = this.opts.height): boolean {
    const d = Math.hypot(b[0] - a[0], b[1] - a[1]);
    const n = Math.max(1, Math.ceil(d / (this.opts.cell * 0.8)));
    let z = a[2];
    for (let i = 1; i <= n; i++) {
      const t = i / n;
      const p = this.world.standAt(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, z, this.opts.step, 0.6, this.opts.radius, height);
      if (!p) return false;
      z = p[2];
    }
    return Math.abs(z - b[2]) < 0.6;
  }

  private smooth(from: V3, cells: V3[], height: number): V3[] {
    const out: V3[] = [];
    let cur = from;
    let i = 0;
    while (i < cells.length) {
      // the farthest cell reachable in a straight line (bounded look-ahead)
      let j = Math.min(cells.length - 1, i + 24);
      while (j > i && !this.walkable(cur, cells[j]!, height)) j--;
      cur = cells[j]!;
      out.push(cur);
      i = j + 1;
    }
    return out;
  }

  /**
   * A random standable point between rMin and rMax from p (optionally away from `away`: the
   * direction from `away` through p, within ~70 degrees), or null.
   */
  randomPoint(p: Readonly<V3>, rMin: number, rMax: number, away?: Readonly<V3>, tries = 14, height = this.opts.height): V3 | null {
    let base = Math.random() * Math.PI * 2;
    let spread = Math.PI;
    if (away) {
      base = Math.atan2(p[1] - away[1], p[0] - away[0]);
      spread = 1.2;
    }
    for (let k = 0; k < tries; k++) {
      const a = base + (Math.random() * 2 - 1) * spread;
      const r = rMin + Math.random() * (rMax - rMin);
      const x = p[0] + Math.cos(a) * r, y = p[1] + Math.sin(a) * r;
      const s = this.world.standAt(x, y, p[2] + 0.5, 1.2, 2.5, this.opts.radius, height);
      if (s) return s;
    }
    return null;
  }
}
