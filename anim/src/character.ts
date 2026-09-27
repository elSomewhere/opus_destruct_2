/**
 * A voxel character: the high-level object a host drives. It ties together
 *  - the voxel model (shared between characters until the first wound, then copied: damage is
 *    per character; `geometryVersion` tells the host to re-mesh),
 *  - the animator (alive) or the ragdoll (dead),
 *  - the held prop (dropped on death),
 *  - health, hit zones, wounds (voxels carved out, flesh and bone inside), severed limbs and
 *    heads, and gibbing by blasts (the pieces come back as gib specs for a GibSystem),
 *  - the retro presentation (baked frames, stepped time, 8-way facing) when a retro set is
 *    attached.
 *
 * The host moves it (setRoot, with its own collision), sets `animator.input`, calls update,
 * and draws `skin` (smooth), or `retroFrame` at `retroTransform` (retro).
 */
import type { Prop } from './characters/props.ts';
import { WorldPose } from './core/skeleton.ts';
import { HumanoidAnimator } from './humanoid/animator.ts';
import { HumanoidRagdoll } from './humanoid/ragdoll.ts';
import { H } from './humanoid/rig.ts';
import { qrotate, qz, type Quat } from './math/quat.ts';
import { vadd, vcopy, vdist, vnorm, vscale, vsub, type V3 } from './math/vec.ts';
import type { CollisionWorld } from './physics/collision.ts';
import { RetroPlayer, retroStateOf, snapYaw8, type RetroSet } from './retro/sequences.ts';
import { carveModel, detachSubtree, partIntegrity, raycastModel, severDisconnected, type CharacterHit, type RemovedVoxel } from './voxel/damage.ts';
import type { Palette, VoxelModel, VoxelPart } from './voxel/model.ts';

/** A piece that came off a character: hand it to GibSystem.spawn. */
export interface GibSpec {
  part: VoxelPart;
  voxelSize: number;
  /** Its bone's world transform at the moment (x -> boneRot (x - boneRestHead) + bonePos). */
  bonePos: V3;
  boneRot: Quat;
  boneRestHead: V3;
  vel: V3;
  ang: V3;
  /** The piece is the held prop (drawn with the prop's palette slots). */
  prop: boolean;
}

export interface WoundResult {
  /** The shot killed the character now. */
  killed: boolean;
  headshot: boolean;
  /** Damage dealt (after hit-zone multipliers). */
  damage: number;
  /** Voxels carved out (blood and flesh bits for the host's effects). */
  removed: RemovedVoxel[];
  /** Pieces that came off (severed limbs, a head). */
  gibs: GibSpec[];
}

/** Damage multipliers by bone (rifle rounds ~ 30-40 damage, health 100). */
const ZONE: Partial<Record<number, number>> = {
  [H.head]: 4,
  [H.neck]: 3,
  [H.chest]: 1.1,
  [H.spine]: 1,
  [H.pelvis]: 0.9,
};

export interface CharacterOptions {
  model: VoxelModel;
  palette: Palette;
  collision: CollisionWorld;
  weapon?: Prop | null;
  health?: number;
  seed?: number;
  /** Shared retro frames for this model (see bakeRetroSet). */
  retro?: RetroSet | null;
}

export class Character {
  /** The model drawn: shared until the first wound, then this character's own copy. */
  model: VoxelModel;
  readonly palette: Palette;
  readonly animator: HumanoidAnimator;
  ragdoll: HumanoidRagdoll | null = null;
  weapon: Prop | null;
  health: number;
  readonly maxHealth: number;
  /** Bumped when `model` changes (a copy was made or voxels were carved): re-mesh. */
  geometryVersion = 0;
  /** True once `model` is this character's own copy. */
  ownsModel = false;
  /** Skin matrices of the current pose (16 floats per bone). */
  readonly skin: Float32Array;
  /** Hit flash 0..1 (the host tints the character). */
  flash = 0;
  /** Seconds since death. */
  deadTime = 0;
  retro: RetroSet | null;
  private retroPlayer: RetroPlayer | null = null;
  /** Retro frame to draw and its transform (root position, facing snapped to 8 ways). */
  retroFrame: VoxelModel | null = null;
  readonly retroPos: V3 = [0, 0, 0];
  retroYaw = 0;
  private pain = 0;
  private firing = 0;
  private readonly collision: CollisionWorld;

  constructor(o: CharacterOptions) {
    this.model = o.model;
    this.palette = o.palette;
    this.collision = o.collision;
    this.animator = new HumanoidAnimator(o.model.skeleton, o.collision, o.seed ?? 1);
    this.weapon = o.weapon ?? null;
    this.animator.weapon = this.weapon;
    this.maxHealth = this.health = o.health ?? 100;
    this.skin = new Float32Array(o.model.skeleton.count * 16);
    this.retro = o.retro ?? null;
  }

