/**
 * Articulated rigid bodies, solved with extended position-based dynamics (XPBD, after Müller et
 * al. 2020, "Detailed Rigid Body Simulation with Extended Position Based Dynamics"): many small
 * substeps with one constraint pass each, which keeps long chains of light and heavy bodies (a
 * hand on an arm on a chest) stiff and stable at any frame rate.
 *
 * - Bodies: mass, principal inertia (body frame), collision spheres.
 * - Joints: a ball or a hinge with limits (an elliptical swing cone with its own reach in each
 *   of four directions, a twist range, a hinge range) and a muscle: a drive towards a target
 *   relative rotation with a stiffness (N m/rad), a damping (N m s/rad, on the relative angular
 *   velocity), a torque limit, and a feed-forward torque.
 * - Attachments pull a point of a body to a world point, and orienters turn a body towards a
 *   world rotation: soft (compliance), force-limited, damped; the "assists" a controller uses to
 *   hold a body up the way legs would, or to pin a planted foot.
 * - Contacts: spheres against a CollisionWorld (found once per step with a margin, solved every
 *   substep as planes) with static and dynamic friction, and sphere pairs between bodies of the
 *   same system (arms against the trunk, leg against leg).
 * - Sleep when everything has been slow for a while.
 *
 * The inner loops are written on scalars to stay allocation-free.
 */
import type { Quat } from '../math/quat.ts';
import type { V3 } from '../math/vec.ts';
import type { CollisionWorld, SphereContact } from './collision.ts';

/** A collision sphere in a body's frame (centre relative to the centre of mass). */
export interface Sphere {
  c: V3;
  r: number;
}

export class RigidBody {
  /** Centre of mass and rotation (world). */
  readonly x: V3;
  readonly q: Quat;
  /** Linear and angular velocity (world). */
  readonly v: V3 = [0, 0, 0];
  readonly w: V3 = [0, 0, 0];
  /** Pose at the start of the substep. */
  readonly px: V3 = [0, 0, 0];
  readonly pq: Quat = [0, 0, 0, 1];
  /** Velocities before the substep's constraints. */
  readonly v0: V3 = [0, 0, 0];
  readonly w0: V3 = [0, 0, 0];
  readonly mass: number;
  readonly invMass: number;
  /** Principal inverse inertia in the body frame. */
  readonly invI: V3;
  readonly spheres: Sphere[];
  /** External force and torque (world) applied every substep of the next step, then cleared. */
  readonly force: V3 = [0, 0, 0];
  readonly torque: V3 = [0, 0, 0];
  /** Friction against the world. */
  friction = 0.8;
  /** Touched the world during the last step, and the last contact normal. */
  contact = false;
  readonly contactNormal: V3 = [0, 0, 1];
  /** Where it last touched the world (world). */
  readonly contactPoint: V3 = [0, 0, 0];
  /** Largest contact impulse of the last step (N s): how hard it hit something. */
  impact = 0;
  /**
   * How hard obstacles (other bodies, debris, the player) pushed it sideways over the last
   * step: the horizontal impulse they gave it (N s; resting on them does not count).
   */
  bumped = 0;
  /**
   * Passes through other bodies (not the world): a limb that strikes, whose blow the host
   * deals (it is neither stopped by the body it hits nor one that body runs into).
   */
  ghost = false;
  /** Index in its system. */
  index = -1;
  /**
   * The body's long axis (body frame) and how fast a spin about it dies away (1/s): tissue
   * resists twisting a limb (a light limb would otherwise spin freely about its length).
   */
  readonly longAxis: V3 = [0, 0, 1];
  twistDamping = 0;

  constructor(mass: number, inertia: Readonly<V3>, x: Readonly<V3>, q: Readonly<Quat>, spheres: Sphere[] = []) {
    this.mass = mass;
    this.invMass = mass > 0 ? 1 / mass : 0;
    this.invI = [inertia[0] > 0 ? 1 / inertia[0] : 0, inertia[1] > 0 ? 1 / inertia[1] : 0, inertia[2] > 0 ? 1 / inertia[2] : 0];
    this.x = [x[0], x[1], x[2]];
    this.q = [q[0], q[1], q[2], q[3]];
    this.spheres = spheres;
    this.updateInertia();
  }

  /**
   * World inverse inertia R diag(invI) R^T (symmetric: xx, yy, zz, xy, xz, yz), refreshed once
   * per substep (constraints within a substep use it as the body turns a little: the usual
   * approximation).
   */
  readonly iw = new Float64Array(6);

  /** Refreshes the world inverse inertia from the current rotation. */
  updateInertia(): void {
    const x = this.q[0], y = this.q[1], z = this.q[2], w = this.q[3];
    // rotation matrix columns
    const r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    const r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    const a = this.invI[0], b = this.invI[1], c = this.invI[2];
    const m = this.iw;
    m[0] = r00 * r00 * a + r01 * r01 * b + r02 * r02 * c;
    m[1] = r10 * r10 * a + r11 * r11 * b + r12 * r12 * c;
    m[2] = r20 * r20 * a + r21 * r21 * b + r22 * r22 * c;
    m[3] = r00 * r10 * a + r01 * r11 * b + r02 * r12 * c;
    m[4] = r00 * r20 * a + r01 * r21 * b + r02 * r22 * c;
    m[5] = r10 * r20 * a + r11 * r21 * b + r12 * r22 * c;
  }

  /** out = I^-1 v (world, with the substep's inertia). */
  invInertiaMul(vx: number, vy: number, vz: number, out: V3): V3 {
    const m = this.iw;
    out[0] = m[0]! * vx + m[3]! * vy + m[4]! * vz;
    out[1] = m[3]! * vx + m[1]! * vy + m[5]! * vz;
    out[2] = m[4]! * vx + m[5]! * vy + m[2]! * vz;
    return out;
  }

  /** Generalized inverse mass of a positional correction along unit n at offset r (world). */
  invMassAt(rx: number, ry: number, rz: number, nx: number, ny: number, nz: number): number {
    const cx = ry * nz - rz * ny, cy = rz * nx - rx * nz, cz = rx * ny - ry * nx;
    const t = this.invInertiaMul(cx, cy, cz, S0);
    return this.invMass + cx * t[0] + cy * t[1] + cz * t[2];
  }

  /** Generalized inverse mass of a rotation about unit n. */
  invMassRot(nx: number, ny: number, nz: number): number {
    const t = this.invInertiaMul(nx, ny, nz, S0);
    return nx * t[0] + ny * t[1] + nz * t[2];
  }

  /** Applies a positional impulse p at offset r (world): moves and turns the body. */
  applyPos(px: number, py: number, pz: number, rx: number, ry: number, rz: number): void {
    if (this.invMass === 0) return;
    this.x[0] += px * this.invMass;
    this.x[1] += py * this.invMass;
    this.x[2] += pz * this.invMass;
    const t = this.invInertiaMul(ry * pz - rz * py, rz * px - rx * pz, rx * py - ry * px, S1);
    this.rotate(t[0], t[1], t[2]);
  }

  /** Applies an angular positional impulse (world). */
  applyRot(lx: number, ly: number, lz: number): void {
    if (this.invMass === 0) return;
    const t = this.invInertiaMul(lx, ly, lz, S1);
    this.rotate(t[0], t[1], t[2]);
  }

