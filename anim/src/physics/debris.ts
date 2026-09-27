/**
 * Gibs and blood: renderer-agnostic debris on top of a CollisionWorld.
 *
 * - Gibs are rigid voxel chunks (a severed limb, a head, a piece of torso, a dropped weapon):
 *   a VoxelPart in rest model space moved by a rigid transform (x -> pos + rot (x - pivot),
 *   pivot = the part's rest centre of mass). Their dynamics are those of a rigid body with the
 *   inertia of a solid box (the part's cell bounds, density ~1000 kg/m^3). Collision samples a
 *   few dozen surface cells as spheres of half a voxel against the world (sequential impulses
 *   with restitution and Coulomb friction, positional correction), with adaptive substeps so
 *   that no sample point moves more than a few centimetres per substep (no tunnelling at
 *   20 m/s). Gibs sleep when at rest and wake on impulses or when their support is gone.
 * - Blood drops are ballistic particles (gravity, light drag). A drop that hits the world
 *   (swept with raycast) becomes a stain on that surface: position, unit normal, size; nearby
 *   stains merge and grow. The host draws drops as small cubes and stains as decals or flat
 *   voxels.
 *
 * Sizes are radii (half-extents) in metres. Everything is deterministic for a given seed and
 * call sequence.
 */
import { writeRigid } from '../math/mat4.ts';
import { qconj, qexp, qmul, qnormalize, qrotate, type Quat } from '../math/quat.ts';
import { Rng } from '../math/random.ts';
import { vcopy, vcross, vdot, vlen, vnorm, vsub, type V3 } from '../math/vec.ts';
import type { VoxelPart } from '../voxel/model.ts';
import type { CollisionWorld, SphereContact } from './collision.ts';

export interface Gib {
  id: number;
  /** Cells in rest model space (lattice of voxelSize). */
  part: VoxelPart;
  voxelSize: number;
  /** Rigid transform: a rest-space point x is at pos + rot (x - pivot). */
  pos: V3;
  rot: Quat;
  /** Rest-space centre of mass of the part's cells. */
  pivot: V3;
  /** World linear (m/s) and angular (rad/s) velocity. */
  vel: V3;
  ang: V3;
  /** Bounding radius about the pivot. */
  radius: number;
  asleep: boolean;
  /** Seconds since spawn. */
  age: number;
  /** Blood drops per second shed while the gib moves (0: none; the host sets it). */
  bleed: number;
  /** Arbitrary host data (mesh handles, palette): never read by the system. */
  user: unknown;
}

export interface BloodDrop {
  pos: V3;
  vel: V3;
  /** Radius (m). */
  size: number;
  age: number;
  /** Linear RGB. */
  color: [number, number, number];
}

export interface BloodStain {
  pos: V3;
  /** Unit surface normal (pointing out of the surface). */
  normal: V3;
  /** Radius (m). */
  size: number;
  age: number;
  color: [number, number, number];
}

export interface GibSystemOptions {
  maxGibs?: number;
  maxDrops?: number;
  maxStains?: number;
  /** m/s^2 (default 9.81). */
  gravity?: number;
  /** Gibs below this height are removed (default -200 m). */
  killZ?: number;
  /** Seed of the spray and spin randomness. */
  seed?: number;
}

/** Internal per-gib physical data. */
interface Body {
  gib: Gib;
  mass: number;
  invMass: number;
  /** Inverse principal inertia in the rest frame (the box axes). */
  invI: V3;
  /** Collision sample points: rest-space offsets from the pivot. */
  samples: V3[];
  sleepTimer: number;
  supportTimer: number;
  bleedAcc: number;
  /** Low-passed speed and spin (sleep test). */
  calmV: number;
  calmW: number;
}