  get alive(): boolean {
    return this.ragdoll === null;
  }

  /** The current world pose (animated or ragdoll). */
  get pose(): WorldPose {
    return this.ragdoll ? this.ragdoll.world : this.animator.world;
  }

  /** Places the character (feet on the ground at pos, facing yaw; see HumanoidAnimator). */
  place(pos: Readonly<V3>, yaw: number): void {
    this.animator.place(pos, yaw);
    this.pose.writeSkin(this.skin);
  }

  setRoot(pos: Readonly<V3>, yaw: number): void {
    if (this.alive) this.animator.setRoot(pos, yaw);
  }

  /** A shot fired (recoil; the retro fire frames). */
  fire(): void {
    if (!this.alive) return;
    this.animator.fire();
    this.firing = 0.15;
  }

  update(dt: number): void {
    this.flash = Math.max(0, this.flash - dt * 6);
    this.pain = Math.max(0, this.pain - dt);
    this.firing = Math.max(0, this.firing - dt);
    if (this.ragdoll) {
      this.deadTime += dt;
      this.ragdoll.update(dt);
    } else this.animator.update(dt);
    this.pose.writeSkin(this.skin);
    if (this.retro) this.updateRetro(dt);
  }

  private updateRetro(dt: number): void {
    const a = this.animator;
    if (!this.alive || this.ownsModel) {
      // dead (the ragdoll) or wounded (the baked frames show no wounds): the host draws the
      // model itself, posed in retro steps
      this.retroFrame = null;
      return;
    }
    this.retroPlayer ??= new RetroPlayer(this.retro!);
    const speed = Math.hypot(a.velocity[0], a.velocity[1]);
    const state = retroStateOf({
      speed,
      crouch: a.input.crouch,
      aiming: a.weapon !== null && a.input.carry === 'aim' && a.input.aimAt !== null,
      firing: this.firing > 0,
      mood: a.input.mood,
      pain: this.pain > 0,
    });
    const seq = this.retroPlayer.sequence(state);
    const rate = seq.speed > 0 ? Math.max(0.3, speed / seq.speed) : 1;
    this.retroFrame = this.retroPlayer.update(dt, state, rate);
    vcopy(a.rootPos, this.retroPos);
    this.retroYaw = snapYaw8(a.rootYaw);
  }

  /** Bounding sphere (world) of the body. */
  bounds(): { center: V3; radius: number } {
    const w = this.pose;
    return { center: vcopy(w.p[H.pelvis]!), radius: this.alive ? 1.05 : 1.2 };
  }

  /** The voxel hit by a ray (unit dir), or null. */
  raycast(origin: Readonly<V3>, dir: Readonly<V3>, maxDist: number): CharacterHit | null {
    const b = this.bounds();
    // ray vs bounding sphere first
    const oc = vsub(origin, b.center);
    const t = -(oc[0] * dir[0] + oc[1] * dir[1] + oc[2] * dir[2]);
    const c2 = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - t * t;
    if (c2 > b.radius * b.radius || t < -b.radius || t - b.radius > maxDist) return null;
    return raycastModel(this.model, this.skin, origin, dir, maxDist);
  }

  private ownModel(): void {
    if (this.ownsModel) return;
    this.model = this.model.clone();
    this.ownsModel = true;
    this.geometryVersion++;
  }

  /**
   * A bullet (or blade) wound at `hit` travelling along `dir`: carves a hole of `radius`
   * voxels' worth, deals `damage` times the zone multiplier, flinches the body (or pushes the
   * ragdoll) with `impulse` m/s, and may sever limbs or the head.
   */
  wound(hit: CharacterHit, dir: Readonly<V3>, damage: number, radius = 0.045, impulse = 2.5): WoundResult {
    const res: WoundResult = { killed: false, headshot: false, damage: 0, removed: [], gibs: [] };
    this.ownModel();
    carveModel(this.model, hit.restPoint, radius, undefined, res.removed);
    // the exit side: a round also opens the body a little further along its path
    const exit = vadd(hit.restPoint, vscale(this.restDir(hit.bone, dir), radius * 2.2));
    carveModel(this.model, exit, radius * 0.8, undefined, res.removed);
    this.geometryVersion++;
    const mult = ZONE[hit.bone] ?? 0.6;
    res.damage = damage * mult;
    res.headshot = hit.bone === H.head || hit.bone === H.neck;
    this.flash = 1;
    const wasAlive = this.alive;
    this.health -= res.damage;
    // limbs and heads come off when their part is shot through
    res.gibs.push(...this.severAfterDamage(hit.bone, dir));
    if (wasAlive) {
      this.pain = 0.25;
      if (this.health <= 0 || res.gibs.some((g) => g.part.bone === H.head)) {
        this.die(hit.point, vscale(vnorm(dir), impulse));
        res.killed = true;
      } else this.animator.hit(dir, damage / 30, heightOf(hit.bone));
    } else if (this.ragdoll) this.ragdoll.hit(hit.point, vscale(vnorm(dir), impulse * 1.4));
    return res;
  }

