/**
 * Actions: authored motions layered on the procedural body (strikes, blocks, reloads,
 * gestures, idle poses and fidgets).
 *
 * An action is a set of keyframed channels (curve.ts) over its duration. Channels do not store
 * joint angles of the whole body: they steer the procedural rig. Hand and foot IK targets,
 * the reach towards a target (so a punch lands where the opponent's jaw is), trunk and head
 * offsets, the pelvis, the held weapon. So actions blend with walking, aiming, reactions and
 * each other, work at any body size, and mirror left/right.
 *
 * Frames: hand targets are in the CHEST frame (origin at the chest joint, axes rotating with
 * the chest; x right, y forward, z up, metres at height scale 1). Foot targets are in model
 * space (the character's root frame). Euler angles are degrees in joint frames (x: + tips an
 * upright bone back / swings a hanging limb forward; y: roll, + to the left side down for an
 * upright bone; z: yaw, + turns left). Hand orientations are relative to the canonical fist:
 * knuckles forward (+y), palm down (-z).
 */
import { Track, type Key } from '../core/curve.ts';
import type { V3 } from '../math/vec.ts';

export type Channel =
  | 'handR' | 'handL'
  | 'handRw' | 'handLw'
  | 'handRrot' | 'handLrot'
  | 'elbowR' | 'elbowL'
  | 'strikeR' | 'strikeL'
  | 'footR' | 'footL'
  | 'footRw' | 'footLw'
  | 'strikeFootR' | 'strikeFootL'
  | 'spine' | 'chest' | 'neck' | 'head'
  | 'clavR' | 'clavL'
  | 'pelvis' | 'pelvisRot'
  | 'crouch'
  | 'weapon' | 'weaponPos' | 'weaponRot'
  | 'look';

export type Limb = 'handR' | 'handL' | 'footR' | 'footL' | 'blade' | 'muzzle';

export interface ActionEvent {
  t: number;
  name: string;
  limb?: Limb;
}

export interface ActionDef {
  name: string;
  duration: number;
  loop?: boolean;
  /** Blend in / out (s). */
  fadeIn?: number;
  fadeOut?: number;
  ch: Partial<Record<Channel, Key[]>>;
  events?: ActionEvent[];
  /** 'pose': held postures (idles, guard, talk); 'act': one-shots on top. */
  layer?: 'pose' | 'act';
  /** Aimed at a target point (strikes). */
  targeted?: boolean;
  /**
   * How far (m, horizontally from the root, at body scale 1) the strike lands without moving
   * in: a target further away makes the body step into it (the pelvis drives forward with the
   * strike; punches also lean the trunk in).
   */
  reach?: number;
  /** Kicks: the foot's pitch (rad) at the strike (-0.6 pointed, >0 toes up: a push kick). */
  kickPitch?: number;
  /** Needs a prop in the right hand ('knife'). */
  prop?: 'knife';
}

const MIRROR_NAME: Partial<Record<Channel, Channel>> = {
  handR: 'handL', handL: 'handR', handRw: 'handLw', handLw: 'handRw', handRrot: 'handLrot', handLrot: 'handRrot',
  elbowR: 'elbowL', elbowL: 'elbowR', strikeR: 'strikeL', strikeL: 'strikeR', footR: 'footL', footL: 'footR',
  footRw: 'footLw', footLw: 'footRw', strikeFootR: 'strikeFootL', strikeFootL: 'strikeFootR', clavR: 'clavL', clavL: 'clavR',
};
const POSITIONS = new Set<Channel>(['handR', 'handL', 'elbowR', 'elbowL', 'footR', 'footL', 'pelvis', 'weaponPos']);
const EULERS = new Set<Channel>(['handRrot', 'handLrot', 'spine', 'chest', 'neck', 'head', 'clavR', 'clavL', 'pelvisRot', 'weaponRot']);
const MIRROR_LIMB: Record<Limb, Limb> = { handR: 'handL', handL: 'handR', footR: 'footL', footL: 'footR', blade: 'blade', muzzle: 'muzzle' };

/** The left/right mirror image of an action (positions x -> -x, rotations y, z -> -y, -z). */
export function mirrorAction(def: ActionDef, name = `${def.name}.m`): ActionDef {
  const ch: Partial<Record<Channel, Key[]>> = {};
  for (const [k, keys] of Object.entries(def.ch) as [Channel, Key[]][]) {
    const to = MIRROR_NAME[k] ?? k;
    ch[to] = keys.map((key) => {
      if (typeof key.v === 'number') return key;
      const v = [...key.v];
      if (POSITIONS.has(k)) v[0] = -v[0]!;
      else if (EULERS.has(k)) {
        v[1] = -v[1]!;
        v[2] = -v[2]!;
      }
      return { ...key, v };
    });
  }
  const out: ActionDef = { ...def, name, ch };
  if (def.events) out.events = def.events.map((e) => (e.limb ? { ...e, limb: MIRROR_LIMB[e.limb] } : e));
  return out;
}

/** Sampled channel values of one frame (absent: the channel is not driven). */
export type ChannelFrame = Partial<Record<Channel, number[]>>;

/** A running action: time, fade, target and its tracks. */
export class ActionPlayer {
  readonly def: ActionDef;
  readonly tracks: [Channel, Track][];
  time = 0;
  rate = 1;
  /** World target point (strikes). */
  target: V3 | null;
  /** Stopping: fading out from `stopAt`. */
  private stopAt = -1;
  private firedTo = -1;

  constructor(def: ActionDef, target: V3 | null = null, rate = 1) {
    this.def = def;
    this.target = target;
    this.rate = rate;
    this.tracks = (Object.entries(def.ch) as [Channel, Key[]][]).map(([c, keys]) => [c, new Track(keys)]);
  }

  get done(): boolean {
    if (this.stopAt >= 0) return this.time - this.stopAt >= (this.def.fadeOut ?? 0.15);
    return !this.def.loop && this.time >= this.def.duration;
  }