const DENSITY = 1000;
const RESTITUTION = 0.25;
const FRICTION = 0.6;
/** Collision sample budget per gib (a hand gets a dozen, a torso chunk the full budget). */
const MAX_SAMPLES = 64;
/** Largest displacement of any sample point in one substep (m). */
const MAX_STEP_DISP = 0.04;
const BASE_SUBSTEP = 1 / 120;
const MAX_SUBSTEPS = 32;
const MAX_SPEED = 60;
const MAX_SPIN = 40;
const SLEEP_SPEED = 0.08;
const SLEEP_SPIN = 0.35;
const SLEEP_TIME = 0.5;
/** Penetration left alone by the positional correction (m). */
const SLOP = 0.002;
const DROP_LIFE = 4;
const DROP_DRAG = 0.4;
const BLOOD: [number, number, number] = [0.22, 0.008, 0.008];

export class GibSystem {
  collision: CollisionWorld;
  readonly gibs: Gib[] = [];
  readonly drops: BloodDrop[] = [];
  readonly stains: BloodStain[] = [];
  readonly maxGibs: number;
  readonly maxDrops: number;
  readonly maxStains: number;
  gravity: number;
  killZ: number;
  private readonly bodies = new Map<Gib, Body>();
  private readonly rng: Rng;
  private nextId = 1;
  private readonly contact: SphereContact = { push: [0, 0, 0], normal: [0, 0, 1] };

  constructor(collision: CollisionWorld, opts: GibSystemOptions = {}) {
    this.collision = collision;
    this.maxGibs = opts.maxGibs ?? 128;
    this.maxDrops = opts.maxDrops ?? 800;
    this.maxStains = opts.maxStains ?? 600;
    this.gravity = opts.gravity ?? 9.81;
    this.killZ = opts.killZ ?? -200;
    this.rng = new Rng(opts.seed ?? 0x61b5);
  }

  /**
   * Adds a gib whose part currently sits where the bone's world pose puts it (x ->
   * boneRot (x - boneRestHead) + bonePos), moving at `vel` and spinning at `ang` (world).
   */
  spawn(part: VoxelPart, voxelSize: number, bonePos: Readonly<V3>, boneRot: Readonly<Quat>, boneRestHead: Readonly<V3>, vel: Readonly<V3>, ang: Readonly<V3>, user?: unknown): Gib {
    const s = voxelSize;
    const [nx, ny, nz] = part.dims;
    const [ox, oy, oz] = part.origin;
    const solid = (x: number, y: number, z: number): boolean =>
      x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz && part.cells[x + nx * (y + ny * z)] !== 0;
    // centre of mass, cell bounds, surface cells
    let cx = 0, cy = 0, cz = 0, count = 0;
    let x0 = Infinity, y0 = Infinity, z0 = Infinity, x1 = -Infinity, y1 = -Infinity, z1 = -Infinity;
    const surface: V3[] = [];
    for (let z = 0; z < nz; z++)
      for (let y = 0; y < ny; y++)
        for (let x = 0; x < nx; x++) {
          if (!solid(x, y, z)) continue;
          const p: V3 = [(ox + x + 0.5) * s, (oy + y + 0.5) * s, (oz + z + 0.5) * s];
          cx += p[0];
          cy += p[1];
          cz += p[2];
          count++;
          if (x < x0) x0 = x;
          if (y < y0) y0 = y;
          if (z < z0) z0 = z;
          if (x > x1) x1 = x;
          if (y > y1) y1 = y;
          if (z > z1) z1 = z;
          if (!solid(x - 1, y, z) || !solid(x + 1, y, z) || !solid(x, y - 1, z) || !solid(x, y + 1, z) || !solid(x, y, z - 1) || !solid(x, y, z + 1)) surface.push(p);
        }
    const pivot: V3 = count > 0 ? [cx / count, cy / count, cz / count] : [(ox + nx / 2) * s, (oy + ny / 2) * s, (oz + nz / 2) * s];
    // box inertia from the cell bounds
    const mass = Math.max(1e-3, count * s * s * s * DENSITY);
    const dx = count > 0 ? (x1 - x0 + 1) * s : s, dy = count > 0 ? (y1 - y0 + 1) * s : s, dz = count > 0 ? (z1 - z0 + 1) * s : s;
    const ix = (mass / 12) * (dy * dy + dz * dz);
    const iy = (mass / 12) * (dx * dx + dz * dz);
    const iz = (mass / 12) * (dx * dx + dy * dy);
    const floorI = mass * s * s * 0.1;
    // sample points: extremes along 14 directions, then evenly strided surface cells
    const offsets = surface.map((p) => vsub(p, pivot));
    const samples = pickSamples(offsets, MAX_SAMPLES, s);
    if (samples.length === 0) samples.push([0, 0, 0]);
    let radius = 0;
    for (const o of offsets) radius = Math.max(radius, vlen(o));
    radius += s * 0.87;
    // the gib's transform: rest point x at boneRot (x - boneRestHead) + bonePos
    const rot = qnormalize(boneRot);
    const pos = vsub(pivot, boneRestHead);
    qrotate(rot, pos, pos);
    pos[0] += bonePos[0];
    pos[1] += bonePos[1];
    pos[2] += bonePos[2];
    const gib: Gib = {
      id: this.nextId++,
      part,
      voxelSize: s,
      pos,
      rot,
      pivot,
      vel: vcopy(vel),
      ang: vcopy(ang),
      radius,
      asleep: false,
      age: 0,
      bleed: 0,
      user,
    };
    this.makeRoom();
    this.gibs.push(gib);
    this.bodies.set(gib, {
      gib,
      mass,
      invMass: 1 / mass,
      invI: [1 / Math.max(ix, floorI), 1 / Math.max(iy, floorI), 1 / Math.max(iz, floorI)],
      samples,
      sleepTimer: 0,
      supportTimer: 0,
      bleedAcc: 0,
      calmV: vlen(vel),
      calmW: vlen(ang),
    });
    return gib;
  }

