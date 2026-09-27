/**
 * A particle ragdoll solver (position-based dynamics with Verlet integration): particles with
 * radii collide with the world (CollisionWorld.sphere) and are held together by distance
 * constraints (bones, braces), distance limits (joint ranges) and hinge constraints (knees and
 * elbows bend one way). A layout (humanoid/ragdoll.ts) maps a skeleton onto particles and back.
 *
 * Fixed substeps, a fixed iteration count, contact friction, and sleep when everything is
 * slow: cheap enough for dozens of bodies.
 */
import { clamp, vcopy, vdot, vsub, type V3 } from '../math/vec.ts';
import type { CollisionWorld, SphereContact } from './collision.ts';

export interface RagdollParticle {
  p: V3;
  prev: V3;
  /** Inverse mass (0 = pinned). */
  w: number;
  r: number;
  /** In contact during the last substep. */
  contact: boolean;
}

export type RagdollConstraint =
  /** |a - b| = rest; stiffness 0..1 per iteration. */
  | { kind: 'dist'; a: number; b: number; rest: number; k: number }
  /** min <= |a - b| <= max. */
  | { kind: 'limit'; a: number; b: number; min: number; max: number }
  /**
   * Hinge: the joint a-m-c bends one way only. Its bend normal ((m - a) x (c - m)) must point
   * along `sign` times the body's side axis f0 -> f1 (the hips for knees, the shoulders for
   * elbows): the middle particle stays on that side of the line a-c, whatever the limb's angle
   * to the body.
   */
  | { kind: 'hinge'; a: number; m: number; c: number; f0: number; f1: number; sign: number };

export interface RagdollOptions {
  gravity?: number;
  substep?: number;
  iterations?: number;
  /** Tangential velocity kept per contact substep (0 = full stop). */
  friction?: number;
  /** Velocity kept per second in the air (air drag). */
  airDamping?: number;
}

export class Ragdoll {
  readonly particles: RagdollParticle[] = [];
  readonly constraints: RagdollConstraint[] = [];
  collision: CollisionWorld;
  gravity: number;
  substep: number;
  iterations: number;
  friction: number;
  airDamping: number;
  asleep = false;
  /** Seconds since the body last moved noticeably. */
  still = 0;
  time = 0;
  private acc = 0;
  private ref: V3[] = [];
  private readonly toneDelta: V3[] = [];
  private readonly pred: V3[] = [];
  /** The fastest particle's speed in the last substep (m/s). */
  private lastMax = Infinity;
  private readonly contact: SphereContact = { push: [0, 0, 0], normal: [0, 0, 1] };
  /** Optional per-particle soft targets (muscle tone) and their weight. */
  targets: V3[] | null = null;
  targetWeight = 0;
  /** Per-particle share of the tone (default 1). */
  targetShare: number[] | null = null;
  /**
   * Called before each substep's constraints (hosts move the tone targets with the body, so the
   * tone holds its shape without holding it in place).
   */
  beforeSubstep: (() => void) | null = null;
  /**
   * Optional per-particle support heights (world z; NaN: none): a particle below its support is
   * pushed back up to it, softly (legs giving way under a collapsing body).
   */
  support: number[] | null = null;

  constructor(collision: CollisionWorld, opts: RagdollOptions = {}) {
    this.collision = collision;
    this.gravity = opts.gravity ?? 9.81;
    this.substep = opts.substep ?? 1 / 120;
    this.iterations = opts.iterations ?? 8;
    this.friction = opts.friction ?? 0.55;
    this.airDamping = opts.airDamping ?? 0.98;
  }

  addParticle(p: Readonly<V3>, r: number, mass = 1): number {
    this.particles.push({ p: vcopy(p), prev: vcopy(p), w: mass > 0 ? 1 / mass : 0, r, contact: false });
    return this.particles.length - 1;
  }