  /** q += 1/2 [a, 0] q (a small rotation vector), normalized. */
  rotate(ax: number, ay: number, az: number): void {
    const q = this.q;
    const qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    let x = qx + 0.5 * (ax * qw + ay * qz - az * qy);
    let y = qy + 0.5 * (ay * qw + az * qx - ax * qz);
    let z = qz + 0.5 * (az * qw + ax * qy - ay * qx);
    let w = qw + 0.5 * (-ax * qx - ay * qy - az * qz);
    const l = 1 / Math.sqrt(x * x + y * y + z * z + w * w);
    x *= l;
    y *= l;
    z *= l;
    w *= l;
    q[0] = x;
    q[1] = y;
    q[2] = z;
    q[3] = w;
  }

  /** Velocity change: an impulse j (N s) at offset r (world). */
  applyImpulse(jx: number, jy: number, jz: number, rx: number, ry: number, rz: number): void {
    if (this.invMass === 0) return;
    this.v[0] += jx * this.invMass;
    this.v[1] += jy * this.invMass;
    this.v[2] += jz * this.invMass;
    const t = this.invInertiaMul(ry * jz - rz * jy, rz * jx - rx * jz, rx * jy - ry * jx, S1);
    this.w[0] += t[0];
    this.w[1] += t[1];
    this.w[2] += t[2];
  }

  /** World position of a body-frame point. */
  point(local: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
    rot(this.q, local[0], local[1], local[2], out);
    out[0] += this.x[0];
    out[1] += this.x[1];
    out[2] += this.x[2];
    return out;
  }

  /** World velocity of a body-frame point. */
  pointVelocity(local: Readonly<V3>, out: V3 = [0, 0, 0]): V3 {
    const r = rot(this.q, local[0], local[1], local[2], S2);
    out[0] = this.v[0] + this.w[1] * r[2] - this.w[2] * r[1];
    out[1] = this.v[1] + this.w[2] * r[0] - this.w[0] * r[2];
    out[2] = this.v[2] + this.w[0] * r[1] - this.w[1] * r[0];
    return out;
  }
}

/** Limits of a ball joint: how far B's twist axis may tilt towards +x, -x, +y, -y of A's joint frame. */
export interface SwingLimits {
  xPos: number;
  xNeg: number;
  yPos: number;
  yNeg: number;
}

export interface JointOptions {
  /** 'ball': swing and twist limits; 'hinge': about the joint frame's x axis. */
  kind: 'ball' | 'hinge';
  /** Anchors in the bodies' frames. */
  anchorA: V3;
  anchorB: V3;
  /**
   * The joint frame in each body's frame (z: the twist axis, B's bone direction; x: the hinge
   * axis). At the rest pose both frames coincide in the world.
   */
  frameA: Quat;
  frameB: Quat;
  swing?: SwingLimits;
  /** Twist range about z (ball). */
  twist?: [number, number];
  /** Angle range about x from A's z to B's z (hinge). */
  hinge?: [number, number];
}

export class Joint {
  readonly a: RigidBody;
  readonly b: RigidBody;
  readonly kind: 'ball' | 'hinge';
  readonly anchorA: V3;
  readonly anchorB: V3;
  readonly frameA: Quat;
  readonly frameB: Quat;
  swing: SwingLimits | null;
  twist: [number, number] | null;
  hinge: [number, number] | null;
  /** Drive target: B's rotation relative to A (body frames: qB = qA target). */
  readonly target: Quat = [0, 0, 0, 1];
  /** Drive stiffness (Nm/rad; 0: no drive). */
  stiffness = 0;
  /** Damping torque per unit relative angular velocity (N m s/rad), relative to `targetVel`. */
  damping = 0;
  /**
   * The drive target's relative angular velocity (B relative to A, in A's frame, rad/s): the
   * damping works towards it, so a joint follows a moving target instead of dragging on it.
   */
  readonly targetVel: V3 = [0, 0, 0];
  /** Largest drive torque (Nm). */
  maxTorque = Infinity;
  /**
   * The inertia the joint really moves (B's whole limb, kg m^2; 0: just the two bodies). The
   * damping slows the relative spin at the rate it would slow that limb: applied between two
   * light bodies alone it would lock them together within a substep.
   */
  effInertia = 0;
  /** Feed-forward torque on B (world, Nm; A gets the reaction), e.g. gravity compensation. */
  readonly feed: V3 = [0, 0, 0];

  constructor(a: RigidBody, b: RigidBody, o: JointOptions) {
    this.a = a;
    this.b = b;
    this.kind = o.kind;
    this.anchorA = [...o.anchorA];
    this.anchorB = [...o.anchorB];
    this.frameA = [...o.frameA];
    this.frameB = [...o.frameB];
    this.swing = o.swing ?? null;
    this.twist = o.twist ?? null;
    this.hinge = o.hinge ?? null;
  }
}

/** Pulls a body point to a world point (soft, force-limited, damped). */
export class Attachment {
  readonly body: RigidBody;
  /** The point in the body's frame. */
  readonly local: V3;
  readonly target: V3 = [0, 0, 0];
  /** Velocity of the target (the damping works relative to it). */
  readonly targetVel: V3 = [0, 0, 0];
  /** Stiffness (N/m; Infinity: rigid) and the largest force (N). */
  stiffness = Infinity;
  maxForce = Infinity;
  /** Damping force per unit velocity of the point relative to the target's (N s/m). */
  damping = 0;
  /** Which world axes it acts on. */
  readonly axes: [boolean, boolean, boolean] = [true, true, true];
  enabled = false;
  /** Force it applied in the last substep (N, world). */
  readonly applied: V3 = [0, 0, 0];

  constructor(body: RigidBody, local: Readonly<V3>) {
    this.body = body;
    this.local = [...local];
  }
}

/** Turns a body towards a world rotation (soft, torque-limited, damped). */
export class Orienter {
  readonly body: RigidBody;
  readonly target: Quat = [0, 0, 0, 1];
  stiffness = Infinity;
  maxTorque = Infinity;
  /** Damping torque per unit angular velocity (N m s/rad). */
  damping = 0;
  /** Only tilt (the body's `up` axis towards the target's), leaving the heading free. */
  tiltOnly = false;
  /** The body-frame axis tilted towards the target's (tiltOnly). */
  readonly up: V3 = [0, 0, 1];
  enabled = false;

  constructor(body: RigidBody) {
    this.body = body;
  }
}

interface WorldContact {
  body: RigidBody;
  /** Sphere centre (body frame) and radius. */
  c: V3;
  r: number;
  /** Plane n . p = d (the surface), n towards the body. */
  n: V3;
  d: number;
  /** Normal impulse of the last substep (0: not touching). */
  lambda: number;
  /** The contact point in the body's frame at the substep's start. */
  local: V3;
  /** Against an obstacle (not the world). */
  obstacle: boolean;
  /** The obstacle's body (its reaction), and the momentum this contact took from us (N s). */
  other: RigidBody | null;
  took: number;
}

/** A sphere of something else the bodies collide with (another body, a piece of debris; world). */
export interface Obstacle {
  c: V3;
  r: number;
  /** The body it belongs to, if it can be pushed: it gets the reaction of what hits it. */
  body?: RigidBody;
}

