/**
 * Steering with weight: how a body moves along a path and turns, as opposed to snapping to it.
 *
 * - `pursue`: the point some way ahead along the path (the body cuts corners gently instead
 *   of turning sharply at each waypoint) and the path length left (for arriving).
 * - `steer`: the horizontal velocity follows the wanted one with limited acceleration and
 *   braking, and a heading that turns at a limited rate (tighter when slow, wide when running);
 *   sharp turns are taken slower.
 * - `turn`: a facing that turns with angular momentum: it speeds up and brakes into the new
 *   direction (no instant spins, no overshoot).
 */
import type { V3 } from 'svx-anim';

export interface SteerOptions {
  /** Speeding up and slowing down (m/s^2). */
  accel: number;
  decel: number;
}

/** Walking bodies speed up gently, runners harder; everyone brakes a little harder. */
export function steerOptions(wantSpeed: number): SteerOptions {
  const t = smooth(1.5, 4, wantSpeed);
  return { accel: 1.9 + 1.6 * t, decel: 3.6 + 1.2 * t };
}

/** Heading turn rate (rad/s) at a speed: a walker turns within half a metre, a runner in metres. */
export function turnRateAt(speed: number): number {
  return Math.max(1.2, Math.min(3.2, 4.4 / Math.max(speed, 0.8)));
}

/**
 * The point `ahead` metres along the path from `pos` (through `path`, ending at `goal`), and
 * the path length left.
 */
export function pursue(pos: Readonly<V3>, path: readonly V3[], goal: Readonly<V3>, ahead: number): { point: V3; remaining: number } {
  const pts = path.length > 0 ? path : [goal];
  let from: Readonly<V3> = pos;
  let travelled = 0;
  let point: V3 | null = null;
  let remaining = 0;
  for (const p of pts) {
    const seg = Math.hypot(p[0] - from[0], p[1] - from[1]);
    if (!point && travelled + seg >= ahead && seg > 1e-6) {
      const t = (ahead - travelled) / seg;
      point = [from[0] + (p[0] - from[0]) * t, from[1] + (p[1] - from[1]) * t, from[2] + (p[2] - from[2]) * t];
    }
    travelled += seg;
    remaining += seg;
    from = p;
  }
  const last = pts[pts.length - 1]!;
  return { point: point ?? [last[0], last[1], last[2]], remaining };
}

/**
 * New horizontal velocity: towards `wantDir` (unit, xy) at `wantSpeed`, from `vel`, with the
 * heading turning at most `turnRateAt(speed)` and the speed changing within accel / decel.
 */
export function steer(vel: Readonly<[number, number]>, wantDir: Readonly<[number, number]>, wantSpeed: number, dt: number, opts: SteerOptions): [number, number] {
  const sp = Math.hypot(vel[0], vel[1]);
  const want = Math.atan2(wantDir[1], wantDir[0]);
  let heading = want;
  let off = 0;
  if (sp > 0.15) {
    const cur = Math.atan2(vel[1], vel[0]);
    off = wrap(want - cur);
    const max = turnRateAt(sp) * dt;
    heading = cur + Math.max(-max, Math.min(max, off));
  }
  // a sharp turn is taken slower
  const target = wantSpeed * (1 - 0.6 * smooth(0.6, 2.0, Math.abs(off)));
  const change = target - sp;
  const next = sp + Math.max(-opts.decel * dt, Math.min(opts.accel * dt, change));
  const s = Math.max(0, next);
  return [Math.cos(heading) * s, Math.sin(heading) * s];
}

/**
 * Turns a facing `yaw` (with its angular velocity `rate`) towards `want`: accelerating at most
 * `accel` rad/s^2 up to `maxRate`, and braking in time to stop on it.
 */
export function turn(yaw: number, rate: number, want: number, dt: number, maxRate: number, accel: number): { yaw: number; rate: number } {
  const d = wrap(want - yaw);
  const brake = Math.sqrt(2 * accel * Math.abs(d)) * 0.92;
  const wanted = Math.sign(d) * Math.min(maxRate, brake);
  let r = rate + Math.max(-accel * dt, Math.min(accel * dt, wanted - rate));
  let step = r * dt;
  // (never past the target)
  if ((d >= 0 && step > d) || (d < 0 && step < d)) {
    step = d;
    r = 0;
  }
  return { yaw: yaw + step, rate: r };
}

export function wrap(a: number): number {
  return Math.atan2(Math.sin(a), Math.cos(a));
}

function smooth(e0: number, e1: number, x: number): number {
  const t = Math.max(0, Math.min(1, (x - e0) / (e1 - e0)));
  return t * t * (3 - 2 * t);
}
