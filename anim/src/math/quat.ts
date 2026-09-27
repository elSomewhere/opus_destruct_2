/**
 * Unit quaternions [x, y, z, w] (Hamilton convention, active rotations: v' = q v q*).
 * `qmul(a, b)` applies b first, then a. Same optional-`out` convention as vec.ts.
 */
import type { V3 } from './vec.ts';

export type Quat = [number, number, number, number];

export function qidentity(out: Quat = [0, 0, 0, 1]): Quat {
  out[0] = 0;
  out[1] = 0;
  out[2] = 0;
  out[3] = 1;
  return out;
}

export function qcopy(a: Readonly<Quat>, out: Quat = [0, 0, 0, 1]): Quat {
  out[0] = a[0];
  out[1] = a[1];
  out[2] = a[2];
  out[3] = a[3];
  return out;
}

export function qmul(a: Readonly<Quat>, b: Readonly<Quat>, out: Quat = [0, 0, 0, 1]): Quat {
  const ax = a[0], ay = a[1], az = a[2], aw = a[3];
  const bx = b[0], by = b[1], bz = b[2], bw = b[3];
  out[0] = aw * bx + ax * bw + ay * bz - az * by;
  out[1] = aw * by - ax * bz + ay * bw + az * bx;
  out[2] = aw * bz + ax * by - ay * bx + az * bw;
  out[3] = aw * bw - ax * bx - ay * by - az * bz;
  return out;
}

export function qconj(a: Readonly<Quat>, out: Quat = [0, 0, 0, 1]): Quat {
  out[0] = -a[0];
  out[1] = -a[1];
  out[2] = -a[2];
  out[3] = a[3];
  return out;
}

export function qdot(a: Readonly<Quat>, b: Readonly<Quat>): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

export function qnormalize(a: Readonly<Quat>, out: Quat = [0, 0, 0, 1]): Quat {
  const l = Math.sqrt(qdot(a, a));
  if (l < 1e-12) return qidentity(out);
  out[0] = a[0] / l;
  out[1] = a[1] / l;
  out[2] = a[2] / l;
  out[3] = a[3] / l;
  return out;
}

/** Rotation by `angle` (radians) about the unit `axis`. */
export function qaxis(axis: Readonly<V3>, angle: number, out: Quat = [0, 0, 0, 1]): Quat {
  const s = Math.sin(angle / 2);
  out[0] = axis[0] * s;
  out[1] = axis[1] * s;
  out[2] = axis[2] * s;
  out[3] = Math.cos(angle / 2);
  return out;
}

export function qx(angle: number, out: Quat = [0, 0, 0, 1]): Quat {
  out[0] = Math.sin(angle / 2);
  out[1] = 0;
  out[2] = 0;
  out[3] = Math.cos(angle / 2);
  return out;
}

export function qy(angle: number, out: Quat = [0, 0, 0, 1]): Quat {
  out[0] = 0;
  out[1] = Math.sin(angle / 2);
  out[2] = 0;
  out[3] = Math.cos(angle / 2);
  return out;
}

export function qz(angle: number, out: Quat = [0, 0, 0, 1]): Quat {
  out[0] = 0;
  out[1] = 0;
  out[2] = Math.sin(angle / 2);
  out[3] = Math.cos(angle / 2);
  return out;
}

/**
 * Joint rotation from angles (radians) in the joint's frame (x right, y forward, z up):
 * roll about y first, then pitch about x, then yaw about z (q = qz * qx * qy).
 * With that frame, +x pitch swings a hanging limb forward, +y roll swings it to -x (the
 * character's left), +z yaw turns it to the left.
 */
export function qeuler(x: number, y: number, z: number, out: Quat = [0, 0, 0, 1]): Quat {
  const cx = Math.cos(x / 2), sx = Math.sin(x / 2);
  const cy = Math.cos(y / 2), sy = Math.sin(y / 2);
  const cz = Math.cos(z / 2), sz = Math.sin(z / 2);
  // qz * qx
  const ax = cz * sx;
  const ay = sz * sx;
  const az = sz * cx;
  const aw = cz * cx;
  // (qz qx) * qy, qy = (0, sy, 0, cy)
  out[0] = ax * cy - az * sy;
  out[1] = aw * sy + ay * cy;
  out[2] = az * cy + ax * sy;
  out[3] = aw * cy - ay * sy;
  return out;
}

/** v rotated by q. */
export function qrotate(q: Readonly<Quat>, v: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
  const x = q[0], y = q[1], z = q[2], w = q[3];
  // t = 2 (q.xyz x v); v' = v + w t + q.xyz x t
  const tx = 2 * (y * v[2] - z * v[1]);
  const ty = 2 * (z * v[0] - x * v[2]);
  const tz = 2 * (x * v[1] - y * v[0]);
  out[0] = v[0] + w * tx + (y * tz - z * ty);
  out[1] = v[1] + w * ty + (z * tx - x * tz);
  out[2] = v[2] + w * tz + (x * ty - y * tx);
  return out;
}

/** Normalized linear interpolation along the shorter arc. */
export function qnlerp(a: Readonly<Quat>, b: Readonly<Quat>, t: number, out: Quat = [0, 0, 0, 1]): Quat {
  const s = qdot(a, b) < 0 ? -1 : 1;
  out[0] = a[0] + (s * b[0] - a[0]) * t;
  out[1] = a[1] + (s * b[1] - a[1]) * t;
  out[2] = a[2] + (s * b[2] - a[2]) * t;
  out[3] = a[3] + (s * b[3] - a[3]) * t;
  return qnormalize(out, out);
}

