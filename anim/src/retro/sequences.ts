/**
 * Retro animation: Doom-style state sequences baked from the smooth procedural animation.
 *
 * For each state (walk, run, fire, pain, ...) a fresh HumanoidAnimator is driven on flat ground
 * until its gait is steady, and a handful of frames are sampled (walk and run: four frames
 * across one gait cycle, Doom's A B C D) and re-voxelized into whole-body voxel models
 * (bake.ts). Playback steps through them in Doom tics (35 Hz), holding each frame for a few tics,
 * with the facing snapped to 8 directions: the retro look is a function of the modern
 * animation, not separate content.
 */
import { WorldPose } from '../core/skeleton.ts';
import type { Prop } from '../characters/props.ts';
import { HumanoidAnimator, type Mood } from '../humanoid/animator.ts';
import { H } from '../humanoid/rig.ts';
import type { Quat } from '../math/quat.ts';
import type { V3 } from '../math/vec.ts';
import { FlatGround } from '../physics/collision.ts';
import type { VoxelModel } from '../voxel/model.ts';
import { bakePose, type BakeProp } from './bake.ts';

export type RetroState = 'idle' | 'walk' | 'run' | 'crouch' | 'crouchWalk' | 'aim' | 'fire' | 'pain' | 'panic' | 'cower' | 'surrender';

export const RETRO_STATES: readonly RetroState[] = ['idle', 'walk', 'run', 'crouch', 'crouchWalk', 'aim', 'fire', 'pain', 'panic', 'cower', 'surrender'];

/** Doom's game tic rate. */
export const TICS_PER_SECOND = 35;

export interface RetroSequence {
  state: RetroState;
  frames: VoxelModel[];
  /** Tics per frame at 35 Hz (at the baked speed). */
  tics: number;
  loop: boolean;
  /** Ground speed the frames were baked at (m/s; 0 standing): scale playback by speed / this. */
  speed: number;
}

export interface RetroSet {
  sequences: Map<RetroState, RetroSequence>;
  voxelSize: number;
}

interface StateSpec {
  speed: number;
  crouch: number;
  mood: Mood;
  aim: boolean;
  /** Gait-cycle frames (sampled by phase), or times after settling (s). */
  cycleFrames?: number;
  times?: number[];
  /** Tics per frame of the time-sampled states. */
  tics?: number;
  loop: boolean;
  /** Action before sampling. */
  action?: 'fire' | 'hit';
}

const SPECS: Record<RetroState, StateSpec> = {
  idle: { speed: 0, crouch: 0, mood: 'normal', aim: false, times: [0, 0.6], tics: 18, loop: true },
  walk: { speed: 1.5, crouch: 0, mood: 'normal', aim: false, cycleFrames: 4, loop: true },
  run: { speed: 4.5, crouch: 0, mood: 'normal', aim: false, cycleFrames: 4, loop: true },
  crouch: { speed: 0, crouch: 1, mood: 'normal', aim: false, times: [0], tics: 10, loop: true },
  crouchWalk: { speed: 1.0, crouch: 1, mood: 'normal', aim: false, cycleFrames: 4, loop: true },
  aim: { speed: 0, crouch: 0, mood: 'normal', aim: true, times: [0], tics: 10, loop: true },
  fire: { speed: 0, crouch: 0, mood: 'normal', aim: true, times: [0.03, 0.2], tics: 3, loop: true, action: 'fire' },
  pain: { speed: 0, crouch: 0, mood: 'normal', aim: false, times: [0.09], tics: 6, loop: false, action: 'hit' },
  panic: { speed: 4.5, crouch: 0, mood: 'panic', aim: false, cycleFrames: 4, loop: true },
  cower: { speed: 0, crouch: 1, mood: 'cower', aim: false, times: [0], tics: 10, loop: true },
  surrender: { speed: 0, crouch: 0, mood: 'surrender', aim: false, times: [0, 0.5], tics: 14, loop: true },
};

const IDENTITY: Quat = [0, 0, 0, 1];
const FACING = Math.PI / 2; // +y: model space and world space coincide (up to the root position)

/**
 * Bakes the retro frames of a character model (a humanoid-rig model), with its held prop if
 * given. Deterministic.
 */
