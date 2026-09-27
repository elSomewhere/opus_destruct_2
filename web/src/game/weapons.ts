/**
 * Weapons, mapped onto the engine's damage commands (docs/API.md):
 * - pistol and shotgun are hitscan: `raycast`, then `carve` a small sphere at the hit;
 * - the rocket launcher fires a visible projectile. It flies straight, so its path is
 *   verified with look-ahead `raycast`s along its line; when it reaches the first hit it
 *   explodes locally at once (effects within a frame) and sends `blast`.
 */
import type { EngineClient } from '../engine/client.ts';
import type { RaycastHit, Vec3 } from '../engine/protocol.ts';
import { cross, normalize } from '../render/math.ts';
import type { Effects } from './effects.ts';

export type WeaponId = 'pistol' | 'shotgun' | 'rocket';

export interface WeaponDef {
  id: WeaponId;
  name: string;
  /** KeyboardEvent.code that selects it. */
  key: string;
  /** Seconds between shots. */
  cooldown: number;
  /** Keeps firing while the button is held. */
  auto: boolean;
}

export const WEAPONS: readonly WeaponDef[] = [
  { id: 'pistol', name: 'Pistol', key: 'Digit1', cooldown: 0.16, auto: false },
  { id: 'shotgun', name: 'Shotgun', key: 'Digit2', cooldown: 0.75, auto: false },
  { id: 'rocket', name: 'Rocket launcher', key: 'Digit3', cooldown: 0.7, auto: true },
];

export const HITSCAN_RANGE = 250;
export const PISTOL_CARVE_RADIUS = 0.15;
export const SHOTGUN_PELLETS = 8;
export const SHOTGUN_CARVE_RADIUS = 0.12;
export const SHOTGUN_SPREAD = 0.065;
export const ROCKET_SPEED = 25;
export const ROCKET_BLAST_RADIUS = 1.0;
/** Joules passed with `blast` (roughly a quarter kilogram of TNT). */
export const ROCKET_ENERGY_J = 1.0e6;
const ROCKET_LIFETIME = 6;
/** Length of each look-ahead raycast and how far ahead the path is kept verified. */
const ROCKET_SEGMENT = 8;
const ROCKET_LOOKAHEAD = 3;

interface Rocket {
  origin: Vec3;
  dir: Vec3;
  /** Distance flown along dir. */
  travelled: number;
  /** Path verified free of geometry up to this distance. */
  checked: number;
  pending: boolean;
  /** Distance to the first hit, once known. */
  impact: number | null;
  age: number;
  dead: boolean;
}

function along(r: Rocket, d: number): Vec3 {
  return [r.origin[0] + r.dir[0] * d, r.origin[1] + r.dir[1] * d, r.origin[2] + r.dir[2] * d];
}

/** Direction jittered inside a cone of half-angle ~`spread` radians. */
function jitter(dir: Vec3, spread: number): Vec3 {
  if (spread <= 0) return dir;
  const right = normalize(cross(dir, Math.abs(dir[2]) < 0.99 ? [0, 0, 1] : [1, 0, 0]));
  const up = cross(right, dir);
  const r = spread * Math.sqrt(Math.random());
  const a = Math.random() * Math.PI * 2;
  const x = Math.cos(a) * r;
  const y = Math.sin(a) * r;
  return normalize([dir[0] + right[0] * x + up[0] * y, dir[1] + right[1] * x + up[1] * y, dir[2] + right[2] * x + up[2] * y]);
}

export class Weapons {
  private readonly engine: EngineClient;
  private readonly effects: Effects;
  private readonly rockets: Rocket[] = [];
  private cooldown = 0;
  current: WeaponDef = WEAPONS[0]!;
  /** Shots fired (all weapons), for the HUD. */
  shots = 0;

  constructor(engine: EngineClient, effects: Effects) {
    this.engine = engine;
    this.effects = effects;
  }