  /** Removes a gib (the host released its mesh). */
  remove(g: Gib): void {
    const i = this.gibs.indexOf(g);
    if (i >= 0) this.gibs.splice(i, 1);
    this.bodies.delete(g);
  }

  private makeRoom(): void {
    while (this.gibs.length >= this.maxGibs) {
      // the oldest sleeping gib goes first, else the oldest
      let victim = this.gibs.findIndex((g) => g.asleep);
      if (victim < 0) victim = 0;
      const g = this.gibs[victim]!;
      this.gibs.splice(victim, 1);
      this.bodies.delete(g);
    }
  }

  /** World position of a rest-space point of the gib. */
  worldPoint(g: Gib, rest: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
    vsub(rest, g.pivot, out);
    qrotate(g.rot, out, out);
    out[0] += g.pos[0];
    out[1] += g.pos[1];
    out[2] += g.pos[2];
    return out;
  }

  /** Skin matrix (column-major 4x4 at `offset`) of the gib's part mesh built in rest space. */
  writeSkin(g: Gib, out: Float32Array, offset = 0): void {
    writeRigid(out, offset, g.pos, g.rot, g.pivot);
  }

  /**
   * Radial push (explosions): gibs and drops within `radius` of `center` get up to `speed` m/s
   * away from it (linear falloff, a little upward bias and spin); sleeping gibs wake.
   */
  impulse(center: Readonly<V3>, radius: number, speed: number): void {
    if (radius <= 0) return;
    for (const g of this.gibs) {
      const d = vsub(g.pos, center);
      const dist = vlen(d);
      if (dist >= radius + g.radius) continue;
      const f = Math.max(0, 1 - Math.max(0, dist - g.radius) / radius);
      if (f <= 0) continue;
      const dir = vnorm(d, [0, 0, 0], [0, 0, 1]);
      dir[2] += 0.35;
      vnorm(dir, dir);
      const b = this.bodies.get(g)!;
      const k = speed * f;
      for (let a = 0; a < 3; a++) {
        g.vel[a] = g.vel[a]! + dir[a]! * k;
        g.ang[a] = g.ang[a]! + (this.rng.next() * 2 - 1) * k * 1.5;
      }
      clampVelocity(g);
      g.asleep = false;
      b.sleepTimer = 0;
      b.calmV = vlen(g.vel);
      b.calmW = vlen(g.ang);
    }
    for (const dr of this.drops) {
      const d = vsub(dr.pos, center);
      const dist = vlen(d);
      if (dist >= radius) continue;
      const k = speed * (1 - dist / radius);
      const dir = vnorm(d, [0, 0, 0], [0, 0, 1]);
      for (let a = 0; a < 3; a++) dr.vel[a] = dr.vel[a]! + dir[a]! * k;
    }
  }

