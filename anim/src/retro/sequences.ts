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
import type { Prop, PropKind } from '../characters/props.ts';
import { HumanoidAnimator, type Carry, type Mood } from '../humanoid/animator.ts';
import { H } from '../humanoid/rig.ts';
import type { GroundVariant, Stance } from '../humanoid/stances.ts';
import { qconj, qmul, qrotate, type Quat } from '../math/quat.ts';
import { vsub, type V3 } from '../math/vec.ts';
import { FlatGround } from '../physics/collision.ts';
import type { VoxelModel } from '../voxel/model.ts';
import { bakePose, type BakeProp } from './bake.ts';

export type RetroState =
  // locomotion, weapons, moods
  | 'idle' | 'walk' | 'run' | 'crouch' | 'crouchWalk' | 'aim' | 'fire' | 'pain' | 'panic' | 'cower' | 'surrender'
  // conversation
  | 'talk' | 'listen'
  // stances
  | 'sit' | 'sitDesk' | 'ground' | 'kneel' | 'kneelFire' | 'prone' | 'proneFire' | 'crawl' | 'peek'
  // hand to hand
  | 'guard' | 'punch' | 'kick' | 'stab' | 'block'
  // weapons
  | 'reload' | 'hipFire' | 'pistolAim' | 'pistolFire'
  // knocked down
  | 'down' | 'getUp';

/** Every retro state, in baking order. */
export const RETRO_STATES: readonly RetroState[] = [
  'idle', 'walk', 'run', 'crouch', 'crouchWalk', 'aim', 'fire', 'pain', 'panic', 'cower', 'surrender',
  'talk', 'listen', 'sit', 'sitDesk', 'ground', 'kneel', 'kneelFire', 'prone', 'proneFire', 'crawl', 'peek',
  'guard', 'punch', 'kick', 'stab', 'block', 'reload', 'hipFire', 'pistolAim', 'pistolFire', 'down', 'getUp',
];

/** The states of the first retro sets (locomotion, weapons, moods). */
export const BASIC_RETRO_STATES: readonly RetroState[] = ['idle', 'walk', 'run', 'crouch', 'crouchWalk', 'aim', 'fire', 'pain', 'panic', 'cower', 'surrender'];

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
  /** What the frames hold (null: empty hands). */
  weapon: PropKind | null;
}

/** What a state needs in the hands to be baked (states that do not fit the prop are skipped). */
type Needs = 'longGun' | 'gun' | 'pistol' | 'knife' | 'notLongGun';

interface StateSpec {
  speed: number;
  crouch: number;
  mood: Mood;
  aim: boolean;
  /** Weapon carry (default: aim when aiming, ready when moving, else relaxed). */
  carry?: Carry;
  stance?: Stance;
  /** Sitting: on a bench, or at a desk. */
  seat?: 'bench' | 'desk';
  groundVariant?: GroundVariant;
  talk?: 'speak' | 'listen';
  guard?: boolean;
  lean?: number;
  /** Aim target height (m) when aiming (default 1.4). */
  aimZ?: number;
  /** Gait-cycle frames (sampled by phase), a time-periodic cycle, or times after settling (s). */
  cycleFrames?: number;
  period?: { seconds: number; frames: number };
  times?: number[];
  /** Tics per frame of the time-sampled states. */
  tics?: number;
  loop: boolean;
  /** Something that happens right before sampling. */
  action?: 'fire' | 'hit' | 'down' | 'getUp';
  /** A one-shot action played before sampling (by the prop held), aimed at the head or the gut. */
  play?: { name: (kind: PropKind | null) => string; at: 'head' | 'gut' | null };
  needs?: Needs;
}

const LONG_GUNS: readonly PropKind[] = ['rifle', 'smg', 'lmg'];

