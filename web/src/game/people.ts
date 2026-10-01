/**
 * The characters as the front end sees them (protocol `characters` messages): the drive city's
 * people, and the gibs that come off them, posed by their skin matrices one batch behind, between
 * the last two samples, like the rigid pieces (render/islands.ts) and the vehicles (vehicles.ts),
 * by the same clock (engine/poseclock.ts) - a car that runs someone over is drawn where it hits them. They fade in as they
 * come and out as they go (the engine brings people in beyond the near radius and takes them away
 * out of range, and the longest dead when there are too many).
 */
import { PoseClock } from '../engine/poseclock.ts';
import type { Vec3 } from '../engine/protocol.ts';
import { CHARACTER_BONES, CHARACTER_SKIN_FLOATS, CHARACTER_STRIDE, CharacterFlag } from '../engine/protocol.ts';

const FADE_IN_S = 0.6;
const FADE_OUT_S = 0.6;
/**
 * The humanoid rig's feet and toes (anim/src/rig.cpp): one planted flat has its matrix's
 * translation on the ground (the rest pose stands on z = 0), whatever the root does - a physical
 * body's rides with its pelvis.
 */
const SOLE_BONES: readonly number[] = [16, 17, 20, 21];

/** A character posed for a frame: what the renderer draws (render/characters.ts, CharacterDraw). */
export interface CharacterPose {
  mesh: number;
  palette: number;
  /** CHARACTER_SKIN_FLOATS: its bones' matrices, rest model space -> world. */
  skin: Float32Array;
  /** The matrices that count: CHARACTER_BONES, a gib's first alone. */
  bones: number;
  /** Bounding sphere (world). */
  centre: Vec3;
  radius: number;
  /** 0..1, dithered. */
  opacity: number;
  /** 0..1: a hit's flash. */
  flash: number;
  /** On its feet: where its blob shadow goes, on the ground under it (null: none). */
  shadow: Vec3 | null;
  /** Its prop's mesh (0: none) and matrix. */
  propMesh: number;
  prop: Float32Array | null;
}

interface Sample {
  t: number;
  /** Views into the message's arrays (CHARACTER_SKIN_FLOATS; the prop's 16). */
  skin: Float32Array;
  prop: Float32Array | null;
}

export interface CharacterState {
  id: number;
  mesh: number;
  palette: number;
  /** CharacterFlag bits. */
  flags: number;
  /** Bounding sphere (world), as the engine last said. */
  centre: Vec3;
  radius: number;
  /** 0..1: a hit's flash. */
  flash: number;
  /** 0..1 of its full health (0 from engines that do not say). */
  health: number;
  propMesh: number;
  prev: Sample | null;
  cur: Sample;
  /** When it came, and when it went (-1: the engine still has it). */
  born: number;
  gone: number;
  seen: number;
  /** What is drawn of it (its skin posed each frame), and its shadow's place. */
  draw: CharacterPose;
  shadowAt: Vec3;
}

function lerpInto(out: Float32Array, a: Float32Array, b: Float32Array, s: number, n: number): void {
  for (let i = 0; i < n; i++) out[i] = a[i]! + (b[i]! - a[i]!) * s;
}

export class CharacterTracker {
  readonly characters = new Map<number, CharacterState>();
  private readonly list: CharacterPose[] = [];
  private stamp = 0;

  /** clock: when poses are drawn (the page's, shared with the pieces and the vehicles). */
  private readonly clock: PoseClock;

  constructor(clock = new PoseClock()) {
    this.clock = clock;
  }

  clear(): void {
    this.characters.clear();
    this.list.length = 0;
  }

  /** The characters (and gibs) the engine has now: not those fading out. */
  get size(): number {
    let n = 0;
    for (const c of this.characters.values()) if (c.gone < 0) n++;
    return n;
  }