  /** Advances gibs, drops and stains by dt seconds. */
  update(dt: number): void {
    if (dt <= 0) return;
    for (let i = this.gibs.length - 1; i >= 0; i--) {
      const g = this.gibs[i]!;
      const b = this.bodies.get(g)!;
      g.age += dt;
      if (g.asleep) {
        this.checkSupport(b, dt);
        if (g.asleep) continue;
      }
      const reach = vlen(g.vel) + vlen(g.ang) * g.radius;
      const n = Math.min(MAX_SUBSTEPS, Math.max(Math.ceil(dt / BASE_SUBSTEP), Math.ceil((reach * dt) / MAX_STEP_DISP)));
      const h = dt / n;
      for (let k = 0; k < n && !g.asleep; k++) this.stepGib(b, h);
      if (g.bleed > 0 && !g.asleep && vlen(g.vel) > 0.5) {
        b.bleedAcc += g.bleed * dt;
        while (b.bleedAcc >= 1) {
          b.bleedAcc -= 1;
          const o = b.samples[Math.floor(this.rng.next() * b.samples.length)]!;
          const p = qrotate(g.rot, o);
          p[0] += g.pos[0];
          p[1] += g.pos[1];
          p[2] += g.pos[2];
          this.addDrop(p, [g.vel[0] * 0.3 + this.rng.range(-0.4, 0.4), g.vel[1] * 0.3 + this.rng.range(-0.4, 0.4), g.vel[2] * 0.3 + this.rng.range(0, 0.6)], this.rng.range(0.008, 0.018), BLOOD);
        }
      }
      if (g.pos[2] < this.killZ) {
        this.gibs.splice(i, 1);
        this.bodies.delete(g);
      }
    }
    this.updateDrops(dt);
    for (const st of this.stains) st.age += dt;
  }

