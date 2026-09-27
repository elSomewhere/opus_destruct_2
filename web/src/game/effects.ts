/**
 * Visual feedback: particles for hits, cracks, impacts and detachments, a transient
 * point light (muzzle flash / explosions) and camera shake ("trauma" model).
 *
 * Engine events can come in floods (a collapse announces hundreds of pieces, every split of
 * a piece is a new detached event, up to 24 cracks per tick), so their particles draw on
 * per-frame budgets, refilled in `update`.
 */
import type { CrackEvent, DetachedEvent, ImpactEvent, MaterialId, RaycastHit, SplashEvent, Vec3 } from '../engine/protocol.ts';
import { FLAME_STRIDE, Material, SMOKE_CELL_VOXELS, SMOKE_STRIDE, VERTEX_STRIDE } from '../engine/protocol.ts';
import { distance, normalize } from '../render/math.ts';
import { FIELD_CAPACITY, FLOATS_PER_INSTANCE, type ParticleSystem } from '../render/particles.ts';

type Rgb = [number, number, number];

const MATERIAL_DUST: Record<number, Rgb> = {
  [Material.Rc]: [0.36, 0.36, 0.36],
  [Material.Concrete]: [0.42, 0.41, 0.38],
  [Material.Steel]: [0.3, 0.33, 0.38],
  [Material.Masonry]: [0.45, 0.25, 0.18],
  [Material.Soil]: [0.3, 0.22, 0.14],
  [Material.Rock]: [0.28, 0.26, 0.24],
  [Material.Bedrock]: [0.1, 0.1, 0.1],
};

/** Particles per frame from detached events (dust) and from cracks (chips and puffs). */
const DETACHED_DUST_PER_FRAME = 300;
const CRACK_PARTICLES_PER_FRAME = 150;
/** Fire: flame tongues per burning voxel and second, and per-frame caps (flames, embers, smoke). */
const FLAME_RATE = 2.5;
const FLAME_PARTICLES_PER_FRAME = 140;
const EMBER_RATE = 0.05;
const FIRE_SMOKE_RATE = 0.08;
const FIRE_SMOKE_PER_FRAME = 6;

function dustColor(material: MaterialId): Rgb {
  return MATERIAL_DUST[material] ?? [0.4, 0.4, 0.4];
}

function rand(a: number, b: number): number {
  return a + Math.random() * (b - a);
}

function randomUnit(): Vec3 {
  const u = rand(-1, 1);
  const phi = rand(0, Math.PI * 2);
  const s = Math.sqrt(1 - u * u);
  return [s * Math.cos(phi), s * Math.sin(phi), u];
}

/** Random direction in the hemisphere around n, biased towards n. */
function hemisphere(n: Vec3, spread: number): Vec3 {
  const r = randomUnit();
  const d: Vec3 = [n[0] + r[0] * spread, n[1] + r[1] * spread, n[2] + r[2] * spread];
  return normalize(d);
}

export class Effects {
  private readonly particles: ParticleSystem;
  /** Camera shake trauma 0..1 (shake amplitude ~ trauma^2). */
  trauma = 0;
  flashPos: Vec3 = [0, 0, 0];
  flashIntensity = 0;
  /** Seconds the on-screen muzzle flash stays visible. */
  muzzle = 0;
  private detachedBudget = DETACHED_DUST_PER_FRAME;
  private crackBudget = CRACK_PARTICLES_PER_FRAME;
  /** The engine's flames (FLAME_STRIDE floats each). */
  private flames: Float32Array = new Float32Array(0);
  /** The engine's smoke cells (SMOKE_STRIDE floats each), and their sprites. */
  private smoke: Float32Array = new Float32Array(0);
  private readonly smokeSprites = new Float32Array(FIELD_CAPACITY * FLOATS_PER_INSTANCE);
  private smokeBuilt = -1;
  private smokeDirty = true;
  private fireCarry = 0;
  private emberCarry = 0;
  private smokeCarry = 0;

  constructor(particles: ParticleSystem) {
    this.particles = particles;
  }