  /** Blend weight now (fade in, fade out at the end or after stop()). */
  get weight(): number {
    const fi = this.def.fadeIn ?? 0.12;
    const fo = this.def.fadeOut ?? 0.15;
    let w = fi > 0 ? Math.min(1, this.time / fi) : 1;
    if (this.stopAt >= 0) w *= Math.max(0, 1 - (this.time - this.stopAt) / fo);
    else if (!this.def.loop && fo > 0) w *= Math.min(1, Math.max(0, (this.def.duration - this.time) / fo));
    return w * w * (3 - 2 * w);
  }

  stop(): void {
    if (this.stopAt < 0) this.stopAt = this.time;
  }

  /** Advances; returns the events crossed. */
  advance(dt: number): ActionEvent[] {
    const before = this.time;
    this.time += dt * this.rate;
    const out: ActionEvent[] = [];
    const evs = this.def.events;
    if (!evs) return out;
    const d = this.def.duration;
    const local = (t: number): number => (this.def.loop ? t % d : t);
    for (let i = 0; i < evs.length; i++) {
      const e = evs[i]!;
      // one-shots: each event once; loops: each cycle
      const a = local(before), b = local(this.time);
      const crossed = b >= a ? e.t > a && e.t <= b : e.t > a || e.t <= b;
      if (crossed && (this.def.loop || i > this.firedTo)) {
        out.push(e);
        if (!this.def.loop) this.firedTo = i;
      }
    }
    return out;
  }

  /** Samples every channel into `out` (weights of hand/foot channels default to 1). */
  sample(out: ChannelFrame): ChannelFrame {
    const t = this.def.loop ? this.time % this.def.duration : Math.min(this.time, this.def.duration);
    for (const [c, tr] of this.tracks) out[c] = tr.sample(t, out[c] ?? new Array<number>(tr.dim).fill(0));
    return out;
  }
}

// ---- the library -------------------------------------------------------------------------

type P3 = [number, number, number];
const k = (t: number, v: number | P3, e?: Key['e']): Key => (e ? { t, v, e } : { t, v });

/** Fists up by the chin, elbows in (the fighting guard, as held by 'guard'). */
export const GUARD_R: P3 = [0.13, 0.22, 0.3];
export const GUARD_L: P3 = [-0.11, 0.26, 0.33];
const FIST_UP_R: P3 = [65, 80, 10];
const FIST_UP_L: P3 = [65, -80, -10];
const POLE_R: P3 = [0.45, -0.1, -1];
const POLE_L: P3 = [-0.45, -0.1, -1];

const guardHands = (t0: number, t1: number): Partial<Record<Channel, Key[]>> => ({
  handR: [k(t0, GUARD_R), k(t1, GUARD_R)],
  handL: [k(t0, GUARD_L), k(t1, GUARD_L)],
  handRrot: [k(t0, FIST_UP_R), k(t1, FIST_UP_R)],
  handLrot: [k(t0, FIST_UP_L), k(t1, FIST_UP_L)],
  elbowR: [k(t0, POLE_R), k(t1, POLE_R)],
  elbowL: [k(t0, POLE_L), k(t1, POLE_L)],
});