  private stepGib(b: Body, h: number): void {
    const g = b.gib;
    const col = this.collision;
    const r = g.voxelSize * 0.5;
    // integrate
    g.vel[2] -= this.gravity * h;
    clampVelocity(g);
    g.pos[0] += g.vel[0] * h;
    g.pos[1] += g.vel[1] * h;
    g.pos[2] += g.vel[2] * h;
    const spin = vlen(g.ang);
    if (spin > 1e-9) g.rot = qnormalize(qmul(qexp([g.ang[0] * h, g.ang[1] * h, g.ang[2] * h]), g.rot));
    // broad phase: nothing near the whole chunk
    if (!col.sphere(g.pos, g.radius + r, this.contact)) {
      b.sleepTimer = 0;
      return;
    }
    // contacts of the sample points
    const contacts: { rw: V3; n: V3 }[] = [];
    const out = this.contact;
    for (const o of b.samples) {
      const rw = qrotate(g.rot, o);
      const p: V3 = [g.pos[0] + rw[0], g.pos[1] + rw[1], g.pos[2] + rw[2]];
      if (col.sphere(p, r, out)) contacts.push({ rw, n: vcopy(out.normal) });
    }
    if (contacts.length === 0) {
      b.sleepTimer = 0;
      return;
    }
    // velocity: sequential impulses (normal with restitution, then Coulomb friction)
    const inv = qconj(g.rot);
    const applyInvI = (v: Readonly<V3>): V3 => {
      const l = qrotate(inv, v);
      l[0] *= b.invI[0];
      l[1] *= b.invI[1];
      l[2] *= b.invI[2];
      return qrotate(g.rot, l);
    };
    const pointVel = (rw: Readonly<V3>): V3 => {
      const w = vcross(g.ang, rw);
      return [g.vel[0] + w[0], g.vel[1] + w[1], g.vel[2] + w[2]];
    };
    const applyImpulse = (rw: Readonly<V3>, j: Readonly<V3>): void => {
      g.vel[0] += j[0] * b.invMass;
      g.vel[1] += j[1] * b.invMass;
      g.vel[2] += j[2] * b.invMass;
      const dw = applyInvI(vcross(rw, j));
      g.ang[0] += dw[0];
      g.ang[1] += dw[1];
      g.ang[2] += dw[2];
    };
    const effMass = (rw: Readonly<V3>, n: Readonly<V3>): number => b.invMass + vdot(n, vcross(applyInvI(vcross(rw, n)), rw));
    for (let pass = 0; pass < 4; pass++) {
      for (const c of contacts) {
        const vp = pointVel(c.rw);
        const vn = vdot(vp, c.n);
        if (vn >= 0) continue;
        const e = vn < -1.2 && pass === 0 ? RESTITUTION : 0;
        const jn = (-(1 + e) * vn) / effMass(c.rw, c.n);
        applyImpulse(c.rw, [c.n[0] * jn, c.n[1] * jn, c.n[2] * jn]);
        // friction against the tangential velocity, bounded by mu * jn
        const vp2 = pointVel(c.rw);
        const vn2 = vdot(vp2, c.n);
        const vt: V3 = [vp2[0] - c.n[0] * vn2, vp2[1] - c.n[1] * vn2, vp2[2] - c.n[2] * vn2];
        const vtl = vlen(vt);
        if (vtl < 1e-6) continue;
        const t: V3 = [vt[0] / vtl, vt[1] / vtl, vt[2] / vtl];
        const jt = Math.min(vtl / effMass(c.rw, t), FRICTION * jn);
        applyImpulse(c.rw, [-t[0] * jt, -t[1] * jt, -t[2] * jt]);
      }
    }
    // rolling resistance; slow bodies in contact settle (no micro-jitter between contacts)
    const slow = vlen(g.vel) < 0.6 && vlen(g.ang) < 2;
    const damp = Math.exp(-(slow ? 12 : 4) * h);
    g.ang[0] *= damp;
    g.ang[1] *= damp;
    g.ang[2] *= damp;
    if (slow) {
      const lin = Math.exp(-6 * h);
      g.vel[0] *= lin;
      g.vel[1] *= lin;
      if (g.vel[2] > 0) g.vel[2] *= lin;
    }
    // positions: push out of the deepest penetration, a few times
    for (let it = 0; it < 3; it++) {
      let best = 0;
      const push: V3 = [0, 0, 0];
      for (const o of b.samples) {
        const rw = qrotate(g.rot, o);
        const p: V3 = [g.pos[0] + rw[0], g.pos[1] + rw[1], g.pos[2] + rw[2]];
        if (!col.sphere(p, r, out)) continue;
        const l = vlen(out.push);
        if (l > best) {
          best = l;
          vcopy(out.push, push);
        }
      }
      if (best <= SLOP) break;
      const k = (best - SLOP * 0.5) / best;
      g.pos[0] += push[0] * k;
      g.pos[1] += push[1] * k;
      g.pos[2] += push[2] * k;
    }
    // sleep on the smoothed motion (a resting body still sees tiny contact impulses)
    const a = 1 - Math.exp(-h / 0.15);
    b.calmV += (vlen(g.vel) - b.calmV) * a;
    b.calmW += (vlen(g.ang) - b.calmW) * a;
    if (b.calmV < SLEEP_SPEED && b.calmW < SLEEP_SPIN) {
      b.sleepTimer += h;
      if (b.sleepTimer >= SLEEP_TIME) {
        g.asleep = true;
        g.vel[0] = g.vel[1] = g.vel[2] = 0;
        g.ang[0] = g.ang[1] = g.ang[2] = 0;
        b.supportTimer = 0;
      }
    } else b.sleepTimer = 0;
  }