  update(dt: number): void {
    this.detachedBudget = DETACHED_DUST_PER_FRAME;
    this.crackBudget = CRACK_PARTICLES_PER_FRAME;
    this.trauma = Math.max(0, this.trauma - dt * 1.1);
    this.flashIntensity *= Math.exp(-dt * 18);
    if (this.flashIntensity < 0.01) this.flashIntensity = 0;
    this.muzzle = Math.max(0, this.muzzle - dt);
  }

  /** Current shake offsets: yaw/pitch in radians and a positional jitter in metres. */
  shake(timeS: number): { yaw: number; pitch: number; offset: Vec3 } {
    const s = this.trauma * this.trauma;
    if (s <= 0) return { yaw: 0, pitch: 0, offset: [0, 0, 0] };
    const n = (f: number, p: number): number => Math.sin(timeS * f + p) * 0.6 + Math.sin(timeS * f * 2.3 + p * 1.7) * 0.4;
    return {
      yaw: 0.04 * s * n(37, 1.1),
      pitch: 0.04 * s * n(41, 2.3),
      offset: [0.06 * s * n(29, 0.3), 0.06 * s * n(31, 4.1), 0.06 * s * n(33, 2.9)],
    };
  }

  addTrauma(amount: number): void {
    this.trauma = Math.min(1, this.trauma + amount);
  }

  light(pos: Vec3, intensity: number): void {
    if (intensity >= this.flashIntensity) {
      this.flashPos = [...pos];
      this.flashIntensity = intensity;
    }
  }

  muzzleFlash(muzzle: Vec3): void {
    this.muzzle = 0.06;
    this.light(muzzle, 1.6);
    for (let k = 0; k < 3; k++) {
      this.particles.spawn({ pos: muzzle, vel: randomUnit().map((v) => v * 0.5) as Vec3, life: 0.05, size: 0.05, color: [3, 2, 0.8, 1], additive: true, gravity: 0 });
    }
  }

  bulletImpact(hit: RaycastHit): void {
    const n = hit.normal[0] === 0 && hit.normal[1] === 0 && hit.normal[2] === 0 ? ([0, 0, 1] as Vec3) : hit.normal;
    const p: Vec3 = [hit.pos[0] + n[0] * 0.02, hit.pos[1] + n[1] * 0.02, hit.pos[2] + n[2] * 0.02];
    const c = dustColor(hit.material);
    for (let k = 0; k < 6; k++) {
      const d = hemisphere(n, 0.9);
      const v = rand(3, 9);
      this.particles.spawn({ pos: p, vel: [d[0] * v, d[1] * v, d[2] * v], life: rand(0.08, 0.25), size: 0.015, color: [4, 2.6, 1, 1], additive: true, gravity: 1, drag: 2 });
    }
    for (let k = 0; k < 7; k++) {
      const d = hemisphere(n, 1.2);
      const v = rand(1.5, 5);
      this.particles.spawn({ pos: p, vel: [d[0] * v, d[1] * v, d[2] * v], life: rand(0.4, 0.9), size: rand(0.012, 0.03), color: [c[0] * 0.8, c[1] * 0.8, c[2] * 0.8, 1], gravity: 1 });
    }
    for (let k = 0; k < 4; k++) {
      const d = hemisphere(n, 1);
      const v = rand(0.3, 1.2);
      this.particles.spawn({ pos: p, vel: [d[0] * v, d[1] * v, d[2] * v], life: rand(0.8, 1.6), size: rand(0.06, 0.12), grow: 0.25, color: [c[0], c[1], c[2], 0.5], drag: 2.5, gravity: -0.03 });
    }
  }

