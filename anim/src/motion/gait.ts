/**
 * Gait parameters as functions of speed and crouch: a human walk (inverted pendulum, double
 * support) blends into a run (spring-mass, flight phase) between ~2 and ~3 m/s. The numbers
 * follow normal-gait data: cadence and stride length rise with speed, the stance fraction
 * (duty factor) falls from ~0.62 walking to ~0.33 sprinting.
 */
import { clamp, lerp, smoothstep } from '../math/vec.ts';

export interface GaitParams {
  /** Gait cycles per second (a cycle is two steps). */
  freq: number;
  /** Fraction of the cycle a foot is on the ground. */
  duty: number;
  /** 0 walking .. 1 running. */
  run: number;
  /** Peak height of the swinging foot (m). */
  lift: number;
  /** Vertical pelvis oscillation (m). */
  bob: number;
  /** Lateral pelvis sway (m). */
  sway: number;
  /** Pelvis yaw and roll amplitudes (rad). */
  hipYaw: number;
  hipRoll: number;
  /** Forward trunk lean (rad). */
  lean: number;
  /** Arm swing amplitude and elbow bend (rad). */
  armSwing: number;
  elbow: number;
  /** Knee bend: how far the pelvis sits below standing height (m). */
  sink: number;
}

const V = [0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.5, 7.5];
const F = [0.75, 0.74, 0.86, 0.95, 1.04, 1.16, 1.28, 1.36, 1.45, 1.55];

function table(xs: readonly number[], ys: readonly number[], x: number): number {
  if (x <= xs[0]!) return ys[0]!;
  for (let i = 1; i < xs.length; i++) {
    if (x <= xs[i]!) {
      const t = (x - xs[i - 1]!) / (xs[i]! - xs[i - 1]!);
      return lerp(ys[i - 1]!, ys[i]!, t);
    }
  }
  return ys[ys.length - 1]!;
}

/**
 * @param speed ground speed (m/s)
 * @param crouch 0 standing .. 1 crouched
 * @param scale character height scale (1 = 1.78 m): strides scale with leg length
 */
export function gaitFor(speed: number, crouch: number, scale = 1): GaitParams {
  const v = Math.max(0, speed) / Math.sqrt(scale);
  const c = clamp(crouch, 0, 1);
  const run = smoothstep(1.9, 2.9, v) * (1 - c);
  let freq = table(V, F, v) * Math.sqrt(1 / scale);
  // crouched strides are short: at most ~0.9 m per cycle
  if (c > 0) freq = lerp(freq, Math.max(freq, (v * Math.sqrt(scale)) / 0.9, 0.8), c);
  const duty = lerp(lerp(0.62, 0.56, smoothstep(0.5, 2, v)), lerp(0.36, 0.26, smoothstep(3, 6.5, v)), run);
  return {
    freq,
    duty,
    run,
    lift: lerp(lerp(0.06, 0.1, smoothstep(0.5, 2, v)), lerp(0.2, 0.3, smoothstep(3, 6, v)), run) * scale * (1 - 0.35 * c),
    bob: lerp(lerp(0.008, 0.02, smoothstep(0.3, 1.8, v)), 0.035, run) * scale * (1 - 0.5 * c),
    sway: lerp(0.022, 0.008, run) * smoothstep(0.05, 0.6, v) * scale,
    hipYaw: lerp(0.08, 0.14, run) * smoothstep(0.1, 1.2, v),
    hipRoll: lerp(0.07, 0.04, run) * smoothstep(0.1, 1.2, v),
    lean: lerp(lerp(0.04, 0.08, smoothstep(0.5, 2, v)), lerp(0.16, 0.26, smoothstep(3, 6.5, v)), run),
    armSwing: lerp(lerp(0.1, 0.32, smoothstep(0.3, 2, v)), lerp(0.55, 0.8, smoothstep(3, 6.5, v)), run),
    elbow: lerp(lerp(0.2, 0.35, smoothstep(0.3, 2, v)), 1.45, run),
    sink: lerp(lerp(0.012, 0.03, smoothstep(0.3, 2, v)), 0.07, run) * scale,
  };
}
