/**
 * Fight choreography for hand-to-hand brawls and knife fights between two characters: keeping
 * range and circling, choosing strikes by distance (jabs, crosses, hooks, uppercuts, front and
 * roundhouse kicks; stabs and slashes with a knife), combinations, blocking what the opponent
 * throws, backing off a downed opponent, and resolving what lands: a strike event of the
 * motion plan becomes a hit where the fist, foot or blade met the opponent's body, with a force by
 * strike (a jab stings, a roundhouse to the head drops people).
 *
 * The host moves the fighters: `move` is the desired velocity (world, m/s) and `yaw` the
 * facing; while a fighter's body leads (knocked back, down) the host follows its root motion.
 */
import type { Character, WoundResult } from '../character.ts';
import { Rng } from '../math/random.ts';
import { clamp, vdist, vnorm, vsub, type V3 } from '../math/vec.ts';
import { KNIFE_ATTACKS } from '../motion/actions.ts';
import type { AnimEvent } from '../motion/plan.ts';
import { H } from './rig.ts';

export interface BrawlerOptions {
  /** 0 cautious .. 1 relentless. */
  aggression?: number;
  /** Chance to block a strike it sees coming. */
  skill?: number;
  seed?: number;
}

export interface LandedBlow {
  attacker: Character;
  victim: Character;
  point: V3;
  dir: V3;
  kind: 'blunt' | 'blade';
  blocked: boolean;
  result: WoundResult & { zone: string };
}

const FORCE: Record<string, number> = { jab: 0.7, cross: 1.2, hook: 1.45, uppercut: 1.55, frontKick: 1.9, roundhouse: 2.3, stab: 1.1, slash: 0.9, gutStab: 1.2, forehandSlash: 0.95, riflePush: 1.3 };
/** Where each strike aims: bone. */
const AIM: Record<string, number> = { jab: H.head, cross: H.head, hook: H.head, uppercut: H.head, frontKick: H.spine, roundhouse: H.chest, stab: H.spine, slash: H.chest, gutStab: H.spine, forehandSlash: H.chest, riflePush: H.chest };

export class Brawler {
  readonly self: Character;
  opponent: Character | null = null;
  readonly move: V3 = [0, 0, 0];
  yaw = 0;
  private readonly rng: Rng;
  private readonly aggression: number;
  private readonly skill: number;
  private cooldown = 0.6;
  private circle = 1;
  private circleT = 0;
  private reacted = '';
  private lastStrike = '';

  constructor(self: Character, opts: BrawlerOptions = {}) {
    this.self = self;
    this.rng = new Rng((opts.seed ?? 1) * 977 + 3);
    this.aggression = opts.aggression ?? 0.5;
    this.skill = opts.skill ?? 0.35;
  }

  private get knife(): boolean {
    return this.self.weapon?.kind === 'knife';
  }

  /** The opponent's body point a strike aims at. */
  private aimPoint(strike: string): V3 {
    const o = this.opponent!;
    const b = AIM[strike] ?? H.chest;
    const w = o.pose;
    if (b === H.head) {
      const h = o.model.skeleton.restHead[H.head]!;
      return w.pointOf(H.head, [h[0], h[1] + 0.06, h[2] + 0.07]);
    }
    const p = w.p[b]!;
    const q = w.q[b]!;
    // the front of the body
    const f = [2 * (q[0] * q[1] - q[2] * q[3]), 1 - 2 * (q[0] * q[0] + q[2] * q[2]), 2 * (q[1] * q[2] + q[0] * q[3])];
    return [p[0] + f[0]! * 0.1, p[1] + f[1]! * 0.1, p[2] + f[2]! * 0.1 + 0.06];
  }