/** Spherical linear interpolation along the shorter arc. */
export function qslerp(a: Readonly<Quat>, b: Readonly<Quat>, t: number, out: Quat = [0, 0, 0, 1]): Quat {
  let d = qdot(a, b);
  let s = 1;
  if (d < 0) {
    d = -d;
    s = -1;
  }
  if (d > 0.9995) return qnlerp(a, b, t, out);
  const th = Math.acos(d);
  const sn = Math.sin(th);
  const wa = Math.sin((1 - t) * th) / sn;
  const wb = (s * Math.sin(t * th)) / sn;
  out[0] = a[0] * wa + b[0] * wb;
  out[1] = a[1] * wa + b[1] * wb;
  out[2] = a[2] * wa + b[2] * wb;
  out[3] = a[3] * wa + b[3] * wb;
  return out;
}

/** Shortest-arc rotation taking unit vector a to unit vector b. */
export function qfromTo(a: Readonly<V3>, b: Readonly<V3>, out: Quat = [0, 0, 0, 1]): Quat {
  const d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  if (d < -0.999999) {
    // opposite: rotate by pi about any axis orthogonal to a
    let ax = 0, ay = -a[2], az = a[1];
    if (ax * ax + ay * ay + az * az < 1e-8) {
      ax = a[2];
      ay = 0;
      az = -a[0];
    }
    const l = Math.sqrt(ax * ax + ay * ay + az * az);
    out[0] = ax / l;
    out[1] = ay / l;
    out[2] = az / l;
    out[3] = 0;
    return out;
  }
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
  out[3] = 1 + d;
  return qnormalize(out, out);
}

/** Rotation vector (axis * angle, radians) of q; the inverse of qexp. */
export function qlog(q: Readonly<Quat>, out: V3 = [0, 0, 0]): V3 {
  let x = q[0], y = q[1], z = q[2], w = q[3];
  if (w < 0) {
    x = -x;
    y = -y;
    z = -z;
    w = -w;
  }
  const s = Math.sqrt(x * x + y * y + z * z);
  if (s < 1e-9) {
    out[0] = 2 * x;
    out[1] = 2 * y;
    out[2] = 2 * z;
    return out;
  }
  const k = (2 * Math.atan2(s, w)) / s;
  out[0] = x * k;
  out[1] = y * k;
  out[2] = z * k;
  return out;
}

/** Quaternion of the rotation vector r (axis * angle). */
export function qexp(r: Readonly<V3>, out: Quat = [0, 0, 0, 1]): Quat {
  const a = Math.sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  if (a < 1e-9) {
    out[0] = r[0] / 2;
    out[1] = r[1] / 2;
    out[2] = r[2] / 2;
    out[3] = 1;
    return qnormalize(out, out);
  }
  const s = Math.sin(a / 2) / a;
  out[0] = r[0] * s;
  out[1] = r[1] * s;
  out[2] = r[2] * s;
  out[3] = Math.cos(a / 2);
  return out;
}

/**
 * Rotation whose columns are the orthonormal basis (x, y, z) (a rotation matrix given by its
 * axes; the basis must be right-handed).
 */
export function qfromBasis(x: Readonly<V3>, y: Readonly<V3>, z: Readonly<V3>, out: Quat = [0, 0, 0, 1]): Quat {
  const m00 = x[0], m10 = x[1], m20 = x[2];
  const m01 = y[0], m11 = y[1], m21 = y[2];
  const m02 = z[0], m12 = z[1], m22 = z[2];
  const tr = m00 + m11 + m22;
  if (tr > 0) {
    const s = Math.sqrt(tr + 1) * 2;
    out[3] = 0.25 * s;
    out[0] = (m21 - m12) / s;
    out[1] = (m02 - m20) / s;
    out[2] = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const s = Math.sqrt(1 + m00 - m11 - m22) * 2;
    out[3] = (m21 - m12) / s;
    out[0] = 0.25 * s;
    out[1] = (m01 + m10) / s;
    out[2] = (m02 + m20) / s;
  } else if (m11 > m22) {
    const s = Math.sqrt(1 + m11 - m00 - m22) * 2;
    out[3] = (m02 - m20) / s;
    out[0] = (m01 + m10) / s;
    out[1] = 0.25 * s;
    out[2] = (m12 + m21) / s;
  } else {
    const s = Math.sqrt(1 + m22 - m00 - m11) * 2;
    out[3] = (m10 - m01) / s;
    out[0] = (m02 + m20) / s;
    out[1] = (m12 + m21) / s;
    out[2] = 0.25 * s;
  }
  return qnormalize(out, out);
}

/** Mirror of a joint rotation across the character's sagittal plane (x -> -x). */
export function qmirrorX(a: Readonly<Quat>, out: Quat = [0, 0, 0, 1]): Quat {
  out[0] = a[0];
  out[1] = -a[1];
  out[2] = -a[2];
  out[3] = a[3];
  return out;
}

/** Heading (yaw about +z, radians) of the rotated +y axis. */
export function qyaw(q: Readonly<Quat>): number {
  const f = qrotate(q, [0, 1, 0]);
  return Math.atan2(f[1], f[0]) - Math.PI / 2;
}
