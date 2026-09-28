/**
 * Ropes and rods: the engine's distance joints (protocol `joints` messages), drawn as thin
 * square tubes between their ends with the world pipeline (lit like the world, identity model).
 * The mesh is built again whenever the joints change (every tick while anything hangs on them).
 */
import { JOINT_STRIDE, Material, TEXTURE_MATERIAL_BASE, type Vec3 } from '../engine/protocol.ts';
import { MeshBuilder } from '../engine/vertex.ts';

/** Half the side of a rope's square section (m). */
const HALF = 0.018;
const DISTANCE_JOINT = 4;
const TEXTURE = TEXTURE_MATERIAL_BASE + Material.Rebar;

export class RopeRenderer {
  private readonly device: GPUDevice;
  private readonly builder = new MeshBuilder(256);
  private vbuf: GPUBuffer | null = null;
  private ibuf: GPUBuffer | null = null;
  private indexCount = 0;

  constructor(device: GPUDevice) {
    this.device = device;
  }

  get count(): number {
    return this.indexCount / 24;
  }

  /** The joints now (JOINT_STRIDE doubles each): the distance joints' tubes. */
  set(joints: Float64Array): void {
    const b = this.builder;
    b.reset();
    for (let o = 0; o + JOINT_STRIDE <= joints.length; o += JOINT_STRIDE) {
      if (joints[o + 1] !== DISTANCE_JOINT) continue;
      const a: Vec3 = [joints[o + 2]!, joints[o + 3]!, joints[o + 4]!];
      const e: Vec3 = [joints[o + 5]!, joints[o + 6]!, joints[o + 7]!];
      tube(b, a, e);
    }
    const mesh = b.finish();
    this.indexCount = mesh.indexCount;
    if (mesh.indexCount === 0) return;
    const vbytes = mesh.vertexCount * 28;
    const ibytes = mesh.indexCount * 4;
    if (!this.vbuf || this.vbuf.size < vbytes) {
      this.vbuf?.destroy();
      this.vbuf = this.device.createBuffer({ label: 'ropes vertices', size: Math.max(256, vbytes * 2), usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    }
    if (!this.ibuf || this.ibuf.size < ibytes) {
      this.ibuf?.destroy();
      this.ibuf = this.device.createBuffer({ label: 'ropes indices', size: Math.max(256, ibytes * 2), usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    }
    this.device.queue.writeBuffer(this.vbuf, 0, mesh.vertices, 0, vbytes);
    this.device.queue.writeBuffer(this.ibuf, 0, mesh.indices, 0, ibytes);
  }

  /** Draws the ropes (the world pipeline and an identity object slot must be bound). */
  draw(pass: GPURenderPassEncoder): number {
    if (this.indexCount === 0 || !this.vbuf || !this.ibuf) return 0;
    pass.setVertexBuffer(0, this.vbuf);
    pass.setIndexBuffer(this.ibuf, 'uint32');
    pass.drawIndexed(this.indexCount);
    return this.indexCount / 3;
  }

  clear(): void {
    this.indexCount = 0;
  }
}

/** A square tube from a to e: four quads, each with its own normal. */
function tube(b: MeshBuilder, a: Vec3, e: Vec3): void {
  const d: Vec3 = [e[0] - a[0], e[1] - a[1], e[2] - a[2]];
  const len = Math.hypot(d[0], d[1], d[2]);
  if (len < 1e-6) return;
  const t: Vec3 = [d[0] / len, d[1] / len, d[2] / len];
  // two directions square to it
  const ref: Vec3 = Math.abs(t[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0];
  let u: Vec3 = [t[1] * ref[2] - t[2] * ref[1], t[2] * ref[0] - t[0] * ref[2], t[0] * ref[1] - t[1] * ref[0]];
  const ul = Math.hypot(u[0], u[1], u[2]);
  u = [u[0] / ul, u[1] / ul, u[2] / ul];
  const v: Vec3 = [t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0]];
  const side = (n: Vec3, s: Vec3): void => {
    // the face's corners: centre line offset by HALF along n, +-HALF along s
    const base = b.vertexCount;
    for (const [p, w] of [
      [a, -1],
      [a, 1],
      [e, 1],
      [e, -1],
    ] as [Vec3, number][]) {
      b.vertex(
        p[0] + (n[0] + s[0] * w) * HALF,
        p[1] + (n[1] + s[1] * w) * HALF,
        p[2] + (n[2] + s[2] * w) * HALF,
        n[0],
        n[1],
        n[2],
        1,
        0,
        0,
        TEXTURE,
        255,
        0,
      );
    }
    b.triangle(base, base + 1, base + 2);
    b.triangle(base, base + 2, base + 3);
  };
  side(u, v);
  side(v, [-u[0], -u[1], -u[2]]);
  side([-u[0], -u[1], -u[2]], [-v[0], -v[1], -v[2]]);
  side([-v[0], -v[1], -v[2]], u);
}