/** A push this system gave an obstacle's body (world impulse at a point), to hand on. */
export interface Reaction {
  body: RigidBody;
  j: V3;
  at: V3;
}

/** A pair of spheres of two bodies that must not overlap. */
export interface SpherePair {
  a: RigidBody;
  sa: number;
  b: RigidBody;
  sb: number;
}

export interface RigidSystemOptions {
  gravity?: number;
  /** Longest substep (s). */
  maxSubstep?: number;
  /** Collision margin (m) added to every sphere when looking for contacts. */
  margin?: number;
}

export class RigidSystem {
  readonly bodies: RigidBody[] = [];
  readonly joints: Joint[] = [];
  readonly attachments: Attachment[] = [];
  readonly orienters: Orienter[] = [];
  /** Sphere pairs of bodies that must not overlap (checked with a margin once per step). */
  readonly pairs: SpherePair[] = [];
  private readonly nearPairs: SpherePair[] = [];
  collision: CollisionWorld | null;
  /** Other things to collide with this step (spheres, world; the host fills it). */
  obstacles: readonly Obstacle[] = [];
  /** The pushes the last step gave obstacles that belong to bodies (to apply to them). */
  readonly reactions: Reaction[] = [];
  gravity: number;
  maxSubstep: number;
  margin: number;
  /** How fast overlapping parts of the same system are pushed apart at most (m/s). */
  pairSpeed = 1.5;
  /** Parts of the same system keep apart (off: a body that has come to rest). */
  pairsEnabled = true;
  /** The most the constraints change a body's velocity in one substep (m/s, rad/s). */
  maxDv = 1.2;
  maxDw = 12;
  /** Air drag: velocity kept per second. */
  linearDrag = 0.98;
  angularDrag = 0.9;
  asleep = false;
  /** Seconds nothing has moved further than `stillDistance` (m). */
  still = 0;
  stillDistance = 0.02;
  private stillAt = new Float64Array(0);
  private anchored = false;
  /** The fastest body's speed over the last step (m/s). */
  lastSpeed = 0;
  /** Contacts of the current step. */
  private readonly contacts: WorldContact[] = [];
  private contactCount = 0;
  private readonly probe: SphereContact = { push: [0, 0, 0], normal: [0, 0, 1] };
  /** The substep of the last step (s). */
  substep = 1 / 480;
  private frameStart = new Float64Array(0);

  constructor(collision: CollisionWorld | null, o: RigidSystemOptions = {}) {
    this.collision = collision;
    this.gravity = o.gravity ?? 9.81;
    this.maxSubstep = o.maxSubstep ?? 1 / 480;
    this.margin = o.margin ?? 0.02;
  }

  add(b: RigidBody): RigidBody {
    b.index = this.bodies.length;
    this.bodies.push(b);
    this.frameStart = new Float64Array(this.bodies.length * 3);
    this.stillAt = new Float64Array(this.bodies.length * 3);
    this.anchored = false;
    return b;
  }

  addJoint(j: Joint): Joint {
    this.joints.push(j);
    return j;
  }

  attach(body: RigidBody, local: Readonly<V3>): Attachment {
    const a = new Attachment(body, local);
    this.attachments.push(a);
    return a;
  }

  orienter(body: RigidBody): Orienter {
    const o = new Orienter(body);
    this.orienters.push(o);
    return o;
  }

  wake(): void {
    this.asleep = false;
    this.still = 0;
    this.anchored = false;
  }

  /** Advances the system by dt (split into substeps). */
  step(dt: number): void {
    if (dt <= 0) return;
    if (this.asleep) {
      for (const b of this.bodies) {
        b.force[0] = b.force[1] = b.force[2] = 0;
        b.torque[0] = b.torque[1] = b.torque[2] = 0;
      }
      return;
    }
    // (never let a runaway body hang the host: velocities are bounded, a broken state reset)
    for (const b of this.bodies) {
      if (!(Number.isFinite(b.x[0] + b.x[1] + b.x[2]) && Number.isFinite(b.q[0] + b.q[1] + b.q[2] + b.q[3]))) {
        b.x[0] = b.px[0];
        b.x[1] = b.px[1];
        b.x[2] = b.px[2];
        b.q[0] = b.pq[0];
        b.q[1] = b.pq[1];
        b.q[2] = b.pq[2];
        b.q[3] = b.pq[3];
        if (!Number.isFinite(b.x[0] + b.x[1] + b.x[2] + b.q[0] + b.q[1] + b.q[2] + b.q[3])) {
          b.x[0] = b.x[1] = b.x[2] = 0;
          b.q[0] = b.q[1] = b.q[2] = 0;
          b.q[3] = 1;
        }
        b.v[0] = b.v[1] = b.v[2] = 0;
        b.w[0] = b.w[1] = b.w[2] = 0;
      }
      clampLength(b.v, MAX_SPEED);
      clampLength(b.w, MAX_SPIN);
    }
    const n = Math.max(1, Math.ceil(dt / this.maxSubstep - 1e-6));
    const h = dt / n;
    this.substep = h;
    const start = this.frameStart;
    for (let i = 0; i < this.bodies.length; i++) {
      const b = this.bodies[i]!;
      start[3 * i] = b.x[0];
      start[3 * i + 1] = b.x[1];
      start[3 * i + 2] = b.x[2];
    }
    this.findContacts(dt);
    this.findPairs(dt);
    for (const b of this.bodies) {
      b.impact = 0;
      b.bumped = 0;
    }
    for (let s = 0; s < n; s++) this.substepOnce(h);
    for (const b of this.bodies) {
      b.force[0] = b.force[1] = b.force[2] = 0;
      b.torque[0] = b.torque[1] = b.torque[2] = 0;
    }
    // what the obstacles' bodies get back (equal and opposite, handed on by the host)
    this.reactions.length = 0;
    for (let i = 0; i < this.contactCount; i++) {
      const k = this.contacts[i]!;
      if (!k.other || k.took <= 0) continue;
      const p = k.body.point(k.c);
      const took = Math.min(k.took, 60);
      this.reactions.push({ body: k.other, j: [-k.n[0] * took, -k.n[1] * took, -k.n[2] * took], at: [p[0] - k.n[0] * k.r, p[1] - k.n[1] * k.r, p[2] - k.n[2] * k.r] });
    }
    // still: nothing has gone further than `stillDistance` from where it was when the stillness
    // began (a body at rest may jitter on its contacts, but it goes nowhere)
    let fast = 0, far = 0;
    const anchor = this.stillAt;
    for (let i = 0; i < this.bodies.length; i++) {
      const b = this.bodies[i]!;
      const v = Math.hypot(b.x[0] - start[3 * i]!, b.x[1] - start[3 * i + 1]!, b.x[2] - start[3 * i + 2]!) / dt;
      if (v > fast) fast = v;
      // (light parts - a hand, a foot - may shift a little more before they count)
      const d = Math.hypot(b.x[0] - anchor[3 * i]!, b.x[1] - anchor[3 * i + 1]!, b.x[2] - anchor[3 * i + 2]!) / (b.mass < 2 ? 3 : 1);
      if (d > far) far = d;
    }
    this.lastSpeed = fast;
    if (far < this.stillDistance && this.anchored) this.still += dt;
    else {
      this.still = 0;
      this.anchored = true;
      for (let i = 0; i < this.bodies.length; i++) {
        const b = this.bodies[i]!;
        anchor[3 * i] = b.x[0];
        anchor[3 * i + 1] = b.x[1];
        anchor[3 * i + 2] = b.x[2];
      }
    }
  }