  /** A sleeping gib whose support was blasted away wakes up (checked a few times a second). */
  private checkSupport(b: Body, dt: number): void {
    b.supportTimer += dt;
    if (b.supportTimer < 0.25) return;
    b.supportTimer = 0;
    const g = b.gib;
    const r = g.voxelSize * 0.5;
    // the three lowest sample points, probed a little below
    const pts = b.samples.map((o) => {
      const rw = qrotate(g.rot, o);
      return [g.pos[0] + rw[0], g.pos[1] + rw[1], g.pos[2] + rw[2]] as V3;
    });
    pts.sort((a, c) => a[2] - c[2]);
    for (let i = 0; i < Math.min(3, pts.length); i++) {
      const p = pts[i]!;
      if (this.collision.sphere([p[0], p[1], p[2] - r * 1.5], r, this.contact)) return;
    }
    g.asleep = false;
    b.sleepTimer = 0;
    b.calmV = b.calmW = 1;
  }

  // ---- blood --------------------------------------------------------------------------------

  /**
   * Sprays `count` drops from `pos` along `dir` (any length) at up to `speed` m/s, scattered by
   * `spread` (0 = a jet, 1 = a hemisphere), in `color` (linear RGB; default dark red).
   */
  spray(pos: Readonly<V3>, dir: Readonly<V3>, count: number, speed: number, spread: number, color: [number, number, number] = BLOOD): void {
    const d = vnorm(dir, [0, 0, 0], [0, 0, 1]);
    for (let i = 0; i < count; i++) {
      // a random direction in the unit ball, scaled by the spread
      let rx = 0, ry = 0, rz = 0;
      for (let t = 0; t < 8; t++) {
        rx = this.rng.next() * 2 - 1;
        ry = this.rng.next() * 2 - 1;
        rz = this.rng.next() * 2 - 1;
        if (rx * rx + ry * ry + rz * rz <= 1) break;
      }
      const v = vnorm([d[0] + rx * spread * 1.5, d[1] + ry * spread * 1.5, d[2] + rz * spread * 1.5], [0, 0, 0], d);
      const sp = speed * this.rng.range(0.35, 1);
      this.addDrop(vcopy(pos), [v[0] * sp, v[1] * sp, v[2] * sp], this.rng.range(0.008, 0.022), color);
    }
  }

  private addDrop(pos: V3, vel: V3, size: number, color: [number, number, number]): void {
    if (this.drops.length >= this.maxDrops) this.drops.shift();
    this.drops.push({ pos, vel, size, age: 0, color });
  }

  private updateDrops(dt: number): void {
    const col = this.collision;
    const drag = Math.exp(-DROP_DRAG * dt);
    let w = 0;
    for (let i = 0; i < this.drops.length; i++) {
      const dr = this.drops[i]!;
      dr.age += dt;
      if (dr.age > DROP_LIFE || dr.pos[2] < this.killZ) continue;
      dr.vel[2] -= this.gravity * dt;
      dr.vel[0] *= drag;
      dr.vel[1] *= drag;
      dr.vel[2] *= drag;
      const step: V3 = [dr.vel[0] * dt, dr.vel[1] * dt, dr.vel[2] * dt];
      const len = vlen(step);
      if (len > 1e-9) {
        const dir: V3 = [step[0] / len, step[1] / len, step[2] / len];
        const t = col.raycast(dr.pos, dir, len + dr.size);
        if (t >= 0) {
          const hit: V3 = [dr.pos[0] + dir[0] * t, dr.pos[1] + dir[1] * t, dr.pos[2] + dir[2] * t];
          const n = this.surfaceNormal(hit, dir);
          this.addStain(hit, n, dr.size * this.rng.range(1.6, 2.6), dr.color);
          continue;
        }
        dr.pos[0] += step[0];
        dr.pos[1] += step[1];
        dr.pos[2] += step[2];
      }
      this.drops[w++] = dr;
    }
    this.drops.length = w;
  }