function fits(needs: Needs | undefined, kind: PropKind | null): boolean {
  switch (needs) {
    case undefined:
      return true;
    case 'longGun':
      return kind !== null && LONG_GUNS.includes(kind);
    case 'gun':
      return kind !== null && kind !== 'knife';
    case 'pistol':
      return kind === 'pistol';
    case 'knife':
      return kind === 'knife';
    case 'notLongGun':
      return kind === null || !LONG_GUNS.includes(kind);
  }
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
  // conversation: gestures come by themselves while speaking; listeners nod
  talk: { speed: 0, crouch: 0, mood: 'normal', aim: false, talk: 'speak', times: [0, 0.4, 0.8, 1.2], tics: 14, loop: true },
  listen: { speed: 0, crouch: 0, mood: 'normal', aim: false, talk: 'listen', times: [0, 0.7], tics: 24, loop: true },
  // stances (armed characters aim from the kneel and prone)
  sit: { speed: 0, crouch: 0, mood: 'normal', aim: false, stance: 'sit', seat: 'bench', times: [0], tics: 20, loop: true },
  sitDesk: { speed: 0, crouch: 0, mood: 'normal', aim: false, stance: 'sit', seat: 'desk', times: [0, 0.2, 0.45], tics: 7, loop: true },
  ground: { speed: 0, crouch: 0, mood: 'normal', aim: false, stance: 'ground', groundVariant: 'kneesUp', times: [0], tics: 20, loop: true },
  kneel: { speed: 0, crouch: 0, mood: 'normal', aim: true, stance: 'kneel', times: [0], tics: 10, loop: true },
  kneelFire: { speed: 0, crouch: 0, mood: 'normal', aim: true, stance: 'kneel', times: [0.03, 0.2], tics: 3, loop: true, action: 'fire', needs: 'longGun' },
  prone: { speed: 0, crouch: 0, mood: 'normal', aim: true, aimZ: 0.5, stance: 'prone', times: [0], tics: 10, loop: true },
  proneFire: { speed: 0, crouch: 0, mood: 'normal', aim: true, aimZ: 0.5, stance: 'prone', times: [0.03, 0.2], tics: 3, loop: true, action: 'fire', needs: 'longGun' },
  // crawling: the crawl cycle runs at min(1.2, speed / 0.35) cycles per second
  crawl: { speed: 0.4, crouch: 0, mood: 'normal', aim: false, carry: 'ready', stance: 'prone', period: { seconds: 0.35 / 0.4, frames: 4 }, loop: true },
  peek: { speed: 0, crouch: 0.2, mood: 'normal', aim: true, lean: -1, times: [0], tics: 10, loop: true },
  // hand to hand (fists up)
  guard: { speed: 0, crouch: 0, mood: 'normal', aim: false, guard: true, times: [0, 0.3], tics: 10, loop: true, needs: 'notLongGun' },
  punch: {
    speed: 0,
    crouch: 0,
    mood: 'normal',
    aim: false,
    guard: true,
    play: { name: (k) => (k !== null && LONG_GUNS.includes(k) ? 'riflePush' : 'jab'), at: 'head' },
    times: [0.06, 0.12, 0.24],
    tics: 4,
    loop: false,
  },
  kick: { speed: 0, crouch: 0, mood: 'normal', aim: false, guard: true, play: { name: () => 'frontKick', at: 'gut' }, times: [0.2, 0.33, 0.55], tics: 6, loop: false },
  stab: { speed: 0, crouch: 0, mood: 'normal', aim: false, guard: true, play: { name: () => 'stab', at: 'gut' }, times: [0.14, 0.28, 0.45], tics: 5, loop: false, needs: 'knife' },
  block: { speed: 0, crouch: 0, mood: 'normal', aim: false, guard: true, play: { name: () => 'block', at: null }, times: [0.2], tics: 10, loop: false, needs: 'notLongGun' },
  // weapons
  reload: {
    speed: 0,
    crouch: 0,
    mood: 'normal',
    aim: true,
    play: { name: (k) => (k === 'pistol' ? 'reloadPistol' : 'reloadRifle'), at: null },
    times: [0.35, 0.8, 1.3, 1.85],
    tics: 16,
    loop: false,
    needs: 'gun',
  },
  hipFire: { speed: 0, crouch: 0, mood: 'normal', aim: true, carry: 'hip', times: [0.03, 0.12], tics: 3, loop: true, action: 'fire', needs: 'longGun' },
  pistolAim: { speed: 0, crouch: 0, mood: 'normal', aim: true, times: [0], tics: 10, loop: true, needs: 'pistol' },
  pistolFire: { speed: 0, crouch: 0, mood: 'normal', aim: true, times: [0.03, 0.18], tics: 4, loop: true, action: 'fire', needs: 'pistol' },
  // knocked down: the fall and lying (held), then getting up
  down: { speed: 0, crouch: 0, mood: 'normal', aim: false, times: [0.15, 0.35, 0.75], tics: 5, loop: false, action: 'down' },
  getUp: { speed: 0, crouch: 0, mood: 'normal', aim: false, times: [0, 0.35, 0.75, 1.05], tics: 12, loop: false, action: 'getUp' },
};

const IDENTITY: Quat = [0, 0, 0, 1];
const FACING = Math.PI / 2; // +y: model space and world space coincide (up to the root position)