  /** Puts the system to sleep if it has been still for `after` seconds (callers decide when it may). */
  trySleep(after = 0.6): boolean {
    if (this.still >= after) {
      this.asleep = true;
      for (const b of this.bodies) {
        b.v[0] = b.v[1] = b.v[2] = 0;
        b.w[0] = b.w[1] = b.w[2] = 0;
      }
    }
    return this.asleep;
  }

  // ---- contacts ------------------------------------------------------------------------------

  private findContacts(dt: number): void {
    this.contactCount = 0;
    const world = this.collision;
    for (const b of this.bodies) b.contact = false;
    if (this.obstacles.length > 0) this.findObstacles(dt);
    if (!world) return;
    const p = this.probe;
    const c: V3 = [0, 0, 0];
    for (const b of this.bodies) {
      const speed = Math.hypot(b.v[0], b.v[1], b.v[2]);
      for (const s of b.spheres) {
        const reach = speed + Math.hypot(b.w[0], b.w[1], b.w[2]) * Math.hypot(s.c[0], s.c[1], s.c[2]);
        const m = Math.min(this.margin + reach * dt, 0.6);
        b.point(s.c, c);
        if (!world.sphere(c, s.r + m, p)) continue;
        this.addContact(b, s, p.normal, c[0] + p.push[0], c[1] + p.push[1], c[2] + p.push[2], s.r + m);
        // a second surface (a corner: the floor and a wall)
        const c2x = c[0] + p.push[0], c2y = c[1] + p.push[1], c2z = c[2] + p.push[2];
        const n0x = p.normal[0], n0y = p.normal[1], n0z = p.normal[2];
        if (world.sphere([c2x, c2y, c2z], s.r + m, p) && p.normal[0] * n0x + p.normal[1] * n0y + p.normal[2] * n0z < 0.8) {
          this.addContact(b, s, p.normal, c2x + p.push[0], c2y + p.push[1], c2z + p.push[2], s.r + m);
        }
      }
    }
  }

  private findObstacles(dt: number): void {
    const c: V3 = [0, 0, 0];
    const n: V3 = [0, 0, 0];
    for (const b of this.bodies) {
      if (b.ghost) continue;
      const speed = Math.hypot(b.v[0], b.v[1], b.v[2]);
      for (const s of b.spheres) {
        b.point(s.c, c);
        const m = Math.min(this.margin + speed * dt, 0.6);
        for (const o of this.obstacles) {
          const dx = c[0] - o.c[0], dy = c[1] - o.c[1], dz = c[2] - o.c[2];
          const rr = s.r + m + o.r;
          if (dx * dx + dy * dy + dz * dz >= rr * rr) continue;
          const d = Math.sqrt(dx * dx + dy * dy + dz * dz) || 1e-6;
          n[0] = dx / d;
          n[1] = dy / d;
          n[2] = dz / d;
          // the sphere freed: touching the obstacle's surface at s.r + margin
          const f = o.r + s.r + m;
          this.addContact(b, s, n, o.c[0] + n[0] * f, o.c[1] + n[1] * f, o.c[2] + n[2] * f, s.r + m);
          const k = this.contacts[this.contactCount - 1]!;
          k.obstacle = true;
          k.other = o.body ?? null;
        }
      }
    }
  }

  private findPairs(dt: number): void {
    this.nearPairs.length = 0;
    if (!this.pairsEnabled) return;
    for (const p of this.pairs) {
      const A = p.a, B = p.b;
      const sa = A.spheres[p.sa]!, sb = B.spheres[p.sb]!;
      const ca = A.point(sa.c, J0);
      const cb = B.point(sb.c, J1);
      const rv = Math.hypot(B.v[0] - A.v[0], B.v[1] - A.v[1], B.v[2] - A.v[2]) + 0.5 * (Math.hypot(A.w[0], A.w[1], A.w[2]) + Math.hypot(B.w[0], B.w[1], B.w[2]));
      const d = Math.hypot(cb[0] - ca[0], cb[1] - ca[1], cb[2] - ca[2]);
      if (d < sa.r + sb.r + this.margin + rv * dt) this.nearPairs.push(p);
    }
  }

  private addContact(b: RigidBody, s: Sphere, n: Readonly<V3>, cx: number, cy: number, cz: number, rr: number): void {
    let k = this.contacts[this.contactCount];
    if (!k) {
      k = { body: b, c: [0, 0, 0], r: 0, n: [0, 0, 1], d: 0, lambda: 0, local: [0, 0, 0], obstacle: false, other: null, took: 0 };
      this.contacts.push(k);
    }
    this.contactCount++;
    k.body = b;
    k.c[0] = s.c[0];
    k.c[1] = s.c[1];
    k.c[2] = s.c[2];
    k.r = s.r;
    k.n[0] = n[0];
    k.n[1] = n[1];
    k.n[2] = n[2];
    // the freed centre touches the surface at distance rr
    k.d = n[0] * cx + n[1] * cy + n[2] * cz - rr;
    k.lambda = 0;
    k.obstacle = false;
    k.other = null;
    k.took = 0;
  }

  // ---- the substep -----------------------------------------------------------------------------