  explosion(pos: Vec3, radius: number): void {
    this.light(pos, 14);
    for (let k = 0; k < 40; k++) {
      const d = randomUnit();
      const v = rand(2, 9) * radius;
      this.particles.spawn({ pos, vel: [d[0] * v, d[1] * v, d[2] * v], life: rand(0.15, 0.45), size: rand(0.15, 0.4) * radius, grow: 1.2, color: [4, 1.8, 0.45, 0.9], additive: true, drag: 4, gravity: -0.2 });
    }
    for (let k = 0; k < 30; k++) {
      const d = randomUnit();
      const v = rand(4, 16);
      this.particles.spawn({ pos, vel: [d[0] * v, d[1] * v, Math.abs(d[2]) * v], life: rand(0.3, 0.8), size: 0.02, color: [5, 3, 1.2, 1], additive: true, drag: 1.5, gravity: 1 });
    }
    for (let k = 0; k < 26; k++) {
      const d = randomUnit();
      const v = rand(0.5, 3) * radius;
      this.particles.spawn({ pos, vel: [d[0] * v, d[1] * v, d[2] * v + 0.8], life: rand(1.5, 3), size: rand(0.25, 0.5) * radius, grow: 0.5, color: [0.08, 0.075, 0.07, 0.55], drag: 1.6, gravity: -0.05 });
    }
  }

  /** A few chips and a puff per crack (strength = utilization, about 1..2); a burst for dust. */
  crack(ev: CrackEvent): void {
    if (ev.voxels !== undefined && ev.voxels > 0) {
      this.dust(ev);
      return;
    }
    const n = ev.normal;
    const s = Math.min(1, Math.max(0, ev.strength - 0.5));
    const chips = Math.min(2 + Math.round(s * 3), this.crackBudget - 1);
    if (chips < 0) return;
    this.crackBudget -= chips + 1;
    for (let k = 0; k < chips; k++) {
      const d = hemisphere(n, 1.1);
      const v = rand(0.5, 2.5) * (0.5 + s);
      this.particles.spawn({ pos: ev.pos, vel: [d[0] * v, d[1] * v, d[2] * v], life: rand(0.5, 1.2), size: rand(0.015, 0.035), color: [0.3, 0.29, 0.27, 1], gravity: 1 });
    }
    this.particles.spawn({ pos: ev.pos, vel: [n[0] * 0.3, n[1] * 0.3, n[2] * 0.3], life: 1.4, size: 0.1, grow: 0.3, color: [0.4, 0.39, 0.36, 0.35], drag: 2, gravity: -0.02 });
  }

  /** Crushed material or a shard too small to be a piece: chips flying on with it and a cloud. */
  private dust(ev: CrackEvent): void {
    const vox = ev.voxels ?? 1;
    const r = Math.max(0.1, ev.radius ?? 0.2);
    const v0 = ev.velocity ?? [0, 0, 0];
    const chips = Math.min(3 + Math.round(vox / 3), 12, this.crackBudget - 3);
    if (chips < 0) return;
    this.crackBudget -= chips + 3;
    for (let k = 0; k < chips; k++) {
      const d = randomUnit();
      const v = rand(0.5, 2.5);
      this.particles.spawn({
        pos: [ev.pos[0] + d[0] * r * 0.5, ev.pos[1] + d[1] * r * 0.5, ev.pos[2] + d[2] * r * 0.5],
        vel: [v0[0] * 0.8 + d[0] * v, v0[1] * 0.8 + d[1] * v, v0[2] * 0.8 + d[2] * v + 0.5],
        life: rand(0.8, 1.8),
        size: rand(0.03, 0.07),
        color: [0.32, 0.31, 0.29, 1],
        gravity: 1,
      });
    }
    for (let k = 0; k < 3; k++) {
      const d = randomUnit();
      this.particles.spawn({
        pos: ev.pos,
        vel: [v0[0] * 0.3 + d[0] * 0.4, v0[1] * 0.3 + d[1] * 0.4, v0[2] * 0.3 + d[2] * 0.2 + 0.2],
        life: rand(1.5, 2.6),
        size: r * rand(0.9, 1.4),
        grow: 0.6,
        color: [0.42, 0.41, 0.38, 0.4],
        drag: 2,
        gravity: -0.03,
      });
    }
  }

