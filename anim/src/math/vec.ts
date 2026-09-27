/**
 * 3-vectors as mutable tuples. Every function that produces a vector takes an optional `out`
 * as its last argument: without it a new tuple is allocated (convenient), with it the result is
 * written in place (hot paths). `out` may alias an input.
 */
export type V3 = [number, number, number];

export function v3(x = 0, y = 0, z = 0): V3 {
  return [x, y, z];
}

export function vcopy(a: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
  out[0] = a[0];
  out[1] = a[1];
  out[2] = a[2];
  return out;
}

export function vset(out: V3, x: number, y: number, z: number): V3 {
  out[0] = x;
  out[1] = y;
  out[2] = z;
  return out;
}

export function vadd(a: Readonly<V3>, b: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
  out[0] = a[0] + b[0];
  out[1] = a[1] + b[1];
  out[2] = a[2] + b[2];
  return out;
}

export function vsub(a: Readonly<V3>, b: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
  out[0] = a[0] - b[0];
  out[1] = a[1] - b[1];
  out[2] = a[2] - b[2];
  return out;
}

export function vscale(a: Readonly<V3>, k: number, out: V3 = [0, 0, 0]): V3 {
  out[0] = a[0] * k;
  out[1] = a[1] * k;
  out[2] = a[2] * k;
  return out;
}

/** a + b k */
export function vmadd(a: Readonly<V3>, b: Readonly<V3>, k: number, out: V3 = [0, 0, 0]): V3 {
  out[0] = a[0] + b[0] * k;
  out[1] = a[1] + b[1] * k;
  out[2] = a[2] + b[2] * k;
  return out;
}

export function vlerp(a: Readonly<V3>, b: Readonly<V3>, t: number, out: V3 = [0, 0, 0]): V3 {
  out[0] = a[0] + (b[0] - a[0]) * t;
  out[1] = a[1] + (b[1] - a[1]) * t;
  out[2] = a[2] + (b[2] - a[2]) * t;
  return out;
}

export function vdot(a: Readonly<V3>, b: Readonly<V3>): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

export function vcross(a: Readonly<V3>, b: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
  const x = a[1] * b[2] - a[2] * b[1];
  const y = a[2] * b[0] - a[0] * b[2];
  const z = a[0] * b[1] - a[1] * b[0];
  out[0] = x;
  out[1] = y;
  out[2] = z;
  return out;
}

export function vlen(a: Readonly<V3>): number {
  return Math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

export function vlen2(a: Readonly<V3>): number {
  return a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
}

export function vdist(a: Readonly<V3>, b: Readonly<V3>): number {
  const x = a[0] - b[0];
  const y = a[1] - b[1];
  const z = a[2] - b[2];
  return Math.sqrt(x * x + y * y + z * z);
}

/** Unit vector along a; `fallback` (default +z) when a is (nearly) zero. */
export function vnorm(a: Readonly<V3>, out: V3 = [0, 0, 0], fallback: Readonly<V3> = [0, 0, 1]): V3 {
  const l = vlen(a);
  if (l < 1e-12) return vcopy(fallback, out);
  return vscale(a, 1 / l, out);
}

/** The component of a orthogonal to the unit vector n. */
export function vreject(a: Readonly<V3>, n: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
  return vmadd(a, n, -vdot(a, n), out);
}

export function clamp(x: number, lo: number, hi: number): number {
  return x < lo ? lo : x > hi ? hi : x;
}

export function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

export function smoothstep(e0: number, e1: number, x: number): number {
  const t = clamp((x - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
}

/** Smooth 0..1..0 bump over t in [0, 1]. */
export function bump(t: number): number {
  const s = clamp(t, 0, 1);
  return 16 * s * s * (1 - s) * (1 - s);
}

/** Wraps an angle to (-pi, pi]. */
export function wrapAngle(a: number): number {
  let x = a % (2 * Math.PI);
  if (x > Math.PI) x -= 2 * Math.PI;
  else if (x <= -Math.PI) x += 2 * Math.PI;
  return x;
}

/** Fractional part in [0, 1). */
export function fract(x: number): number {
  return x - Math.floor(x);
}

export const DEG = Math.PI / 180;
