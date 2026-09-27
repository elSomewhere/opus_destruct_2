/**
 * Voxel characters on the GPU (svx_anim meshes, character.wgsl):
 * - meshes in the svx_anim character vertex format (20 B), uploaded once per geometry (a
 *   damaged character gets its own);
 * - per frame, instances: a mesh, its skin matrices (rigid skinning), a palette, sector light,
 *   opacity and a tint (hit flash). Skin matrices of all instances go into one storage buffer;
 *   each draw selects its instance record with `firstInstance`;
 * - decals (blob shadows under characters, blood splats) and voxel bits (instanced cubes:
 *   blood drops, casings, chips).
 *
 * The frame bind group (group 0) is shared with the world pipelines; group 1 holds the
 * character storage buffers.
 */
import type { Vec3 } from '../engine/protocol.ts';
import { CHAR_VERTEX_STRIDE, type CharacterMesh, type Palette } from 'svx-anim';
import { sphereVisible } from './math.ts';
import characterWgsl from './shaders/character.wgsl?raw';

export interface GpuCharacterMesh {
  vbuf: GPUBuffer;
  ibuf: GPUBuffer;
  indexCount: number;
  bytes: number;
}

export interface CharacterInstanceOptions {
  /** Bounding sphere (world) for culling. */
  center: Vec3;
  radius: number;
  /** Sector light 0..1 (default 1). */
  light?: number;
  /** 0..1, dithered (default 1). */
  opacity?: number;
  /** Tint colour (linear rgb) and amount 0..1. */
  tint?: [number, number, number, number];
}

export interface CharacterDrawStats {
  instances: number;
  drawn: number;
  triangles: number;
  decals: number;
  bits: number;
}

const INSTANCE_BYTES = 32;
const DECAL_FLOATS = 12;
const BIT_FLOATS = 12;
const MAX_PALETTES = 512;

interface Pending {
  mesh: GpuCharacterMesh;
  boneBase: number;
  palette: number;
  light: number;
  opacity: number;
  tint: [number, number, number, number];
  center: Vec3;
  radius: number;
}

function growBuffer(device: GPUDevice, old: GPUBuffer | null, bytes: number, usage: number, label: string): GPUBuffer {
  if (old && old.size >= bytes) return old;
  old?.destroy();
  let size = 1024;
  while (size < bytes) size *= 2;
  return device.createBuffer({ label, size, usage });
}

export class CharacterRenderer {
  private readonly device: GPUDevice;
  private readonly charPipeline: GPURenderPipeline;
  private readonly decalPipeline: GPURenderPipeline;
  private readonly bitPipeline: GPURenderPipeline;
  private readonly groupLayout: GPUBindGroupLayout;
  private bindGroup: GPUBindGroup | null = null;
  private boneBuffer: GPUBuffer | null = null;
  private instanceBuffer: GPUBuffer | null = null;
  private readonly paletteBuffer: GPUBuffer;
  private decalBuffer: GPUBuffer | null = null;
  private bitBuffer: GPUBuffer | null = null;
  private readonly palettes = new Map<Palette, number>();
  private readonly paletteData = new Float32Array(MAX_PALETTES * 16 * 4);
  private paletteDirty = false;
  private bones = new Float32Array(64 * 16 * 16);
  private boneCount = 0;
  private readonly pending: Pending[] = [];
  private decals = new Float32Array(256 * DECAL_FLOATS);
  private decalCount = 0;
  private bits = new Float32Array(1024 * BIT_FLOATS);
  private bitCount = 0;
  private meshBytes = 0;