  private substepOnce(h: number): void {
    const g = this.gravity;
    const dragL = Math.pow(this.linearDrag, h);
    const dragA = Math.pow(this.angularDrag, h);
    for (const b of this.bodies) {
      b.px[0] = b.x[0];
      b.px[1] = b.x[1];
      b.px[2] = b.x[2];
      b.pq[0] = b.q[0];
      b.pq[1] = b.q[1];
      b.pq[2] = b.q[2];
      b.pq[3] = b.q[3];
      if (b.invMass === 0) continue;
      b.updateInertia();
      b.v[0] = (b.v[0] + h * b.force[0] * b.invMass) * dragL;
      b.v[1] = (b.v[1] + h * b.force[1] * b.invMass) * dragL;
      b.v[2] = (b.v[2] + h * (b.force[2] * b.invMass - g)) * dragL;
      const t = b.invInertiaMul(b.torque[0], b.torque[1], b.torque[2], S3);
      b.w[0] = (b.w[0] + h * t[0]) * dragA;
      b.w[1] = (b.w[1] + h * t[1]) * dragA;
      b.w[2] = (b.w[2] + h * t[2]) * dragA;
      if (b.twistDamping > 0) {
        const a = rot(b.q, b.longAxis[0], b.longAxis[1], b.longAxis[2], S2);
        const spin = (b.w[0] * a[0] + b.w[1] * a[1] + b.w[2] * a[2]) * (1 - Math.exp(-b.twistDamping * h));
        b.w[0] -= a[0] * spin;
        b.w[1] -= a[1] * spin;
        b.w[2] -= a[2] * spin;
      }
      b.x[0] += h * b.v[0];
      b.x[1] += h * b.v[1];
      b.x[2] += h * b.v[2];
      b.rotate(h * b.w[0], h * b.w[1], h * b.w[2]);
      b.updateInertia();
      b.v0[0] = b.v[0];
      b.v0[1] = b.v[1];
      b.v0[2] = b.v[2];
      b.w0[0] = b.w[0];
      b.w0[1] = b.w[1];
      b.w0[2] = b.w[2];
    }
    for (const j of this.joints) this.solveJoint(j, h);
    for (const a of this.attachments) if (a.enabled) this.solveAttachment(a, h);
    for (const o of this.orienters) if (o.enabled) this.solveOrienter(o, h);
    for (const p of this.nearPairs) this.solvePair(p);
    for (let i = 0; i < this.contactCount; i++) this.solveContact(this.contacts[i]!);
    // velocities from the positions
    const ih = 1 / h;
    for (const b of this.bodies) {
      if (b.invMass === 0) continue;
      b.v[0] = (b.x[0] - b.px[0]) * ih;
      b.v[1] = (b.x[1] - b.px[1]) * ih;
      b.v[2] = (b.x[2] - b.px[2]) * ih;
      // dq = q pq^-1
      const q = b.q, p = b.pq;
      const dx = q[3] * -p[0] + q[0] * p[3] + q[1] * -p[2] - q[2] * -p[1];
      const dy = q[3] * -p[1] + q[1] * p[3] + q[2] * -p[0] - q[0] * -p[2];
      const dz = q[3] * -p[2] + q[2] * p[3] + q[0] * -p[1] - q[1] * -p[0];
      const dw = q[3] * p[3] - q[0] * -p[0] - q[1] * -p[1] - q[2] * -p[2];
      const s = dw < 0 ? -2 * ih : 2 * ih;
      b.w[0] = dx * s;
      b.w[1] = dy * s;
      b.w[2] = dz * s;
      // a correction may move a body but not launch it (a limb folded past its range, a
      // contact found deep, is put right without the energy it would take to do it that fast)
      clampChange(b.v, b.v0, this.maxDv);
      clampChange(b.w, b.w0, this.maxDw);
    }
    this.limitVelocity();
    for (const j of this.joints) if (j.damping > 0) this.dampJoint(j, h);
    for (const a of this.attachments) if (a.enabled && a.damping > 0) this.dampAttachment(a, h);
    for (const o of this.orienters) if (o.enabled && o.damping > 0) this.dampOrienter(o, h);
    for (let i = 0; i < this.contactCount; i++) this.contactVelocity(this.contacts[i]!, h);
  }

  // ---- joints ----------------------------------------------------------------------------------

  private solveJoint(j: Joint, h: number): void {
    const A = j.a, B = j.b;
    // the anchors meet
    const ra = rot(A.q, j.anchorA[0], j.anchorA[1], j.anchorA[2], J0);
    const rb = rot(B.q, j.anchorB[0], j.anchorB[1], j.anchorB[2], J1);
    let dx = B.x[0] + rb[0] - A.x[0] - ra[0];
    let dy = B.x[1] + rb[1] - A.x[1] - ra[1];
    let dz = B.x[2] + rb[2] - A.x[2] - ra[2];
    const c = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (c > 1e-9) {
      dx /= c;
      dy /= c;
      dz /= c;
      const w = A.invMassAt(ra[0], ra[1], ra[2], dx, dy, dz) + B.invMassAt(rb[0], rb[1], rb[2], dx, dy, dz);
      if (w > 0) {
        const p = c / w;
        A.applyPos(p * dx, p * dy, p * dz, ra[0], ra[1], ra[2]);
        B.applyPos(-p * dx, -p * dy, -p * dz, rb[0], rb[1], rb[2]);
      }
    }
    // the muscle: towards the target relative rotation
    if (j.stiffness > 0) {
      // e = log(qB (qA target)^-1): how far B is turned past its target (world)
      qmulTo(A.q, j.target, J2q);
      const e = qerror(B.q, J2q, J3);
      const th = Math.hypot(e[0], e[1], e[2]);
      if (th > 1e-7) this.turn(A, B, e[0] / th, e[1] / th, e[2] / th, th, 1 / (j.stiffness * h * h), j.maxTorque * h * h);
    }
    if (j.feed[0] !== 0 || j.feed[1] !== 0 || j.feed[2] !== 0) {
      const f = j.feed;
      const hh = h * h;
      A.applyRot(-f[0] * hh, -f[1] * hh, -f[2] * hh);
      B.applyRot(f[0] * hh, f[1] * hh, f[2] * hh);
    }
    // limits
    const fa = qmulTo(A.q, j.frameA, J4q);
    const fb = qmulTo(B.q, j.frameB, J5q);
    if (j.kind === 'hinge') {
      // the hinge axes line up
      const ax = rot(fa, 1, 0, 0, J0);
      const bx = rot(fb, 1, 0, 0, J1);
      this.align(A, B, ax, bx);
      if (j.hinge) {
        const fa2 = qmulTo(A.q, j.frameA, J4q);
        const fb2 = qmulTo(B.q, j.frameB, J5q);
        const n = rot(fa2, 1, 0, 0, J0);
        const n1 = rot(fa2, 0, 0, 1, J1);
        const n2 = rot(fb2, 0, 0, 1, J3);
        this.limitAngle(A, B, n, n1, n2, j.hinge[0], j.hinge[1]);
      }
      return;
    }
    const az = rot(fa, 0, 0, 1, J0);
    const bz = rot(fb, 0, 0, 1, J1);
    if (j.swing) {
      // the tilt of B's twist axis in A's joint frame, and the limit in that direction
      const cx = az[1] * bz[2] - az[2] * bz[1];
      const cy = az[2] * bz[0] - az[0] * bz[2];
      const cz = az[0] * bz[1] - az[1] * bz[0];
      const sn = Math.sqrt(cx * cx + cy * cy + cz * cz);
      const cs = az[0] * bz[0] + az[1] * bz[1] + az[2] * bz[2];
      const angle = Math.atan2(sn, cs);
      if (sn > 1e-6) {
        const d = rotInv(fa, bz[0], bz[1], bz[2], J2);
        const l = Math.hypot(d[0], d[1]) || 1;
        const ux = d[0] / l, uy = d[1] / l;
        const lx = ux >= 0 ? j.swing.xPos : j.swing.xNeg;
        const ly = uy >= 0 ? j.swing.yPos : j.swing.yNeg;
        const max = 1 / Math.sqrt((ux * ux) / (lx * lx) + (uy * uy) / (ly * ly));
        if (angle > max) {
          this.turn(A, B, cx / sn, cy / sn, cz / sn, Math.min(angle - max, LIMIT_STEP), 0, Infinity);
          this.limitHit(A, B, cx / sn, cy / sn, cz / sn, 1);
        }
      }
    }
    if (j.twist) {
      // swing-twist: the relative rotation fa^-1 fb = swing * twist, twist about z; turning B
      // about its own z changes the twist alone (it cannot fight the swing limit)
      const fa2 = qmulTo(A.q, j.frameA, J4q);
      const fb2 = qmulTo(B.q, j.frameB, J5q);
      // r = fa2^-1 fb2 (only z and w are needed)
      const ax = -fa2[0], ay = -fa2[1], az2 = -fa2[2], aw = fa2[3];
      const rz = aw * fb2[2] + az2 * fb2[3] + ax * fb2[1] - ay * fb2[0];
      const rw = aw * fb2[3] - ax * fb2[0] - ay * fb2[1] - az2 * fb2[2];
      let tw = 2 * Math.atan2(rz, rw);
      if (tw > Math.PI) tw -= 2 * Math.PI;
      else if (tw < -Math.PI) tw += 2 * Math.PI;
      const [lo, hi] = j.twist;
      if (tw > hi || tw < lo) {
        const n = rot(fb2, 0, 0, 1, J0);
        const excess = tw > hi ? Math.min(tw - hi, LIMIT_STEP) : Math.max(tw - lo, -LIMIT_STEP);
        this.turn(A, B, n[0], n[1], n[2], excess, 0, Infinity);
        this.limitHit(A, B, n[0], n[1], n[2], excess > 0 ? 1 : -1);
      }
    }
  }