  /** Blast or landing debris: dust ring and camera shake by energy and distance. */
  impact(ev: ImpactEvent, eye: Vec3): void {
    const e = Math.max(0, ev.energy);
    const scaleE = Math.min(3, Math.cbrt(e / 1e5));
    const n = 10 + Math.round(scaleE * 14);
    for (let k = 0; k < n; k++) {
      const a = rand(0, Math.PI * 2);
      const v = rand(1, 3.5) * (0.5 + scaleE * 0.5);
      this.particles.spawn({
        pos: [ev.pos[0], ev.pos[1], ev.pos[2] + 0.1],
        vel: [Math.cos(a) * v, Math.sin(a) * v, rand(0.2, 1.2)],
        life: rand(1.2, 2.8),
        size: rand(0.2, 0.45) * (0.6 + scaleE * 0.4),
        grow: 0.45,
        color: [0.36, 0.34, 0.31, 0.45],
        drag: 1.8,
        gravity: -0.02,
      });
    }
    const d = distance(eye, ev.pos);
    this.addTrauma(Math.min(0.9, (0.25 * scaleE) / (1 + d * d * 0.02)));
  }

  /** Dust over the surface of a detached piece (sampled from its mesh vertices). */
  detached(ev: DetachedEvent): void {
    const m = ev.mesh;
    if (m.vertexCount === 0) return;
    const count = Math.min(120, 8 + Math.round(ev.voxels / 40), this.detachedBudget);
    if (count <= 0) return;
    this.detachedBudget -= count;
    const f = new Float32Array(m.vertices, 0, m.vertexCount * (VERTEX_STRIDE / 4));
    for (let k = 0; k < count; k++) {
      const i = Math.floor(Math.random() * m.vertexCount) * (VERTEX_STRIDE / 4);
      const p: Vec3 = [f[i]!, f[i + 1]!, f[i + 2]!];
      this.particles.spawn({
        pos: p,
        vel: [ev.velocity[0] * 0.5 + rand(-0.4, 0.4), ev.velocity[1] * 0.5 + rand(-0.4, 0.4), ev.velocity[2] * 0.5 + rand(-0.2, 0.5)],
        life: rand(1, 2.2),
        size: rand(0.12, 0.3),
        grow: 0.35,
        color: [0.4, 0.38, 0.35, 0.4],
        drag: 1.5,
        gravity: 0.05,
      });
    }
  }

  /** The engine's burning voxels (an `env` message). */
  setFlames(flames: Float32Array): void {
    this.flames = flames;
  }

  setSmoke(smoke: Float32Array): void {
    this.smoke = smoke;
    this.smokeDirty = true;
  }

  /**
   * The smoke field as soft sprites (one per cell, a little bigger than it, jittered and slowly
   * drifting so the grid does not show), handed to the particle system. Built again when new
   * smoke comes and ten times a second for the drift (not every frame); sprites near the eye
   * fade (no screen-filling layers).
   */
  smokeField(timeS: number, voxelSize: number, eye: Vec3): void {
    if (!this.smokeDirty && timeS - this.smokeBuilt < 0.1) return;
    this.smokeDirty = false;
    this.smokeBuilt = timeS;
    const s = this.smoke;
    const total = Math.min(s.length / SMOKE_STRIDE, FIELD_CAPACITY);
    const cell = voxelSize * SMOKE_CELL_VOXELS;
    const out = this.smokeSprites;
    let n = 0;
    for (let q = 0; q < total; q++) {
      const i = q * SMOKE_STRIDE;
      const dx = s[i]! - eye[0];
      const dy = s[i + 1]! - eye[1];
      const dz = s[i + 2]! - eye[2];
      const near = Math.hypot(dx, dy, dz);
      if (near < 0.35 * cell) continue;
      const fade = Math.min(1, (near - 0.35 * cell) / (2.5 * cell));
      const k = n++;
      const x = s[i]!;
      const y = s[i + 1]!;
      const z = s[i + 2]!;
      // (a hash of the cell: its own jitter and phase)
      const hsh = Math.abs(Math.sin(x * 12.9898 + y * 78.233 + z * 37.719) * 43758.5453) % 1;
      const ph = hsh * 6.283;
      const o = k * FLOATS_PER_INSTANCE;
      out[o] = x + cell * (0.25 * Math.sin(ph * 3.1) + 0.15 * Math.sin(timeS * 0.5 + ph));
      out[o + 1] = y + cell * (0.25 * Math.cos(ph * 2.3) + 0.15 * Math.cos(timeS * 0.43 + ph));
      out[o + 2] = z + cell * (0.2 * Math.sin(ph * 5.7) + 0.1 * Math.sin(timeS * 0.61 + ph * 1.3));
      out[o + 3] = cell * (0.95 + 0.25 * hsh);
      const g = 0.11 + 0.04 * hsh;
      out[o + 4] = g;
      out[o + 5] = g;
      out[o + 6] = g * 0.95;
      out[o + 7] = fade * 0.6 * (1 - Math.exp(-1.6 * s[i + 3]!));
    }
    this.particles.setField(out.subarray(0, n * FLOATS_PER_INSTANCE));
  }