  constructor(device: GPUDevice, frameLayout: GPUBindGroupLayout, frameWgsl: string, format: GPUTextureFormat, samples: number, depthFormat: GPUTextureFormat) {
    this.device = device;
    this.groupLayout = device.createBindGroupLayout({
      label: 'characters',
      entries: [
        { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: 'read-only-storage' } },
        { binding: 1, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } },
        { binding: 2, visibility: GPUShaderStage.VERTEX, buffer: { type: 'read-only-storage' } },
      ],
    });
    this.paletteBuffer = device.createBuffer({ label: 'character palettes', size: this.paletteData.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
    const module = device.createShaderModule({ label: 'characters', code: `${frameWgsl}\n${characterWgsl}` });
    void module.getCompilationInfo().then((info) => {
      for (const msg of info.messages) {
        const log = msg.type === 'error' ? console.error : console.warn;
        log(`[wgsl characters] ${msg.type} ${msg.lineNum}:${msg.linePos} ${msg.message}`);
      }
    });
    const layout = device.createPipelineLayout({ label: 'frame+characters', bindGroupLayouts: [frameLayout, this.groupLayout] });
    const frameOnly = device.createPipelineLayout({ label: 'frame only (character fx)', bindGroupLayouts: [frameLayout] });
    const multisample: GPUMultisampleState = { count: samples };
    this.charPipeline = device.createRenderPipeline({
      label: 'characters',
      layout,
      vertex: {
        module,
        entryPoint: 'vsChar',
        buffers: [
          {
            arrayStride: CHAR_VERTEX_STRIDE,
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x3' },
              { shaderLocation: 1, offset: 12, format: 'snorm8x4' },
              { shaderLocation: 2, offset: 16, format: 'uint32' },
            ],
          },
        ],
      },
      fragment: { module, entryPoint: 'fsChar', targets: [{ format }] },
      primitive: { topology: 'triangle-list', cullMode: 'back', frontFace: 'ccw' },
      depthStencil: { format: depthFormat, depthWriteEnabled: true, depthCompare: 'greater' },
      multisample,
    });
    const premultiplied: GPUBlendComponent = { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' };
    this.decalPipeline = device.createRenderPipeline({
      label: 'character decals',
      layout: frameOnly,
      vertex: {
        module,
        entryPoint: 'vsDecal',
        buffers: [
          {
            arrayStride: DECAL_FLOATS * 4,
            stepMode: 'instance',
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x4' },
              { shaderLocation: 1, offset: 16, format: 'float32x4' },
              { shaderLocation: 2, offset: 32, format: 'float32x4' },
            ],
          },
        ],
      },
      fragment: { module, entryPoint: 'fsDecal', targets: [{ format, blend: { color: premultiplied, alpha: premultiplied } }] },
      primitive: { topology: 'triangle-list', cullMode: 'none' },
      depthStencil: { format: depthFormat, depthWriteEnabled: false, depthCompare: 'greater' },
      multisample,
    });
    this.bitPipeline = device.createRenderPipeline({
      label: 'voxel bits',
      layout: frameOnly,
      vertex: {
        module,
        entryPoint: 'vsBit',
        buffers: [
          {
            arrayStride: BIT_FLOATS * 4,
            stepMode: 'instance',
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x4' },
              { shaderLocation: 1, offset: 16, format: 'float32x4' },
              { shaderLocation: 2, offset: 32, format: 'float32x4' },
            ],
          },
        ],
      },
      fragment: { module, entryPoint: 'fsBit', targets: [{ format }] },
      primitive: { topology: 'triangle-list', cullMode: 'back', frontFace: 'ccw' },
      depthStencil: { format: depthFormat, depthWriteEnabled: true, depthCompare: 'greater' },
      multisample,
    });
  }

  get gpuBytes(): number {
    return this.meshBytes;
  }

  uploadMesh(mesh: CharacterMesh, label = 'character'): GpuCharacterMesh {
    const vbytes = Math.max(4, mesh.vertexCount * CHAR_VERTEX_STRIDE);
    const ibytes = Math.max(4, mesh.indexCount * 4);
    const vbuf = this.device.createBuffer({ label: `${label} vertices`, size: vbytes, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    const ibuf = this.device.createBuffer({ label: `${label} indices`, size: ibytes, usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    if (mesh.vertexCount > 0) this.device.queue.writeBuffer(vbuf, 0, mesh.vertices, 0, mesh.vertexCount * CHAR_VERTEX_STRIDE);
    if (mesh.indexCount > 0) this.device.queue.writeBuffer(ibuf, 0, mesh.indices.buffer, mesh.indices.byteOffset, mesh.indexCount * 4);
    this.meshBytes += vbytes + ibytes;
    return { vbuf, ibuf, indexCount: mesh.indexCount, bytes: vbytes + ibytes };
  }

  releaseMesh(m: GpuCharacterMesh): void {
    m.vbuf.destroy();
    m.ibuf.destroy();
    this.meshBytes -= m.bytes;
  }

  /** Palette index (registered once per Palette object). */
  palette(p: Palette): number {
    const have = this.palettes.get(p);
    if (have !== undefined) return have;
    const id = this.palettes.size % MAX_PALETTES;
    this.palettes.set(p, id);
    for (let s = 0; s < 16; s++) {
      const c = p[s] ?? [1, 0, 1];
      this.paletteData.set([c[0], c[1], c[2], 1], (id * 16 + s) * 4);
    }
    this.paletteDirty = true;
    return id;
  }

  /** Starts collecting this frame's instances, decals and bits. */
  begin(): void {
    this.pending.length = 0;
    this.boneCount = 0;
    this.decalCount = 0;
    this.bitCount = 0;
  }

  /** One character (or gib) this frame: `skin` holds `bones` 4x4 matrices. */
  add(mesh: GpuCharacterMesh, skin: Float32Array, bones: number, palette: number, o: CharacterInstanceOptions): void {
    if (mesh.indexCount === 0) return;
    const need = (this.boneCount + bones) * 16;
    if (need > this.bones.length) {
      const next = new Float32Array(Math.max(need, this.bones.length * 2));
      next.set(this.bones.subarray(0, this.boneCount * 16));
      this.bones = next;
    }
    this.bones.set(skin.subarray(0, bones * 16), this.boneCount * 16);
    this.pending.push({
      mesh,
      boneBase: this.boneCount,
      palette,
      light: o.light ?? 1,
      opacity: o.opacity ?? 1,
      tint: o.tint ?? [0, 0, 0, 0],
      center: o.center,
      radius: o.radius,
    });
    this.boneCount += bones;
  }

  /**
   * A decal quad on a surface: `kind` 0 = soft blob (shadows), 1 = blocky splat (blood).
   * `color` is linear rgb + strength.
   */
  decal(pos: Vec3, normal: Vec3, radius: number, color: [number, number, number, number], kind: 0 | 1): void {
    if ((this.decalCount + 1) * DECAL_FLOATS > this.decals.length) {
      const next = new Float32Array(this.decals.length * 2);
      next.set(this.decals);
      this.decals = next;
    }
    this.decals.set([pos[0], pos[1], pos[2], radius, normal[0], normal[1], normal[2], kind, color[0], color[1], color[2], color[3]], this.decalCount * DECAL_FLOATS);
    this.decalCount++;
  }

  /** A small cube: centre, half size, rotation (quaternion xyzw), linear colour, sector light. */
  bit(pos: Vec3, half: number, rot: readonly number[], color: readonly number[], light = 1): void {
    if ((this.bitCount + 1) * BIT_FLOATS > this.bits.length) {
      const next = new Float32Array(this.bits.length * 2);
      next.set(this.bits);
      this.bits = next;
    }
    const o = this.bitCount * BIT_FLOATS;
    const b = this.bits;
    b[o] = pos[0];
    b[o + 1] = pos[1];
    b[o + 2] = pos[2];
    b[o + 3] = half;
    b[o + 4] = rot[0]!;
    b[o + 5] = rot[1]!;
    b[o + 6] = rot[2]!;
    b[o + 7] = rot[3]!;
    b[o + 8] = color[0]!;
    b[o + 9] = color[1]!;
    b[o + 10] = color[2]!;
    b[o + 11] = light;
    this.bitCount++;
  }

  /** Uploads the frame's data (call before the render pass). */
  upload(): void {
    const d = this.device;
    const usage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;
    const boneBytes = Math.max(64, this.boneCount * 64);
    const instBytes = Math.max(INSTANCE_BYTES, this.pending.length * INSTANCE_BYTES);
    const oldBones = this.boneBuffer;
    const oldInst = this.instanceBuffer;
    this.boneBuffer = growBuffer(d, this.boneBuffer, boneBytes, usage, 'character bones');
    this.instanceBuffer = growBuffer(d, this.instanceBuffer, instBytes, usage, 'character instances');
    if (!this.bindGroup || oldBones !== this.boneBuffer || oldInst !== this.instanceBuffer) {
      this.bindGroup = d.createBindGroup({
        label: 'characters',
        layout: this.groupLayout,
        entries: [
          { binding: 0, resource: { buffer: this.boneBuffer } },
          { binding: 1, resource: { buffer: this.instanceBuffer } },
          { binding: 2, resource: { buffer: this.paletteBuffer } },
        ],
      });
    }
    if (this.boneCount > 0) d.queue.writeBuffer(this.boneBuffer, 0, this.bones, 0, this.boneCount * 16);
    if (this.pending.length > 0) {
      const buf = new ArrayBuffer(this.pending.length * INSTANCE_BYTES);
      const u = new Uint32Array(buf);
      const f = new Float32Array(buf);
      this.pending.forEach((p, i) => {
        const o = i * 8;
        u[o] = p.boneBase;
        u[o + 1] = p.palette;
        f[o + 2] = p.light;
        f[o + 3] = p.opacity;
        f.set(p.tint, o + 4);
      });
      d.queue.writeBuffer(this.instanceBuffer, 0, buf);
    }
    if (this.paletteDirty) {
      d.queue.writeBuffer(this.paletteBuffer, 0, this.paletteData);
      this.paletteDirty = false;
    }
    if (this.decalCount > 0) {
      this.decalBuffer = growBuffer(d, this.decalBuffer, this.decalCount * DECAL_FLOATS * 4, GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST, 'decals');
      d.queue.writeBuffer(this.decalBuffer, 0, this.decals, 0, this.decalCount * DECAL_FLOATS);
    }
    if (this.bitCount > 0) {
      this.bitBuffer = growBuffer(d, this.bitBuffer, this.bitCount * BIT_FLOATS * 4, GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST, 'voxel bits');
      d.queue.writeBuffer(this.bitBuffer, 0, this.bits, 0, this.bitCount * BIT_FLOATS);
    }
  }

  /**
   * Draws decals (before the characters, so shadows never darken them), characters and bits.
   * Group 0 (frame) must be bound.
   */
  draw(pass: GPURenderPassEncoder, planes: Float32Array, eye: Vec3, maxDistance: number): CharacterDrawStats {
    let drawn = 0;
    let triangles = 0;
    if (this.decalCount > 0 && this.decalBuffer) {
      pass.setPipeline(this.decalPipeline);
      pass.setVertexBuffer(0, this.decalBuffer);
      pass.draw(6, this.decalCount);
    }
    if (this.pending.length > 0 && this.bindGroup) {
      pass.setPipeline(this.charPipeline);
      pass.setBindGroup(1, this.bindGroup);
      this.pending.forEach((p, i) => {
        const dx = p.center[0] - eye[0], dy = p.center[1] - eye[1], dz = p.center[2] - eye[2];
        const reach = maxDistance + p.radius;
        if (dx * dx + dy * dy + dz * dz > reach * reach) return;
        if (!sphereVisible(planes, p.center, p.radius)) return;
        pass.setVertexBuffer(0, p.mesh.vbuf);
        pass.setIndexBuffer(p.mesh.ibuf, 'uint32');
        pass.drawIndexed(p.mesh.indexCount, 1, 0, 0, i);
        drawn++;
        triangles += p.mesh.indexCount / 3;
      });
    }
    if (this.bitCount > 0 && this.bitBuffer) {
      pass.setPipeline(this.bitPipeline);
      pass.setVertexBuffer(0, this.bitBuffer);
      pass.draw(36, this.bitCount);
      triangles += this.bitCount * 12;
    }
    return { instances: this.pending.length, drawn, triangles, decals: this.decalCount, bits: this.bitCount };
  }
}
