/**
 * Vehicles' wheels (docs/VEHICLES.md): the engine casts them (they are not voxels), the front end
 * draws a tyre on an alloy rim at each wheel's pose (protocol `vehicles` messages, interpolated in
 * game/vehicles.ts). A mesh per wheel size and side (the rim's face outwards), built once, drawn
 * with the world pipeline at an object slot of its own per wheel; a wheel spinning fast is drawn
 * with its spokes blurred (no strobing).
 */
import { VERTEX_STRIDE, type Vec3 } from '../engine/protocol.ts';
import { MeshBuilder } from '../engine/vertex.ts';
import { mat4FromQuatAbout, sphereVisible, type Mat4 } from './math.ts';
import { buildWheel } from './wheel-mesh.ts';

export { RIM_BLUR_SLOT } from './wheel-mesh.ts';

/** rad/s above which the spokes are drawn blurred. */
const BLUR_SPIN = 14;

export interface WheelDraw {
  centre: Vec3;
  rot: readonly [number, number, number, number];
  radius: number;
  width: number;
  /** +1: the rim faces +y (a left wheel), -1: -y. */
  side: number;
  /** rad/s about its axle (blurs the spokes). */
  spin: number;
}

interface GpuMesh {
  vbuf: GPUBuffer;
  ibuf: GPUBuffer;
  indexCount: number;
}

interface Slot {
  slot: number;
  mesh: GpuMesh;
  model: Mat4;
  pos: Vec3;
  radius: number;
}

export class WheelRenderer {
  private readonly device: GPUDevice;
  private readonly meshes = new Map<string, GpuMesh>();
  private readonly firstSlot: number;
  readonly maxWheels: number;
  private list: Slot[] = [];

  constructor(device: GPUDevice, firstSlot: number, maxWheels: number) {
    this.device = device;
    this.firstSlot = firstSlot;
    this.maxWheels = maxWheels;
  }

  get count(): number {
    return this.list.length;
  }

  /** The wheels this frame: their slots' model matrices go into `objects` (floats per slot: `stride`). */
  set(wheels: readonly WheelDraw[], objects: Float32Array, stride: number): { lo: number; hi: number } {
    const n = Math.min(wheels.length, this.maxWheels);
    const list: Slot[] = [];
    for (let i = 0; i < n; i++) {
      const w = wheels[i]!;
      const blur = Math.abs(w.spin) > BLUR_SPIN;
      const mesh = this.mesh(w.radius, w.width, w.side, blur);
      const slot = this.firstSlot + i;
      const base = slot * stride;
      const model = mat4FromQuatAbout(objects.subarray(base, base + 16) as Mat4, w.rot as [number, number, number, number], [0, 0, 0], w.centre);
      objects[base + 16] = 1;
      objects[base + 17] = 0;
      objects[base + 18] = 100; // (no per-voxel variation on a cast wheel)
      list.push({ slot, mesh, model, pos: w.centre, radius: Math.hypot(w.radius, w.width / 2) });
    }
    this.list = list;
    return { lo: this.firstSlot, hi: this.firstSlot + n - 1 };
  }

  draw(pass: GPURenderPassEncoder, objectGroup: GPUBindGroup, slotBytes: number, planes: Float32Array, eye: Vec3, maxDistance: number): { drawn: number; triangles: number } {
    let drawn = 0;
    let triangles = 0;
    for (const s of this.list) {
      const dx = s.pos[0] - eye[0];
      const dy = s.pos[1] - eye[1];
      const dz = s.pos[2] - eye[2];
      if (dx * dx + dy * dy + dz * dz > maxDistance * maxDistance) continue;
      if (!sphereVisible(planes, s.pos, s.radius)) continue;
      pass.setBindGroup(1, objectGroup, [s.slot * slotBytes]);
      pass.setVertexBuffer(0, s.mesh.vbuf);
      pass.setIndexBuffer(s.mesh.ibuf, 'uint32');
      pass.drawIndexed(s.mesh.indexCount);
      drawn++;
      triangles += s.mesh.indexCount / 3;
    }
    return { drawn, triangles };
  }

  clear(): void {
    this.list = [];
  }

  private mesh(radius: number, width: number, side: number, blur: boolean): GpuMesh {
    const key = `${radius.toFixed(3)}:${width.toFixed(3)}:${side}:${blur ? 1 : 0}`;
    let m = this.meshes.get(key);
    if (m) return m;
    const b = new MeshBuilder(1024);
    buildWheel(b, radius, width, side, blur);
    const data = b.finish();
    const vbuf = this.device.createBuffer({ label: `wheel ${key} vertices`, size: Math.max(4, data.vertexCount * VERTEX_STRIDE), usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    const ibuf = this.device.createBuffer({ label: `wheel ${key} indices`, size: Math.max(4, data.indexCount * 4), usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    this.device.queue.writeBuffer(vbuf, 0, data.vertices, 0, data.vertexCount * VERTEX_STRIDE);
    this.device.queue.writeBuffer(ibuf, 0, data.indices, 0, data.indexCount * 4);
    m = { vbuf, ibuf, indexCount: data.indexCount };
    this.meshes.set(key, m);
    return m;
  }
}