  /** Normal of the surface at a ray hit: the contact normal of a small sphere there. */
  private surfaceNormal(hit: Readonly<V3>, dir: Readonly<V3>): V3 {
    const back: V3 = [hit[0] - dir[0] * 1e-3, hit[1] - dir[1] * 1e-3, hit[2] - dir[2] * 1e-3];
    if (this.collision.sphere(back, 0.01, this.contact)) return vnorm(this.contact.normal);
    return [-dir[0], -dir[1], -dir[2]];
  }

  private addStain(pos: V3, normal: V3, size: number, color: [number, number, number]): void {
    // merge into a recent stain nearby on the same surface
    const from = Math.max(0, this.stains.length - 64);
    for (let i = this.stains.length - 1; i >= from; i--) {
      const st = this.stains[i]!;
      if (vdot(st.normal, normal) < 0.9) continue;
      const d = Math.hypot(st.pos[0] - pos[0], st.pos[1] - pos[1], st.pos[2] - pos[2]);
      if (d < st.size * 0.6) {
        st.size = Math.min(0.35, Math.sqrt(st.size * st.size + size * size * 0.6));
        return;
      }
    }
    if (this.stains.length >= this.maxStains) this.stains.shift();
    this.stains.push({ pos, normal, size, age: 0, color });
  }

  forEachDrop(fn: (pos: V3, size: number, color: [number, number, number]) => void): void {
    for (const d of this.drops) fn(d.pos, d.size, d.color);
  }

  forEachStain(fn: (pos: V3, normal: V3, size: number, age: number, color: [number, number, number]) => void): void {
    for (const s of this.stains) fn(s.pos, s.normal, s.size, s.age, s.color);
  }

  clear(): void {
    this.gibs.length = 0;
    this.bodies.clear();
    this.drops.length = 0;
    this.stains.length = 0;
  }
}

function clampVelocity(g: Gib): void {
  const v = vlen(g.vel);
  if (v > MAX_SPEED) for (let a = 0; a < 3; a++) g.vel[a] = (g.vel[a]! * MAX_SPEED) / v;
  const w = vlen(g.ang);
  if (w > MAX_SPIN) for (let a = 0; a < 3; a++) g.ang[a] = (g.ang[a]! * MAX_SPIN) / w;
}

/** Directions whose extreme points are always sampled: the 26 neighbour directions of a cube. */
const EXTREME_DIRS: readonly V3[] = (() => {
  const out: V3[] = [];
  for (let x = -1; x <= 1; x++)
    for (let y = -1; y <= 1; y++)
      for (let z = -1; z <= 1; z++) if (x || y || z) out.push(vnorm([x, y, z]));
  return out;
})();

/**
 * At most `max` of the points (offsets from the pivot), spread evenly: the extremes along
 * EXTREME_DIRS, then the outermost point of each cell of a spatial grid, the grid coarsened
 * until the budget holds (so no stretch of surface goes unsampled).
 */
function pickSamples(points: readonly V3[], max: number, cell: number): V3[] {
  if (points.length <= max) return points.map((p) => vcopy(p));
  const chosen = new Set<number>();
  for (const d of EXTREME_DIRS) {
    let best = -1, bestDot = -Infinity;
    points.forEach((p, i) => {
      const k = vdot(p, d);
      if (k > bestDot) {
        bestDot = k;
        best = i;
      }
    });
    if (best >= 0) chosen.add(best);
  }
  for (let size = cell * 2; ; size *= 1.25) {
    const bins = new Map<string, number>();
    points.forEach((p, i) => {
      const key = `${Math.floor(p[0] / size)},${Math.floor(p[1] / size)},${Math.floor(p[2] / size)}`;
      const cur = bins.get(key);
      if (cur === undefined || vlen(p) > vlen(points[cur]!)) bins.set(key, i);
    });
    const all = new Set(chosen);
    for (const i of bins.values()) all.add(i);
    if (all.size <= max || size > 1) return [...all].slice(0, max).map((i) => vcopy(points[i]!));
  }
}