  get flameCount(): number {
    return this.flames.length / FLAME_STRIDE;
  }

  /**
   * Fire, every frame: flame tongues licking up from random burning voxels, embers, smoke, and a
   * flickering light at the flames nearest the eye.
   */
  fire(dt: number, eye: Vec3, timeS: number): void {
    const f = this.flames;
    const n = f.length / FLAME_STRIDE;
    if (n === 0) return;
    const pick = (): number => Math.floor(Math.random() * n) * FLAME_STRIDE;
    this.fireCarry += n * FLAME_RATE * dt;
    const tongues = Math.min(FLAME_PARTICLES_PER_FRAME, Math.floor(this.fireCarry));
    this.fireCarry = Math.min(this.fireCarry - tongues, 4);
    for (let k = 0; k < tongues; k++) {
      const i = pick();
      const hot = Math.min(1.3, f[i + 3]! / 800);
      this.particles.spawn({
        pos: [f[i]! + rand(-0.07, 0.07), f[i + 1]! + rand(-0.07, 0.07), f[i + 2]! + rand(0, 0.08)],
        vel: [rand(-0.15, 0.15), rand(-0.15, 0.15), rand(0.6, 1.4)],
        life: rand(0.3, 0.65),
        size: rand(0.08, 0.15),
        grow: -0.08,
        color: [2.6 * hot, rand(0.8, 1.3) * hot, 0.22 * hot, 0.75],
        additive: true,
        drag: 1.2,
        gravity: -0.12,
      });
    }
    this.emberCarry += n * EMBER_RATE * dt;
    for (; this.emberCarry >= 1; this.emberCarry--) {
      const i = pick();
      this.particles.spawn({
        pos: [f[i]!, f[i + 1]!, f[i + 2]! + 0.1],
        vel: [rand(-0.6, 0.6), rand(-0.6, 0.6), rand(1.5, 3.5)],
        life: rand(1.2, 2.8),
        size: rand(0.008, 0.016),
        color: [5, 1.8, 0.4, 1],
        additive: true,
        drag: 0.8,
        gravity: -0.04,
      });
    }
    this.emberCarry = Math.min(this.emberCarry, 4);
    this.smokeCarry += n * FIRE_SMOKE_RATE * dt;
    const puffs = Math.min(FIRE_SMOKE_PER_FRAME, Math.floor(this.smokeCarry));
    this.smokeCarry = Math.min(this.smokeCarry - puffs, 4);
    for (let k = 0; k < puffs; k++) {
      const i = pick();
      this.particles.spawn({
        pos: [f[i]!, f[i + 1]!, f[i + 2]! + 0.35],
        vel: [rand(-0.2, 0.2), rand(-0.2, 0.2), rand(0.8, 1.6)],
        life: rand(2.5, 4.5),
        size: rand(0.2, 0.35),
        grow: 0.45,
        color: [0.06, 0.055, 0.05, 0.4],
        drag: 0.6,
        gravity: -0.03,
      });
    }
    // firelight: the flames near the eye (a sample), flickering
    let best = Infinity;
    let at = 0;
    let near = 0;
    const tries = Math.min(n, 48);
    for (let k = 0; k < tries; k++) {
      const i = tries === n ? k * FLAME_STRIDE : pick();
      const d = distance(eye, [f[i]!, f[i + 1]!, f[i + 2]!]);
      if (d < 20) near++;
      if (d < best) {
        best = d;
        at = i;
      }
    }
    if (best < 40) {
      const flicker = 0.8 + 0.2 * Math.sin(timeS * 23) * Math.sin(timeS * 7.3 + 1.1);
      this.light([f[at]!, f[at + 1]!, f[at + 2]! + 0.4], (1.2 + Math.log2(1 + (near * n) / tries) * 0.5) * flicker);
    }
  }