  /** Direction `dir` (world) in the rest space of bone b. */
  private restDir(b: number, dir: Readonly<V3>): V3 {
    const q = this.pose.q[b]!;
    return qrotate([-q[0], -q[1], -q[2], q[3]], vnorm(dir));
  }

  private severAfterDamage(bone: number, dir: Readonly<V3>): GibSpec[] {
    const out: GibSpec[] = [];
    const m = this.model;
    const sk = m.skeleton;
    // the damaged bone and the ones around it (a joint wound opens both parts)
    const bones = new Set([bone, sk.parents[bone]!, ...sk.children[bone]!]);
    for (const b of bones) {
      if (b <= H.pelvis || b === H.weapon || b === H.spine || b === H.chest) continue;
      const pi = m.partOfBone[b]!;
      if (pi < 0) continue;
      const part = m.parts[pi]!;
      if (part.count === 0) continue;
      let pieces: VoxelPart[];
      // limbs and heads give way after a few hits (a thin limb shot through twice is gone)
      if (partIntegrity(part) < 0.55) pieces = detachSubtree(m, b, true);
      else {
        pieces = severDisconnected(m, pi, 0.045 * (sk.restHead[H.pelvis]![2] / 0.97));
        // a piece carrying the far end of the bone takes the bones below with it
        const tail = sk.restTail[b]!;
        if (pieces.some((p) => nearTail(p, tail, m.voxelSize))) {
          for (const c of sk.children[b]!) pieces.push(...detachSubtree(m, c, true));
        }
      }
      for (const p of pieces) out.push(this.gibSpec(p, dir, 2.5));
    }
    if (out.length > 0) this.geometryVersion++;
    return out;
  }

  private gibSpec(p: VoxelPart, dir: Readonly<V3>, speed: number): GibSpec {
    const w = this.pose;
    const b = p.bone;
    const d = vnorm(dir);
    const r = (): number => Math.random() * 2 - 1;
    return {
      part: p,
      voxelSize: this.model.voxelSize,
      bonePos: vcopy(w.p[b]!),
      boneRot: [...w.q[b]!] as Quat,
      boneRestHead: vcopy(this.model.skeleton.restHead[b]!),
      vel: [d[0] * speed + r() * 0.8, d[1] * speed + r() * 0.8, d[2] * speed + 1.5 + Math.random()],
      ang: [r() * 8, r() * 8, r() * 8],
      prop: false,
    };
  }

  /** Dies: switches to the ragdoll (keeping the momentum of the pose) with a push `dv` at `point`. */
  die(point?: Readonly<V3>, dv?: Readonly<V3>): void {
    if (!this.alive) return;
    this.health = Math.min(this.health, 0);
    const a = this.animator;
    this.ragdoll = new HumanoidRagdoll(a.skeleton, this.collision, a.world, a.prevWorld, 1 / 60, 0.7);
    if (point && dv) this.ragdoll.hit(point, dv);
    this.retroFrame = null;
  }

  /** The held prop as a gib (on death), or null. The character lets go of it. */
  dropWeapon(): GibSpec | null {
    const prop = this.weapon;
    if (!prop) return null;
    this.weapon = null;
    this.animator.weapon = null;
    const a = this.animator;
    const v = vsub(a.world.p[H.chest]!, a.prevWorld.p[H.chest]!);
    return {
      part: prop.model.parts[0]!,
      voxelSize: prop.model.voxelSize,
      bonePos: vcopy(a.weaponPos),
      boneRot: [...a.weaponRot] as Quat,
      boneRestHead: [0, 0, 0],
      vel: [v[0] * 60 + (Math.random() - 0.5), v[1] * 60 + (Math.random() - 0.5), v[2] * 60 + 1],
      ang: [Math.random() * 6 - 3, Math.random() * 6 - 3, Math.random() * 6 - 3],
      prop: true,
    };
  }