  /** Turns B towards A so that B's axis `bx` lines up with A's `ax`. */
  private align(A: RigidBody, B: RigidBody, ax: Readonly<V3>, bx: Readonly<V3>): void {
    const cx = ax[1] * bx[2] - ax[2] * bx[1];
    const cy = ax[2] * bx[0] - ax[0] * bx[2];
    const cz = ax[0] * bx[1] - ax[1] * bx[0];
    const sn = Math.sqrt(cx * cx + cy * cy + cz * cz);
    if (sn < 1e-7) return;
    const angle = Math.atan2(sn, ax[0] * bx[0] + ax[1] * bx[1] + ax[2] * bx[2]);
    this.turn(A, B, cx / sn, cy / sn, cz / sn, angle, 0, Infinity);
  }

  /**
   * Keeps the angle from n1 (on A) to n2 (on B) about n within [lo, hi] (n1, n2 orthogonal to
   * n, not necessarily unit).
   */
  private limitAngle(A: RigidBody, B: RigidBody, n: Readonly<V3>, n1: Readonly<V3>, n2: Readonly<V3>, lo: number, hi: number): void {
    const cx = n1[1] * n2[2] - n1[2] * n2[1];
    const cy = n1[2] * n2[0] - n1[0] * n2[2];
    const cz = n1[0] * n2[1] - n1[1] * n2[0];
    const phi = Math.atan2(cx * n[0] + cy * n[1] + cz * n[2], n1[0] * n2[0] + n1[1] * n2[1] + n1[2] * n2[2]);
    // (a joint past its range is brought back over a few substeps: corrections that fight each
    // other at the edge of a range would otherwise pump energy into the limb)
    if (phi > hi) {
      this.turn(A, B, n[0], n[1], n[2], Math.min(phi - hi, LIMIT_STEP), 0, Infinity);
      this.limitHit(A, B, n[0], n[1], n[2], 1);
    } else if (phi < lo) {
      this.turn(A, B, n[0], n[1], n[2], Math.max(phi - lo, -LIMIT_STEP), 0, Infinity);
      this.limitHit(A, B, n[0], n[1], n[2], -1);
    }
  }

  /** Joint limits pushed back this substep (joint bodies and the axis B was turned about, -n). */
  private limitHits: { a: RigidBody; b: RigidBody; n: V3 }[] = [];
  private limitCount = 0;

  /** A limit turned B back about -n: its rebound is taken out in the velocity pass. */
  private limitHit(A: RigidBody, B: RigidBody, nx: number, ny: number, nz: number, sign: number): void {
    let h = this.limitHits[this.limitCount];
    if (!h) {
      h = { a: A, b: B, n: [0, 0, 0] };
      this.limitHits.push(h);
    }
    this.limitCount++;
    h.a = A;
    h.b = B;
    h.n[0] = nx * sign;
    h.n[1] = ny * sign;
    h.n[2] = nz * sign;
  }

  /** Limits are inelastic: B does not spin on away from where the limit put it. */
  private limitVelocity(): void {
    for (let i = 0; i < this.limitCount; i++) {
      const { a: A, b: B, n } = this.limitHits[i]!;
      const rel = (B.w[0] - A.w[0]) * n[0] + (B.w[1] - A.w[1]) * n[1] + (B.w[2] - A.w[2]) * n[2];
      // (the limit turned B towards -n; spinning on that way is the bounce)
      if (rel >= 0) continue;
      const w = A.invMassRot(n[0], n[1], n[2]) + B.invMassRot(n[0], n[1], n[2]);
      if (w <= 0) continue;
      const l = -rel / w;
      const ta = A.invInertiaMul(n[0] * l, n[1] * l, n[2] * l, S3);
      A.w[0] -= ta[0];
      A.w[1] -= ta[1];
      A.w[2] -= ta[2];
      const tb = B.invInertiaMul(n[0] * l, n[1] * l, n[2] * l, S3);
      B.w[0] += tb[0];
      B.w[1] += tb[1];
      B.w[2] += tb[2];
    }
    this.limitCount = 0;
  }

  /**
   * Reduces B's rotation relative to A about unit n by `angle` (A turns +, B turns -, by their
   * inverse inertias), with compliance and an impulse limit.
   */
  private turn(A: RigidBody, B: RigidBody, nx: number, ny: number, nz: number, angle: number, compliance: number, maxImpulse: number): void {
    const w = A.invMassRot(nx, ny, nz) + B.invMassRot(nx, ny, nz);
    if (w <= 0) return;
    let p = angle / (w + compliance);
    if (p > maxImpulse) p = maxImpulse;
    else if (p < -maxImpulse) p = -maxImpulse;
    A.applyRot(p * nx, p * ny, p * nz);
    B.applyRot(-p * nx, -p * ny, -p * nz);
  }

  private dampJoint(j: Joint, h: number): void {
    const A = j.a, B = j.b;
    const tv = j.targetVel;
    const t = tv[0] !== 0 || tv[1] !== 0 || tv[2] !== 0 ? rot(A.q, tv[0], tv[1], tv[2], J3) : ZERO;
    let rx = B.w[0] - A.w[0] - t[0], ry = B.w[1] - A.w[1] - t[1], rz = B.w[2] - A.w[2] - t[2];
    const r = Math.sqrt(rx * rx + ry * ry + rz * rz);
    if (r < 1e-9) return;
    rx /= r;
    ry /= r;
    rz /= r;
    const w = A.invMassRot(rx, ry, rz) + B.invMassRot(rx, ry, rz);
    if (w <= 0) return;
    // the share of the relative spin a damper c takes out of the limb in a substep
    const frac = j.effInertia > 0 ? Math.min(1, (j.damping * h) / j.effInertia) : Math.min(1, j.damping * h * w);
    const l = (r * frac) / w;
    const ta = A.invInertiaMul(rx * l, ry * l, rz * l, S3);
    A.w[0] += ta[0];
    A.w[1] += ta[1];
    A.w[2] += ta[2];
    const tb = B.invInertiaMul(rx * l, ry * l, rz * l, S3);
    B.w[0] -= tb[0];
    B.w[1] -= tb[1];
    B.w[2] -= tb[2];
  }

  // ---- assists -----------------------------------------------------------------------------------

