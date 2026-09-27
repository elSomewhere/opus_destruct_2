/**
 * Body stances besides standing: kneeling, prone (and crawling), sitting on a seat (upright,
 * leaning back, legs crossed, elbows on the knees, at a desk), sitting on the ground
 * (cross-legged, knees up, legs out) and lying knocked down (on the back or the front).
 *
 * A stance is sampled as a StanceSample in model space (the character's root frame): the pelvis
 * transform, base rotations of the trunk joints, the feet (ankle targets, foot rotations, knee
 * poles, toe bend) and where free hands rest. Samples blend linearly, so a transition is a
 * blend of two stances over time (the animator runs the transitions, through intermediate
 * stances: stand -> kneel -> prone). Aim, look, actions and reactions are layered on top.
 */
import { qeuler, qmul, qnlerp, qx, qz, type Quat } from '../math/quat.ts';
import { vlerp, type V3 } from '../math/vec.ts';

export type Stance = 'stand' | 'kneel' | 'prone' | 'sit' | 'ground' | 'down';
export type SitVariant = 'upright' | 'leanBack' | 'crossLegs' | 'elbowsOnKnees' | 'desk';
export type GroundVariant = 'cross' | 'kneesUp' | 'legsOut';

export interface FootPose {
  ankle: V3;
  rot: Quat;
  pole: V3;
  /** Toe bend (rad, local x). */
  toe: number;
}

export interface StanceSample {
  pelvisPos: V3;
  pelvisRot: Quat;
  spine: Quat;
  chest: Quat;
  neck: Quat;
  head: Quat;
  /** Left, right. */
  feet: [FootPose, FootPose];
  /** Where free hands rest (model space palm targets), left and right; null: hang free. */
  hands: [V3 | null, V3 | null];
  /** How much the trunk may turn to aim / look (1 standing). */
  turn: number;
}

export function newSample(): StanceSample {
  const foot = (): FootPose => ({ ankle: [0, 0, 0], rot: [0, 0, 0, 1], pole: [0, 1, 0], toe: 0 });
  return { pelvisPos: [0, 0, 0], pelvisRot: [0, 0, 0, 1], spine: [0, 0, 0, 1], chest: [0, 0, 0, 1], neck: [0, 0, 0, 1], head: [0, 0, 0, 1], feet: [foot(), foot()], hands: [null, null], turn: 1 };
}

/** out = a + (b - a) w (quaternions nlerp'd; free hands blend when both rest). */
export function blendSamples(a: StanceSample, b: StanceSample, w: number, out: StanceSample): StanceSample {
  vlerp(a.pelvisPos, b.pelvisPos, w, out.pelvisPos);
  out.pelvisRot = qnlerp(a.pelvisRot, b.pelvisRot, w);
  out.spine = qnlerp(a.spine, b.spine, w);
  out.chest = qnlerp(a.chest, b.chest, w);
  out.neck = qnlerp(a.neck, b.neck, w);
  out.head = qnlerp(a.head, b.head, w);
  for (let i = 0; i < 2; i++) {
    const fa = a.feet[i]!, fb = b.feet[i]!, fo = out.feet[i]!;
    vlerp(fa.ankle, fb.ankle, w, fo.ankle);
    fo.rot = qnlerp(fa.rot, fb.rot, w);
    vlerp(fa.pole, fb.pole, w, fo.pole);
    fo.toe = fa.toe + (fb.toe - fa.toe) * w;
    const ha = a.hands[i], hb = b.hands[i];
    out.hands[i] = ha && hb ? vlerp(ha, hb, w) : w < 0.5 ? (ha ? [...ha] : null) : hb ? [...hb] : null;
  }
  out.turn = a.turn + (b.turn - a.turn) * w;
  return out;
}

export interface Dims {
  /** Height scale (1 = 1.78 m). */
  k: number;
  ankleH: number;
  footX: number;
}

const flat = (yaw = 0): Quat => qz(yaw);

/** Kneeling on the right knee, left foot forward (a firing position). */
export function kneelSample(d: Dims, out: StanceSample): StanceSample {
  const k = d.k;
  out.pelvisPos = [0.01 * k, -0.03 * k, 0.56 * k];
  out.pelvisRot = qeuler(-0.05, 0, -0.12);
  out.spine = qeuler(-0.08, 0, 0.05);
  out.chest = qeuler(0.02, 0, 0.05);
  out.neck = qx(0.04);
  out.head = qx(0.02);
  const [l, r] = out.feet;
  l.ankle = [-0.14 * k, 0.36 * k, d.ankleH];
  l.rot = flat(0.15);
  l.pole = [-0.1, 1, 0.3];
  l.toe = 0;
  r.ankle = [0.15 * k, -0.42 * k, 0.1 * k];
  r.rot = qx(-1.2);
  r.pole = [0.1, 0.5, -1];
  r.toe = 1.1;
  out.hands = [[-0.13 * k, 0.34 * k, 0.53 * k], [0.14 * k, 0.12 * k, 0.5 * k]];
  out.turn = 0.9;
  return out;
}

