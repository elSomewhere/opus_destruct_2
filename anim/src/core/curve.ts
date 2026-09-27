/**
 * Keyframe tracks: timed keys of numbers or vectors, sampled with smooth (cubic Hermite with
 * Catmull-Rom tangents, zero at the ends: eases in and out), linear, eased or held segments.
 * They drive the channels of actions (actions.ts): hand and foot targets, strike reach,
 * trunk offsets, weights.
 */

/** How the segment arriving at a key is interpolated. */
export type Ease =
  /** C1 cubic through the keys (default). */
  | 'smooth'
  | 'linear'
  /** Slow start, fast arrival (wind-up into a strike). */
  | 'in'
  /** Fast start, slow arrival (recovery, settling). */
  | 'out'
  | 'inout'
  /** Explosive: most of the motion in the first third (strikes, flinches). */
  | 'snap'
  /** Keeps the previous value until the key. */
  | 'hold';

export interface Key {
  t: number;
  v: number | readonly number[];
  e?: Ease;
}

function ease(e: Ease, u: number): number {
  switch (e) {
    case 'in':
      return u * u * u;
    case 'out':
      return 1 - (1 - u) * (1 - u) * (1 - u);
    case 'inout':
      return u * u * (3 - 2 * u);
    case 'snap':
      return 1 - Math.pow(1 - u, 4);
    case 'hold':
      return u >= 1 ? 1 : 0;
    default:
      return u;
  }
}

export class Track {
  readonly dim: number;
  private readonly t: Float64Array;
  private readonly v: Float64Array;
  private readonly e: Ease[];

  constructor(keys: readonly Key[]) {
    if (keys.length === 0) throw new Error('a track needs keys');
    const sorted = [...keys].sort((a, b) => a.t - b.t);
    const first = sorted[0]!.v;
    this.dim = typeof first === 'number' ? 1 : first.length;
    this.t = new Float64Array(sorted.map((k) => k.t));
    this.v = new Float64Array(sorted.length * this.dim);
    this.e = sorted.map((k) => k.e ?? 'smooth');
    sorted.forEach((k, i) => {
      if (typeof k.v === 'number') this.v[i * this.dim] = k.v;
      else for (let d = 0; d < this.dim; d++) this.v[i * this.dim + d] = k.v[d] ?? 0;
    });
  }

  get start(): number {
    return this.t[0]!;
  }

  get end(): number {
    return this.t[this.t.length - 1]!;
  }

  /** The value at time `time` (clamped to the keys) into `out`. */
  sample(time: number, out: number[] = new Array<number>(this.dim).fill(0)): number[] {
    const n = this.t.length;
    const D = this.dim;
    if (n === 1 || time <= this.t[0]!) {
      for (let d = 0; d < D; d++) out[d] = this.v[d]!;
      return out;
    }
    if (time >= this.t[n - 1]!) {
      for (let d = 0; d < D; d++) out[d] = this.v[(n - 1) * D + d]!;
      return out;
    }
    let i = 0;
    while (i < n - 2 && time >= this.t[i + 1]!) i++;
    const t0 = this.t[i]!, t1 = this.t[i + 1]!;
    const h = t1 - t0;
    const u = h > 0 ? (time - t0) / h : 1;
    const e = this.e[i + 1]!;
    if (e !== 'smooth') {
      const w = ease(e, u);
      for (let d = 0; d < D; d++) out[d] = this.v[i * D + d]! + (this.v[(i + 1) * D + d]! - this.v[i * D + d]!) * w;
      return out;
    }
    // cubic Hermite, Catmull-Rom tangents for uneven spacing (zero at the ends)
    const u2 = u * u, u3 = u2 * u;
    const h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    for (let d = 0; d < D; d++) {
      const p0 = this.v[i * D + d]!, p1 = this.v[(i + 1) * D + d]!;
      let m0 = 0, m1 = 0;
      if (i > 0) m0 = ((p1 - this.v[(i - 1) * D + d]!) / (t1 - this.t[i - 1]!)) * h;
      if (i + 2 < n) m1 = ((this.v[(i + 2) * D + d]! - p0) / (this.t[i + 2]! - t0)) * h;
      out[d] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
    }
    return out;
  }
}
