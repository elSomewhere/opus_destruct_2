/**
 * GPU residency of chunk meshes, keyed by the engine's opaque chunk key.
 *
 * Meshes are replaced often (edits, cracks, debug views; with v1 engines also displaced chunks
 * re-sent every tick), so buffers are reused whenever the new data fits, and grown with slack
 * otherwise.
 * Bounds come from the actual vertex positions, so displaced geometry is culled correctly.
 */
import type { ChunkMesh, Vec3 } from '../engine/protocol.ts';
import { vertexBounds } from '../engine/vertex.ts';
import { aabbVisible } from './math.ts';

interface GpuChunk {
  key: string;
  vbuf: GPUBuffer;
  ibuf: GPUBuffer;
  indexCount: number;
  min: Vec3;
  max: Vec3;
  /** Squared distance to the eye, refreshed while drawing (for front-to-back order). */
  d2: number;
}

export interface ChunkDrawStats {
  drawn: number;
  triangles: number;
}

function capacityFor(bytes: number): number {
  // 25% slack, multiple of 256 (and therefore of 4, as writeBuffer requires).
  return Math.max(256, Math.ceil((bytes * 1.25) / 256) * 256);
}

export class ChunkStore {
  private readonly device: GPUDevice;
  private readonly chunks = new Map<string, GpuChunk>();
  private visible: GpuChunk[] = [];
  private gpuBytes = 0;
  /** Meshes uploaded since the last `takeUploadStats`. */
  private uploads = 0;
  private uploadedBytes = 0;

  constructor(device: GPUDevice) {
    this.device = device;
  }

  get count(): number {
    return this.chunks.size;
  }

  get bytes(): number {
    return this.gpuBytes;
  }

  takeUploadStats(): { uploads: number; bytes: number } {
    const r = { uploads: this.uploads, bytes: this.uploadedBytes };
    this.uploads = 0;
    this.uploadedBytes = 0;
    return r;
  }

  upsert(mesh: ChunkMesh): void {
    if (mesh.indexCount === 0 || mesh.vertexCount === 0) {
      this.remove(mesh.key);
      return;
    }
    const bounds = vertexBounds(mesh.vertices, mesh.vertexCount);
    if (!bounds) return;
    const vbytes = mesh.vertexCount * 28;
    const ibytes = mesh.indexCount * 4;
    let c = this.chunks.get(mesh.key);
    if (c && (c.vbuf.size < vbytes || c.ibuf.size < ibytes)) {
      this.destroy(c);
      this.chunks.delete(mesh.key);
      c = undefined;
    }
    if (!c) {
      const vbuf = this.device.createBuffer({
        label: `chunk ${mesh.key} vertices`,
        size: capacityFor(vbytes),
        usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
      });
      const ibuf = this.device.createBuffer({
        label: `chunk ${mesh.key} indices`,
        size: capacityFor(ibytes),
        usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST,
      });
      this.gpuBytes += vbuf.size + ibuf.size;
      c = { key: mesh.key, vbuf, ibuf, indexCount: 0, min: bounds.min, max: bounds.max, d2: 0 };
      this.chunks.set(mesh.key, c);
    }
    this.device.queue.writeBuffer(c.vbuf, 0, mesh.vertices, 0, vbytes);
    this.device.queue.writeBuffer(c.ibuf, 0, mesh.indices, 0, ibytes);
    c.indexCount = mesh.indexCount;
    c.min = bounds.min;
    c.max = bounds.max;
    this.uploads++;
    this.uploadedBytes += vbytes + ibytes;
  }

  remove(key: string): void {
    const c = this.chunks.get(key);
    if (!c) return;
    this.destroy(c);
    this.chunks.delete(key);
  }

  clear(): void {
    for (const c of this.chunks.values()) this.destroy(c);
    this.chunks.clear();
  }

  private destroy(c: GpuChunk): void {
    this.gpuBytes -= c.vbuf.size + c.ibuf.size;
    c.vbuf.destroy();
    c.ibuf.destroy();
  }

  /**
   * Draws frustum-visible chunks within `maxDistance`, nearest first (early depth
   * rejection). The pipeline and bind groups must already be set.
   */
  /**
   * `backToFront`: farthest first (translucent surfaces blended over what is behind them)
   * instead of nearest first.
   */
  draw(
    pass: GPURenderPassEncoder,
    planes: Float32Array,
    eye: Vec3,
    maxDistance: number,
    inflation?: (min: Vec3, max: Vec3) => number,
    backToFront = false,
  ): ChunkDrawStats {
    const vis = this.visible;
    vis.length = 0;
    const max2 = maxDistance * maxDistance;
    for (const c of this.chunks.values()) {
      // geometry inside displacement fields may move beyond its mesh bounds
      let min = c.min;
      let max = c.max;
      const r = inflation ? inflation(min, max) : 0;
      if (r > 0) {
        min = [min[0] - r, min[1] - r, min[2] - r];
        max = [max[0] + r, max[1] + r, max[2] + r];
      }
      if (!aabbVisible(planes, min, max)) continue;
      // Distance from the eye to the box (0 inside).
      let d2 = 0;
      for (let a = 0; a < 3; a++) {
        const v = eye[a]! < min[a]! ? min[a]! - eye[a]! : eye[a]! > max[a]! ? eye[a]! - max[a]! : 0;
        d2 += v * v;
      }
      if (d2 > max2) continue;
      c.d2 = d2;
      vis.push(c);
    }
    vis.sort(backToFront ? (a, b) => b.d2 - a.d2 : (a, b) => a.d2 - b.d2);
    let triangles = 0;
    for (const c of vis) {
      pass.setVertexBuffer(0, c.vbuf);
      pass.setIndexBuffer(c.ibuf, 'uint32');
      pass.drawIndexed(c.indexCount);
      triangles += c.indexCount / 3;
    }
    return { drawn: vis.length, triangles };
  }
}
