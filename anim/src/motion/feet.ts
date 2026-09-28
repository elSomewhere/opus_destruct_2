/**
 * The foot planner: where the feet go, in the world.
 *
 * A gait clock drives the walk and the run: a foot in stance stays where it landed (no sliding
 * at any speed or turn rate); a swinging foot lands where the hip will be at mid-stance of the
 * next step, on the ground the CollisionWorld finds (stairs, rubble). Standing, the feet take
 * unhurried corrective and turning steps.
 *
 * On top of the clock, the body's balance can take steps of its own (`step`): a foot is sent
 * to a point now, with a duration, and the clock waits (a stagger, a catch after a trip, a step
 * away from a blast). A swing clears what lies on its path: the planner samples the ground
 * along it and lifts the foot over the highest point, with a margin that shrinks with
 * inattention (running, panic), so a foot can still catch on a kerb or on rubble (the body's
 * physics finds out).
 */
import { qrotate, qz } from '../math/quat.ts';
import { clamp, fract, lerp, smoothstep, vcopy, vlerp, wrapAngle, type V3 } from '../math/vec.ts';
import type { CollisionWorld } from '../physics/collision.ts';
import type { GaitParams } from './gait.ts';
import type { GaitStyle } from './style.ts';

export interface Foot {
  side: -1 | 1;
  /** Rig bones of the leg. */
  thigh: number;
  shin: number;
  foot: number;
  toe: number;
  /** Phase offset in the gait cycle. */
  offset: number;
  planted: boolean;
  /** Taken over by an action (a kick): the planter leaves it alone. */
  held: boolean;
  /** A step the body's balance asked for (not the gait's). */
  forced: boolean;
  /** Where the foot rests (sole, world) and its heading. */
  pos: V3;
  yaw: number;
  /** The swing: from where, carried with the body from `liftRoot`, to `target` over `swing` 0..1. */
  lift: V3;
  liftRoot: V3;
  liftYaw: number;
  swing: number;
  swingRate: number;
  target: V3;
  targetYaw: number;
  /** Extra height the swing clears (obstacles on its path). */
  clear: number;
  /** Ankle (world) and pitch (heel-toe roll) this frame. */
  ankle: V3;
  pitch: number;
  /** Seconds since it last landed. */
  since: number;
}

export interface FeetDims {
  k: number;
  legLen: number;
  ankleH: number;
  ballFwd: number;
  heelBack: number;
  footX: number;
}

/** What the planner needs to know each frame. */
export interface FeetContext {
  root: Readonly<V3>;
  /** Heading of the lower body. */
  bodyYaw: number;
  vel: Readonly<V3>;
  speed: number;
  gait: GaitParams;
  moving: boolean;
  airborne: boolean;
  crouch: number;
  style: GaitStyle;
  /** Hip joints (world) this frame (a foot stays within the leg's reach). */
  hips: [Readonly<V3>, Readonly<V3>];
  /** 0..1: how much attention the steps get (clearance over obstacles). */
  care: number;
  /** Ground height of the character's root (reference for ground queries). */
  groundZ: number;
}

export class FootPlanner {
  readonly feet: [Foot, Foot];
  readonly dims: FeetDims;
  collision: CollisionWorld;
  /** Things lying about (spheres, world): a swing clears them too, as far as it sees them. */
  obstacles: readonly { c: V3; r: number }[] = [];
  phase = 0;
  /** Standing still, correcting the feet (unhurried steps). */
  stepping = false;
  /** Landed this update (footfalls: the body settles onto the leg). */
  readonly landed: boolean[] = [false, false];
  private wasAirborne = false;

  constructor(dims: FeetDims, collision: CollisionWorld, bones: { thigh: number; shin: number; foot: number; toe: number }[]) {
    this.dims = dims;
    this.collision = collision;
    const foot = (side: -1 | 1, b: { thigh: number; shin: number; foot: number; toe: number }): Foot => ({
      side,
      ...b,
      offset: side < 0 ? 0 : 0.5,
      planted: true,
      held: false,
      forced: false,
      pos: [0, 0, 0],
      yaw: 0,
      lift: [0, 0, 0],
      liftRoot: [0, 0, 0],
      liftYaw: 0,
      swing: 0,
      swingRate: 1,
      target: [0, 0, 0],
      targetYaw: 0,
      clear: 0,
      ankle: [0, 0, 0],
      pitch: 0,
      since: 1,
    });
    this.feet = [foot(-1, bones[0]!), foot(1, bones[1]!)];
  }

