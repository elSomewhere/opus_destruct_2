/**
 * Minimal linear algebra for the renderer. Matrices are column-major Float32Array(16),
 * matching WGSL mat4x4f. World space is right-handed with z up.
 */
import type { Vec3 } from '../engine/protocol.ts';

export type Mat4 = Float32Array<ArrayBuffer>;

export function mat4(): Mat4 {
  const m = new Float32Array(16);
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}

export function mat4Identity(out: Mat4): Mat4 {
  out.fill(0);
  out[0] = out[5] = out[10] = out[15] = 1;
  return out;
}

export function mat4Multiply(out: Mat4, a: Mat4, b: Mat4): Mat4 {
  const r = new Float32Array(16);
  for (let c = 0; c < 4; c++) {
    for (let row = 0; row < 4; row++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + row]! * b[c * 4 + k]!;
      r[c * 4 + row] = s;
    }
  }
  out.set(r);
  return out;
}

/**
 * Perspective with reversed depth and an infinite far plane: depth 1 at `near`, 0 at
 * infinity (clear depth to 0, compare 'greater'). WebGPU clip z range is [0, 1].
 */
export function mat4PerspectiveReversedInfinite(out: Mat4, fovY: number, aspect: number, near: number): Mat4 {
  const f = 1 / Math.tan(fovY / 2);
  out.fill(0);
  out[0] = f / aspect;
  out[5] = f;
  out[11] = -1;
  out[14] = near;
  return out;
}

/** View matrix looking from `eye` along `forward` (unit), with `up` roughly +z. */
export function mat4LookDir(out: Mat4, eye: Vec3, forward: Vec3, up: Vec3): Mat4 {
  const f = normalize(forward);
  const s = normalize(cross(f, up));
  const u = cross(s, f);
  out[0] = s[0];
  out[1] = u[0];
  out[2] = -f[0];
  out[3] = 0;
  out[4] = s[1];
  out[5] = u[1];
  out[6] = -f[1];
  out[7] = 0;
  out[8] = s[2];
  out[9] = u[2];
  out[10] = -f[2];
  out[11] = 0;
  out[12] = -dot(s, eye);
  out[13] = -dot(u, eye);
  out[14] = dot(f, eye);
  out[15] = 1;
  return out;
}

/**
 * Rigid transform: rotate by `angle` about unit `axis` through `pivot`, then translate so
 * the pivot lands on `to`. p' = R (p - pivot) + to.
 */
export function mat4RotateAbout(out: Mat4, axis: Vec3, angle: number, pivot: Vec3, to: Vec3): Mat4 {
  const [x, y, z] = axis;
  const c = Math.cos(angle);
  const s = Math.sin(angle);
  const t = 1 - c;
  // Rodrigues rotation, column-major.
  const r00 = t * x * x + c;
  const r01 = t * x * y - s * z;
  const r02 = t * x * z + s * y;
  const r10 = t * x * y + s * z;
  const r11 = t * y * y + c;
  const r12 = t * y * z - s * x;
  const r20 = t * x * z - s * y;
  const r21 = t * y * z + s * x;
  const r22 = t * z * z + c;
  out[0] = r00;
  out[1] = r10;
  out[2] = r20;
  out[3] = 0;
  out[4] = r01;
  out[5] = r11;
  out[6] = r21;
  out[7] = 0;
  out[8] = r02;
  out[9] = r12;
  out[10] = r22;
  out[11] = 0;
  out[12] = to[0] - (r00 * pivot[0] + r01 * pivot[1] + r02 * pivot[2]);
  out[13] = to[1] - (r10 * pivot[0] + r11 * pivot[1] + r12 * pivot[2]);
  out[14] = to[2] - (r20 * pivot[0] + r21 * pivot[1] + r22 * pivot[2]);
  out[15] = 1;
  return out;
}

/**
 * Rigid transform that rotates by the unit quaternion q = [x, y, z, w] about `pivot` and moves
 * the pivot to `to` (column-major).
 */
