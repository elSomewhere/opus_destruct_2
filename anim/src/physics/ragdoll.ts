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
   * Hinge: the middle particle m must stay on the side `sign` of the line a-c, measured along
   * the direction given by particles (f0 -> f1) (e.g. the pelvis forward for knees).
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
  private readonly contact: SphereContact = { push: [0, 0, 0], normal: [0, 0, 1] };
  /** Optional per-particle soft targets (muscle tone) and their weight. */
  targets: V3[] | null = null;
  targetWeight = 0;

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
    const damp = Math.pow(this.airDamping, h);
    let maxMove = 0;
    for (const q of this.particles) {
      if (q.w === 0) continue;
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
    const muscle = this.targets && this.targetWeight > 0 ? clamp(this.targetWeight, 0, 1) : 0;
    for (let it = 0; it < this.iterations; it++) {
      for (const c of this.constraints) this.solve(c);
      if (muscle > 0) {
        const t = this.targets!;
        this.particles.forEach((q, i) => {
          const g = t[i];
          if (!g || q.w === 0) return;
          const k = muscle * 0.08;
          q.p[0] += (g[0] - q.p[0]) * k;
          q.p[1] += (g[1] - q.p[1]) * k;
          q.p[2] += (g[2] - q.p[2]) * k;
        });
      }
      if (it === this.iterations - 1 || it % 2 === 1) this.collide(it === this.iterations - 1);
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
    if (drift >= 0.015) {
      this.ref = this.particles.map((q) => vcopy(q.p));
      this.still = 0;
    } else this.still += h;
    if (this.still > 0.5 && this.time > 0.8) this.asleep = true;
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
          const keepN = vn < 0 ? 0 : 1;
          q.prev[0] = q.p[0] - (tx + nx * keepN);
          q.prev[1] = q.p[1] - (ty + ny * keepN);
          q.prev[2] = q.p[2] - (tz + nz * keepN);
        }
        if (k === 0) break;
      }
    }
  }

  private solve(c: RagdollConstraint): void {
    const P = this.particles;
    if (c.kind === 'hinge') {
      const a = P[c.a]!.p, m = P[c.m]!, cc = P[c.c]!.p;
      const f0 = P[c.f0]!.p, f1 = P[c.f1]!.p;
      // direction the joint must bend towards
      let fx = f1[0] - f0[0], fy = f1[1] - f0[1], fz = f1[2] - f0[2];
      const lx = cc[0] - a[0], ly = cc[1] - a[1], lz = cc[2] - a[2];
      const ll = lx * lx + ly * ly + lz * lz;
      if (ll < 1e-8) return;
      // keep only the part of the direction orthogonal to the limb line
      const fd = (fx * lx + fy * ly + fz * lz) / ll;
      fx -= lx * fd;
      fy -= ly * fd;
      fz -= lz * fd;
      const fl = Math.hypot(fx, fy, fz);
      if (fl < 1e-6) return;
      fx /= fl;
      fy /= fl;
      fz /= fl;
      // offset of the middle particle from the line
      const t = ((m.p[0] - a[0]) * lx + (m.p[1] - a[1]) * ly + (m.p[2] - a[2]) * lz) / ll;
      const ox = m.p[0] - (a[0] + lx * t), oy = m.p[1] - (a[1] + ly * t), oz = m.p[2] - (a[2] + lz * t);
      const side = (ox * fx + oy * fy + oz * fz) * c.sign;
      if (side >= 0 || m.w === 0) return;
      // move the joint back onto the allowed side (a little past the line)
      const k = (-side + 0.004) * c.sign;
      m.p[0] += fx * k;
      m.p[1] += fy * k;
      m.p[2] += fz * k;
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