  apply(data: Float64Array, skin: Float32Array, props: Float32Array | undefined, nowS: number): void {
    const stamp = ++this.stamp;
    const n = Math.floor(data.length / CHARACTER_STRIDE);
    for (let k = 0; k < n; k++) {
      const o = k * CHARACTER_STRIDE;
      const f = (i: number): number => data[o + i]!;
      const id = f(0);
      const cur: Sample = {
        t: nowS,
        skin: skin.subarray(k * CHARACTER_SKIN_FLOATS, (k + 1) * CHARACTER_SKIN_FLOATS),
        prop: props ? props.subarray(k * 16, k * 16 + 16) : null,
      };
      let c = this.characters.get(id);
      if (!c) {
        c = {
          id,
          mesh: 0,
          palette: 0,
          flags: 0,
          centre: [0, 0, 0],
          radius: 1,
          flash: 0,
          health: 0,
          propMesh: 0,
          prev: null,
          cur,
          born: nowS,
          gone: -1,
          seen: stamp,
          draw: {
            mesh: 0,
            palette: 0,
            skin: new Float32Array(CHARACTER_SKIN_FLOATS),
            bones: CHARACTER_BONES,
            centre: [0, 0, 0],
            radius: 1,
            opacity: 0,
            flash: 0,
            shadow: null,
            propMesh: 0,
            prop: null,
          },
          shadowAt: [0, 0, 0],
        };
        this.characters.set(id, c);
      } else {
        // (after a gap: from the new sample on)
        c.prev = nowS - c.cur.t > 4 * this.clock.interval ? { ...cur, t: nowS - this.clock.interval } : c.cur;
        c.cur = cur;
        c.gone = -1;
      }
      c.mesh = f(1);
      c.palette = f(2);
      c.flags = f(3);
      c.centre = [f(4), f(5), f(6)];
      c.radius = f(7);
      c.flash = f(8);
      c.propMesh = f(9);
      c.health = f(10);
      c.seen = stamp;
    }
    for (const c of this.characters.values()) if (c.seen !== stamp && c.gone < 0) c.gone = nowS;
  }

  /**
   * The characters to draw at `nowS`: posed a batch behind (interpolated), fading as they come and
   * go (the gone keep their last pose while they fade).
   */
  frame(nowS: number): readonly CharacterPose[] {
    const list = this.list;
    list.length = 0;
    for (const [id, c] of this.characters) {
      let opacity = Math.min(1, (nowS - c.born) / FADE_IN_S);
      if (c.gone >= 0) {
        const left = 1 - (nowS - c.gone) / FADE_OUT_S;
        if (left <= 0) {
          this.characters.delete(id);
          continue;
        }
        opacity = Math.min(opacity, left);
      }
      const d = c.draw;
      const gib = (c.flags & CharacterFlag.Gib) !== 0;
      d.bones = gib ? 1 : CHARACTER_BONES;
      const n = d.bones * 16;
      const a = c.prev;
      const b = c.cur;
      const s = a ? this.clock.weight(a.t, b.t, nowS) : 1;
      if (a && s < 1) lerpInto(d.skin, a.skin, b.skin, s, n);
      else d.skin.set(b.skin.subarray(0, n));
      if (b.prop) {
        d.prop ??= new Float32Array(16);
        if (a?.prop && s < 1) lerpInto(d.prop, a.prop, b.prop, s, 16);
        else d.prop.set(b.prop);
      }
      d.mesh = c.mesh;
      d.palette = c.palette;
      d.centre = c.centre;
      d.radius = c.radius;
      d.opacity = Math.max(0, opacity);
      d.flash = c.flash;
      d.propMesh = b.prop ? c.propMesh : 0;
      d.shadow = null;
      if (!gib && (c.flags & CharacterFlag.Alive) !== 0 && (c.flags & CharacterFlag.Down) === 0) {
        // under its root, at its lowest sole
        const p = c.shadowAt;
        p[0] = d.skin[12]!;
        p[1] = d.skin[13]!;
        p[2] = Infinity;
        for (const bone of SOLE_BONES) p[2] = Math.min(p[2], d.skin[bone * 16 + 14]!);
        d.shadow = p;
      }
      list.push(d);
    }
    return list;
  }
}
