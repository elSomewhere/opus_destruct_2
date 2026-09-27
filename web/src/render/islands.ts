/**
 * Detached islands: meshes of pieces that lost support. Engines that simulate rigid debris
 * (DetachedEvent.rigid) send poses in `debris` messages: the piece follows them, interpolated
 * one engine tick behind, and fades with the engine-given opacity. Otherwise the piece flies
 * ballistically (gravity, the engine-given linear and angular velocity, no collision) and fades
 * out with a dithered dissolve (plan §B7, v1 "vanish" semantics).
 */
import type { DebrisPose, DetachedEvent, Vec3 } from '../engine/protocol.ts';
import { mat4, mat4FromQuatAbout, mat4RotateAbout, type Mat4 } from './math.ts';

export const ISLAND_LIFETIME_S = 1.5;
const FADE_START_S = 0.25;
const GRAVITY = 9.81;
/** Engine tick; rigid pieces are drawn this far in the past to interpolate between poses. */
const TICK_S = 1 / 60;
/** A rigid piece whose first pose never arrives falls back to the ballistic path. */
const POSE_TIMEOUT_S = 0.5;

type Quat = [number, number, number, number];

interface PoseSample {
  t: number;
  pos: Vec3;
  rot: Quat;
}

export interface GpuIsland {
  id: number;
  vbuf: GPUBuffer;
  ibuf: GPUBuffer;
  indexCount: number;
  centroid: Vec3;
  velocity: Vec3;
  axis: Vec3;
  spin: number;
  born: number;
  /** Engine-simulated (poses from `debris` messages). */
  rigid: boolean;
  prev: PoseSample | null;
  cur: PoseSample | null;
  /** Updated by `update`. */
  model: Mat4;
  opacity: number;
  /** Current centre, for effects. */
  position: Vec3;
}

export class IslandRenderer {
  private readonly device: GPUDevice;
  private islands: GpuIsland[] = [];
  readonly maxIslands: number;

  constructor(device: GPUDevice, maxIslands: number) {
    this.device = device;
    this.maxIslands = maxIslands;
  }

  get list(): readonly GpuIsland[] {
    return this.islands;
  }

  add(ev: DetachedEvent, nowS: number): GpuIsland | null {
    const m = ev.mesh;
    if (m.indexCount === 0) return null;
    if (this.islands.length >= this.maxIslands) this.release(this.islands.shift()!);
    const vbuf = this.device.createBuffer({
      label: `island ${ev.id} vertices`,
      size: Math.max(4, m.vertexCount * 28),
      usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
    });
    const ibuf = this.device.createBuffer({
      label: `island ${ev.id} indices`,
      size: Math.max(4, m.indexCount * 4),
      usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST,
    });
    this.device.queue.writeBuffer(vbuf, 0, m.vertices, 0, m.vertexCount * 28);
    this.device.queue.writeBuffer(ibuf, 0, m.indices, 0, m.indexCount * 4);
    const w = ev.angular;
    const spin = Math.hypot(w[0], w[1], w[2]);
    const island: GpuIsland = {
      id: ev.id,
      vbuf,
      ibuf,
      indexCount: m.indexCount,
      centroid: [...ev.centroid],
      velocity: [...ev.velocity],
      axis: spin > 0 ? [w[0] / spin, w[1] / spin, w[2] / spin] : [0, 0, 1],
      spin,
      born: nowS,
      rigid: ev.rigid === true,
      prev: null,
      cur: null,
      model: mat4(),
      opacity: 1,
      position: [...ev.centroid],
    };
    this.islands.push(island);
    return island;
  }

  /** Engine poses of the rigid pieces; rigid pieces missing from the list are released. */
  applyDebris(poses: readonly DebrisPose[], nowS: number): void {
    const byId = new Map<number, DebrisPose>();
    for (const p of poses) byId.set(p.id, p);
    const keep: GpuIsland[] = [];
    for (const isl of this.islands) {
      if (!isl.rigid) {
        keep.push(isl);
        continue;
      }
      const p = byId.get(isl.id);
      if (!p) {
        if (isl.cur !== null) {
          this.release(isl); // the engine removed it
          continue;
        }
        keep.push(isl); // its first pose is still on the way
        continue;
      }
      isl.prev = isl.cur ?? { t: nowS - TICK_S, pos: [...isl.centroid], rot: [0, 0, 0, 1] };
      isl.cur = { t: nowS, pos: [...p.pos], rot: [...p.rot] };
      isl.opacity = Math.max(0, Math.min(1, p.opacity));
      keep.push(isl);
    }
    this.islands = keep;
  }

  /** Advances motion and fades; releases islands past their lifetime. */
  update(nowS: number): void {
    const keep: GpuIsland[] = [];
    for (const isl of this.islands) {
      const t = nowS - isl.born;
      if (isl.rigid && isl.cur === null && t > POSE_TIMEOUT_S) isl.rigid = false; // no poses: fall back
      if (isl.rigid) {
        this.poseRigid(isl, nowS);
        keep.push(isl);
        continue;
      }
      if (t >= ISLAND_LIFETIME_S) {
        this.release(isl);
        continue;
      }
      const c = isl.centroid;
      const v = isl.velocity;
      isl.position = [c[0] + v[0] * t, c[1] + v[1] * t, c[2] + v[2] * t - 0.5 * GRAVITY * t * t];
      mat4RotateAbout(isl.model, isl.axis, isl.spin * t, c, isl.position);
      const f = Math.min(1, Math.max(0, (t - FADE_START_S) / (ISLAND_LIFETIME_S - FADE_START_S)));
      isl.opacity = 1 - f * f * (3 - 2 * f);
      keep.push(isl);
    }
    this.islands = keep;
  }

  clear(): void {
    for (const isl of this.islands) this.release(isl);
    this.islands = [];
  }

  /** Pose at `nowS - TICK_S`, interpolated between the last two engine poses (nlerp). */
  private poseRigid(isl: GpuIsland, nowS: number): void {
    const a = isl.prev;
    const b = isl.cur;
    if (!a || !b) {
      isl.position = [...isl.centroid];
      mat4FromQuatAbout(isl.model, [0, 0, 0, 1], isl.centroid, isl.position);
      return;
    }
    const span = Math.max(1e-6, b.t - a.t);
    const s = Math.min(1, Math.max(0, (nowS - TICK_S - a.t) / span));
    const pos: Vec3 = [
      a.pos[0] + (b.pos[0] - a.pos[0]) * s,
      a.pos[1] + (b.pos[1] - a.pos[1]) * s,
      a.pos[2] + (b.pos[2] - a.pos[2]) * s,
    ];
    const sign = a.rot[0] * b.rot[0] + a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3] < 0 ? -1 : 1;
    const q: Quat = [0, 0, 0, 0];
    let n = 0;
    for (let k = 0; k < 4; k++) {
      q[k] = a.rot[k]! * (1 - s) + sign * b.rot[k]! * s;
      n += q[k]! * q[k]!;
    }
    n = Math.sqrt(n) || 1;
    for (let k = 0; k < 4; k++) q[k] = q[k]! / n;
    isl.position = pos;
    mat4FromQuatAbout(isl.model, q, isl.centroid, pos);
  }

  private release(isl: GpuIsland): void {
    isl.vbuf.destroy();
    isl.ibuf.destroy();
  }
}
