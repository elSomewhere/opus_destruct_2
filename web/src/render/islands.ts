/**
 * Detached islands: meshes of pieces that lost support. Engines that simulate rigid pieces
 * (DetachedEvent.rigid) send poses in `debris` messages: the piece follows them, interpolated
 * one batch behind (engine/poseclock.ts), and fades with the engine-given opacity; it stays (as rubble) until
 * the engine drops it from the poses. Otherwise the piece flies ballistically (gravity, the
 * engine-given linear and angular velocity, no collision) and fades out with a dithered
 * dissolve (plan §B7, v1 "vanish" semantics).
 *
 * There can be thousands of pieces: islands live in a Map by id (insertion order = age), each
 * keeps a fixed object-uniform slot, and only islands whose transform or opacity changed are
 * marked for upload. The engine's `removed` events release them; over capacity, the least missed
 * goes (a ballistic or fading one, else the smallest).
 */
import { PoseClock } from '../engine/poseclock.ts';
import type { DetachedEvent, Vec3 } from '../engine/protocol.ts';
import { DEBRIS_STRIDE, VERTEX_STRIDE } from '../engine/protocol.ts';
import { mat4, mat4FromQuatAbout, mat4RotateAbout, sphereVisible, type Mat4 } from './math.ts';

export const ISLAND_LIFETIME_S = 1.5;
const FADE_START_S = 0.25;
const GRAVITY = 9.81;
/**
 * A rigid piece that pose messages keep leaving out, before its first pose, falls back to the
 * ballistic path (a slow page gets its poses late: time alone says nothing - a car's body would
 * fall away from its wheels).
 */
const POSE_MISSES = 3;

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
  /** Stamp of the last `debris` message that carried this piece. */
  seen: number;
  /** Pose messages since it came that did not have it (while it has had none). */
  missed: number;
  /** Updated by `update`. */
  model: Mat4;
  opacity: number;
  /** Current centre, for effects and culling. */
  position: Vec3;
  /** Bounding sphere radius about the centroid (metres). */
  radius: number;
  /** Object uniform slot (1..maxIslands; slot 0 is the chunks' identity). */
  slot: number;
  /** `model` / `opacity` changed since the renderer last uploaded the slot. */
  dirty: boolean;
}

export interface IslandDrawStats {
  drawn: number;
  triangles: number;
}

export class IslandRenderer {
  private readonly device: GPUDevice;
  private readonly islands = new Map<number, GpuIsland>();
  private readonly freeSlots: number[] = [];
  private readonly scratch = mat4();
  private stamp = 0;
  readonly maxIslands: number;
  /** When poses are drawn (the page's, shared with the vehicles and characters). */
  clock = new PoseClock();

  constructor(device: GPUDevice, maxIslands: number) {
    this.device = device;
    this.maxIslands = maxIslands;
    for (let s = maxIslands; s >= 1; s--) this.freeSlots.push(s);
  }

  /** Live islands, oldest first. */
  get list(): MapIterator<GpuIsland> {
    return this.islands.values();
  }

  get count(): number {
    return this.islands.size;
  }

