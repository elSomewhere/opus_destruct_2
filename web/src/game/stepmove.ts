/**
 * Player moves with Doom-style step-up, composed only of standard `collide` sweeps
 * (docs/API.md), so any conforming engine supports it:
 *
 *   1. sweep the requested move;
 *   2. if a grounded box lost horizontal progress, also try: up by `stepHeight`, across by
 *      the horizontal move, down again; keep that result if it got further.
 *
 * The extra sweeps only happen when a grounded move is blocked (walls, ledges). The logic is
 * one generator driven either by worker round trips (`moveWithStep`) or by a local collider
 * (`moveWithStepSync`, see occupancy.ts).
 */
import type { Vec3 } from '../engine/protocol.ts';

export interface SweepResult {
  move: Vec3;
  onGround: boolean;
}

export type CollideFn = (min: Vec3, max: Vec3, move: Vec3) => Promise<SweepResult>;
export type CollideSyncFn = (min: Vec3, max: Vec3, move: Vec3) => SweepResult;

export interface StepMoveResult extends SweepResult {
  /** Height climbed by the step-up (0 when the plain move was used). */
  stepped: number;
}

const EPS = 1e-4;

function offset(v: Vec3, d: Vec3): Vec3 {
  return [v[0] + d[0], v[1] + d[1], v[2] + d[2]];
}

type Sweep = [Vec3, Vec3, Vec3];

function* stepMove(
  min: Vec3,
  max: Vec3,
  move: Vec3,
  stepHeight: number,
  grounded: boolean,
): Generator<Sweep, StepMoveResult, SweepResult> {
  const plain = yield [min, max, move];
  const wanted = Math.hypot(move[0], move[1]);
  const got = Math.hypot(plain.move[0], plain.move[1]);
  const landed = move[2] < 0 && plain.move[2] > move[2] + EPS;
  if (stepHeight <= 0 || wanted < EPS || got >= wanted - EPS || !(grounded || landed)) {
    return { ...plain, stepped: 0 };
  }

  const up = yield [min, max, [0, 0, stepHeight]];
  const rise = up.move[2];
  if (rise < EPS) return { ...plain, stepped: 0 };
  const across = yield [offset(min, up.move), offset(max, up.move), [move[0], move[1], 0]];
  const gotStepped = Math.hypot(across.move[0], across.move[1]);
  if (gotStepped <= got + EPS) return { ...plain, stepped: 0 };
  const raised = offset(up.move, across.move);
  const down = yield [offset(min, raised), offset(max, raised), [0, 0, -rise + Math.min(0, move[2])]];
  const total = offset(raised, down.move);
  return { move: total, onGround: down.onGround, stepped: Math.max(0, total[2] - plain.move[2]) };
}

export async function moveWithStep(
  collide: CollideFn,
  min: Vec3,
  max: Vec3,
  move: Vec3,
  stepHeight: number,
  grounded: boolean,
): Promise<StepMoveResult> {
  const g = stepMove(min, max, move, stepHeight, grounded);
  let r = g.next();
  while (!r.done) r = g.next(await collide(...r.value));
  return r.value;
}

export function moveWithStepSync(
  collide: CollideSyncFn,
  min: Vec3,
  max: Vec3,
  move: Vec3,
  stepHeight: number,
  grounded: boolean,
): StepMoveResult {
  const g = stepMove(min, max, move, stepHeight, grounded);
  let r = g.next();
  while (!r.done) r = g.next(collide(...r.value));
  return r.value;
}
