/**
 * The world as the actors see it: svx_anim's CollisionWorld over the engine's streamed chunk
 * occupancy (the same bits the player collides with), line of sight, and the characters'
 * kinematic box moves (the player's collide + step-up rules).
 *
 * Engines without occupancy (the mock) get a flat ground at the world's surface.
 */
import { FlatGround, VoxelCollision, type CollisionWorld, type SphereContact, type V3 } from 'svx-anim';
import type { Vec3 } from '../engine/protocol.ts';
import type { OccupancyStore } from '../game/occupancy.ts';
import { moveWithStepSync, type StepMoveResult } from '../game/stepmove.ts';

export class WorldAccess implements CollisionWorld {
  readonly occupancy: OccupancyStore;
  readonly h: number;
  private readonly voxels: VoxelCollision;
  private readonly flat: FlatGround;

  constructor(occupancy: OccupancyStore, voxelSize: number) {
    this.occupancy = occupancy;
    this.h = voxelSize;
    this.voxels = new VoxelCollision(voxelSize, (i, j, k) => occupancy.solid(i, j, k));
    this.flat = new FlatGround(-voxelSize / 2);
  }

  /** Occupancy has arrived (the WASM engine sends it; the mock does not). */
  get voxelBacked(): boolean {
    return this.occupancy.ready;
  }

  private get c(): CollisionWorld {
    return this.occupancy.ready ? this.voxels : this.flat;
  }

  solid(i: number, j: number, k: number): boolean {
    return this.occupancy.ready ? this.occupancy.solid(i, j, k) : k * this.h < -this.h / 2;
  }

  groundHeight(x: number, y: number, zTop: number, zBottom: number): number | null {
    return this.c.groundHeight(x, y, zTop, zBottom);
  }

  sphere(c: Readonly<V3>, r: number, out: SphereContact): boolean {
    return this.c.sphere(c, r, out);
  }

  raycast(o: Readonly<V3>, d: Readonly<V3>, maxDist: number): number {
    return this.c.raycast(o, d, maxDist);
  }

  /** Nothing solid between a and b. */
  lineOfSight(a: Readonly<V3>, b: Readonly<V3>): boolean {
    const d: V3 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    const l = Math.hypot(d[0], d[1], d[2]);
    if (l < 1e-6) return true;
    const t = this.raycast(a, [d[0] / l, d[1] / l, d[2] / l], l);
    return t < 0;
  }

  /** A box move with gravity-style step-up (the player's rules); free movement without occupancy. */
  moveBox(min: Vec3, max: Vec3, move: Vec3, stepHeight: number, grounded: boolean): StepMoveResult {
    if (this.occupancy.ready) return moveWithStepSync((a, b, m) => this.occupancy.collide(a, b, m), min, max, move, stepHeight, grounded);
    const ground = -this.h / 2;
    const z = Math.max(min[2] + move[2], ground);
    return { move: [move[0], move[1], z - min[2]], onGround: z <= ground + 1e-6, stepped: 0 };
  }

  /**
   * Whether a standing box of this size fits with its feet at p. Things lower than `from`
   * above the feet do not count (steps: the move climbs them).
   */
  fits(p: Readonly<V3>, radius: number, height: number, from = 0.02): boolean {
    if (!this.occupancy.ready) return true;
    return !this.occupancy.overlaps([p[0] - radius, p[1] - radius, p[2] + from], [p[0] + radius, p[1] + radius, p[2] + height]);
  }

  /**
   * A standable spot near (x, y) at about height z: the ground within [z - drop, z + rise] with
   * room for a box of `height` above it; null if none.
   */
  standAt(x: number, y: number, z: number, rise: number, drop: number, radius: number, height: number): V3 | null {
    const g = this.groundHeight(x, y, z + rise, z - drop);
    if (g === null) return null;
    const p: V3 = [x, y, g];
    return this.fits(p, radius, height, 0.3) ? p : null;
  }
}