  add(ev: DetachedEvent, nowS: number): GpuIsland | null {
    const m = ev.mesh;
    if (m.indexCount === 0) return null;
    const old = this.islands.get(ev.id);
    if (old) this.release(old);
    // over capacity: what is least missed goes (never the oldest for its age: those are the
    // parked cars' bodies, the standing rubble)
    if (this.freeSlots.length === 0) {
      const victim = this.leastMissed();
      if (victim) this.release(victim);
    }
    const vbuf = this.device.createBuffer({
      label: `island ${ev.id} vertices`,
      size: Math.max(4, m.vertexCount * VERTEX_STRIDE),
      usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
    });
    const ibuf = this.device.createBuffer({
      label: `island ${ev.id} indices`,
      size: Math.max(4, m.indexCount * 4),
      usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST,
    });
    this.device.queue.writeBuffer(vbuf, 0, m.vertices, 0, m.vertexCount * VERTEX_STRIDE);
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
      seen: 0,
      missed: 0,
      model: mat4(), // the detachment pose: identity
      opacity: 1,
      position: [...ev.centroid],
      radius: meshRadius(m.vertices, m.vertexCount, ev.centroid),
      slot: this.freeSlots.pop()!,
      dirty: true,
    };
    this.islands.set(ev.id, island);
    return island;
  }

  /** A piece the engine removed (a `removed` event): its mesh goes. */
  remove(id: number): void {
    const isl = this.islands.get(id);
    if (isl) this.release(isl);
  }

  /**
   * Engine poses of the rigid pieces (DEBRIS_STRIDE doubles each). Rigid pieces that had a pose
   * before and are missing now were removed by the engine (split, faded or gone) and are
   * released; a piece whose first pose is still on the way is kept.
   */
  applyDebris(poses: Float64Array, nowS: number): void {
    const stamp = ++this.stamp;
    for (let o = 0; o + DEBRIS_STRIDE <= poses.length; o += DEBRIS_STRIDE) {
      const isl = this.islands.get(poses[o]!);
      if (!isl || !isl.rigid) continue;
      const pos: Vec3 = [poses[o + 1]!, poses[o + 2]!, poses[o + 3]!];
      const rot: Quat = [poses[o + 4]!, poses[o + 5]!, poses[o + 6]!, poses[o + 7]!];
      const last = isl.cur;
      // (resting pieces are not re-sent every tick: after a gap, interpolate from the old pose)
      isl.prev = last === null ? { t: nowS - this.clock.interval, pos: [...isl.centroid], rot: [0, 0, 0, 1] } : this.clock.from(last, nowS);
      isl.cur = { t: nowS, pos, rot };
      const opacity = Math.max(0, Math.min(1, poses[o + 8]!));
      if (opacity !== isl.opacity) {
        isl.opacity = opacity;
        isl.dirty = true;
      }
      isl.seen = stamp;
    }
    for (const isl of this.islands.values()) {
      if (!isl.rigid || isl.seen === stamp) continue;
      if (isl.cur !== null) this.release(isl); // the engine removed it
      else if (++isl.missed >= POSE_MISSES) isl.rigid = false; // never posed: fall back
    }
  }

  /** Advances motion and fades; releases ballistic islands past their lifetime. */
  update(nowS: number): void {
    for (const isl of this.islands.values()) {
      const t = nowS - isl.born;
      if (isl.rigid) {
        this.poseRigid(isl, nowS);
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
      isl.dirty = true;
    }
  }

  /**
   * Draws the islands whose bounding sphere is in the frustum and within `maxDistance`, each
   * with its object slot (`slotBytes` apart). The world pipeline and group 0 must be set.
   */
  draw(
    pass: GPURenderPassEncoder,
    objectGroup: GPUBindGroup,
    slotBytes: number,
    planes: Float32Array,
    eye: Vec3,
    maxDistance: number,
  ): IslandDrawStats {
    let drawn = 0;
    let triangles = 0;
    for (const isl of this.islands.values()) {
      if (isl.opacity <= 0) continue;
      const p = isl.position;
      const reach = maxDistance + isl.radius;
      const dx = p[0] - eye[0];
      const dy = p[1] - eye[1];
      const dz = p[2] - eye[2];
      if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
      if (!sphereVisible(planes, p, isl.radius)) continue;
      pass.setBindGroup(1, objectGroup, [isl.slot * slotBytes]);
      pass.setVertexBuffer(0, isl.vbuf);
      pass.setIndexBuffer(isl.ibuf, 'uint32');
      pass.drawIndexed(isl.indexCount);
      drawn++;
      triangles += isl.indexCount / 3;
    }
    return { drawn, triangles };
  }

  clear(): void {
    for (const isl of this.islands.values()) this.release(isl);
  }

  /** Pose one batch interval before `nowS`, interpolated between the last two engine poses (nlerp). */
  private poseRigid(isl: GpuIsland, nowS: number): void {
    const a = isl.prev;
    const b = isl.cur;
    if (!a || !b) return; // still at the detachment pose
    const s = this.clock.weight(a.t, b.t, nowS);
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
    // resting rubble keeps its matrix: only a changed transform is uploaded again
    const m = mat4FromQuatAbout(this.scratch, q, isl.centroid, pos);
    let same = true;
    for (let k = 0; k < 16 && same; k++) same = m[k] === isl.model[k];
    if (same) return;
    isl.model.set(m);
    isl.position = pos;
    isl.dirty = true;
  }

  /**
   * The island to give up for a new one: a ballistic one, then one fading out (the faintest
   * first), then the one of fewest triangles - the oldest of equals.
   */
  private leastMissed(): GpuIsland | null {
    let best: GpuIsland | null = null;
    let key = Infinity;
    for (const isl of this.islands.values()) {
      const k = !isl.rigid ? isl.opacity - 2 : isl.opacity < 1 ? isl.opacity - 1 : isl.indexCount;
      if (k < key) {
        key = k;
        best = isl;
      }
    }
    return best;
  }

  private release(isl: GpuIsland): void {
    isl.vbuf.destroy();
    isl.ibuf.destroy();
    this.islands.delete(isl.id);
    this.freeSlots.push(isl.slot);
  }
}

/** Largest distance of a mesh vertex from `c`: the bounding sphere about the pivot. */
function meshRadius(vertices: ArrayBuffer, vertexCount: number, c: Vec3): number {
  const floats = VERTEX_STRIDE / 4;
  const f = new Float32Array(vertices, 0, vertexCount * floats);
  let r2 = 0;
  for (let i = 0; i < f.length; i += floats) {
    const dx = f[i]! - c[0];
    const dy = f[i + 1]! - c[1];
    const dz = f[i + 2]! - c[2];
    r2 = Math.max(r2, dx * dx + dy * dy + dz * dz);
  }
  return Math.sqrt(r2);
}
