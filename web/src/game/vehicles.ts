/**
 * The vehicles as the front end sees them (protocol `vehicles` messages, docs/VEHICLES.md): their
 * state, and their poses and their wheels' interpolated one engine tick behind like the rigid
 * pieces (render/islands.ts) - a car's body is its chassis piece, drawn from the piece's poses;
 * its wheels, the chase camera and the driver's seat follow these, sampled on the same ticks.
 */
import type { Vec3 } from '../engine/protocol.ts';
import { VEHICLE_KINDS, VEHICLE_STRIDE, VehicleFlag, WHEEL_STRIDE } from '../engine/protocol.ts';

export type Quat = [number, number, number, number];

/** Engine tick: poses are drawn this far in the past, between the last two samples. */
const TICK_S = 1 / 60;

interface Sample {
  t: number;
  /** Centre of mass. */
  pos: Vec3;
  /** Frame rotation (x forward, y left, z up). */
  rot: Quat;
  /** Frame origin (on the ground under the middle between the axles). */
  origin: Vec3;
}

export interface VehicleState {
  id: number;
  /** Its chassis piece (the island drawn for it; 0: none yet). */
  chassis: number;
  kind: number;
  paint: number;
  vel: Vec3;
  /** m/s along its forward. */
  speed: number;
  rpm: number;
  redline: number;
  /** -1 reverse, 0 neutral, 1.. */
  gear: number;
  throttle: number;
  brake: number;
  steer: number;
  handbrake: boolean;
  flags: number;
  /** The driver's seat in its frame. */
  seat: Vec3;
  /** Its box about its frame's origin: +-x, +-y, 0..z. */
  halfExtent: Vec3;
  wheels: number;
  damage: number;
  /** Its parts still on (doors, bonnet, bumpers, ...), of those it was built with. */
  parts: number;
  partsBuilt: number;
  prev: Sample | null;
  cur: Sample;
  seen: number;
}

export interface WheelState {
  id: number;
  vehicle: number;
  radius: number;
  width: number;
  contact: boolean;
  /** m/s of tyre sliding. */
  slip: number;
  material: number;
  compression: number;
  /** +1: its outer face is along its +y (a left wheel), -1 along -y. */
  side: number;
  prev: { t: number; centre: Vec3; rot: Quat } | null;
  cur: { t: number; centre: Vec3; rot: Quat };
  seen: number;
}

/** An interpolated pose. */
export interface Pose {
  pos: Vec3;
  rot: Quat;
  origin: Vec3;
}

export function rotate(q: Quat, v: Vec3): Vec3 {
  const [x, y, z, w] = q;
  // v + 2 w (q x v) + 2 q x (q x v)
  const tx = 2 * (y * v[2] - z * v[1]);
  const ty = 2 * (z * v[0] - x * v[2]);
  const tz = 2 * (x * v[1] - y * v[0]);
  return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)];
}

export function rotateInv(q: Quat, v: Vec3): Vec3 {
  return rotate([-q[0], -q[1], -q[2], q[3]], v);
}

function nlerp(a: Quat, b: Quat, s: number): Quat {
  const sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1 : 1;
  const q: Quat = [0, 0, 0, 0];
  let n = 0;
  for (let k = 0; k < 4; k++) {
    q[k] = a[k]! * (1 - s) + sign * b[k]! * s;
    n += q[k]! * q[k]!;
  }
  n = Math.sqrt(n) || 1;
  return [q[0] / n, q[1] / n, q[2] / n, q[3] / n];
}

function lerp3(a: Vec3, b: Vec3, s: number): Vec3 {
  return [a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s, a[2] + (b[2] - a[2]) * s];
}

/** The interpolation weight of `b` at `nowS - TICK_S`. */
function weight(at: number, bt: number, nowS: number): number {
  const span = Math.max(1e-6, bt - at);
  return Math.min(1, Math.max(0, (nowS - TICK_S - at) / span));
}

export function kindName(kind: number): string {
  return VEHICLE_KINDS[kind] ?? 'vehicle';
}

/** The car paints' names (`Paint` 1..14). */
const PAINT_NAMES: readonly string[] = ['', 'white', 'silver', 'black', 'red', 'blue', 'green', 'yellow', 'orange', 'taxi-yellow', 'navy', 'maroon', 'beige', 'graphite', 'teal'];

export function paintName(paint: number): string {
  return PAINT_NAMES[paint] ?? '';
}

export class VehicleTracker {
  readonly vehicles = new Map<number, VehicleState>();
  readonly wheels = new Map<number, WheelState>();
  /** The player's vehicle as the engine last said (0: on foot). */
  player = 0;
  private stamp = 0;

  clear(): void {
    this.vehicles.clear();
    this.wheels.clear();
    this.player = 0;
  }

  get size(): number {
    return this.vehicles.size;
  }