/**
 * Prone, face down along +y, raised on the elbows. `crawl` 0..1 blends in the crawl cycle at
 * phase `phase` (a commando crawl: knee drawn up on one side, the opposite elbow forward).
 */
export function proneSample(d: Dims, crawl: number, phase: number, out: StanceSample): StanceSample {
  const k = d.k;
  const s = Math.sin(phase * Math.PI * 2);
  const c = crawl;
  out.pelvisPos = [0, 0, 0.13 * k];
  out.pelvisRot = qmul(qz(0.14 * s * c), qx(-Math.PI / 2 + 0.04));
  out.spine = qeuler(0.1, 0, -0.1 * s * c);
  out.chest = qeuler(0.28, 0, -0.08 * s * c);
  out.neck = qx(0.38);
  out.head = qx(0.32);
  const [l, r] = out.feet;
  const drawL = Math.max(0, s) * c, drawR = Math.max(0, -s) * c;
  l.ankle = [(-0.15 - 0.14 * drawL) * k, (-0.86 + 0.3 * drawL) * k, 0.09 * k];
  r.ankle = [(0.15 + 0.14 * drawR) * k, (-0.86 + 0.3 * drawR) * k, 0.09 * k];
  const toesDown = qx(-Math.PI / 2 - 0.35);
  l.rot = qmul(qz(0.3 * drawL), toesDown);
  r.rot = qmul(qz(-0.3 * drawR), toesDown);
  l.pole = [-0.8 * drawL, 0.1, -1];
  r.pole = [0.8 * drawR, 0.1, -1];
  l.toe = 0.5;
  r.toe = 0.5;
  // elbows alternate forward while crawling
  out.hands = [[-0.17 * k, (0.6 + 0.12 * s * c) * k, 0.07 * k], [0.17 * k, (0.6 - 0.12 * s * c) * k, 0.07 * k]];
  out.turn = 0.35;
  return out;
}

export interface SeatModel {
  /** Seat surface centre (model space). */
  pos: V3;
  backrest: boolean;
  /** Desk top height (model z), or null. */
  desk: number | null;
  variant: SitVariant;
}