  /** A piece hitting the water: a crown of droplets and a little mist. */
  splash(ev: SplashEvent): void {
    const s = Math.min(1, Math.cbrt(ev.strength / 2000));
    const n = 12 + Math.round(40 * s);
    for (let k = 0; k < n; k++) {
      const a = rand(0, Math.PI * 2);
      const v = rand(1, 3) * (0.5 + s);
      this.particles.spawn({
        pos: [ev.pos[0] + Math.cos(a) * 0.3, ev.pos[1] + Math.sin(a) * 0.3, ev.pos[2]],
        vel: [Math.cos(a) * v * 0.5, Math.sin(a) * v * 0.5, rand(2, 5) * (0.5 + s)],
        life: rand(0.6, 1.2),
        size: rand(0.02, 0.05),
        color: [0.7, 0.8, 0.85, 0.8],
        gravity: 1,
        drag: 0.5,
      });
    }
    for (let k = 0; k < 4; k++) {
      this.particles.spawn({ pos: ev.pos, vel: [rand(-0.5, 0.5), rand(-0.5, 0.5), rand(0.3, 1)], life: rand(1, 1.8), size: 0.25 + 0.3 * s, grow: 0.5, color: [0.8, 0.85, 0.9, 0.25], drag: 2, gravity: 0.05 });
    }
  }

  /** The flamethrower's flames or the hose's water, from the muzzle along dir. */
  spray(muzzle: Vec3, dir: Vec3, flame: boolean): void {
    for (let k = 0; k < 5; k++) {
      const v = rand(7, 11);
      const d = hemisphere(dir, 0.08);
      this.particles.spawn(
        flame
          ? { pos: muzzle, vel: [d[0] * v, d[1] * v, d[2] * v], life: rand(0.35, 0.6), size: 0.05, grow: 0.6, color: [3, 1.2, 0.25, 0.8], additive: true, drag: 1.4, gravity: -0.3 }
          : { pos: muzzle, vel: [d[0] * v, d[1] * v, d[2] * v], life: rand(0.5, 0.9), size: 0.03, grow: 0.15, color: [0.6, 0.72, 0.8, 0.6], drag: 0.3, gravity: 1 },
      );
    }
    if (flame) this.light(muzzle, 0.8);
  }

  rocketTrail(pos: Vec3, dir: Vec3): void {
    this.particles.spawn({ pos, vel: [0, 0, 0], life: 0.03, size: 0.16, color: [5, 3, 1.4, 1], additive: true, gravity: 0 });
    this.particles.spawn({ pos, vel: [-dir[0] * 2, -dir[1] * 2, -dir[2] * 2], life: 0.12, size: 0.08, color: [4, 1.6, 0.4, 0.9], additive: true, gravity: 0, drag: 6 });
    this.particles.spawn({ pos, vel: [rand(-0.2, 0.2), rand(-0.2, 0.2), rand(0, 0.3)], life: rand(0.8, 1.4), size: 0.07, grow: 0.35, color: [0.5, 0.5, 0.5, 0.35], drag: 1, gravity: -0.05 });
  }
}