export function bakeRetroSet(model: VoxelModel, opts: { weapon?: Prop | null; voxelSize?: number; states?: readonly RetroState[] } = {}): RetroSet {
  const voxelSize = opts.voxelSize ?? model.voxelSize;
  const sequences = new Map<RetroState, RetroSequence>();
  const mw = new WorldPose(model.skeleton);
  const frameOf = (an: HumanoidAnimator, state: RetroState, index: number): VoxelModel => {
    // the local pose recomputed at an identity root: model space, root at the origin
    mw.compute(an.pose, [0, 0, 0], IDENTITY);
    const props: BakeProp[] = [];
    if (an.weapon) props.push({ model: an.weapon.model, pos: [...mw.p[H.weapon]!] as V3, rot: [...mw.q[H.weapon]!] as Quat });
    return bakePose(model, mw, { voxelSize, props, name: `${model.name}-${state}-${index}` });
  };

  for (const state of opts.states ?? RETRO_STATES) {
    const spec = SPECS[state];
    const an = new HumanoidAnimator(model.skeleton, new FlatGround(0), 1);
    an.weapon = opts.weapon ?? null;
    an.input.crouch = spec.crouch;
    an.input.mood = spec.mood;
    an.input.carry = spec.aim ? 'aim' : spec.speed > 0 ? 'ready' : 'relaxed';
    const root: V3 = [0, 0, 0];
    an.place(root, FACING);
    let t = 0;
    const step = (dt: number): void => {
      t += dt;
      root[1] += spec.speed * Math.min(1, t / 0.4) * dt;
      if (spec.aim) an.input.aimAt = [root[0], root[1] + 10, 1.4];
      an.setRoot(root, FACING);
      an.update(dt);
    };
    // settle: the gait, crouch and moods reach a steady state
    for (let i = 0; i < 60; i++) step(1 / 30);
    const frames: VoxelModel[] = [];
    let tics: number;
    if (spec.cycleFrames) {
      // frames at phases 0, 1/n, ...: first run up to phase 0, then sample each crossing
      const n = spec.cycleFrames;
      const dt = 1 / 240;
      let guard = 0;
      for (let f = 0; f < n; f++) {
        const target = f / n;
        for (;;) {
          const before = an.phase;
          step(dt);
          const after = an.phase;
          const crossed = after >= before ? before < target && target <= after : target > before || target <= after;
          if (crossed || ++guard > 4000) break;
        }
        frames.push(frameOf(an, state, f));
      }
      // hold each frame for its share of the gait cycle at the baked speed
      tics = Math.max(1, Math.round(TICS_PER_SECOND / (an.gait.freq * n)));
    } else {
      const times = spec.times!;
      if (spec.action === 'fire') an.fire();
      if (spec.action === 'hit') an.hit([0, -1, 0], 1, 0.7);
      let clock = 0;
      times.forEach((at, i) => {
        while (clock < at - 1e-9) {
          const dt = Math.min(1 / 120, at - clock);
          step(dt);
          clock += dt;
        }
        frames.push(frameOf(an, state, i));
      });
      tics = spec.tics!;
    }
    sequences.set(state, { state, frames, tics, loop: spec.loop, speed: spec.speed });
  }
  return { sequences, voxelSize };
}

/** Where a missing state falls back to. */
const FALLBACK: Record<RetroState, RetroState> = {
  idle: 'idle',
  walk: 'idle',
  run: 'walk',
  crouch: 'idle',
  crouchWalk: 'crouch',
  aim: 'idle',
  fire: 'aim',
  pain: 'idle',
  panic: 'run',
  cower: 'crouch',
  surrender: 'idle',
};

/** Frame selection for playback: stepped time in Doom tics, a clock per state. */
export class RetroPlayer {
  readonly set: RetroSet;
  state: RetroState;
  frameIndex = 0;
  /** Tics into the current frame (fractional). */
  private clock = 0;

  constructor(set: RetroSet, initial: RetroState = 'idle') {
    this.set = set;
    this.state = initial;
  }

  /** The sequence played for a state (following fallbacks when it was not baked). */
  sequence(state: RetroState): RetroSequence {
    let s: RetroState = state;
    for (let i = 0; i < 4; i++) {
      const seq = this.set.sequences.get(s);
      if (seq && seq.frames.length > 0) return seq;
      s = FALLBACK[s];
    }
    const any = this.set.sequences.values().next();
    if (any.done) throw new Error('empty retro set');
    return any.value;
  }

  /**
   * Advances by dt seconds in `state` (switching state restarts at frame 0, like Doom's
   * SetState). `rate` scales the tic clock (e.g. actual speed / baked speed for walk cycles).
   */
  update(dt: number, state: RetroState, rate = 1): VoxelModel {
    const seq = this.sequence(state);
    if (state !== this.state) {
      this.state = state;
      this.frameIndex = 0;
      this.clock = 0;
    }
    if (this.frameIndex >= seq.frames.length) this.frameIndex = 0;
    this.clock += dt * TICS_PER_SECOND * Math.max(0, rate);
    while (this.clock >= seq.tics) {
      this.clock -= seq.tics;
      if (this.frameIndex + 1 < seq.frames.length) this.frameIndex++;
      else if (seq.loop) this.frameIndex = 0;
      else {
        this.clock = 0;
        break;
      }
    }
    return seq.frames[this.frameIndex]!;
  }

  /** The current frame. */
  get frame(): VoxelModel {
    const seq = this.sequence(this.state);
    return seq.frames[Math.min(this.frameIndex, seq.frames.length - 1)]!;
  }
}

/** Snaps a facing yaw to 8 directions (Doom sprite rotations). */
export function snapYaw8(yaw: number): number {
  const q = Math.PI / 4;
  return Math.round(yaw / q) * q;
}

/** The retro state that shows what a character is doing. */
export function retroStateOf(o: { speed: number; crouch: number; aiming: boolean; firing: boolean; mood: Mood; pain: boolean }): RetroState {
  if (o.pain) return 'pain';
  if (o.mood === 'cower') return 'cower';
  if (o.mood === 'surrender') return 'surrender';
  if (o.mood === 'panic' && o.speed > 0.3) return 'panic';
  if (o.firing) return 'fire';
  if (o.crouch > 0.5) return o.speed > 0.3 ? 'crouchWalk' : 'crouch';
  if (o.speed > 2.6) return 'run';
  if (o.speed > 0.3) return 'walk';
  return o.aiming ? 'aim' : 'idle';
}