/** Sitting on a seat behind the root (the feet stay where the character stood). */
export function sitSample(d: Dims, seat: SeatModel, t: number, out: StanceSample): StanceSample {
  const k = d.k;
  const v = seat.variant === 'desk' && seat.desk === null ? 'upright' : seat.variant;
  const hipZ = seat.pos[2] + 0.1 * k;
  const py = seat.pos[1] + 0.03 * k;
  out.pelvisPos = [seat.pos[0], py, hipZ + 0.045 * k];
  const tilt = v === 'leanBack' ? 0.24 : v === 'elbowsOnKnees' ? -0.12 : v === 'desk' ? 0.02 : 0.1;
  out.pelvisRot = qx(tilt);
  const breathe = Math.sin(t * 1.7) * 0.015;
  switch (v) {
    case 'leanBack':
      out.spine = qx(0.1 + breathe);
      out.chest = qx(0.06);
      out.neck = qx(-0.18);
      out.head = qx(-0.08);
      break;
    case 'elbowsOnKnees':
      out.spine = qx(-0.42 + breathe);
      out.chest = qx(-0.3);
      out.neck = qx(0.32);
      out.head = qx(0.25);
      break;
    case 'desk':
      out.spine = qx(-0.2 + breathe);
      out.chest = qx(-0.12);
      out.neck = qx(-0.05);
      out.head = qx(-0.2);
      break;
    default:
      out.spine = qx(-0.06 + breathe);
      out.chest = qx(0.02);
      out.neck = qx(0);
      out.head = qx(0);
  }
  const kneeY = py + 0.4 * k;
  const [l, r] = out.feet;
  const ay = v === 'leanBack' ? kneeY + 0.12 * k : v === 'desk' ? kneeY + 0.02 * k : kneeY - 0.04 * k;
  l.ankle = [-0.14 * k, ay, d.ankleH];
  r.ankle = [0.14 * k, ay, d.ankleH];
  l.rot = flat(0.12);
  r.rot = flat(-0.12);
  l.pole = [-0.15, 1, 0.3];
  r.pole = [0.15, 1, 0.3];
  l.toe = 0;
  r.toe = 0;
  if (v === 'crossLegs') {
    // right leg over the left knee, the foot hanging past it
    r.ankle = [-0.1 * k, kneeY + 0.14 * k, hipZ + 0.02 * k];
    r.rot = qmul(qz(0.3), qx(-0.5));
    r.pole = [0.25, 1, 1.2];
    l.ankle = [-0.1 * k, kneeY - 0.02 * k, d.ankleH];
  }
  const lap: V3 = [0, py + 0.2 * k, hipZ + 0.1 * k];
  switch (v) {
    case 'leanBack':
      out.hands = seat.backrest ? [[-0.4 * k, py - 0.12 * k, hipZ + 0.3 * k], [0.08 * k, py + 0.24 * k, hipZ + 0.1 * k]] : [[-0.06 * k, lap[1], lap[2]], [0.06 * k, lap[1], lap[2]]];
      break;
    case 'elbowsOnKnees':
      out.hands = [[-0.04 * k, kneeY + 0.02 * k, hipZ + 0.06 * k], [0.04 * k, kneeY + 0.04 * k, hipZ + 0.08 * k]];
      break;
    case 'desk': {
      const z = (seat.desk ?? hipZ + 0.3) + 0.035 * k;
      // typing / writing
      const a = Math.sin(t * 7) * 0.012 * k, b = Math.sin(t * 6.3 + 1) * 0.012 * k;
      out.hands = [[-0.14 * k, py + 0.46 * k + a, z], [0.14 * k, py + 0.44 * k + b, z]];
      break;
    }
    case 'crossLegs':
      out.hands = [[-0.06 * k, kneeY - 0.02 * k, hipZ + 0.12 * k], [0.02 * k, kneeY + 0.02 * k, hipZ + 0.14 * k]];
      break;
    default:
      out.hands = [[-0.14 * k, py + 0.24 * k, hipZ + 0.08 * k], [0.14 * k, py + 0.24 * k, hipZ + 0.08 * k]];
  }
  out.turn = 0.6;
  return out;
}

/** Sitting on the ground at the root. */
export function groundSample(d: Dims, variant: GroundVariant, t: number, out: StanceSample): StanceSample {
  const k = d.k;
  const breathe = Math.sin(t * 1.6) * 0.015;
  const [l, r] = out.feet;
  switch (variant) {
    case 'cross':
      out.pelvisPos = [0, 0, 0.16 * k];
      out.pelvisRot = qx(0.15);
      out.spine = qx(-0.1 + breathe);
      out.chest = qx(-0.02);
      out.neck = qx(0.05);
      out.head = qx(0);
      l.ankle = [0.08 * k, 0.3 * k, 0.07 * k];
      r.ankle = [-0.09 * k, 0.36 * k, 0.09 * k];
      l.rot = qmul(qz(-1.3), qeuler(0, -0.6, 0));
      r.rot = qmul(qz(1.3), qeuler(0, 0.6, 0));
      l.pole = [-1, 0.25, 0.25];
      r.pole = [1, 0.25, 0.25];
      out.hands = [[-0.24 * k, 0.28 * k, 0.24 * k], [0.24 * k, 0.28 * k, 0.24 * k]];
      break;
    case 'legsOut':
      out.pelvisPos = [0, 0, 0.14 * k];
      out.pelvisRot = qx(0.35);
      out.spine = qx(0.1 + breathe);
      out.chest = qx(0.05);
      out.neck = qx(-0.3);
      out.head = qx(-0.1);
      l.ankle = [-0.15 * k, 0.78 * k, 0.07 * k];
      r.ankle = [0.13 * k, 0.74 * k, 0.07 * k];
      l.rot = qmul(qz(0.3), qx(0.9));
      r.rot = qmul(qz(-0.3), qx(0.9));
      l.pole = [-0.2, 0.2, 1];
      r.pole = [0.2, 0.2, 1];
      out.hands = [[-0.22 * k, -0.28 * k, 0.03 * k], [0.22 * k, -0.28 * k, 0.03 * k]];
      break;
    default: // knees up, arms round them
      out.pelvisPos = [0, 0, 0.14 * k];
      out.pelvisRot = qx(0.28);
      out.spine = qx(-0.32 + breathe);
      out.chest = qx(-0.22);
      out.neck = qx(0.25);
      out.head = qx(0.1);
      l.ankle = [-0.12 * k, 0.42 * k, d.ankleH];
      r.ankle = [0.12 * k, 0.42 * k, d.ankleH];
      l.rot = flat(0.1);
      r.rot = flat(-0.1);
      l.pole = [-0.2, 0.3, 1];
      r.pole = [0.2, 0.3, 1];
      out.hands = [[-0.08 * k, 0.4 * k, 0.34 * k], [0.08 * k, 0.4 * k, 0.36 * k]];
  }
  l.toe = 0;
  r.toe = 0;
  out.turn = 0.55;
  return out;
}

