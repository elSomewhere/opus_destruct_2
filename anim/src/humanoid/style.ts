/**
 * Movement personality: how a character walks and holds itself. Every character gets its own
 * style (seeded), so a crowd never moves in step: long confident strides or short hurried
 * ones, bouncy or smooth, swaying hips or stiff ones, big or small arm swing, upright or
 * slouched, light or heavy on its feet. Soldiers are upright, heavy (gear) and steady.
 */
import { Rng } from '../math/random.ts';

export interface GaitStyle {
  /** Stride length multiplier (the cadence adapts to the speed). */
  stride: number;
  /** Vertical bounce multiplier. */
  bounce: number;
  /** Hip sway and roll multiplier. */
  sway: number;
  /** Arm swing multiplier. */
  arms: number;
  /** Extra elbow bend (rad). */
  elbow: number;
  /** Posture: -1 slouched (chest and head down) .. 1 upright and proud. */
  posture: number;
  /** Step width multiplier. */
  width: number;
  /** Toe-out (rad). */
  toeOut: number;
  /** 0 light .. 1 heavy: footfall compression, slower to accelerate, more lean. */
  heavy: number;
  /** 0 .. 1: how still the head is kept (soldiers, dancers). */
  headStill: number;
  /** Fidgetiness when idle 0..1 (how often idles change). */
  fidget: number;
}

export const NEUTRAL_STYLE: Readonly<GaitStyle> = {
  stride: 1,
  bounce: 1,
  sway: 1,
  arms: 1,
  elbow: 0,
  posture: 0,
  width: 1,
  toeOut: 0.12,
  heavy: 0.4,
  headStill: 0.5,
  fidget: 0.5,
};

export type StyleKind = 'soldier' | 'civilian' | 'civilianFemale' | 'thug';

/** A random style of a kind (deterministic by seed). */
export function randomStyle(seed: number, kind: StyleKind): GaitStyle {
  const r = new Rng(seed * 7919 + 13);
  const n = (c: number, s: number): number => c + (r.next() + r.next() - 1) * s;
  if (kind === 'soldier') {
    return {
      stride: n(1.02, 0.05),
      bounce: n(0.85, 0.12),
      sway: n(0.75, 0.12),
      arms: n(0.9, 0.1),
      elbow: n(0.1, 0.05),
      posture: n(0.55, 0.2),
      width: n(1.15, 0.06),
      toeOut: n(0.14, 0.04),
      heavy: n(0.75, 0.1),
      headStill: n(0.8, 0.1),
      fidget: n(0.3, 0.15),
    };
  }
  if (kind === 'thug') {
    // a swagger: wide, rolling shoulders, arms swinging out, heavy on the feet, restless
    return {
      stride: n(1.02, 0.05),
      bounce: n(1.15, 0.12),
      sway: n(1.35, 0.15),
      arms: n(1.3, 0.12),
      elbow: n(0.28, 0.06),
      posture: n(0.15, 0.3),
      width: n(1.22, 0.06),
      toeOut: n(0.22, 0.04),
      heavy: n(0.65, 0.1),
      headStill: n(0.35, 0.1),
      fidget: n(0.75, 0.1),
    };
  }
  const female = kind === 'civilianFemale';
  // a few archetypes, then jitter: brisk commuter, stroller, sloucher, swaggerer, shuffler
  const type = r.int(0, 4);
  const base: GaitStyle = [
    { stride: 1.05, bounce: 0.9, sway: 1, arms: 0.9, elbow: 0.15, posture: 0.4, width: 1, toeOut: 0.1, heavy: 0.4, headStill: 0.6, fidget: 0.4 },
    { stride: 0.95, bounce: 1.1, sway: 1.2, arms: 1.1, elbow: 0.05, posture: 0.1, width: 1, toeOut: 0.14, heavy: 0.3, headStill: 0.4, fidget: 0.6 },
    { stride: 0.9, bounce: 0.8, sway: 0.8, arms: 0.6, elbow: 0.1, posture: -0.7, width: 1.05, toeOut: 0.16, heavy: 0.55, headStill: 0.3, fidget: 0.7 },
    { stride: 1.1, bounce: 1.25, sway: 1.35, arms: 1.35, elbow: 0.25, posture: 0.6, width: 1.2, toeOut: 0.22, heavy: 0.45, headStill: 0.4, fidget: 0.5 },
    { stride: 0.82, bounce: 0.7, sway: 0.7, arms: 0.55, elbow: 0.2, posture: -0.4, width: 0.95, toeOut: 0.08, heavy: 0.6, headStill: 0.5, fidget: 0.35 },
  ][type]!;
  const s: GaitStyle = {
    stride: n(base.stride, 0.05),
    bounce: n(base.bounce, 0.15),
    sway: n(base.sway, 0.15) * (female ? 1.35 : 1),
    arms: n(base.arms, 0.12),
    elbow: n(base.elbow, 0.08) + (female ? 0.12 : 0),
    posture: n(base.posture, 0.25),
    width: n(base.width, 0.06) * (female ? 0.8 : 1),
    toeOut: n(base.toeOut, 0.04) * (female ? 0.6 : 1),
    heavy: n(base.heavy, 0.1),
    headStill: n(base.headStill, 0.12),
    fidget: n(base.fidget, 0.15),
  };
  return s;
}
