/**
 * Column-major 4x4 matrices in Float32Arrays (the GPU layout), for skinning output. The
 * animation itself works on (translation, quaternion) pairs; matrices are only produced at the
 * end.
 */
import type { Quat } from './quat.ts';
import type { V3 } from './vec.ts';

/**
 * Writes the rigid transform x -> q (x - pivot) + t at `offset` (16 floats) of `out`: the skin
 * matrix of a bone whose rest head is `pivot`, now at `t` with rotation `q`.
 */
export function writeRigid(out: Float32Array, offset: number, t: Readonly<V3>, q: Readonly<Quat>, pivot: Readonly<V3> = [0, 0, 0]): void {
  const x = q[0], y = q[1], z = q[2], w = q[3];
  const r00 = 1 - 2 * (y * y + z * z);
  const r01 = 2 * (x * y - z * w);
  const r02 = 2 * (x * z + y * w);
  const r10 = 2 * (x * y + z * w);
  const r11 = 1 - 2 * (x * x + z * z);
  const r12 = 2 * (y * z - x * w);
  const r20 = 2 * (x * z - y * w);
  const r21 = 2 * (y * z + x * w);
  const r22 = 1 - 2 * (x * x + y * y);
  const o = offset;
  out[o] = r00;
  out[o + 1] = r10;
  out[o + 2] = r20;
  out[o + 3] = 0;
  out[o + 4] = r01;
  out[o + 5] = r11;
  out[o + 6] = r21;
  out[o + 7] = 0;
  out[o + 8] = r02;
  out[o + 9] = r12;
  out[o + 10] = r22;
  out[o + 11] = 0;
  out[o + 12] = t[0] - (r00 * pivot[0] + r01 * pivot[1] + r02 * pivot[2]);
  out[o + 13] = t[1] - (r10 * pivot[0] + r11 * pivot[1] + r12 * pivot[2]);
  out[o + 14] = t[2] - (r20 * pivot[0] + r21 * pivot[1] + r22 * pivot[2]);
  out[o + 15] = 1;
}

/** Applies the matrix at `offset` of `m` to point p. */
export function transformPoint(m: Float32Array, offset: number, p: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
  const o = offset;
  const x = p[0], y = p[1], z = p[2];
  out[0] = m[o]! * x + m[o + 4]! * y + m[o + 8]! * z + m[o + 12]!;
  out[1] = m[o + 1]! * x + m[o + 5]! * y + m[o + 9]! * z + m[o + 13]!;
  out[2] = m[o + 2]! * x + m[o + 6]! * y + m[o + 10]! * z + m[o + 14]!;
  return out;
}