  /**
   * A blast at `center` (radius of full effect, m; strength 1 ~ a rocket). Returns damage dealt,
   * whether it killed, and gib specs when the body is torn apart (close to the centre).
   */
  blast(center: Readonly<V3>, radius: number, strength = 1): { damage: number; killed: boolean; gibbed: boolean; gibs: GibSpec[] } {
    const c = this.bounds().center;
    const d = vdist(c, center);
    const reach = radius * 3.5;
    if (d > reach) return { damage: 0, killed: false, gibbed: false, gibs: [] };
    const f = Math.max(0, 1 - d / reach);
    const damage = 330 * strength * f * f;
    const away = vnorm(vsub(c, center), [0, 0, 1]);
    const wasAlive = this.alive;
    this.health -= damage;
    this.flash = 1;
    const gibs: GibSpec[] = [];
    let gibbed = false;
    if (this.health <= -40 || d < radius * 1.3) {
      // torn apart: every part flies, the torso in chunks
      gibbed = true;
      this.ownModel();
      if (wasAlive) this.die();
      const m = this.model;
      for (const p of m.parts) {
        if (p.count === 0) continue;
        // a few holes so the pieces are ragged
        const sk = m.skeleton;
        const mid = [(sk.restHead[p.bone]![0] + sk.restTail[p.bone]![0]) / 2, (sk.restHead[p.bone]![1] + sk.restTail[p.bone]![1]) / 2, (sk.restHead[p.bone]![2] + sk.restTail[p.bone]![2]) / 2] as V3;
        carveModel(m, [mid[0] + (Math.random() - 0.5) * 0.1, mid[1] + (Math.random() - 0.5) * 0.1, mid[2]], 0.05, [m.parts.indexOf(p)]);
      }
      for (let b = 0; b < m.skeleton.count; b++) {
        const pi = m.partOfBone[b]!;
        if (pi < 0 || m.parts[pi]!.count === 0) continue;
        for (const piece of detachSubtree(m, b, false)) {
          const g = this.gibSpec(piece, away, 4 + 8 * f * strength);
          // outwards from the blast, per piece
          const pc = this.pose.p[b]!;
          const out = vnorm(vsub(pc, center), [0, 0, 1]);
          g.vel = [out[0] * (5 + 9 * f) + (Math.random() - 0.5) * 3, out[1] * (5 + 9 * f) + (Math.random() - 0.5) * 3, Math.abs(out[2]) * 6 + 3 + Math.random() * 4];
          gibs.push(g);
        }
      }
      this.geometryVersion++;
    } else if (this.health <= 0) {
      if (wasAlive) this.die();
      this.ragdoll!.blast(center, reach, 9 * f * strength + 2);
    } else {
      this.animator.hit(away, 2 * f * strength, 0.6);
      this.pain = 0.3;
    }
    if (!wasAlive && !gibbed && this.ragdoll) this.ragdoll.blast(center, reach, 9 * f * strength);
    return { damage, killed: wasAlive && !this.alive, gibbed, gibs };
  }

  /** World position of the muzzle of the held prop (or the eyes without one). */
  muzzle(out: V3 = [0, 0, 0]): V3 {
    if (this.weapon) return this.animator.propPoint(this.weapon.muzzle, out);
    return this.animator.eyes(out);
  }

  /** The skin matrix of a retro frame (a one-bone model at the root, facing snapped). */
  writeRetroSkin(out: Float32Array, offset = 0): void {
    const q = qz(this.retroYaw - Math.PI / 2);
    const x = q[0], y = q[1], z = q[2], w = q[3];
    const o = offset;
    out[o] = 1 - 2 * (y * y + z * z);
    out[o + 1] = 2 * (x * y + z * w);
    out[o + 2] = 2 * (x * z - y * w);
    out[o + 3] = 0;
    out[o + 4] = 2 * (x * y - z * w);
    out[o + 5] = 1 - 2 * (x * x + z * z);
    out[o + 6] = 2 * (y * z + x * w);
    out[o + 7] = 0;
    out[o + 8] = 2 * (x * z + y * w);
    out[o + 9] = 2 * (y * z - x * w);
    out[o + 10] = 1 - 2 * (x * x + y * y);
    out[o + 11] = 0;
    out[o + 12] = this.retroPos[0];
    out[o + 13] = this.retroPos[1];
    out[o + 14] = this.retroPos[2];
    out[o + 15] = 1;
  }
}

function heightOf(bone: number): number {
  if (bone === H.head || bone === H.neck) return 1;
  if (bone === H.chest || bone >= H.clavicleL && bone <= H.handR) return 0.75;
  if (bone === H.spine) return 0.55;
  if (bone === H.pelvis) return 0.4;
  return 0.15;
}

function nearTail(p: VoxelPart, tail: Readonly<V3>, s: number): boolean {
  const [nx, ny, nz] = p.dims;
  const r2 = (2.5 * s) ** 2;
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++)
      for (let x = 0; x < nx; x++) {
        if (p.cells[x + nx * (y + ny * z)] === 0) continue;
        const cx = (p.origin[0] + x + 0.5) * s - tail[0];
        const cy = (p.origin[1] + y + 0.5) * s - tail[1];
        const cz = (p.origin[2] + z + 0.5) * s - tail[2];
        if (cx * cx + cy * cy + cz * cz < r2) return true;
      }
  return false;
}