  private solveAttachment(a: Attachment, h: number): void {
    const B = a.body;
    const r = rot(B.q, a.local[0], a.local[1], a.local[2], J0);
    let dx = a.axes[0] ? a.target[0] - B.x[0] - r[0] : 0;
    let dy = a.axes[1] ? a.target[1] - B.x[1] - r[1] : 0;
    let dz = a.axes[2] ? a.target[2] - B.x[2] - r[2] : 0;
    const c = Math.sqrt(dx * dx + dy * dy + dz * dz);
    a.applied[0] = a.applied[1] = a.applied[2] = 0;
    if (c < 1e-9) return;
    dx /= c;
    dy /= c;
    dz /= c;
    const w = B.invMassAt(r[0], r[1], r[2], dx, dy, dz);
    if (w <= 0) return;
    const compliance = a.stiffness === Infinity ? 0 : 1 / (a.stiffness * h * h);
    let p = c / (w + compliance);
    const max = a.maxForce * h * h;
    if (p > max) p = max;
    B.applyPos(p * dx, p * dy, p * dz, r[0], r[1], r[2]);
    const f = p / (h * h);
    a.applied[0] = f * dx;
    a.applied[1] = f * dy;
    a.applied[2] = f * dz;
  }

  private dampAttachment(a: Attachment, h: number): void {
    const B = a.body;
    const r = rot(B.q, a.local[0], a.local[1], a.local[2], J0);
    let vx = a.axes[0] ? a.targetVel[0] - (B.v[0] + B.w[1] * r[2] - B.w[2] * r[1]) : 0;
    let vy = a.axes[1] ? a.targetVel[1] - (B.v[1] + B.w[2] * r[0] - B.w[0] * r[2]) : 0;
    let vz = a.axes[2] ? a.targetVel[2] - (B.v[2] + B.w[0] * r[1] - B.w[1] * r[0]) : 0;
    const s = Math.sqrt(vx * vx + vy * vy + vz * vz);
    if (s < 1e-9) return;
    vx /= s;
    vy /= s;
    vz /= s;
    const w = B.invMassAt(r[0], r[1], r[2], vx, vy, vz);
    if (w <= 0) return;
    let j = s * Math.min(a.damping * h, 1 / w);
    const max = a.maxForce * h;
    if (j > max) j = max;
    B.applyImpulse(j * vx, j * vy, j * vz, r[0], r[1], r[2]);
  }

  private solveOrienter(o: Orienter, h: number): void {
    const B = o.body;
    let ex: number, ey: number, ez: number;
    if (o.tiltOnly) {
      // turn the body's up axis onto the target's
      const u = rot(B.q, o.up[0], o.up[1], o.up[2], J0);
      const t = rot(o.target, o.up[0], o.up[1], o.up[2], J1);
      const cx = t[1] * u[2] - t[2] * u[1];
      const cy = t[2] * u[0] - t[0] * u[2];
      const cz = t[0] * u[1] - t[1] * u[0];
      const sn = Math.sqrt(cx * cx + cy * cy + cz * cz);
      if (sn < 1e-7) return;
      const ang = Math.atan2(sn, t[0] * u[0] + t[1] * u[1] + t[2] * u[2]);
      ex = (cx / sn) * ang;
      ey = (cy / sn) * ang;
      ez = (cz / sn) * ang;
    } else {
      const e = qerror(B.q, o.target, J3);
      ex = e[0];
      ey = e[1];
      ez = e[2];
    }
    const th = Math.sqrt(ex * ex + ey * ey + ez * ez);
    if (th < 1e-7) return;
    const nx = ex / th, ny = ey / th, nz = ez / th;
    const w = B.invMassRot(nx, ny, nz);
    if (w <= 0) return;
    const compliance = o.stiffness === Infinity ? 0 : 1 / (o.stiffness * h * h);
    let p = th / (w + compliance);
    const max = o.maxTorque * h * h;
    if (p > max) p = max;
    B.applyRot(-p * nx, -p * ny, -p * nz);
  }

  private dampOrienter(o: Orienter, h: number): void {
    const B = o.body;
    let wx = B.w[0], wy = B.w[1], wz = B.w[2];
    if (o.tiltOnly) {
      // only the tilting part of the spin
      const u = rot(B.q, o.up[0], o.up[1], o.up[2], J0);
      const d = wx * u[0] + wy * u[1] + wz * u[2];
      wx -= d * u[0];
      wy -= d * u[1];
      wz -= d * u[2];
    }
    const s = Math.sqrt(wx * wx + wy * wy + wz * wz);
    if (s < 1e-9) return;
    wx /= s;
    wy /= s;
    wz /= s;
    const w = B.invMassRot(wx, wy, wz);
    if (w <= 0) return;
    let l = s * Math.min(o.damping * h, 1 / w);
    const max = o.maxTorque * h;
    if (l > max) l = max;
    const t = B.invInertiaMul(wx * l, wy * l, wz * l, S3);
    B.w[0] -= t[0];
    B.w[1] -= t[1];
    B.w[2] -= t[2];
  }

  // ---- contacts ----------------------------------------------------------------------------------

  private solvePair(p: SpherePair): void {
    const A = p.a, B = p.b;
    const sa = A.spheres[p.sa]!, sb = B.spheres[p.sb]!;
    const ca = A.point(sa.c, J0);
    const cb = B.point(sb.c, J1);
    let nx = cb[0] - ca[0], ny = cb[1] - ca[1], nz = cb[2] - ca[2];
    const d = Math.sqrt(nx * nx + ny * ny + nz * nz);
    const pen = sa.r + sb.r - d;
    if (pen <= 0 || d < 1e-9) return;
    nx /= d;
    ny /= d;
    nz /= d;
    const rax = ca[0] + nx * sa.r - A.x[0], ray = ca[1] + ny * sa.r - A.x[1], raz = ca[2] + nz * sa.r - A.x[2];
    const rbx = cb[0] - nx * sb.r - B.x[0], rby = cb[1] - ny * sb.r - B.x[1], rbz = cb[2] - nz * sb.r - B.x[2];
    const w = A.invMassAt(rax, ray, raz, nx, ny, nz) + B.invMassAt(rbx, rby, rbz, nx, ny, nz);
    if (w <= 0) return;
    // (a deep overlap - limbs folded into each other - comes apart over a few substeps, not
    // with a bang)
    const l = Math.min(pen, this.pairSpeed * this.substep) / w;
    A.applyPos(-l * nx, -l * ny, -l * nz, rax, ray, raz);
    B.applyPos(l * nx, l * ny, l * nz, rbx, rby, rbz);
  }