  update(dt: number): void {
    const me = this.self;
    const a = me.motion;
    const o = this.opponent;
    this.move[0] = 0;
    this.move[1] = 0;
    if (!me.alive || !o) {
      a.input.guard = false;
      return;
    }
    const d = vdist(a.rootPos, o.motion.rootPos);
    const to = vsub(o.motion.rootPos, a.rootPos);
    this.yaw = Math.atan2(to[1], to[0]);
    const oDown = !o.alive || o.down;
    a.input.guard = !oDown || d < 2.5;
    a.input.lookAt = o.eyes();
    // (knocked about, it fights on once its feet are under it again)
    const bh = me.behaviours;
    const feet = a.feetPlanner.feet;
    if (me.controlled && !(bh.mode === 'reacting' && bh.balanceError < 0 && feet[0].planted && feet[1].planted)) return;
    const want = oDown ? 1.7 : this.knife ? 0.82 : this.lastStrike === 'frontKick' || this.lastStrike === 'roundhouse' ? 1.08 : 0.92;
    // close or open the distance, circle to the side
    this.circleT -= dt;
    if (this.circleT <= 0) {
      this.circleT = 1.5 + this.rng.next() * 2.5;
      this.circle = this.rng.chance(0.5) ? 1 : -1;
    }
    const n = vnorm([to[0], to[1], 0]);
    const radial = clamp((d - want) * 2.8, -1.4, 1.6);
    const lateral = (oDown ? 0 : 0.45) * this.circle;
    this.move[0] = n[0] * radial - n[1] * lateral;
    this.move[1] = n[1] * radial + n[0] * lateral;
    // react to what the opponent throws
    const theirs = o.motion.actionName;
    if (theirs && theirs !== this.reacted && FORCE[theirs.replace('.m', '')] && !a.busy) {
      this.reacted = theirs;
      const r = this.rng.next();
      if (r < this.skill) a.play('block');
      else if (r < this.skill + 0.2) {
        // step back out of it
        this.move[0] -= n[0] * 1.6;
        this.move[1] -= n[1] * 1.6;
      }
    }
    if (!theirs) this.reacted = '';
    // attack
    this.cooldown -= dt;
    if (!oDown && this.cooldown <= 0 && !a.busy && d < want + 0.3) {
      let strike: string;
      if (this.knife) strike = this.rng.pick([...KNIFE_ATTACKS]);
      else if (this.lastStrike === 'jab' && this.rng.chance(0.55)) strike = 'cross';
      else if (d > 1.05) strike = this.rng.pick(['frontKick', 'roundhouse', 'jab']);
      else strike = this.rng.pick(['jab', 'jab', 'cross', 'hook', 'uppercut', 'frontKick']);
      // (not off one leg while still finding its feet)
      if (me.controlled && (strike === 'frontKick' || strike === 'roundhouse')) strike = this.rng.pick(['jab', 'cross']);
      if (me.weapon && me.weapon.kind !== 'knife' && me.weapon.kind !== 'pistol') strike = 'riflePush';
      a.play(strike, this.aimPoint(strike));
      this.lastStrike = strike;
      const combo = strike === 'jab' ? 0.15 : 0;
      this.cooldown = combo || (0.45 + this.rng.next() * 1.1) * (1.4 - this.aggression);
    }
    if (a.busy && a.actionName) {
      const base = a.actionName.replace('.m', '');
      if (AIM[base] !== undefined) a.aimAction(this.aimPoint(base));
    }
  }

  /** Resolves this fighter's animation events: strikes that reach the opponent land. */
  resolve(events: readonly AnimEvent[]): LandedBlow[] {
    const out: LandedBlow[] = [];
    const o = this.opponent;
    if (!o) return out;
    for (const e of events) {
      if (e.name !== 'strike') continue;
      const base = e.action.replace('.m', '');
      const kind = base === 'stab' || base === 'slash' || base === 'gutStab' || base === 'forehandSlash' ? 'blade' : 'blunt';
      // where the limb is against the opponent's body
      const bone = o.nearestBone(e.pos);
      const bp = o.pose.p[bone]!;
      const tail = o.pose.tail(bone);
      const mid: V3 = [(bp[0] + tail[0]) / 2, (bp[1] + tail[1]) / 2, (bp[2] + tail[2]) / 2];
      const reach = e.limb === 'footR' || e.limb === 'footL' ? 0.38 : 0.3;
      if (vdist(e.pos, mid) > reach + (bone === H.chest || bone === H.spine ? 0.12 : 0)) continue;
      const dir = vnorm(vsub(e.target ?? mid, this.self.pose.p[H.chest]!));
      const blocking = o.motion.actionName === 'block' && (bone === H.head || bone === H.neck || bone === H.chest || (bone >= H.upperarmL && bone <= H.handR));
      const force = (FORCE[base] ?? 1) * (blocking ? 0.3 : 1);
      const result = o.melee(e.pos, dir, blocking ? 'blunt' : kind, force);
      out.push({ attacker: this.self, victim: o, point: [...e.pos] as V3, dir, kind, blocked: blocking, result });
    }
    return out;
  }
}
