/**
 * Damped springs for secondary motion (hit flinches, recoil, lags, smoothing): exact
 * integration of x'' = -w^2 (x - target) - 2 z w x', stable at any time step.
 */
import type { V3 } from '../math/vec.ts';

/** Advances one spring coordinate; returns [x, v]. */
export function springStep(x: number, v: number, target: number, omega: number, zeta: number, dt: number): [number, number] {
  if (dt <= 0) return [x, v];
  const y = x - target;
  if (zeta >= 1) {
    // critically (or over-) damped: treat as critical
    const e = Math.exp(-omega * dt);
    const c = v + omega * y;
    const ny = (y + c * dt) * e;
    const nv = (c - omega * (y + c * dt)) * e;
    return [ny + target, nv];
  }
  const wd = omega * Math.sqrt(1 - zeta * zeta);
  const e = Math.exp(-zeta * omega * dt);
  const cs = Math.cos(wd * dt);
  const sn = Math.sin(wd * dt);
  const b = (v + zeta * omega * y) / wd;
  const ny = e * (y * cs + b * sn);
  const nv = e * ((v + zeta * omega * y) * cs - y * wd * sn) - zeta * omega * ny;
  return [ny + target, nv];
}

/** A scalar spring. */
export class Spring {
  x = 0;
  v = 0;
  omega: number;
  zeta: number;

  constructor(omega = 12, zeta = 1, x = 0) {
    this.omega = omega;
    this.zeta = zeta;
    this.x = x;
  }

  update(target: number, dt: number): number {
    const r = springStep(this.x, this.v, target, this.omega, this.zeta, dt);
    this.x = r[0];
    this.v = r[1];
    return this.x;
  }

  /** Adds velocity (an impulse over unit mass). */
  kick(dv: number): void {
    this.v += dv;
  }
}

/** A 3-vector spring (also used for rotation vectors of small angular offsets). */
export class Spring3 {
  readonly x: V3 = [0, 0, 0];
  readonly v: V3 = [0, 0, 0];
  omega: number;
  zeta: number;

  constructor(omega = 12, zeta = 1) {
    this.omega = omega;
    this.zeta = zeta;
  }

  update(target: Readonly<V3>, dt: number): V3 {
    for (let a = 0; a < 3; a++) {
      const r = springStep(this.x[a]!, this.v[a]!, target[a]!, this.omega, this.zeta, dt);
      this.x[a] = r[0];
      this.v[a] = r[1];
    }
    return this.x;
  }

  kick(dv: Readonly<V3>, k = 1): void {
    this.v[0] += dv[0] * k;
    this.v[1] += dv[1] * k;
    this.v[2] += dv[2] * k;
  }

  reset(x: Readonly<V3> = [0, 0, 0]): void {
    this.x[0] = x[0];
    this.x[1] = x[1];
    this.x[2] = x[2];
    this.v[0] = 0;
    this.v[1] = 0;
    this.v[2] = 0;
  }
}