  /** Where a foot stands in the default stance around a root. */
  nominal(f: Foot, root: Readonly<V3>, yaw: number, crouch: number, style: GaitStyle, out: V3 = [0, 0, 0]): V3 {
    const d = this.dims;
    const x = f.side * d.footX * style.width * (1 + 0.45 * crouch);
    const o = qrotate(qz(yaw - Math.PI / 2), [x, -0.02 * d.k, 0]);
    out[0] = root[0] + o[0];
    out[1] = root[1] + o[1];
    out[2] = root[2];
    return out;
  }

  ground(x: number, y: number, zRef: number, fallback: number): number {
    const k = this.dims.k;
    const g = this.collision.groundHeight(x, y, zRef + 0.6 * k, zRef - 0.9 * k);
    return g ?? fallback;
  }

  /** Both feet planted in the default stance around the root. */
  reset(root: Readonly<V3>, yaw: number, crouch: number, style: GaitStyle): void {
    for (const f of this.feet) {
      this.nominal(f, root, yaw, crouch, style, f.pos);
      f.pos[2] = this.ground(f.pos[0], f.pos[1], root[2], root[2]);
      f.yaw = yaw - f.side * style.toeOut;
      f.planted = true;
      f.held = false;
      f.forced = false;
      f.swing = 0;
      f.pitch = 0;
      f.clear = 0;
      vcopy(f.pos, f.target);
      this.ankleFromPlant(f.pos, f.yaw, 0, f.ankle);
    }
  }

  /** Plants the feet where the body's feet are (after a fall, a scramble): soles and headings. */
  placeAt(soles: readonly Readonly<V3>[], yaws: readonly number[]): void {
    this.feet.forEach((f, i) => {
      vcopy(soles[i]!, f.pos);
      f.yaw = yaws[i]!;
      f.planted = true;
      f.forced = false;
      f.held = false;
      f.swing = 0;
      f.pitch = 0;
      vcopy(f.pos, f.target);
      this.ankleFromPlant(f.pos, f.yaw, 0, f.ankle);
    });
    this.stepping = false;
  }

  /**
   * A step the balance asks for: foot i swings from where it is to `target` (world sole) in
   * `duration` seconds, heading `yaw` (null: keeps its heading). Re-sending it mid-swing
   * re-targets the swing (the landing point follows the body).
   */
  step(i: number, target: Readonly<V3>, duration: number, yaw: number | null = null): void {
    const f = this.feet[i]!;
    if (f.held) return;
    if (!f.planted) {
      // re-aim a swing under way (the gait's becomes the balance's, from where the foot is)
      if (!f.forced) {
        f.forced = true;
        f.lift = [f.ankle[0], f.ankle[1], f.ankle[2] - this.dims.ankleH];
        vcopy(f.lift, f.liftRoot);
        f.liftYaw = f.yaw;
        f.swing = 0;
        f.swingRate = 1 / Math.max(0.1, duration);
        f.clear = 0;
      }
      vcopy(target, f.target);
      f.target[2] = this.ground(target[0], target[1], target[2], target[2]);
      if (yaw !== null) f.targetYaw = yaw;
      // the rest of the swing in the time left
      f.swingRate = Math.max(f.swingRate, (1 - f.swing) / Math.max(0.06, duration));
      return;
    }
    f.planted = false;
    f.forced = true;
    vcopy(f.pos, f.lift);
    vcopy(f.pos, f.liftRoot);
    f.liftYaw = f.yaw;
    f.swing = 0;
    f.swingRate = 1 / Math.max(0.12, duration);
    vcopy(target, f.target);
    f.target[2] = this.ground(target[0], target[1], target[2], target[2]);
    f.targetYaw = yaw ?? f.yaw;
    f.clear = this.clearance(f.lift, f.target, 0.6);
  }

  /** A swing stopped short (the foot caught on something): it lands where it is. */
  plantNow(i: number, at: Readonly<V3>): void {
    const f = this.feet[i]!;
    vcopy(at, f.pos);
    f.pos[2] = this.ground(at[0], at[1], at[2] + 0.1, at[2]);
    vcopy(f.pos, f.target);
    f.planted = true;
    f.forced = false;
    f.swing = 0;
    f.since = 0;
  }