/** Whether a state is baked for a character holding a prop of this kind. */
export function retroStateFits(state: RetroState, kind: PropKind | null): boolean {
  return fits(SPECS[state].needs, kind);
}

/**
 * Bakes the retro frames of a character model (a humanoid-rig model), with its held prop if
 * given. `states` defaults to every state that fits the prop (RETRO_STATES); states that do not
 * fit the prop (a pistol stance without a pistol) are skipped even when asked for. The player
 * falls back to a baked neighbour for a missing state. Deterministic.
 */
export function bakeRetroSet(model: VoxelModel, opts: { weapon?: Prop | null; voxelSize?: number; states?: readonly RetroState[] } = {}): RetroSet {
  const voxelSize = opts.voxelSize ?? model.voxelSize;
  const sequences = new Map<RetroState, RetroSequence>();
  const mw = new WorldPose(model.skeleton);
  const weapon = opts.weapon ?? null;
  const kind = weapon?.kind ?? null;
  const frameOf = (an: HumanoidAnimator, state: RetroState, index: number): VoxelModel => {
    // the local pose recomputed at an identity root: model space, root at the origin
    mw.compute(an.pose, [0, 0, 0], IDENTITY);
    const props: BakeProp[] = [];
    if (an.weapon) {
      // the prop's world transform (socket or hand) brought into model space
      const inv = qconj(an.world.q[H.root]!);
      props.push({ model: an.weapon.model, pos: qrotate(inv, vsub(an.weaponPos, an.world.p[H.root]!)), rot: qmul(inv, an.weaponRot) });
    }
    return bakePose(model, mw, { voxelSize, props, name: `${model.name}-${state}-${index}` });
  };

  for (const state of opts.states ?? RETRO_STATES) {
    const spec = SPECS[state];
    if (!fits(spec.needs, kind)) continue;
    const an = new HumanoidAnimator(model.skeleton, new FlatGround(0), 1);
    an.weapon = weapon;
    const inp = an.input;
    inp.idle = false; // clean poses: no idle postures or fidgets of their own
    inp.crouch = spec.crouch;
    inp.mood = spec.mood;
    inp.carry = spec.carry ?? (spec.aim ? 'aim' : spec.speed > 0 ? 'ready' : 'relaxed');
    inp.stance = spec.stance ?? 'stand';
    if (spec.groundVariant) inp.groundVariant = spec.groundVariant;
    inp.talk = spec.talk ?? null;
    inp.guard = spec.guard ?? false;
    inp.lean = spec.lean ?? 0;
    const root: V3 = [0, 0, 0];
    // a seat 0.38 m behind the root (the feet stay where the character stood)
    if (spec.seat) inp.seat = { pos: [0, -0.38, 0.46], backrest: true, ...(spec.seat === 'desk' ? { deskHeight: 0.745, variant: 'desk' as const } : { variant: 'upright' as const }) };
    an.place(root, FACING);
    let t = 0;
    const step = (dt: number): void => {
      t += dt;
      root[1] += spec.speed * Math.min(1, t / 0.4) * dt;
      if (spec.aim) inp.aimAt = [root[0], root[1] + 10, spec.aimZ ?? 1.4];
      an.setRoot(root, FACING);
      an.update(dt);
    };
    // settle: the gait, crouch, moods and stance transitions reach a steady state
    for (let i = 0; i < 60 || (an.transitioning && i < 150); i++) step(1 / 30);
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
    } else if (spec.period) {
      // a cycle that is not the gait's (crawling): evenly in time over one period
      const { seconds, frames: n } = spec.period;
      for (let f = 0; f < n; f++) {
        if (f > 0) for (let clock = 0; clock < seconds / n - 1e-9; clock += 1 / 120) step(Math.min(1 / 120, seconds / n - clock));
        frames.push(frameOf(an, state, f));
      }
      tics = Math.max(1, Math.round((TICS_PER_SECOND * seconds) / n));
    } else {
      const times = spec.times!;
      if (spec.action === 'fire') an.fire();
      if (spec.action === 'hit') an.hit([0, -1, 0], 1, 0.7);
      if (spec.action === 'down' || spec.action === 'getUp') an.knockDown(true);
      if (spec.action === 'getUp') {
        // lie until the get-up has sat the body up (the stance reaches 'ground'), then sample
        for (let i = 0; i < 720 && an.stance !== 'ground'; i++) step(1 / 120);
      }
      if (spec.play) {
        const at = spec.play.at;
        const target: V3 | null = at === 'head' ? [root[0], root[1] + 0.95, 1.55] : at === 'gut' ? [root[0], root[1] + 1.0, 1.05] : null;
        an.play(spec.play.name(kind), target);
      }
      let clock = 0;
      times.forEach((when, i) => {
        while (clock < when - 1e-9) {
          const dt = Math.min(1 / 120, when - clock);
          step(dt);
          clock += dt;
        }
        frames.push(frameOf(an, state, i));
      });
      tics = spec.tics!;
    }
    sequences.set(state, { state, frames, tics, loop: spec.loop, speed: spec.speed });
  }
  return { sequences, voxelSize, weapon: opts.weapon?.kind ?? null };
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
  talk: 'idle',
  listen: 'idle',
  sit: 'idle',
  sitDesk: 'sit',
  ground: 'crouch',
  kneel: 'crouch',
  kneelFire: 'kneel',
  prone: 'idle',
  proneFire: 'prone',
  crawl: 'prone',
  peek: 'aim',
  guard: 'idle',
  punch: 'guard',
  kick: 'punch',
  stab: 'punch',
  block: 'guard',
  reload: 'aim',
  hipFire: 'fire',
  pistolAim: 'aim',
  pistolFire: 'pistolAim',
  down: 'pain',
  getUp: 'crouch',
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