  apply(vehicles: Float64Array, wheels: Float64Array, player: number, nowS: number): void {
    const stamp = ++this.stamp;
    this.player = player;
    for (let o = 0; o + VEHICLE_STRIDE <= vehicles.length; o += VEHICLE_STRIDE) {
      const f = (k: number): number => vehicles[o + k]!;
      const id = f(0);
      const rot: Quat = [f(7), f(8), f(9), f(10)];
      const pos: Vec3 = [f(4), f(5), f(6)];
      const origin: Vec3 = [f(30), f(31), f(32)];
      const seatW: Vec3 = [f(22), f(23), f(24)];
      const cur: Sample = { t: nowS, pos, rot, origin };
      let v = this.vehicles.get(id);
      const seat = rotateInv(rot, [seatW[0] - origin[0], seatW[1] - origin[1], seatW[2] - origin[2]]);
      if (!v) {
        v = {
          id,
          chassis: 0,
          kind: 0,
          paint: 0,
          vel: [0, 0, 0],
          speed: 0,
          rpm: 0,
          redline: 6500,
          gear: 0,
          throttle: 0,
          brake: 0,
          steer: 0,
          handbrake: false,
          flags: 0,
          seat,
          halfExtent: [1, 1, 1],
          wheels: 0,
          damage: 0,
          parts: 0,
          partsBuilt: 0,
          prev: null,
          cur,
          seen: stamp,
        };
        this.vehicles.set(id, v);
      } else {
        // (after a gap, or a new chassis: from the new sample on)
        const chassis = f(1);
        v.prev = v.chassis !== chassis || nowS - v.cur.t > 4 * TICK_S ? { ...cur, t: nowS - TICK_S } : v.cur;
        v.cur = cur;
      }
      v.chassis = f(1);
      v.kind = f(2);
      v.paint = f(3);
      v.vel = [f(11), f(12), f(13)];
      v.speed = f(14);
      v.rpm = f(15);
      v.gear = f(16);
      v.throttle = f(17);
      v.brake = f(18);
      v.steer = f(19);
      v.handbrake = f(20) > 0;
      v.flags = f(21);
      v.seat = seat;
      v.halfExtent = [f(25), f(26), f(27)];
      v.wheels = f(28);
      v.damage = f(29);
      v.parts = f(34);
      v.partsBuilt = f(35);
      v.redline = f(33) > 0 ? f(33) : 6500;
      v.seen = stamp;
    }
    for (const [id, v] of this.vehicles) if (v.seen !== stamp) this.vehicles.delete(id);
    for (let o = 0; o + WHEEL_STRIDE <= wheels.length; o += WHEEL_STRIDE) {
      const f = (k: number): number => wheels[o + k]!;
      const id = f(1);
      const centre: Vec3 = [f(2), f(3), f(4)];
      const rot: Quat = [f(5), f(6), f(7), f(8)];
      const cur = { t: nowS, centre, rot };
      let w = this.wheels.get(id);
      const veh = this.vehicles.get(f(0));
      let side = 1;
      if (veh) {
        const l = rotateInv(veh.cur.rot, [centre[0] - veh.cur.origin[0], centre[1] - veh.cur.origin[1], centre[2] - veh.cur.origin[2]]);
        side = l[1] >= 0 ? 1 : -1;
      }
      if (!w) {
        w = { id, vehicle: f(0), radius: f(9), width: f(10), contact: false, slip: 0, material: -1, compression: 0, side, prev: null, cur, seen: stamp };
        this.wheels.set(id, w);
      } else {
        w.prev = nowS - w.cur.t > 4 * TICK_S ? { ...cur, t: nowS - TICK_S } : w.cur;
        w.cur = cur;
      }
      w.vehicle = f(0);
      w.radius = f(9);
      w.width = f(10);
      w.contact = f(11) > 0;
      w.slip = f(12);
      w.material = f(13);
      w.compression = f(14);
      w.side = side;
      w.seen = stamp;
    }
    for (const [id, w] of this.wheels) if (w.seen !== stamp) this.wheels.delete(id);
  }

  /** A vehicle's pose at `nowS` (one tick behind, interpolated). */
  pose(v: VehicleState, nowS: number): Pose {
    const a = v.prev;
    const b = v.cur;
    if (!a) return { pos: [...b.pos], rot: [...b.rot], origin: [...b.origin] };
    const s = weight(a.t, b.t, nowS);
    return { pos: lerp3(a.pos, b.pos, s), rot: nlerp(a.rot, b.rot, s), origin: lerp3(a.origin, b.origin, s) };
  }

  /** A wheel's centre and rotation at `nowS`. */
  wheelPose(w: WheelState, nowS: number): { centre: Vec3; rot: Quat } {
    const a = w.prev;
    const b = w.cur;
    if (!a) return { centre: [...b.centre], rot: [...b.rot] };
    const s = weight(a.t, b.t, nowS);
    return { centre: lerp3(a.centre, b.centre, s), rot: nlerp(a.rot, b.rot, s) };
  }

  /** A point of a vehicle's frame in the world, at a pose. */
  static toWorld(p: Pose, local: Vec3): Vec3 {
    const r = rotate(p.rot, local);
    return [p.origin[0] + r[0], p.origin[1] + r[1], p.origin[2] + r[2]];
  }

  static toLocal(p: Pose, world: Vec3): Vec3 {
    return rotateInv(p.rot, [world[0] - p.origin[0], world[1] - p.origin[1], world[2] - p.origin[2]]);
  }

  /**
   * The vehicle nearest to `pos` whose box is within `reach` of it (entering one), or null. Wrecks
   * and driven cars count too: the player takes any of them.
   */
  nearest(pos: Vec3, reach: number, nowS: number): VehicleState | null {
    let best: VehicleState | null = null;
    let bd = reach;
    for (const v of this.vehicles.values()) {
      if (v.chassis === 0 || v.wheels === 0) continue; // (a wreck on its belly: nothing to drive)
      const p = this.pose(v, nowS);
      const l = VehicleTracker.toLocal(p, pos);
      const e = v.halfExtent;
      const dx = Math.max(0, Math.abs(l[0]) - e[0]);
      const dy = Math.max(0, Math.abs(l[1]) - e[1]);
      const dz = Math.max(0, -l[2], l[2] - e[2]);
      const d = Math.hypot(dx, dy, dz);
      if (d < bd) {
        bd = d;
        best = v;
      }
    }
    return best;
  }

  isPlayer(v: VehicleState): boolean {
    return (v.flags & VehicleFlag.Player) !== 0;
  }
}