  private solveContact(k: WorldContact): void {
    const B = k.body;
    const c = B.point(k.c, J0);
    const n = k.n;
    const pen = k.d + k.r - (n[0] * c[0] + n[1] * c[1] + n[2] * c[2]);
    if (pen <= 0) {
      k.lambda = 0;
      return;
    }
    B.contact = true;
    B.contactNormal[0] = n[0];
    B.contactNormal[1] = n[1];
    B.contactNormal[2] = n[2];
    // the contact point on the sphere, relative to the centre of mass
    const rx = c[0] - n[0] * k.r - B.x[0], ry = c[1] - n[1] * k.r - B.x[1], rz = c[2] - n[2] * k.r - B.x[2];
    B.contactPoint[0] = B.x[0] + rx;
    B.contactPoint[1] = B.x[1] + ry;
    B.contactPoint[2] = B.x[2] + rz;
    const w = B.invMassAt(rx, ry, rz, n[0], n[1], n[2]);
    if (w <= 0) return;
    // (an obstacle found deep inside - two bodies overlapping - is left gradually)
    const l = (k.obstacle ? Math.min(pen, DEEP_SPEED * this.substep) : pen) / w;
    B.applyPos(l * n[0], l * n[1], l * n[2], rx, ry, rz);
    k.lambda = l;
    if (k.other) k.took += Math.min(l / this.substep, 40);
    if (k.obstacle) B.bumped += Math.min(l / this.substep, 40) * Math.hypot(n[0], n[1]);
    // static friction: the contact point does not slide within the friction cone
    const loc = rotInv(B.q, rx, ry, rz, J1);
    const p0 = rot(B.pq, loc[0], loc[1], loc[2], J2);
    const p1x = B.x[0] + rx, p1y = B.x[1] + ry, p1z = B.x[2] + rz;
    let tx = p1x - (B.px[0] + p0[0]), ty = p1y - (B.px[1] + p0[1]), tz = p1z - (B.px[2] + p0[2]);
    const dn = tx * n[0] + ty * n[1] + tz * n[2];
    tx -= dn * n[0];
    ty -= dn * n[1];
    tz -= dn * n[2];
    const tl = Math.sqrt(tx * tx + ty * ty + tz * tz);
    if (tl > 1e-9) {
      tx /= tl;
      ty /= tl;
      tz /= tl;
      const wt = B.invMassAt(rx, ry, rz, tx, ty, tz);
      const lt = tl / wt;
      if (lt < B.friction * 1.1 * l) B.applyPos(-lt * tx, -lt * ty, -lt * tz, rx, ry, rz);
    }
  }

  private contactVelocity(k: WorldContact, h: number): void {
    if (k.lambda <= 0) return;
    const B = k.body;
    const n = k.n;
    const c = B.point(k.c, J0);
    const rx = c[0] - n[0] * k.r - B.x[0], ry = c[1] - n[1] * k.r - B.x[1], rz = c[2] - n[2] * k.r - B.x[2];
    const vx = B.v[0] + B.w[1] * rz - B.w[2] * ry;
    const vy = B.v[1] + B.w[2] * rx - B.w[0] * rz;
    const vz = B.v[2] + B.w[0] * ry - B.w[1] * rx;
    const vn = vx * n[0] + vy * n[1] + vz * n[2];
    let tx = vx - vn * n[0], ty = vy - vn * n[1], tz = vz - vn * n[2];
    const vt = Math.sqrt(tx * tx + ty * ty + tz * tz);
    // dynamic friction, bounded by the normal impulse
    if (vt > 1e-6) {
      tx /= vt;
      ty /= vt;
      tz /= vt;
      const wt = B.invMassAt(rx, ry, rz, tx, ty, tz);
      const jt = Math.min((B.friction * k.lambda) / h, vt / wt);
      B.applyImpulse(-jt * tx, -jt * ty, -jt * tz, rx, ry, rz);
    }
    // no bounce
    if (vn < 0) {
      const wn = B.invMassAt(rx, ry, rz, n[0], n[1], n[2]);
      const jn = -vn / wn;
      B.applyImpulse(jn * n[0], jn * n[1], jn * n[2], rx, ry, rz);
      if (jn > B.impact) B.impact = jn;
      if (k.obstacle) B.bumped += jn * Math.hypot(n[0], n[1]);
      if (k.other) k.took += jn;
    }
  }
}

// ---- scalar helpers ----------------------------------------------------------------------------

const S0: V3 = [0, 0, 0];
const S1: V3 = [0, 0, 0];
const S2: V3 = [0, 0, 0];
const S3: V3 = [0, 0, 0];
const J0: V3 = [0, 0, 0];
const J1: V3 = [0, 0, 0];
const J2: V3 = [0, 0, 0];
const J3: V3 = [0, 0, 0];
const ZERO: Readonly<V3> = [0, 0, 0];
/** The most a joint limit turns a joint back in one substep (rad). */
const LIMIT_STEP = 0.025;
const J2q: Quat = [0, 0, 0, 1];
const J4q: Quat = [0, 0, 0, 1];
const J5q: Quat = [0, 0, 0, 1];

/** How fast an overlap with another body is undone (m/s): an approach is met in full, a body found deep inside another leaves it without being flung. */
const DEEP_SPEED = 3;
const MAX_SPEED = 60;
const MAX_SPIN = 80;

function clampLength(v: V3, max: number): void {
  const l = Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l <= max) return;
  const k = max / l;
  v[0] *= k;
  v[1] *= k;
  v[2] *= k;
}

function clampChange(v: V3, v0: Readonly<V3>, max: number): void {
  const dx = v[0] - v0[0], dy = v[1] - v0[1], dz = v[2] - v0[2];
  const l = Math.sqrt(dx * dx + dy * dy + dz * dz);
  if (l <= max) return;
  const k = max / l;
  v[0] = v0[0] + dx * k;
  v[1] = v0[1] + dy * k;
  v[2] = v0[2] + dz * k;
}

/** out = q v. */
function rot(q: Readonly<Quat>, vx: number, vy: number, vz: number, out: V3): V3 {
  const qx = q[0], qy = q[1], qz = q[2], qw = q[3];
  const tx = 2 * (qy * vz - qz * vy);
  const ty = 2 * (qz * vx - qx * vz);
  const tz = 2 * (qx * vy - qy * vx);
  out[0] = vx + qw * tx + (qy * tz - qz * ty);
  out[1] = vy + qw * ty + (qz * tx - qx * tz);
  out[2] = vz + qw * tz + (qx * ty - qy * tx);
  return out;
}

/** out = q^-1 v. */
function rotInv(q: Readonly<Quat>, vx: number, vy: number, vz: number, out: V3): V3 {
  const qx = -q[0], qy = -q[1], qz = -q[2], qw = q[3];
  const tx = 2 * (qy * vz - qz * vy);
  const ty = 2 * (qz * vx - qx * vz);
  const tz = 2 * (qx * vy - qy * vx);
  out[0] = vx + qw * tx + (qy * tz - qz * ty);
  out[1] = vy + qw * ty + (qz * tx - qx * tz);
  out[2] = vz + qw * tz + (qx * ty - qy * tx);
  return out;
}

function qmulTo(a: Readonly<Quat>, b: Readonly<Quat>, out: Quat): Quat {
  const ax = a[0], ay = a[1], az = a[2], aw = a[3];
  const bx = b[0], by = b[1], bz = b[2], bw = b[3];
  out[0] = aw * bx + ax * bw + ay * bz - az * by;
  out[1] = aw * by + ay * bw + az * bx - ax * bz;
  out[2] = aw * bz + az * bw + ax * by - ay * bx;
  out[3] = aw * bw - ax * bx - ay * by - az * bz;
  return out;
}

/** Rotation vector of q t^-1 (how far q is turned past t, world). */
export function qerror(q: Readonly<Quat>, t: Readonly<Quat>, out: V3 = [0, 0, 0]): V3 {
  // d = q t^-1
  const tx = -t[0], ty = -t[1], tz = -t[2], tw = t[3];
  let x = q[3] * tx + q[0] * tw + q[1] * tz - q[2] * ty;
  let y = q[3] * ty + q[1] * tw + q[2] * tx - q[0] * tz;
  let z = q[3] * tz + q[2] * tw + q[0] * ty - q[1] * tx;
  let w = q[3] * tw - q[0] * tx - q[1] * ty - q[2] * tz;
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