  /** Height to clear over the path from a to b (m above the higher end, with a margin by care). */
  clearance(a: Readonly<V3>, b: Readonly<V3>, care: number): number {
    const top = Math.max(a[2], b[2]);
    let high = top;
    for (let s = 1; s <= 4; s++) {
      const t = s / 5;
      const x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t;
      const g = this.collision.groundHeight(x, y, top + 0.55 * this.dims.k, top - 0.3 * this.dims.k);
      if (g !== null && g > high) high = g;
      // (a body, a piece of debris on the path)
      for (const o of this.obstacles) {
        const dx = x - o.c[0], dy = y - o.c[1];
        const h2 = o.r * o.r - dx * dx - dy * dy;
        if (h2 <= 0) continue;
        const z = o.c[2] + Math.sqrt(h2);
        if (z > high && z < top + 0.55 * this.dims.k) high = z;
      }
    }
    if (high <= top + 0.02) return 0;
    // a careful foot clears an obstacle by a hand's width; a hurried one barely
    return high - top + lerp(0.015, 0.08, care) * this.dims.k;
  }

  /** Ankle position (world) of a foot planted at `plant` with heading `yaw` and heel-toe pitch. */
  ankleFromPlant(plant: Readonly<V3>, yaw: number, pitch: number, out: V3): V3 {
    const d = this.dims;
    const fwd: V3 = [Math.cos(yaw), Math.sin(yaw), 0];
    let dy = 0;
    let dz = d.ankleH;
    if (pitch < 0) {
      // toe-off: the foot rolls about the ball
      const c = Math.cos(pitch), s = Math.sin(pitch);
      const by = -d.ballFwd, bz = d.ankleH;
      dy = d.ballFwd + by * c - bz * s;
      dz = by * s + bz * c;
    } else if (pitch > 0) {
      // heel strike: about the heel
      const c = Math.cos(pitch), s = Math.sin(pitch);
      const by = d.heelBack, bz = d.ankleH;
      dy = -d.heelBack + by * c - bz * s;
      dz = by * s + bz * c;
    }
    out[0] = plant[0] + fwd[0] * dy;
    out[1] = plant[1] + fwd[1] * dy;
    out[2] = plant[2] + dz;
    return out;
  }

  /**
   * Advances the gait clock (walking), or the corrective steps (standing). Returns the phase
   * before the advance.
   */
  advanceClock(dt: number, c: FeetContext, limp: readonly [number, number]): number {
    const g = c.gait;
    const st = c.style;
    let prev = this.phase;
    const forced = this.feet[0].forced || this.feet[1].forced;
    if (forced) {
      // the balance is stepping: the clock waits
      this.stepping = false;
      return prev;
    }
    if (c.moving) {
      if (!this.stepping) {
        // start with the foot behind
        const dir: V3 = [c.vel[0] / (c.speed || 1), c.vel[1] / (c.speed || 1), 0];
        const d0 = (this.feet[0].pos[0] - c.root[0]) * dir[0] + (this.feet[0].pos[1] - c.root[1]) * dir[1];
        const d1 = (this.feet[1].pos[0] - c.root[0]) * dir[0] + (this.feet[1].pos[1] - c.root[1]) * dir[1];
        const lead = d0 <= d1 ? this.feet[0] : this.feet[1];
        if (this.feet[0].planted && this.feet[1].planted) {
          this.phase = fract(g.duty - 0.02 - lead.offset);
          prev = this.phase;
        }
        this.stepping = true;
      }
      // a limp hurries the step off the wounded leg
      const inStanceL = fract(this.phase) < g.duty;
      const inStanceR = fract(this.phase + 0.5) < g.duty;
      const hurry = 1 + 0.9 * (inStanceL ? limp[0] : 0) + 0.9 * (inStanceR ? limp[1] : 0);
      this.phase = fract(this.phase + g.freq * hurry * dt);
    } else if (!c.airborne) {
      let need = false;
      const nom: V3 = [0, 0, 0];
      for (const f of this.feet) {
        if (f.held) continue;
        if (!f.planted) need = true;
        else {
          this.nominal(f, c.root, c.bodyYaw, c.crouch, st, nom);
          const err = Math.hypot(nom[0] - f.pos[0], nom[1] - f.pos[1]);
          const yawErr = Math.abs(wrapAngle(c.bodyYaw - f.side * st.toeOut - f.yaw));
          if (err > 0.16 * this.dims.k || yawErr > 0.42 || (this.stepping && err > 0.07 * this.dims.k)) need = true;
        }
      }
      // (turning on the spot and settling: unhurried steps)
      if (need) this.phase = fract(this.phase + 1.15 * dt);
      else this.stepping = false;
    }
    return prev;
  }