const A: ActionDef[] = [
  // ---- fighting ----
  {
    name: 'guard',
    layer: 'pose',
    loop: true,
    duration: 1.2,
    fadeIn: 0.2,
    fadeOut: 0.25,
    ch: {
      ...guardHands(0, 1.2),
      // light bounce on the balls of the feet, chin down, bladed
      crouch: [k(0, 0.18), k(0.3, 0.24), k(0.6, 0.18), k(0.9, 0.24), k(1.2, 0.18)],
      pelvisRot: [k(0, [0, 0, -22]), k(1.2, [0, 0, -22])],
      chest: [k(0, [-6, 0, -8]), k(1.2, [-6, 0, -8])],
      head: [k(0, [-10, 0, 20]), k(1.2, [-10, 0, 20])],
    },
  },
  {
    // lead-hand jab: fast out, fast back
    name: 'jab',
    targeted: true,
    reach: 0.7,
    duration: 0.42,
    fadeIn: 0.04,
    fadeOut: 0.12,
    ch: {
      ...guardHands(0, 0.42),
      strikeL: [k(0, 0), k(0.11, 1, 'snap'), k(0.16, 1), k(0.38, 0, 'out')],
      handLrot: [k(0, FIST_UP_L), k(0.1, [0, 0, 0], 'snap'), k(0.17, [0, 0, 0]), k(0.38, FIST_UP_L, 'out')],
      elbowL: [k(0, POLE_L), k(0.1, [-0.8, -0.2, -0.6]), k(0.38, POLE_L)],
      chest: [k(0, [0, 0, 0]), k(0.1, [-4, 0, -16], 'snap'), k(0.4, [0, 0, 0], 'out')],
      pelvis: [k(0, [0, 0, 0]), k(0.11, [0, 0.06, -0.01], 'snap'), k(0.4, [0, 0, 0], 'out')],
      head: [k(0, [0, 0, 0]), k(0.1, [-4, 0, 8]), k(0.4, [0, 0, 0])],
    },
    events: [{ t: 0.11, name: 'strike', limb: 'handL' }],
  },
  {
    // rear-hand cross: the hip turns it over
    name: 'cross',
    targeted: true,
    reach: 0.68,
    duration: 0.55,
    fadeIn: 0.05,
    fadeOut: 0.15,
    ch: {
      ...guardHands(0, 0.55),
      strikeR: [k(0, 0), k(0.06, -0.1), k(0.17, 1, 'snap'), k(0.22, 1), k(0.5, 0, 'out')],
      handRrot: [k(0, FIST_UP_R), k(0.16, [0, 0, 0], 'snap'), k(0.23, [0, 0, 0]), k(0.5, FIST_UP_R, 'out')],
      elbowR: [k(0, POLE_R), k(0.16, [0.8, -0.2, -0.5]), k(0.5, POLE_R)],
      pelvisRot: [k(0, [0, 0, 0]), k(0.06, [0, 0, -6]), k(0.17, [0, 0, 24], 'snap'), k(0.52, [0, 0, 0], 'out')],
      chest: [k(0, [0, 0, 0]), k(0.06, [0, 0, -6]), k(0.17, [-8, 0, 28], 'snap'), k(0.52, [0, 0, 0], 'out')],
      pelvis: [k(0, [0, 0, 0]), k(0.17, [0.02, 0.1, -0.03], 'snap'), k(0.52, [0, 0, 0], 'out')],
      head: [k(0, [0, 0, 0]), k(0.17, [-6, 0, -18]), k(0.52, [0, 0, 0])],
    },
    events: [{ t: 0.17, name: 'strike', limb: 'handR' }],
  },
  {
    // lead hook: elbow up, fist sweeps round from the side
    name: 'hook',
    targeted: true,
    reach: 0.58,
    duration: 0.6,
    fadeIn: 0.05,
    fadeOut: 0.15,
    ch: {
      ...guardHands(0, 0.6),
      handL: [k(0, GUARD_L), k(0.1, [-0.4, 0.18, 0.3]), k(0.2, [-0.25, 0.42, 0.34]), k(0.55, GUARD_L, 'out')],
      strikeL: [k(0, 0), k(0.1, 0.1), k(0.21, 0.95, 'snap'), k(0.27, 0.9), k(0.55, 0, 'out')],
      handLrot: [k(0, FIST_UP_L), k(0.12, [0, -90, -40]), k(0.21, [0, -90, -70]), k(0.55, FIST_UP_L, 'out')],
      elbowL: [k(0, POLE_L), k(0.12, [-1, -0.1, 0.4]), k(0.21, [-0.6, 0.4, 0.6]), k(0.55, POLE_L)],
      pelvisRot: [k(0, [0, 0, 0]), k(0.1, [0, 0, 10]), k(0.21, [0, 0, -26], 'snap'), k(0.58, [0, 0, 0], 'out')],
      chest: [k(0, [0, 0, 0]), k(0.1, [0, -4, 12]), k(0.21, [-6, 6, -34], 'snap'), k(0.58, [0, 0, 0], 'out')],
      head: [k(0, [0, 0, 0]), k(0.21, [0, 0, 16]), k(0.58, [0, 0, 0])],
    },
    events: [{ t: 0.21, name: 'strike', limb: 'handL' }],
  },
  {
    // rear uppercut: dip, then drive up through the target
    name: 'uppercut',
    targeted: true,
    reach: 0.58,
    duration: 0.65,
    fadeIn: 0.05,
    fadeOut: 0.15,
    ch: {
      ...guardHands(0, 0.65),
      handR: [k(0, GUARD_R), k(0.12, [0.12, 0.2, 0.02]), k(0.24, [0.05, 0.36, 0.3]), k(0.6, GUARD_R, 'out')],
      strikeR: [k(0, 0), k(0.12, 0), k(0.24, 1, 'snap'), k(0.3, 0.9), k(0.6, 0, 'out')],
      handRrot: [k(0, FIST_UP_R), k(0.12, [40, 90, 0]), k(0.24, [80, 90, 0]), k(0.6, FIST_UP_R)],
      elbowR: [k(0, POLE_R), k(0.12, [0.3, -0.6, -0.8]), k(0.24, [0.3, 0.2, -1]), k(0.6, POLE_R)],
      crouch: [k(0, 0), k(0.12, 0.25, 'out'), k(0.24, -0.05, 'snap'), k(0.62, 0, 'out')],
      pelvisRot: [k(0, [0, 0, 0]), k(0.12, [0, 0, -8]), k(0.24, [0, 0, 22], 'snap'), k(0.62, [0, 0, 0])],
      chest: [k(0, [0, 0, 0]), k(0.12, [-14, 6, -10]), k(0.24, [6, 0, 24], 'snap'), k(0.62, [0, 0, 0], 'out')],
    },
    events: [{ t: 0.24, name: 'strike', limb: 'handR' }],
  },
  {
    // rear-leg front (push) kick: chamber, extend, retract, plant
    name: 'frontKick',
    targeted: true,
    reach: 0.8,
    kickPitch: 0.35,
    duration: 0.85,
    fadeIn: 0.08,
    fadeOut: 0.15,
    ch: {
      ...guardHands(0, 0.85),
      footRw: [k(0, 0), k(0.08, 1), k(0.72, 1), k(0.85, 0)],
      footR: [k(0, [0.12, -0.05, 0.09]), k(0.22, [0.1, 0.28, 0.5], 'out'), k(0.5, [0.1, 0.3, 0.52]), k(0.8, [0.12, 0.02, 0.09], 'inout')],
      strikeFootR: [k(0, 0), k(0.22, 0), k(0.33, 1, 'snap'), k(0.4, 1), k(0.52, 0, 'out')],
      spine: [k(0, [0, 0, 0]), k(0.33, [18, 0, 0], 'snap'), k(0.8, [0, 0, 0])],
      chest: [k(0, [0, 0, 0]), k(0.33, [10, 0, 0]), k(0.8, [0, 0, 0])],
      pelvis: [k(0, [0, 0, 0]), k(0.2, [-0.03, -0.02, 0.03]), k(0.33, [-0.04, -0.06, 0.04]), k(0.8, [0, 0, 0])],
      handR: [k(0, GUARD_R), k(0.33, [0.24, 0.05, 0.1]), k(0.7, GUARD_R)],
    },
    events: [{ t: 0.33, name: 'strike', limb: 'footR' }],
  },
  {
    // rear-leg roundhouse: the hip turns over, the shin sweeps round
    name: 'roundhouse',
    targeted: true,
    reach: 0.78,
    kickPitch: -0.9,
    duration: 0.95,
    fadeIn: 0.08,
    fadeOut: 0.15,
    ch: {
      ...guardHands(0, 0.95),
      footRw: [k(0, 0), k(0.08, 1), k(0.8, 1), k(0.95, 0)],
      footR: [k(0, [0.12, -0.05, 0.09]), k(0.22, [0.42, 0.05, 0.55], 'out'), k(0.38, [0.2, 0.5, 0.8]), k(0.6, [0.3, 0.1, 0.45]), k(0.9, [0.12, 0.02, 0.09], 'inout')],
      strikeFootR: [k(0, 0), k(0.25, 0.1), k(0.38, 0.95, 'snap'), k(0.45, 0.8), k(0.6, 0, 'out')],
      pelvisRot: [k(0, [0, 0, 0]), k(0.25, [0, -20, 35]), k(0.38, [0, -28, 60], 'snap'), k(0.9, [0, 0, 0], 'inout')],
      spine: [k(0, [0, 0, 0]), k(0.38, [6, -18, -20]), k(0.9, [0, 0, 0])],
      chest: [k(0, [0, 0, 0]), k(0.38, [0, -10, -30]), k(0.9, [0, 0, 0])],
      head: [k(0, [0, 0, 0]), k(0.38, [0, 10, -10]), k(0.9, [0, 0, 0])],
      handR: [k(0, GUARD_R), k(0.38, [0.3, -0.25, 0.05]), k(0.8, GUARD_R)],
    },
    events: [{ t: 0.38, name: 'strike', limb: 'footR' }],
  },
  {
    // knife thrust
    name: 'stab',
    targeted: true,
    reach: 0.66,
    prop: 'knife',
    duration: 0.62,
    fadeIn: 0.06,
    fadeOut: 0.15,
    ch: {
      handR: [k(0, [0.2, 0.2, 0.0]), k(0.16, [0.22, -0.05, 0.02], 'out'), k(0.6, [0.2, 0.2, 0.0])],
      handRrot: [k(0, [0, 90, 0]), k(0.6, [0, 90, 0])],
      strikeR: [k(0, 0), k(0.16, 0), k(0.28, 1, 'snap'), k(0.34, 1), k(0.58, 0, 'out')],
      elbowR: [k(0, [0.6, -0.4, -0.8]), k(0.6, [0.6, -0.4, -0.8])],
      handL: [k(0, [-0.18, 0.28, 0.12]), k(0.28, [-0.25, 0.1, 0.05]), k(0.6, [-0.18, 0.28, 0.12])],
      handLw: [k(0, 0.7), k(0.6, 0.7)],
      pelvisRot: [k(0, [0, 0, 0]), k(0.16, [0, 0, -10]), k(0.28, [0, 0, 18], 'snap'), k(0.6, [0, 0, 0])],
      chest: [k(0, [0, 0, 0]), k(0.16, [2, 0, -12]), k(0.28, [-12, 0, 22], 'snap'), k(0.6, [0, 0, 0], 'out')],
      pelvis: [k(0, [0, 0, 0]), k(0.28, [0, 0.12, -0.04], 'snap'), k(0.6, [0, 0, 0], 'out')],
      crouch: [k(0, 0.1), k(0.28, 0.2), k(0.6, 0.1)],
    },
    events: [{ t: 0.28, name: 'strike', limb: 'blade' }],
  },
  {
    // knife slash: a backhand arc across the target
    name: 'slash',
    targeted: true,
    reach: 0.62,
    prop: 'knife',
    duration: 0.62,
    fadeIn: 0.06,
    fadeOut: 0.15,
    ch: {
      handR: [k(0, [0.2, 0.2, 0.0]), k(0.14, [0.4, 0.1, 0.32], 'out'), k(0.3, [-0.22, 0.4, 0.05], 'snap'), k(0.6, [0.2, 0.2, 0.0], 'inout')],
      strikeR: [k(0, 0), k(0.14, 0), k(0.22, 0.75, 'snap'), k(0.3, 0.3), k(0.6, 0)],
      handRrot: [k(0, [0, 90, 0]), k(0.14, [30, 170, 40]), k(0.3, [0, 170, -60], 'snap'), k(0.6, [0, 90, 0])],
      elbowR: [k(0, [0.6, -0.4, -0.8]), k(0.14, [1, -0.3, 0.4]), k(0.3, [0.2, 0.4, -1]), k(0.6, [0.6, -0.4, -0.8])],
      chest: [k(0, [0, 0, 0]), k(0.14, [0, 6, -26]), k(0.3, [-6, -6, 30], 'snap'), k(0.6, [0, 0, 0])],
      pelvisRot: [k(0, [0, 0, 0]), k(0.14, [0, 0, -12]), k(0.3, [0, 0, 16], 'snap'), k(0.6, [0, 0, 0])],
      pelvis: [k(0, [0, 0, 0]), k(0.22, [0, 0.08, -0.02]), k(0.6, [0, 0, 0])],
    },
    events: [{ t: 0.22, name: 'strike', limb: 'blade' }],
  },
  {
    // shove / rifle jab: both hands (or the weapon) driven forward
    name: 'riflePush',
    targeted: true,
    reach: 0.7,
    duration: 0.55,
    fadeIn: 0.05,
    fadeOut: 0.15,
    ch: {
      weaponPos: [k(0, [0, 0, 0]), k(0.1, [0, -0.08, 0]), k(0.2, [0, 0.32, 0.02], 'snap'), k(0.26, [0, 0.3, 0.02]), k(0.52, [0, 0, 0], 'out')],
      weaponRot: [k(0, [0, 0, 0]), k(0.2, [-8, 0, 0]), k(0.52, [0, 0, 0])],
      chest: [k(0, [0, 0, 0]), k(0.1, [4, 0, -8]), k(0.2, [-10, 0, 6], 'snap'), k(0.52, [0, 0, 0])],
      pelvis: [k(0, [0, 0, 0]), k(0.2, [0, 0.12, -0.02], 'snap'), k(0.52, [0, 0, 0], 'out')],
    },
    events: [{ t: 0.2, name: 'strike', limb: 'muzzle' }],
  },
  {
    // forearms up in front of the face
    name: 'block',
    duration: 0.9,
    fadeIn: 0.06,
    fadeOut: 0.2,
    ch: {
      handR: [k(0, [0.08, 0.26, 0.42]), k(0.9, [0.08, 0.26, 0.42])],
      handL: [k(0, [-0.08, 0.28, 0.44]), k(0.9, [-0.08, 0.28, 0.44])],
      handRrot: [k(0, [80, 90, 20]), k(0.9, [80, 90, 20])],
      handLrot: [k(0, [80, -90, -20]), k(0.9, [80, -90, -20])],
      elbowR: [k(0, [0.3, 0.6, -0.7]), k(0.9, [0.3, 0.6, -0.7])],
      elbowL: [k(0, [-0.3, 0.6, -0.7]), k(0.9, [-0.3, 0.6, -0.7])],
      chest: [k(0, [0, 0, 0]), k(0.1, [-10, 0, 0]), k(0.9, [-8, 0, 0])],
      head: [k(0, [0, 0, 0]), k(0.1, [-18, 0, 0]), k(0.9, [-14, 0, 0])],
      crouch: [k(0, 0), k(0.1, 0.25), k(0.9, 0.2)],
    },
  },
  // ---- weapons ----
  {
    name: 'reloadRifle',
    duration: 2.3,
    fadeIn: 0.15,
    fadeOut: 0.25,
    ch: {
      weaponRot: [k(0, [0, 0, 0]), k(0.3, [-12, 28, 8]), k(1.9, [-12, 28, 8]), k(2.2, [0, 0, 0])],
      weaponPos: [k(0, [0, 0, 0]), k(0.3, [0.02, -0.06, -0.04]), k(1.9, [0.02, -0.06, -0.04]), k(2.2, [0, 0, 0])],
      handL: [k(0.2, [0.02, 0.36, -0.05]), k(0.45, [0.04, 0.3, -0.12]), k(0.8, [-0.06, 0.16, -0.12], 'inout'), k(1.1, [-0.06, 0.16, -0.12]), k(1.4, [0.04, 0.3, -0.1], 'inout'), k(1.55, [0.04, 0.3, -0.07], 'snap'), k(1.8, [0.1, 0.28, 0.08]), k(1.95, [0.12, 0.2, 0.08], 'snap')],
      handLw: [k(0, 0), k(0.2, 1), k(2.05, 1), k(2.3, 0)],
      handLrot: [k(0.2, [0, -90, 0]), k(0.8, [-40, -90, 0]), k(1.4, [0, -90, 0]), k(1.8, [0, -90, 40])],
      head: [k(0, [0, 0, 0]), k(0.3, [-20, 8, 6]), k(1.9, [-18, 8, 6]), k(2.2, [0, 0, 0])],
      look: [k(0, 1), k(0.3, 0.2), k(1.9, 0.2), k(2.2, 1)],
    },
    events: [{ t: 2.0, name: 'reloaded' }],
  },
  {
    name: 'reloadPistol',
    duration: 1.6,
    fadeIn: 0.1,
    fadeOut: 0.2,
    ch: {
      weaponRot: [k(0, [0, 0, 0]), k(0.25, [30, 20, 0]), k(1.3, [30, 20, 0]), k(1.55, [0, 0, 0])],
      weaponPos: [k(0, [0, 0, 0]), k(0.25, [0, -0.18, -0.1]), k(1.3, [0, -0.18, -0.1]), k(1.55, [0, 0, 0])],
      handL: [k(0.15, [-0.02, 0.28, 0.05]), k(0.45, [-0.14, 0.12, -0.22], 'inout'), k(0.7, [-0.14, 0.12, -0.22]), k(1.0, [0.02, 0.26, 0.0], 'inout'), k(1.1, [0.02, 0.26, 0.03], 'snap')],
      handLw: [k(0, 0), k(0.15, 1), k(1.3, 1), k(1.55, 0)],
      head: [k(0, [0, 0, 0]), k(0.25, [-22, 0, 4]), k(1.3, [-22, 0, 4]), k(1.55, [0, 0, 0])],
      look: [k(0, 1), k(0.25, 0.2), k(1.3, 0.2), k(1.55, 1)],
    },
    events: [{ t: 1.3, name: 'reloaded' }],
  },
  // ---- idle poses (held) ----
  {
    name: 'armsCrossed',
    layer: 'pose',
    loop: true,
    duration: 4,
    fadeIn: 0.5,
    fadeOut: 0.5,
    ch: {
      handR: [k(0, [-0.1, 0.17, 0.05]), k(2, [-0.1, 0.17, 0.06]), k(4, [-0.1, 0.17, 0.05])],
      handL: [k(0, [0.11, 0.15, 0.1]), k(2, [0.11, 0.15, 0.11]), k(4, [0.11, 0.15, 0.1])],
      handRrot: [k(0, [0, 90, 80]), k(4, [0, 90, 80])],
      handLrot: [k(0, [0, -90, -80]), k(4, [0, -90, -80])],
      elbowR: [k(0, [1, 0.2, -0.3]), k(4, [1, 0.2, -0.3])],
      elbowL: [k(0, [-1, 0.2, -0.3]), k(4, [-1, 0.2, -0.3])],
      chest: [k(0, [4, 0, 0]), k(4, [4, 0, 0])],
    },
  },
  {
    name: 'pockets',
    layer: 'pose',
    loop: true,
    duration: 4,
    fadeIn: 0.5,
    fadeOut: 0.5,
    ch: {
      handR: [k(0, [0.14, 0.07, -0.31]), k(4, [0.14, 0.07, -0.31])],
      handL: [k(0, [-0.14, 0.07, -0.31]), k(4, [-0.14, 0.07, -0.31])],
      handRrot: [k(0, [-80, 90, 0]), k(4, [-80, 90, 0])],
      handLrot: [k(0, [-80, -90, 0]), k(4, [-80, -90, 0])],
      elbowR: [k(0, [0.8, -0.5, -0.3]), k(4, [0.8, -0.5, -0.3])],
      elbowL: [k(0, [-0.8, -0.5, -0.3]), k(4, [-0.8, -0.5, -0.3])],
      chest: [k(0, [-3, 0, 0]), k(4, [-3, 0, 0])],
    },
  },
  {
    name: 'handsOnHips',
    layer: 'pose',
    loop: true,
    duration: 4,
    fadeIn: 0.5,
    fadeOut: 0.5,
    ch: {
      handR: [k(0, [0.2, 0.02, -0.24]), k(4, [0.2, 0.02, -0.24])],
      handL: [k(0, [-0.2, 0.02, -0.24]), k(4, [-0.2, 0.02, -0.24])],
      handRrot: [k(0, [-90, 90, 30]), k(4, [-90, 90, 30])],
      handLrot: [k(0, [-90, -90, -30]), k(4, [-90, -90, -30])],
      elbowR: [k(0, [1, -0.3, 0]), k(4, [1, -0.3, 0])],
      elbowL: [k(0, [-1, -0.3, 0]), k(4, [-1, -0.3, 0])],
      chest: [k(0, [6, 0, 0]), k(4, [6, 0, 0])],
    },
  },
  {
    name: 'handsBehind',
    layer: 'pose',
    loop: true,
    duration: 4,
    fadeIn: 0.5,
    fadeOut: 0.5,
    ch: {
      handR: [k(0, [0.03, -0.17, -0.24]), k(4, [0.03, -0.17, -0.24])],
      handL: [k(0, [-0.03, -0.16, -0.22]), k(4, [-0.03, -0.16, -0.22])],
      handRrot: [k(0, [-90, 90, 180]), k(4, [-90, 90, 180])],
      handLrot: [k(0, [-90, -90, 180]), k(4, [-90, -90, 180])],
      elbowR: [k(0, [0.6, -1, 0]), k(4, [0.6, -1, 0])],
      elbowL: [k(0, [-0.6, -1, 0]), k(4, [-0.6, -1, 0])],
      chest: [k(0, [8, 0, 0]), k(4, [8, 0, 0])],
    },
  },
  {
    name: 'handsFolded',
    layer: 'pose',
    loop: true,
    duration: 4,
    fadeIn: 0.5,
    fadeOut: 0.5,
    ch: {
      handR: [k(0, [0.03, 0.14, -0.22]), k(4, [0.03, 0.14, -0.22])],
      handL: [k(0, [-0.03, 0.15, -0.21]), k(4, [-0.03, 0.15, -0.21])],
      handRrot: [k(0, [-30, 90, 60]), k(4, [-30, 90, 60])],
      handLrot: [k(0, [-30, -90, -60]), k(4, [-30, -90, -60])],
      elbowR: [k(0, [0.7, -0.3, -0.7]), k(4, [0.7, -0.3, -0.7])],
      elbowL: [k(0, [-0.7, -0.3, -0.7]), k(4, [-0.7, -0.3, -0.7])],
    },
  },
  {
    // looking at a phone held in both hands
    name: 'phone',
    layer: 'pose',
    loop: true,
    duration: 3,
    fadeIn: 0.5,
    fadeOut: 0.5,
    ch: {
      handR: [k(0, [0.04, 0.3, 0.12]), k(1.5, [0.04, 0.3, 0.13]), k(3, [0.04, 0.3, 0.12])],
      handL: [k(0, [-0.02, 0.29, 0.1]), k(3, [-0.02, 0.29, 0.1])],
      handRrot: [k(0, [30, 150, 0]), k(3, [30, 150, 0])],
      handLrot: [k(0, [30, -150, 0]), k(3, [30, -150, 0])],
      elbowR: [k(0, [0.6, -0.4, -0.8]), k(3, [0.6, -0.4, -0.8])],
      elbowL: [k(0, [-0.6, -0.4, -0.8]), k(3, [-0.6, -0.4, -0.8])],
      neck: [k(0, [-18, 0, 0]), k(3, [-18, 0, 0])],
      head: [k(0, [-24, 0, 0]), k(3, [-24, 0, 0])],
      look: [k(0, 0.1), k(3, 0.1)],
    },
  },
  // ---- fidgets (one-shots) ----
  {
    name: 'checkWatch',
    duration: 2.2,
    fadeIn: 0.3,
    fadeOut: 0.4,
    ch: {
      handL: [k(0, [-0.1, 0.24, 0.12]), k(2.2, [-0.1, 0.24, 0.12])],
      handLrot: [k(0, [10, -30, -60]), k(2.2, [10, -30, -60])],
      elbowL: [k(0, [-0.8, -0.2, -0.6]), k(2.2, [-0.8, -0.2, -0.6])],
      head: [k(0, [0, 0, 0]), k(0.4, [-30, 0, 18]), k(1.8, [-30, 0, 18]), k(2.2, [0, 0, 0])],
      look: [k(0, 1), k(0.3, 0), k(1.9, 0), k(2.2, 1)],
    },
  },
  {
    name: 'scratchHead',
    duration: 2.4,
    fadeIn: 0.35,
    fadeOut: 0.4,
    ch: {
      handR: [k(0, [0.1, 0.02, 0.52]), k(0.8, [0.08, -0.02, 0.54]), k(1.0, [0.1, 0.02, 0.52]), k(1.2, [0.08, -0.02, 0.54]), k(1.4, [0.1, 0.02, 0.52]), k(2.4, [0.1, 0.02, 0.52])],
      handRrot: [k(0, [120, 90, 60]), k(2.4, [120, 90, 60])],
      elbowR: [k(0, [1, 0.3, 0.2]), k(2.4, [1, 0.3, 0.2])],
      head: [k(0, [0, 0, 0]), k(0.4, [-6, -10, -6]), k(2.0, [-6, -10, -6]), k(2.4, [0, 0, 0])],
    },
  },
  {
    name: 'stretch',
    duration: 3,
    fadeIn: 0.6,
    fadeOut: 0.6,
    ch: {
      handR: [k(0, [0.12, 0.05, 0.75]), k(3, [0.12, 0.05, 0.75])],
      handL: [k(0, [-0.12, 0.05, 0.75]), k(3, [-0.12, 0.05, 0.75])],
      handRrot: [k(0, [170, 90, 0]), k(3, [170, 90, 0])],
      handLrot: [k(0, [170, -90, 0]), k(3, [170, -90, 0])],
      elbowR: [k(0, [1, -0.2, 0.2]), k(3, [1, -0.2, 0.2])],
      elbowL: [k(0, [-1, -0.2, 0.2]), k(3, [-1, -0.2, 0.2])],
      spine: [k(0, [0, 0, 0]), k(1.2, [12, 0, 0]), k(1.8, [12, 0, 0]), k(3, [0, 0, 0])],
      chest: [k(0, [0, 0, 0]), k(1.2, [10, 0, 0]), k(1.8, [10, 0, 0]), k(3, [0, 0, 0])],
      head: [k(0, [0, 0, 0]), k(1.2, [18, 0, 0]), k(1.8, [18, 0, 0]), k(3, [0, 0, 0])],
      look: [k(0, 1), k(0.6, 0), k(2.4, 0), k(3, 1)],
    },
  },
  {
    name: 'rubNeck',
    duration: 2.2,
    fadeIn: 0.35,
    fadeOut: 0.4,
    ch: {
      handR: [k(0, [0.06, -0.08, 0.38]), k(0.9, [0.02, -0.09, 0.4]), k(1.3, [0.07, -0.08, 0.37]), k(2.2, [0.06, -0.08, 0.38])],
      handRrot: [k(0, [120, 90, 120]), k(2.2, [120, 90, 120])],
      elbowR: [k(0, [1, 0.4, 0]), k(2.2, [1, 0.4, 0])],
      head: [k(0, [0, 0, 0]), k(0.5, [-12, -8, 10]), k(1.2, [-8, 8, -8]), k(2.2, [0, 0, 0])],
    },
  },
  {
    name: 'wave',
    duration: 1.8,
    fadeIn: 0.25,
    fadeOut: 0.35,
    ch: {
      handR: [k(0, [0.3, 0.12, 0.5]), k(0.5, [0.26, 0.14, 0.52]), k(0.75, [0.36, 0.12, 0.5]), k(1.0, [0.26, 0.14, 0.52]), k(1.25, [0.36, 0.12, 0.5]), k(1.8, [0.3, 0.12, 0.5])],
      handRrot: [k(0, [160, 0, 0]), k(1.8, [160, 0, 0])],
      elbowR: [k(0, [1, 0, -0.5]), k(1.8, [1, 0, -0.5])],
    },
  },
  // ---- conversation ----
  {
    // the speaker's hands moving with the words (looped; the animator adds beats)
    name: 'talk',
    layer: 'pose',
    loop: true,
    duration: 2.4,
    fadeIn: 0.4,
    fadeOut: 0.5,
    ch: {
      handR: [k(0, [0.12, 0.2, -0.1]), k(0.6, [0.16, 0.26, -0.04]), k(1.2, [0.1, 0.22, -0.12]), k(1.8, [0.18, 0.28, -0.06]), k(2.4, [0.12, 0.2, -0.1])],
      handL: [k(0, [-0.12, 0.21, -0.12]), k(0.8, [-0.15, 0.24, -0.08]), k(1.6, [-0.1, 0.2, -0.13]), k(2.4, [-0.12, 0.21, -0.12])],
      handRrot: [k(0, [10, 130, 0]), k(1.2, [20, 150, 10]), k(2.4, [10, 130, 0])],
      handLrot: [k(0, [10, -130, 0]), k(1.2, [20, -150, -10]), k(2.4, [10, -130, 0])],
      elbowR: [k(0, [0.8, -0.4, -0.6]), k(2.4, [0.8, -0.4, -0.6])],
      elbowL: [k(0, [-0.8, -0.4, -0.6]), k(2.4, [-0.8, -0.4, -0.6])],
      head: [k(0, [0, 0, 0]), k(0.6, [-4, 3, 0]), k(1.2, [2, -2, 0]), k(1.8, [-3, 0, 0]), k(2.4, [0, 0, 0])],
    },
  },
  {
    name: 'gestureOpen',
    duration: 1.5,
    fadeIn: 0.2,
    fadeOut: 0.35,
    ch: {
      handR: [k(0, [0.14, 0.24, -0.08]), k(0.4, [0.3, 0.3, 0.0], 'out'), k(1.0, [0.32, 0.28, 0.0]), k(1.5, [0.14, 0.24, -0.08])],
      handL: [k(0, [-0.14, 0.24, -0.08]), k(0.4, [-0.3, 0.3, 0.0], 'out'), k(1.0, [-0.32, 0.28, 0.0]), k(1.5, [-0.14, 0.24, -0.08])],
      handRrot: [k(0, [10, 150, 0]), k(0.4, [10, 180, -20]), k(1.5, [10, 150, 0])],
      handLrot: [k(0, [10, -150, 0]), k(0.4, [10, -180, 20]), k(1.5, [10, -150, 0])],
      elbowR: [k(0, [0.8, -0.4, -0.6]), k(1.5, [0.8, -0.4, -0.6])],
      elbowL: [k(0, [-0.8, -0.4, -0.6]), k(1.5, [-0.8, -0.4, -0.6])],
      head: [k(0, [0, 0, 0]), k(0.4, [4, 0, 0]), k(1.5, [0, 0, 0])],
    },
  },
  {
    name: 'gesturePoint',
    duration: 1.4,
    fadeIn: 0.15,
    fadeOut: 0.35,
    ch: {
      handR: [k(0, [0.14, 0.24, -0.08]), k(0.3, [0.16, 0.55, 0.18], 'out'), k(0.5, [0.16, 0.52, 0.16]), k(0.7, [0.16, 0.55, 0.18]), k(1.1, [0.16, 0.55, 0.18]), k(1.4, [0.14, 0.24, -0.08])],
      handRrot: [k(0, [10, 130, 0]), k(0.3, [0, 90, 0]), k(1.4, [10, 130, 0])],
      elbowR: [k(0, [0.8, -0.4, -0.6]), k(1.4, [0.8, -0.4, -0.6])],
      chest: [k(0, [0, 0, 0]), k(0.3, [-4, 0, 8]), k(1.4, [0, 0, 0])],
      head: [k(0, [0, 0, 0]), k(0.3, [-6, 0, 0]), k(0.5, [-2, 0, 0]), k(0.7, [-6, 0, 0]), k(1.4, [0, 0, 0])],
    },
  },
  {
    name: 'shrug',
    duration: 1.3,
    fadeIn: 0.15,
    fadeOut: 0.3,
    ch: {
      handR: [k(0, [0.16, 0.2, -0.14]), k(0.35, [0.26, 0.24, -0.06], 'out'), k(0.9, [0.26, 0.24, -0.06]), k(1.3, [0.16, 0.2, -0.14])],
      handL: [k(0, [-0.16, 0.2, -0.14]), k(0.35, [-0.26, 0.24, -0.06], 'out'), k(0.9, [-0.26, 0.24, -0.06]), k(1.3, [-0.16, 0.2, -0.14])],
      handRrot: [k(0, [0, 180, -20]), k(1.3, [0, 180, -20])],
      handLrot: [k(0, [0, -180, 20]), k(1.3, [0, -180, 20])],
      elbowR: [k(0, [1, -0.3, -0.4]), k(1.3, [1, -0.3, -0.4])],
      elbowL: [k(0, [-1, -0.3, -0.4]), k(1.3, [-1, -0.3, -0.4])],
      clavR: [k(0, [0, 0, 0]), k(0.35, [0, -18, 0], 'out'), k(0.9, [0, -18, 0]), k(1.3, [0, 0, 0])],
      clavL: [k(0, [0, 0, 0]), k(0.35, [0, 18, 0], 'out'), k(0.9, [0, 18, 0]), k(1.3, [0, 0, 0])],
      head: [k(0, [0, 0, 0]), k(0.35, [4, -12, 0]), k(0.9, [4, -12, 0]), k(1.3, [0, 0, 0])],
    },
  },
  {
    name: 'handOnChest',
    duration: 1.5,
    fadeIn: 0.2,
    fadeOut: 0.35,
    ch: {
      handR: [k(0, [0.14, 0.24, -0.08]), k(0.35, [0.0, 0.17, 0.1], 'out'), k(1.1, [0.0, 0.17, 0.1]), k(1.5, [0.14, 0.24, -0.08])],
      handRrot: [k(0, [10, 130, 0]), k(0.35, [60, 90, 90]), k(1.1, [60, 90, 90]), k(1.5, [10, 130, 0])],
      elbowR: [k(0, [0.8, -0.4, -0.6]), k(1.5, [0.8, -0.4, -0.6])],
      head: [k(0, [0, 0, 0]), k(0.35, [-4, 0, 0]), k(1.5, [0, 0, 0])],
    },
  },
  {
    name: 'nod',
    duration: 0.9,
    fadeIn: 0.05,
    fadeOut: 0.15,
    ch: { head: [k(0, [0, 0, 0]), k(0.2, [-14, 0, 0]), k(0.4, [2, 0, 0]), k(0.6, [-10, 0, 0]), k(0.9, [0, 0, 0])] },
  },
  {
    name: 'shakeHead',
    duration: 1.0,
    fadeIn: 0.05,
    fadeOut: 0.15,
    ch: { head: [k(0, [0, 0, 0]), k(0.18, [0, 0, 14]), k(0.4, [0, 0, -14]), k(0.62, [0, 0, 12]), k(0.82, [0, 0, -6]), k(1.0, [0, 0, 0])] },
  },
  {
    name: 'laugh',
    duration: 1.6,
    fadeIn: 0.1,
    fadeOut: 0.3,
    ch: {
      head: [k(0, [0, 0, 0]), k(0.2, [16, 0, 0]), k(0.35, [10, 0, 0]), k(0.5, [16, 0, 0]), k(0.65, [10, 0, 0]), k(0.8, [15, 0, 0]), k(1.6, [0, 0, 0])],
      chest: [k(0, [0, 0, 0]), k(0.2, [4, 0, 0]), k(0.35, [-2, 0, 0]), k(0.5, [4, 0, 0]), k(0.65, [-2, 0, 0]), k(0.8, [4, 0, 0]), k(1.6, [0, 0, 0])],
      clavR: [k(0, [0, 0, 0]), k(0.35, [0, -8, 0]), k(0.65, [0, -8, 0]), k(1.6, [0, 0, 0])],
      clavL: [k(0, [0, 0, 0]), k(0.35, [0, 8, 0]), k(0.65, [0, 8, 0]), k(1.6, [0, 0, 0])],
    },
  },
];

export const ACTIONS: ReadonlyMap<string, ActionDef> = (() => {
  const m = new Map<string, ActionDef>();
  for (const a of A) {
    m.set(a.name, a);
    m.set(`${a.name}.m`, mirrorAction(a));
  }
  return m;
})();

export function actionDef(name: string): ActionDef {
  const d = ACTIONS.get(name);
  if (!d) throw new Error(`no action ${name}`);
  return d;
}

export const STRIKES = ['jab', 'cross', 'hook', 'uppercut', 'frontKick', 'roundhouse'] as const;
export const KNIFE_ATTACKS = ['stab', 'slash'] as const;
export const IDLE_POSES = ['armsCrossed', 'pockets', 'handsOnHips', 'handsBehind', 'handsFolded', 'phone'] as const;
export const FIDGETS = ['checkWatch', 'scratchHead', 'stretch', 'rubNeck'] as const;
export const GESTURES = ['gestureOpen', 'gesturePoint', 'shrug', 'handOnChest'] as const;