/** Lying knocked down: on the back (head towards -y) or face down (head towards +y). */
export function downSample(d: Dims, back: boolean, t: number, out: StanceSample): StanceSample {
  const k = d.k;
  const breathe = Math.sin(t * 2.4) * 0.02;
  const [l, r] = out.feet;
  if (back) {
    out.pelvisPos = [0, 0, 0.11 * k];
    out.pelvisRot = qx(Math.PI / 2 - 0.06);
    out.spine = qx(-0.04 + breathe);
    out.chest = qx(-0.04);
    out.neck = qx(-0.35);
    out.head = qeuler(-0.15, 0, 0.3);
    l.ankle = [-0.16 * k, 0.84 * k, 0.08 * k];
    r.ankle = [0.12 * k, 0.5 * k, d.ankleH];
    l.rot = qmul(qz(0.4), qx(Math.PI / 2 - 0.3));
    r.rot = flat(-0.2);
    l.pole = [-0.3, 0.1, 1];
    r.pole = [0.3, 0.2, 1];
    out.hands = [[-0.5 * k, -0.3 * k, 0.05 * k], [0.45 * k, -0.1 * k, 0.05 * k]];
  } else {
    out.pelvisPos = [0, 0, 0.11 * k];
    out.pelvisRot = qx(-Math.PI / 2 + 0.03);
    out.spine = qx(0.02 + breathe);
    out.chest = qx(0.06);
    out.neck = qeuler(0.2, 0, 0.9);
    out.head = qeuler(0.05, 0, 0.5);
    l.ankle = [-0.2 * k, -0.84 * k, 0.09 * k];
    r.ankle = [0.14 * k, -0.8 * k, 0.09 * k];
    l.rot = qx(-Math.PI / 2 - 0.35);
    r.rot = qx(-Math.PI / 2 - 0.35);
    l.pole = [-0.2, 0.1, -1];
    r.pole = [0.2, 0.1, -1];
    out.hands = [[-0.3 * k, 0.52 * k, 0.05 * k], [0.28 * k, -0.05 * k, 0.05 * k]];
  }
  l.toe = 0.3;
  r.toe = 0.3;
  out.turn = 0.15;
  return out;
}

/** The graph of stance transitions: a transition is a blend of two neighbours. */
const EDGES: Readonly<Record<Stance, readonly Stance[]>> = {
  stand: ['kneel', 'sit', 'down'],
  kneel: ['stand', 'prone', 'ground'],
  prone: ['kneel', 'down'],
  sit: ['stand'],
  ground: ['kneel', 'down'],
  down: ['ground'],
};

/** Transition durations (s). */
export function transitionTime(from: Stance, to: Stance): number {
  const key = `${from}>${to}`;
  const t: Record<string, number> = {
    'stand>kneel': 0.45, 'kneel>stand': 0.5, 'kneel>prone': 0.85, 'prone>kneel': 0.8, 'stand>sit': 1.1, 'sit>stand': 1.0,
    'kneel>ground': 0.7, 'ground>kneel': 0.75, 'stand>down': 0.55, 'prone>down': 0.4, 'down>ground': 0.9, 'ground>down': 0.5,
  };
  return t[key] ?? 0.6;
}

/** Intermediate stances from `from` to `to` (the path excludes `from`). */
export function stanceRoute(from: Stance, to: Stance): Stance[] {
  if (from === to) return [];
  // falling goes straight down from anywhere
  if (to === 'down') return ['down'];
  const prev = new Map<Stance, Stance>();
  const q: Stance[] = [from];
  const seen = new Set<Stance>([from]);
  while (q.length > 0) {
    const s = q.shift()!;
    if (s === to) break;
    for (const n of EDGES[s]) {
      if (seen.has(n)) continue;
      seen.add(n);
      prev.set(n, s);
      q.push(n);
    }
  }
  const path: Stance[] = [];
  for (let s: Stance | undefined = to; s && s !== from; s = prev.get(s)) path.unshift(s);
  return path;
}
