/**
 * The oriented grids' chunk meshes (docs/GRIDS.md): meshed in each grid's lattice, drawn with
 * its frame (engine/gridframes.ts) as the model matrix of the grid's object slot. A grid that
 * moves (a kinematic body's) is never meshed again for it: only its model changes.
 */
import type { GridFrame, GridFrames } from '../engine/gridframes.ts';
import type { ChunkMesh, Vec3 } from '../engine/protocol.ts';
import { vertexBounds } from '../engine/vertex.ts';
import { aabbVisible, mat4, mat4FromQuatAbout, type Mat4 } from './math.ts';

interface GpuGridChunk {
  vbuf: GPUBuffer;
  ibuf: GPUBuffer;
  indexCount: number;
  /** Bounds in the grid's lattice, and in the world at its frame (refreshed when it moves). */
  lmin: Vec3;
  lmax: Vec3;
  wmin: Vec3;
  wmax: Vec3;
  d2: number;
}

export interface GpuGrid {
  id: number;
  /** Object uniform slot. */
  slot: number;
  chunks: Map<string, GpuGridChunk>;
  model: Mat4;
  h: number;
  /** The frame version the model and bounds were made for (-1: none yet: not drawn). */
  version: number;
  /** `model` changed since the renderer last uploaded the slot. */
  dirty: boolean;
}

export interface GridDrawStats {
  drawn: number;
  triangles: number;
}

function capacityFor(bytes: number): number {
  return Math.max(256, Math.ceil((bytes * 1.25) / 256) * 256);
}

/** The grid id of a grid chunk key (`g<id>:x,y,z`), or null. */
export function gridOfKey(key: string): number | null {
  if (key.charCodeAt(0) !== 103 /* g */) return null;
  const c = key.indexOf(':');
  const id = c > 1 ? Number(key.slice(1, c)) : NaN;
  return Number.isFinite(id) ? id : null;
}

export class GridRenderer {
  private readonly device: GPUDevice;
  private readonly grids = new Map<number, GpuGrid>();
  private readonly freeSlots: number[] = [];
  private readonly visible: { c: GpuGridChunk; g: GpuGrid }[] = [];
  private gpuBytes = 0;

  /** Slots firstSlot .. firstSlot + maxGrids - 1 of the object uniforms are the grids'. */
  constructor(device: GPUDevice, firstSlot: number, maxGrids: number) {
    this.device = device;
    for (let s = firstSlot + maxGrids - 1; s >= firstSlot; s--) this.freeSlots.push(s);
  }

  get list(): MapIterator<GpuGrid> {
    return this.grids.values();
  }

  get count(): number {
    let n = 0;
    for (const g of this.grids.values()) n += g.chunks.size;
    return n;
  }

  get bytes(): number {
    return this.gpuBytes;
  }

  private gridOf(id: number): GpuGrid | null {
    let g = this.grids.get(id);
    if (g) return g;
    const slot = this.freeSlots.pop();
    if (slot === undefined) return null; // (over capacity: not drawn)
    g = { id, slot, chunks: new Map(), model: mat4(), h: 0, version: -1, dirty: true };
    this.grids.set(id, g);
    return g;
  }