  /** A distance constraint at the current distance. */
  link(a: number, b: number, k = 1): void {
    const pa = this.particles[a]!.p, pb = this.particles[b]!.p;
    this.constraints.push({ kind: 'dist', a, b, rest: Math.hypot(pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2]), k });
  }

  /** A distance range as fractions of the current distance. */
  limit(a: number, b: number, minF: number, maxF: number): void {
    const pa = this.particles[a]!.p, pb = this.particles[b]!.p;
    const d = Math.hypot(pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2]);
    this.constraints.push({ kind: 'limit', a, b, min: d * minF, max: d * maxF });
  }

  /** Sets a particle's velocity (m/s) by moving its previous position. */
  setVelocity(i: number, v: Readonly<V3>, dt = this.substep): void {
    const q = this.particles[i]!;
    q.prev[0] = q.p[0] - v[0] * dt;
    q.prev[1] = q.p[1] - v[1] * dt;
    q.prev[2] = q.p[2] - v[2] * dt;
  }

  velocity(i: number, out: V3 = [0, 0, 0]): V3 {
    const q = this.particles[i]!;
    out[0] = (q.p[0] - q.prev[0]) / this.substep;
    out[1] = (q.p[1] - q.prev[1]) / this.substep;
    out[2] = (q.p[2] - q.prev[2]) / this.substep;
    return out;
  }

  /** Adds a velocity change to particle i (and a fraction to its neighbours, via constraints). */
  impulse(i: number, dv: Readonly<V3>): void {
    const q = this.particles[i]!;
    q.prev[0] -= dv[0] * this.substep;
    q.prev[1] -= dv[1] * this.substep;
    q.prev[2] -= dv[2] * this.substep;
    this.wake();
  }

  /** Radial push: particles within `radius` of `center` gain up to `speed` away from it. */
  blast(center: Readonly<V3>, radius: number, speed: number): void {
    for (let i = 0; i < this.particles.length; i++) {
      const q = this.particles[i]!;
      const d = vsub(q.p, center);
      const l = Math.hypot(d[0], d[1], d[2]);
      if (l >= radius) continue;
      const f = speed * (1 - l / radius);
      const n = l > 1e-6 ? 1 / l : 0;
      this.impulse(i, [d[0] * n * f, d[1] * n * f, d[2] * n * f + f * 0.35]);
    }
  }

  wake(): void {
    this.asleep = false;
    this.still = 0;
    this.ref = [];
    this.lastMax = Infinity;
  }

  /** Index of the particle nearest to a world point. */
  nearest(p: Readonly<V3>): number {
    let best = 0, bd = Infinity;
    this.particles.forEach((q, i) => {
      const d = (q.p[0] - p[0]) ** 2 + (q.p[1] - p[1]) ** 2 + (q.p[2] - p[2]) ** 2;
      if (d < bd) {
        bd = d;
        best = i;
      }
    });
    return best;
  }

  update(dt: number): void {
    if (this.asleep) return;
    this.acc = Math.min(this.acc + dt, 0.1);
    while (this.acc >= this.substep) {
      this.acc -= this.substep;
      this.step(this.substep);
    }
  }

  private step(h: number): void {
    this.time += h;
    // a body nearly at rest: what lies on the ground loses its last motion quickly (settles
    // instead of trembling); what hangs free still falls
    const settling = this.lastMax < 0.4 && this.time > 0.4;
    const air = Math.pow(this.airDamping, h);
    const rest = air * Math.pow(0.05, h);
    let maxMove = 0;
    for (const q of this.particles) {
      if (q.w === 0) continue;
      const damp = settling && q.contact ? rest : air;
      const vx = (q.p[0] - q.prev[0]) * damp;
      const vy = (q.p[1] - q.prev[1]) * damp;
      const vz = (q.p[2] - q.prev[2]) * damp;
      vcopy(q.p, q.prev);
      // cap per-substep motion (no tunnelling through voxel floors)
      const cap = 0.09;
      const m = Math.hypot(vx, vy, vz);
      const s = m > cap ? cap / m : 1;
      q.p[0] += vx * s;
      q.p[1] += vy * s;
      q.p[2] += vz * s - this.gravity * h * h;
      maxMove = Math.max(maxMove, m / h);
    }
    this.beforeSubstep?.();
    // where the particles would go on their own (constraint corrections are measured from it)
    const pred = this.pred;
    for (let i = 0; i < this.particles.length; i++) vcopy(this.particles[i]!.p, (pred[i] ??= [0, 0, 0]));
    const muscle = this.targets && this.targetWeight > 0 ? clamp(this.targetWeight, 0, 1) : 0;
    for (let it = 0; it < this.iterations; it++) {
      for (const c of this.constraints) this.solve(c);
      if (muscle > 0) {
        // (muscles are internal: the pulls move the body's parts, never the body as a whole, so
        // their net momentum is taken back out)
        const t = this.targets!;
        const share = this.targetShare;
        const d = this.toneDelta;
        let mx = 0, my = 0, mz = 0, msum = 0;
        this.particles.forEach((q, i) => {
          const g = t[i];
          const k = g && q.w > 0 ? muscle * 0.08 * (share ? share[i]! : 1) : 0;
          const e = (d[i] ??= [0, 0, 0]);
          if (k <= 0 || !g) {
            e[0] = e[1] = e[2] = NaN;
            return;
          }
          e[0] = (g[0] - q.p[0]) * k;
          e[1] = (g[1] - q.p[1]) * k;
          e[2] = (g[2] - q.p[2]) * k;
          const m = 1 / q.w;
          mx += e[0] * m;
          my += e[1] * m;
          mz += e[2] * m;
          msum += m;
        });
        if (msum > 0) {
          mx /= msum;
          my /= msum;
          mz /= msum;
        }
        this.particles.forEach((q, i) => {
          const e = d[i]!;
          if (e[0] !== e[0]) return;
          q.p[0] += e[0] - mx;
          q.p[1] += e[1] - my;
          q.p[2] += e[2] - mz;
        });
      }
      if (this.support) {
        const sp = this.support;
        for (let i = 0; i < this.particles.length; i++) {
          const z = sp[i]!;
          const q = this.particles[i]!;
          // (inelastic: the particle stops sinking, it is not thrown back up)
          if (z === z && q.p[2] < z) {
            q.p[2] += (z - q.p[2]) * 0.35;
            q.prev[2] = Math.max(q.prev[2], q.p[2]);
          }
        }
      }
      if (it === this.iterations - 1 || it % 2 === 1) this.collide(it === this.iterations - 1);
    }
    // stabilisation: constraints and contacts may move a particle far in one substep (fighting
    // each other in a folded body), but that must not turn into speed; beyond a limit the
    // correction moves the particle without launching it (hits and blasts, applied as
    // velocities outside the solve, are untouched)
    const maxCorr = 1.5 * h;
    for (let i = 0; i < this.particles.length; i++) {
      const q = this.particles[i]!;
      if (q.w === 0) continue;
      const a = pred[i]!;
      const dx = q.p[0] - a[0], dy = q.p[1] - a[1], dz = q.p[2] - a[2];
      const l = Math.hypot(dx, dy, dz);
      if (l <= maxCorr) continue;
      const f = 1 - maxCorr / l;
      q.prev[0] += dx * f;
      q.prev[1] += dy * f;
      q.prev[2] += dz * f;
    }
    // sleep: no particle strayed more than 1.5 cm from where it was for half a second
    // (contacts leave a little jitter, so speeds alone never settle)
    let drift = maxMove > 1.5 ? Infinity : 0;
    if (this.ref.length !== this.particles.length) drift = Infinity;
    else
      for (let i = 0; i < this.particles.length && drift < 0.015; i++) {
        const a = this.particles[i]!.p, b = this.ref[i]!;
        drift = Math.max(drift, Math.abs(a[0] - b[0]), Math.abs(a[1] - b[1]), Math.abs(a[2] - b[2]));
      }
    if (drift >= 0.015 && maxMove > 0.06) {
      this.ref = this.particles.map((q) => vcopy(q.p));
      this.still = 0;
    } else this.still += h;
    this.lastMax = maxMove;
    if (this.still > 0.4 && this.time > 0.8) this.asleep = true;
  }

  private collide(final: boolean): void {
    const c = this.contact;
    // a thin skin: a particle resting exactly on a surface still counts as touching it
    // (friction), without being pushed off it
    const skin = 0.004;
    for (const q of this.particles) {
      q.contact = false;
      for (let pass = 0; pass < 2; pass++) {
        if (!this.collision.sphere(q.p, q.r + skin, c)) break;
        const len = Math.hypot(c.push[0], c.push[1], c.push[2]);
        const k = len > skin ? (len - skin) / len : 0;
        q.p[0] += c.push[0] * k;
        q.p[1] += c.push[1] * k;
        q.p[2] += c.push[2] * k;
        q.contact = true;
        if (final) {
          // friction: remove part of the tangential velocity; no bounce along the normal
          const v: V3 = [q.p[0] - q.prev[0], q.p[1] - q.prev[1], q.p[2] - q.prev[2]];
          const vn = vdot(v, c.normal);
          const nx = c.normal[0] * vn, ny = c.normal[1] * vn, nz = c.normal[2] * vn;
          const tx = (v[0] - nx) * this.friction, ty = (v[1] - ny) * this.friction, tz = (v[2] - nz) * this.friction;
          // no bounce: moving into the surface stops; moving off it is capped (being pushed out
          // of a surface is not a throw)
          const sep = vn < 0 ? 0 : Math.min(1, (0.6 * this.substep) / Math.max(vn, 1e-9));
          q.prev[0] = q.p[0] - (tx + nx * sep);
          q.prev[1] = q.p[1] - (ty + ny * sep);
          q.prev[2] = q.p[2] - (tz + nz * sep);
        }
        if (k === 0) break;
      }
    }
  }

  private solve(c: RagdollConstraint): void {
    const P = this.particles;
    if (c.kind === 'hinge') {
      const A = P[c.a]!, M = P[c.m]!, C = P[c.c]!;
      const a = A.p, cc = C.p;
      const f0 = P[c.f0]!.p, f1 = P[c.f1]!.p;
      const lx = cc[0] - a[0], ly = cc[1] - a[1], lz = cc[2] - a[2];
      const ll = lx * lx + ly * ly + lz * lz;
      if (ll < 1e-8) return;
      // the side the joint may lie on: sign * (line x side axis), across the line
      const rx = f1[0] - f0[0], ry = f1[1] - f0[1], rz = f1[2] - f0[2];
      let fx = (ly * rz - lz * ry) * c.sign, fy = (lz * rx - lx * rz) * c.sign, fz = (lx * ry - ly * rx) * c.sign;
      const fl = Math.hypot(fx, fy, fz);
      // (a limb along the side axis, as in the splits, has no defined side)
      if (fl < 1e-4 * Math.sqrt(ll) * (Math.hypot(rx, ry, rz) + 1e-9)) return;
      fx /= fl;
      fy /= fl;
      fz /= fl;
      const t = ((M.p[0] - a[0]) * lx + (M.p[1] - a[1]) * ly + (M.p[2] - a[2]) * lz) / ll;
      const ox = M.p[0] - (a[0] + lx * t), oy = M.p[1] - (a[1] + ly * t), oz = M.p[2] - (a[2] + lz * t);
      const side = ox * fx + oy * fy + oz * fz;
      if (side >= 0) return;
      // only a nearly straight limb can bend the wrong way; a folded one's line is short and its
      // side ill-defined (enforcing it there only shakes the joint)
      const span = Math.hypot(M.p[0] - a[0], M.p[1] - a[1], M.p[2] - a[2]) + Math.hypot(cc[0] - M.p[0], cc[1] - M.p[1], cc[2] - M.p[2]);
      const straight = Math.sqrt(ll) / (span || 1);
      const weight = straight <= 0.55 ? 0 : straight >= 0.8 ? 1 : ((straight - 0.55) / 0.25) ** 2 * (3 - (2 * (straight - 0.55)) / 0.25);
      if (weight <= 0) return;
      // back onto the allowed side (a little past the line), shared by the three joints by
      // their masses so the limb as a whole is not pushed anywhere
      const d = (-side + 0.004) * weight;
      const W = M.w + 0.25 * (A.w + C.w);
      if (W <= 0) return;
      const km = (d * M.w) / W, ka = (-0.5 * d * A.w) / W, kc = (-0.5 * d * C.w) / W;
      M.p[0] += fx * km;
      M.p[1] += fy * km;
      M.p[2] += fz * km;
      A.p[0] += fx * ka;
      A.p[1] += fy * ka;
      A.p[2] += fz * ka;
      C.p[0] += fx * kc;
      C.p[1] += fy * kc;
      C.p[2] += fz * kc;
      return;
    }
    const a = P[c.a]!, b = P[c.b]!;
    const dx = b.p[0] - a.p[0], dy = b.p[1] - a.p[1], dz = b.p[2] - a.p[2];
    const d = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (d < 1e-9) return;
    let target: number;
    let k = 1;
    if (c.kind === 'dist') {
      target = c.rest;
      k = c.k;
    } else {
      if (d < c.min) target = c.min;
      else if (d > c.max) target = c.max;
      else return;
    }
    const wsum = a.w + b.w;
    if (wsum === 0) return;
    const diff = ((d - target) / d) * k;
    const ax = dx * diff * (a.w / wsum), ay = dy * diff * (a.w / wsum), az = dz * diff * (a.w / wsum);
    const bx = dx * diff * (b.w / wsum), by = dy * diff * (b.w / wsum), bz = dz * diff * (b.w / wsum);
    a.p[0] += ax;
    a.p[1] += ay;
    a.p[2] += az;
    b.p[0] -= bx;
    b.p[1] -= by;
    b.p[2] -= bz;
  }
}