  /** Moves the feet for this frame. */
  update(dt: number, c: FeetContext, prevPhase: number): void {
    const d = this.dims;
    const k = d.k;
    const st = c.style;
    const g = c.gait;
    this.landed[0] = this.landed[1] = false;
    if (this.wasAirborne && !c.airborne) {
      for (const f of this.feet) {
        this.nominal(f, c.root, c.bodyYaw, c.crouch, st, f.pos);
        f.pos[2] = this.ground(f.pos[0], f.pos[1], c.groundZ, c.groundZ);
        f.yaw = c.bodyYaw - f.side * st.toeOut;
        f.planted = true;
        f.forced = false;
      }
      this.landed[0] = this.landed[1] = true;
    }
    this.wasAirborne = c.airborne;
    const D = g.duty;
    const freq = c.moving ? g.freq : 1.5;
    const stanceT = D / freq;
    const moveAmt = smoothstep(0.1, 0.9, c.speed);
    const forcedAny = this.feet[0].forced || this.feet[1].forced;
    for (let fi = 0; fi < 2; fi++) {
      const f = this.feet[fi]!;
      f.since += dt;
      if (f.held) continue;
      const p0 = fract(prevPhase + f.offset);
      const p1 = fract(this.phase + f.offset);
      const advanced = this.phase !== prevPhase;
      if (c.airborne) f.planted = false;
      else if (f.planted && !forcedAny) {
        const wrapped = p1 < p0;
        const crossedLift = advanced && ((!wrapped && p0 < D && p1 >= D) || (wrapped && p0 < D));
        // a foot still down late in its swing phase (it landed late) goes now, so the feet keep
        // alternating
        const missedLift = c.moving && advanced && p1 > D + 0.08 && p1 < 0.9;
        // a foot left behind out of reach pushes off early (a long running stride shortens the
        // ground contact; a sudden start); it still lands on the gait's beat below
        const far = Math.hypot(f.pos[0] - c.root[0], f.pos[1] - c.root[1]) > 0.62 * d.legLen;
        if (crossedLift || missedLift || far) {
          f.planted = false;
          vcopy(f.pos, f.lift);
          vcopy(c.root, f.liftRoot);
          f.liftYaw = f.yaw;
          f.swing = 0;
          f.clear = -1;
          // land when the gait says this foot lands (phase 1), however it left the ground
          f.swingRate = crossedLift ? freq / (1 - D) : c.moving ? 1 / clamp((1 - p1) / freq, 0.12, 1 / freq) : 1 / 0.2;
        }
      }
      if (!c.airborne && !f.planted) {
        f.swing = Math.min(1, f.swing + f.swingRate * dt);
        if (!f.forced) {
          const remain = (1 - f.swing) / f.swingRate;
          const pred: V3 = [c.root[0] + c.vel[0] * remain, c.root[1] + c.vel[1] * remain, c.root[2]];
          const tgt = this.nominal(f, pred, c.bodyYaw, c.crouch, st);
          const reach = lerp(0.52, 0.34, g.run) * d.legLen;
          const ahead = stanceT * lerp(0.5, 0.36, g.run);
          let hx = c.vel[0] * ahead, hy = c.vel[1] * ahead;
          const hl = Math.hypot(hx, hy);
          if (hl > reach) {
            hx *= reach / hl;
            hy *= reach / hl;
          }
          tgt[0] += hx;
          tgt[1] += hy;
          tgt[2] = this.ground(tgt[0], tgt[1], c.groundZ, c.groundZ);
          vcopy(tgt, f.target);
          // turning on the spot: a step opens the foot at most ~43 degrees past the other one
          let heading = c.bodyYaw;
          if (!c.moving) {
            const o = this.feet[1 - fi]!;
            const oh = o.yaw + o.side * st.toeOut;
            heading = oh + clamp(wrapAngle(c.bodyYaw - oh), -0.75, 0.75);
          }
          f.targetYaw = heading - f.side * st.toeOut;
          // what lies on the path (sampled once, at lift-off)
          if (f.clear < 0) f.clear = this.clearance(f.lift, f.target, c.care);
        }
        if (f.swing >= 1) {
          f.planted = true;
          f.forced = false;
          vcopy(f.target, f.pos);
          f.yaw = f.targetYaw;
          f.since = 0;
          this.landed[fi] = true;
        }
      }
      if (c.airborne) {
        const nom = this.nominal(f, c.root, c.bodyYaw, c.crouch, st);
        nom[2] = c.root[2] + 0.12 * k + (f.side < 0 ? 0.05 : 0) * k;
        vcopy(nom, f.ankle);
        f.pitch = -0.3;
        f.yaw = c.bodyYaw;
        vcopy(nom, f.pos);
        f.pos[2] -= d.ankleH;
        f.lift = vcopy(f.pos);
        vcopy(c.root, f.liftRoot);
        continue;
      }
      if (f.planted) {
        const gz = this.ground(f.pos[0], f.pos[1], f.pos[2] + 0.2 * k, f.pos[2]);
        if (gz < f.pos[2] - 0.01) f.pos[2] = Math.max(gz, f.pos[2] - 3 * dt);
        const u = p1 < D ? p1 / D : 0.3;
        let pitch = 0;
        if (c.moving && !forcedAny) {
          const hs = lerp(0.28, 0.1, g.run) * moveAmt;
          const to = lerp(0.45, 0.65, g.run) * moveAmt;
          pitch = hs * (1 - smoothstep(0, 0.18, u)) - to * smoothstep(0.5, 1.0, u);
        }
        f.pitch = pitch;
        this.ankleFromPlant(f.pos, f.yaw, pitch, f.ankle);
      } else {
        const s = f.swing;
        const run = f.forced ? 0 : g.run;
        const sh = lerp(s, Math.pow(s, 1.6), run);
        const eh = sh * sh * (3 - 2 * sh);
        // the foot leaves the ground moving with the body (a runner's heel kicks up and comes
        // through under the hip; a walker's foot peels off more slowly)
        const carry = c.moving && !f.forced ? lerp(0.35, 1, run) * (1 - eh) : 0;
        const from: V3 = [f.lift[0] + (c.root[0] - f.liftRoot[0]) * carry, f.lift[1] + (c.root[1] - f.liftRoot[1]) * carry, f.lift[2]];
        const hz = vlerp(from, f.target, eh);
        const peak = Math.sin(Math.PI * Math.pow(s, lerp(1, 0.62, run)));
        const baseLift = f.forced ? 0.07 * k : c.moving ? g.lift : 0.06 * k;
        const lift = baseLift * peak + Math.max(0, f.target[2] - f.lift[2]) * 0.3 * peak + Math.max(0, f.clear) * Math.sin(Math.PI * clamp(s * 1.15, 0, 1));
        hz[2] += lift;
        const e = s * s * (3 - 2 * s);
        const yaw = f.liftYaw + wrapAngle(f.targetYaw - f.liftYaw) * e;
        const to = c.moving ? lerp(0.45, 0.65, run) * moveAmt : 0.15;
        const hs = c.moving ? lerp(0.28, 0.1, run) * moveAmt : 0.05;
        f.pitch = lerp(-to, hs, smoothstep(0.15, 0.95, s));
        f.ankle[0] = hz[0];
        f.ankle[1] = hz[1];
        f.ankle[2] = hz[2] + d.ankleH;
        // within the leg's reach of the hip: a foot left behind rises (the heel kicks up)
        const hip = c.hips[fi]!;
        const reach = 0.97 * d.legLen;
        const dx = f.ankle[0] - hip[0], dy = f.ankle[1] - hip[1];
        const hd = Math.hypot(dx, dy);
        if (hd < reach) f.ankle[2] = Math.max(f.ankle[2], hip[2] - Math.sqrt(reach * reach - hd * hd));
        else {
          f.ankle[0] = hip[0] + (dx / hd) * reach * 0.95;
          f.ankle[1] = hip[1] + (dy / hd) * reach * 0.95;
          f.ankle[2] = Math.max(f.ankle[2], hip[2] - reach * 0.31);
        }
        f.yaw = yaw;
      }
    }
  }

  /** The feet (world), for debugging and tests. */
  state(): { planted: boolean; pos: V3; ankle: V3; yaw: number; forced: boolean }[] {
    return this.feet.map((f) => ({ planted: f.planted, pos: [...f.pos] as V3, ankle: [...f.ankle] as V3, yaw: f.yaw, forced: f.forced }));
  }
}