  upsert(mesh: ChunkMesh): void {
    if (mesh.grid === undefined) return;
    if (mesh.indexCount === 0 || mesh.vertexCount === 0) {
      this.remove(mesh.key);
      return;
    }
    const g = this.gridOf(mesh.grid);
    if (!g) return;
    const bounds = vertexBounds(mesh.vertices, mesh.vertexCount);
    if (!bounds) return;
    const vbytes = mesh.vertexCount * 28;
    const ibytes = mesh.indexCount * 4;
    let c = g.chunks.get(mesh.key);
    if (c && (c.vbuf.size < vbytes || c.ibuf.size < ibytes)) {
      this.destroy(c);
      g.chunks.delete(mesh.key);
      c = undefined;
    }
    if (!c) {
      const vbuf = this.device.createBuffer({ label: `grid chunk ${mesh.key} vertices`, size: capacityFor(vbytes), usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
      const ibuf = this.device.createBuffer({ label: `grid chunk ${mesh.key} indices`, size: capacityFor(ibytes), usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
      this.gpuBytes += vbuf.size + ibuf.size;
      c = { vbuf, ibuf, indexCount: 0, lmin: bounds.min, lmax: bounds.max, wmin: [0, 0, 0], wmax: [0, 0, 0], d2: 0 };
      g.chunks.set(mesh.key, c);
    }
    this.device.queue.writeBuffer(c.vbuf, 0, mesh.vertices, 0, vbytes);
    this.device.queue.writeBuffer(c.ibuf, 0, mesh.indices, 0, ibytes);
    c.indexCount = mesh.indexCount;
    c.lmin = bounds.min;
    c.lmax = bounds.max;
    if (g.version >= 0) this.worldBounds(g, c);
  }

  /** Removes a grid chunk by its key; false: not a grid chunk's key. */
  remove(key: string): boolean {
    const id = gridOfKey(key);
    if (id === null) return false;
    const g = this.grids.get(id);
    const c = g?.chunks.get(key);
    if (g && c) {
      this.destroy(c);
      g.chunks.delete(key);
    }
    return true;
  }

  /** A grid gone: its chunks and its slot. */
  removeGrid(id: number): void {
    const g = this.grids.get(id);
    if (!g) return;
    for (const c of g.chunks.values()) this.destroy(c);
    this.grids.delete(id);
    this.freeSlots.push(g.slot);
  }

  clear(): void {
    for (const id of [...this.grids.keys()]) this.removeGrid(id);
  }

  /** The grids' models from their frames (only those whose frame changed). */
  update(frames: GridFrames): void {
    for (const g of this.grids.values()) {
      const f = frames.get(g.id);
      if (!f || f.version === g.version) continue;
      this.place(g, f);
    }
  }

  private place(g: GpuGrid, f: GridFrame): void {
    mat4FromQuatAbout(g.model, f.rot, [0, 0, 0], f.origin);
    g.h = f.h;
    g.version = f.version;
    g.dirty = true;
    for (const c of g.chunks.values()) this.worldBounds(g, c);
  }

  private worldBounds(g: GpuGrid, c: GpuGridChunk): void {
    const m = g.model;
    const mn: Vec3 = [Infinity, Infinity, Infinity];
    const mx: Vec3 = [-Infinity, -Infinity, -Infinity];
    for (let k = 0; k < 8; k++) {
      const x = k & 1 ? c.lmax[0] : c.lmin[0];
      const y = k & 2 ? c.lmax[1] : c.lmin[1];
      const z = k & 4 ? c.lmax[2] : c.lmin[2];
      for (let a = 0; a < 3; a++) {
        const w = m[a]! * x + m[4 + a]! * y + m[8 + a]! * z + m[12 + a]!;
        if (w < mn[a]!) mn[a] = w;
        if (w > mx[a]!) mx[a] = w;
      }
    }
    c.wmin = mn;
    c.wmax = mx;
  }

  /** Draws the visible grid chunks, nearest first, each with its grid's slot. */
  draw(pass: GPURenderPassEncoder, objectGroup: GPUBindGroup, slotBytes: number, planes: Float32Array, eye: Vec3, maxDistance: number): GridDrawStats {
    const vis = this.visible;
    vis.length = 0;
    const max2 = maxDistance * maxDistance;
    for (const g of this.grids.values()) {
      if (g.version < 0) continue; // (no frame yet)
      for (const c of g.chunks.values()) {
        if (!aabbVisible(planes, c.wmin, c.wmax)) continue;
        let d2 = 0;
        for (let a = 0; a < 3; a++) {
          const v = eye[a]! < c.wmin[a]! ? c.wmin[a]! - eye[a]! : eye[a]! > c.wmax[a]! ? eye[a]! - c.wmax[a]! : 0;
          d2 += v * v;
        }
        if (d2 > max2) continue;
        c.d2 = d2;
        vis.push({ c, g });
      }
    }
    vis.sort((a, b) => a.c.d2 - b.c.d2);
    let triangles = 0;
    let bound = -1;
    for (const { c, g } of vis) {
      if (g.slot !== bound) {
        pass.setBindGroup(1, objectGroup, [g.slot * slotBytes]);
        bound = g.slot;
      }
      pass.setVertexBuffer(0, c.vbuf);
      pass.setIndexBuffer(c.ibuf, 'uint32');
      pass.drawIndexed(c.indexCount);
      triangles += c.indexCount / 3;
    }
    return { drawn: vis.length, triangles };
  }

  private destroy(c: GpuGridChunk): void {
    this.gpuBytes -= c.vbuf.size + c.ibuf.size;
    c.vbuf.destroy();
    c.ibuf.destroy();
  }
}