  select(id: WeaponId): void {
    const w = WEAPONS.find((d) => d.id === id);
    if (w && w !== this.current) {
      this.current = w;
      this.cooldown = Math.max(this.cooldown, 0.15);
    }
  }

  cycle(step: number): void {
    const i = WEAPONS.indexOf(this.current);
    const n = WEAPONS.length;
    this.select(WEAPONS[(((i + step) % n) + n) % n]!.id);
  }

  get liveRockets(): number {
    return this.rockets.length;
  }

  /**
   * @param held trigger held this frame
   * @param clicked trigger pressed this frame (edge)
   */
  update(dt: number, held: boolean, clicked: boolean, eye: Vec3, forward: Vec3): void {
    this.cooldown = Math.max(0, this.cooldown - dt);
    const wants = this.current.auto ? held : clicked;
    if (wants && this.cooldown <= 0) {
      this.fire(eye, forward);
      this.cooldown = this.current.cooldown;
    }
    this.updateRockets(dt);
  }

  /** Fires the current weapon along `forward` from `eye` (also used by the debug API). */
  fire(eye: Vec3, forward: Vec3): void {
    this.shots++;
    const muzzle: Vec3 = [eye[0] + forward[0] * 0.5, eye[1] + forward[1] * 0.5, eye[2] + forward[2] * 0.5 - 0.1];
    this.effects.muzzleFlash(muzzle);
    switch (this.current.id) {
      case 'pistol':
        this.hitscan(eye, jitter(forward, 0.004), PISTOL_CARVE_RADIUS);
        this.effects.addTrauma(0.05);
        break;
      case 'shotgun':
        for (let k = 0; k < SHOTGUN_PELLETS; k++) this.hitscan(eye, jitter(forward, SHOTGUN_SPREAD), SHOTGUN_CARVE_RADIUS);
        this.effects.addTrauma(0.18);
        break;
      case 'rocket':
        this.rockets.push({
          origin: [eye[0], eye[1], eye[2] - 0.12],
          dir: normalize(forward),
          travelled: 0,
          checked: 0,
          pending: false,
          impact: null,
          age: 0,
          dead: false,
        });
        this.effects.addTrauma(0.12);
        break;
    }
  }

  private hitscan(eye: Vec3, dir: Vec3, radius: number): void {
    this.engine
      .raycast(eye, dir, HITSCAN_RANGE)
      .then((hit: RaycastHit | null) => {
        if (!hit) return;
        this.engine.carve(hit.pos, radius);
        this.effects.bulletImpact(hit);
      })
      .catch(() => undefined);
  }

  private updateRockets(dt: number): void {
    for (const r of this.rockets) {
      r.age += dt;
      const next = r.travelled + ROCKET_SPEED * dt;
      // Keep the verified-clear path ahead of the rocket (one request in flight).
      if (!r.pending && r.impact === null && r.checked < next + ROCKET_LOOKAHEAD) {
        const from = r.checked;
        r.pending = true;
        this.engine
          .raycast(along(r, from), r.dir, ROCKET_SEGMENT)
          .then((hit) => {
            r.pending = false;
            if (hit) r.impact = from + hit.distance;
            else r.checked = from + ROCKET_SEGMENT;
          })
          .catch(() => {
            r.dead = true;
          });
      }
      if (r.impact !== null && next >= r.impact) {
        this.explode(along(r, r.impact));
        r.dead = true;
      } else if (r.age > ROCKET_LIFETIME) {
        r.dead = true;
      } else {
        r.travelled = next;
        this.effects.rocketTrail(along(r, r.travelled), r.dir);
      }
    }
    for (let i = this.rockets.length - 1; i >= 0; i--) if (this.rockets[i]!.dead) this.rockets.splice(i, 1);
  }

  private explode(pos: Vec3): void {
    this.effects.explosion(pos, ROCKET_BLAST_RADIUS);
    this.engine.blast(pos, ROCKET_BLAST_RADIUS, ROCKET_ENERGY_J);
  }
}
