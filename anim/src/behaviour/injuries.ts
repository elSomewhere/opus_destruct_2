/**
 * Injuries: where a body was hurt and how badly, and what that does to how it holds itself.
 *
 * Every wound is kept on the physical body part it hit (a point in the part's frame), with a
 * severity that stings at first and settles into a lasting share: a leg wound leaves a limp and
 * a weak knee, an arm wound a weak arm, a trunk wound a hunch and pain; hands go to fresh
 * wounds and keep pressing on serious ones.
 */
import type { V3 } from '../math/vec.ts';
import { B } from '../body/humanoid.ts';

export type HitKind = 'bullet' | 'blunt' | 'blade' | 'blast';
export type Zone = 'head' | 'chest' | 'gut' | 'pelvis' | 'armL' | 'armR' | 'legL' | 'legR';

export interface HitInfo {
  /** Where the blow lands and the direction it travels (world). */
  point: V3;
  dir: V3;
  /** ~1 a rifle round or a punch; 0.6 a pistol round; 1.8 a kick; 2.5 a shotgun; blasts up to 6. */
  force: number;
  kind: HitKind;
  /** The rig bone struck, if known (else the nearest to the point). */
  bone?: number;
}

export interface Injury {
  /** Physical body part and the wound's point on it (body frame, relative to its centre of mass). */
  part: number;
  local: V3;
  /** Surface normal at the wound (body frame). */
  normal: V3;
  zone: Zone;
  kind: HitKind;
  /** 0..1 now (stings, then settles to `lasting`). */
  severity: number;
  lasting: number;
  /** Seconds since it happened. */
  age: number;
  /** A hand stays on it until this age (s). */
  holdUntil: number;
}

export function zoneOfPart(part: number): Zone {
  switch (part) {
    case B.head:
      return 'head';
    case B.chest:
      return 'chest';
    case B.spine:
      return 'gut';
    case B.pelvis:
      return 'pelvis';
    case B.upperarmL:
    case B.forearmL:
    case B.handL:
      return 'armL';
    case B.upperarmR:
    case B.forearmR:
    case B.handR:
      return 'armR';
    case B.thighL:
    case B.shinL:
    case B.footL:
      return 'legL';
    default:
      return 'legR';
  }
}

/** The injuries of one body and their summary. */
export class Injuries {
  readonly list: Injury[] = [];
  /** Summaries (0..1), updated by `update`. */
  legL = 0;
  legR = 0;
  armL = 0;
  armR = 0;
  trunk = 0;
  head = 0;
  /** Overall pain (0..1): hunched, slower, breathing hard. */
  pain = 0;

  add(i: Injury): void {
    this.list.push(i);
    if (this.list.length > 12) this.list.shift();
  }

  update(dt: number): void {
    this.legL = this.legR = this.armL = this.armR = this.trunk = this.head = 0;
    let pain = 0;
    for (const i of this.list) {
      i.age += dt;
      // the sting fades into the lasting share over half a minute
      const settle = Math.exp(-i.age / 20);
      // (the hurt sinks in over a moment: the blow shows first)
      const s = (i.lasting + (i.severity - i.lasting) * settle) * Math.min(1, i.age / 0.35);
      const add = (v: number): number => Math.min(1, v + s * (1 - v));
      switch (i.zone) {
        case 'legL':
          this.legL = add(this.legL);
          break;
        case 'legR':
          this.legR = add(this.legR);
          break;
        case 'armL':
          this.armL = add(this.armL);
          break;
        case 'armR':
          this.armR = add(this.armR);
          break;
        case 'head':
          this.head = add(this.head);
          break;
        default:
          this.trunk = add(this.trunk);
      }
      pain = Math.min(1, pain + s * 0.6 * (1 - pain));
    }
    this.pain = pain;
  }

  /** The wound a hand should be on now (the worst fresh or serious one), or null. */
  toHold(): Injury | null {
    let best: Injury | null = null;
    for (const i of this.list) {
      if (i.age > i.holdUntil || i.kind === 'blunt') continue;
      if (!best || i.severity > best.severity) best = i;
    }
    return best;
  }
}