/** What a character is doing, for choosing its retro state (the new facts are optional). */
export interface RetroFacts {
  /** Ground speed (m/s). */
  speed: number;
  crouch: number;
  aiming: boolean;
  firing: boolean;
  mood: Mood;
  pain: boolean;
  /** The animator's settled stance (HumanoidAnimator.stance). */
  stance?: Stance;
  talk?: 'speak' | 'listen' | null;
  guard?: boolean;
  /** The running one-shot action (HumanoidAnimator.actionName; a '.m' mirror suffix is ignored). */
  action?: string | null;
  /** The kind of the held prop. */
  weapon?: PropKind | null;
  carry?: Carry;
  /** Peeking (HumanoidInput.lean). */
  lean?: number;
  /** HumanoidAnimator.knockedDown / .transitioning. */
  knockedDown?: boolean;
  transitioning?: boolean;
  /** Sitting at a desk. */
  desk?: boolean;
}

const ACTION_STATE: Record<string, RetroState> = {
  jab: 'punch',
  cross: 'punch',
  hook: 'punch',
  uppercut: 'punch',
  riflePush: 'punch',
  frontKick: 'kick',
  roundhouse: 'kick',
  stab: 'stab',
  slash: 'stab',
  block: 'block',
  reloadRifle: 'reload',
  reloadPistol: 'reload',
};

/**
 * The retro state that shows what a character is doing. Priority: knocked down (falling and
 * lying: 'down'; sitting up and rising: 'getUp') > pain > a one-shot action (punch, kick, stab,
 * block, reload) > stance (prone, crawl, kneel, sit, ground) > peeking > talking > guard > mood >
 * firing and aiming (pistol, hip fire) > locomotion.
 */
export function retroStateOf(o: RetroFacts): RetroState {
  const pistol = o.weapon === 'pistol';
  if (o.knockedDown) return o.stance === 'ground' || o.stance === 'kneel' ? 'getUp' : 'down';
  if (o.stance === 'down') return 'down';
  if (o.pain) return 'pain';
  const act = o.action ? ACTION_STATE[o.action.replace(/\.m$/, '')] : undefined;
  if (act) return act;
  switch (o.stance) {
    case 'prone':
      return o.speed > 0.1 ? 'crawl' : o.firing ? 'proneFire' : 'prone';
    case 'kneel':
      return o.firing ? 'kneelFire' : 'kneel';
    case 'sit':
      return o.desk ? 'sitDesk' : 'sit';
    case 'ground':
      return 'ground';
    default:
      break;
  }
  if (o.lean !== undefined && Math.abs(o.lean) > 0.3) return 'peek';
  if (o.talk === 'speak') return 'talk';
  if (o.talk === 'listen') return 'listen';
  if (o.guard) return 'guard';
  if (o.mood === 'cower') return 'cower';
  if (o.mood === 'surrender') return 'surrender';
  if (o.mood === 'panic' && o.speed > 0.3) return 'panic';
  if (o.firing) return pistol ? 'pistolFire' : o.carry === 'hip' ? 'hipFire' : 'fire';
  if (o.crouch > 0.5) return o.speed > 0.3 ? 'crouchWalk' : 'crouch';
  if (o.speed > 2.6) return 'run';
  if (o.speed > 0.3) return 'walk';
  return o.aiming ? (pistol ? 'pistolAim' : 'aim') : 'idle';
}