export function mat4FromQuatAbout(out: Mat4, q: readonly [number, number, number, number], pivot: Vec3, to: Vec3): Mat4 {
  const [x, y, z, w] = q;
  const r00 = 1 - 2 * (y * y + z * z);
  const r01 = 2 * (x * y - z * w);
  const r02 = 2 * (x * z + y * w);
  const r10 = 2 * (x * y + z * w);
  const r11 = 1 - 2 * (x * x + z * z);
  const r12 = 2 * (y * z - x * w);
  const r20 = 2 * (x * z - y * w);
  const r21 = 2 * (y * z + x * w);
  const r22 = 1 - 2 * (x * x + y * y);
  out[0] = r00;
  out[1] = r10;
  out[2] = r20;
  out[3] = 0;
  out[4] = r01;
  out[5] = r11;
  out[6] = r21;
  out[7] = 0;
  out[8] = r02;
  out[9] = r12;
  out[10] = r22;
  out[11] = 0;
  out[12] = to[0] - (r00 * pivot[0] + r01 * pivot[1] + r02 * pivot[2]);
  out[13] = to[1] - (r10 * pivot[0] + r11 * pivot[1] + r12 * pivot[2]);
  out[14] = to[2] - (r20 * pivot[0] + r21 * pivot[1] + r22 * pivot[2]);
  out[15] = 1;
  return out;
}

export function dot(a: Vec3, b: Vec3): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

export function cross(a: Vec3, b: Vec3): Vec3 {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

export function length(a: Vec3): number {
  return Math.hypot(a[0], a[1], a[2]);
}

export function normalize(a: Vec3): Vec3 {
  const l = length(a);
  return l > 0 ? [a[0] / l, a[1] / l, a[2] / l] : [0, 0, 0];
}

export function add(a: Vec3, b: Vec3): Vec3 {
  return [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
}

export function sub(a: Vec3, b: Vec3): Vec3 {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

export function scale(a: Vec3, k: number): Vec3 {
  return [a[0] * k, a[1] * k, a[2] * k];
}

export function madd(a: Vec3, b: Vec3, k: number): Vec3 {
  return [a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k];
}

export function distance(a: Vec3, b: Vec3): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

/**
 * Frustum side and near planes (a,b,c,d with a*x+b*y+c*z+d >= 0 inside) from a
 * view-projection matrix; the far plane is at infinity and omitted.
 */
export function frustumPlanes(viewProj: Mat4, out = new Float32Array(20)): Float32Array {
  const m = viewProj;
  const row = (i: number): [number, number, number, number] => [m[i]!, m[4 + i]!, m[8 + i]!, m[12 + i]!];
  const r0 = row(0);
  const r1 = row(1);
  const r2 = row(2);
  const r3 = row(3);
  const planes = [
    r3.map((v, i) => v + r0[i]!),
    r3.map((v, i) => v - r0[i]!),
    r3.map((v, i) => v + r1[i]!),
    r3.map((v, i) => v - r1[i]!),
    r3.map((v, i) => v - r2[i]!),
  ];
  planes.forEach((p, k) => {
    const l = Math.hypot(p[0]!, p[1]!, p[2]!) || 1;
    for (let i = 0; i < 4; i++) out[k * 4 + i] = p[i]! / l;
  });
  return out;
}

/** Conservative AABB-vs-frustum test (true = possibly visible). */
export function aabbVisible(planes: Float32Array, min: Vec3, max: Vec3): boolean {
  for (let k = 0; k < planes.length; k += 4) {
    const a = planes[k]!;
    const b = planes[k + 1]!;
    const c = planes[k + 2]!;
    const d = planes[k + 3]!;
    const x = a > 0 ? max[0] : min[0];
    const y = b > 0 ? max[1] : min[1];
    const z = c > 0 ? max[2] : min[2];
    if (a * x + b * y + c * z + d < 0) return false;
  }
  return true;
}

/** Sphere-vs-frustum test on normalized planes (true = possibly visible). */
export function sphereVisible(planes: Float32Array, c: Vec3, r: number): boolean {
  for (let k = 0; k < planes.length; k += 4) {
    if (planes[k]! * c[0] + planes[k + 1]! * c[1] + planes[k + 2]! * c[2] + planes[k + 3]! < -r) return false;
  }
  return true;
}
