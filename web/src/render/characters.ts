/**
 * Voxel characters on the GPU: the engine's people and the gibs that come off them (protocol
 * `characterMeshes`, `characters` and `blood` messages; shaders/character.wgsl), after euphoria_3's
 * character renderer:
 * - meshes in the svx_anim character vertex format (20 B, rest model space), uploaded as they come
 *   and destroyed when the engine drops them (a damaged character gets one of its own); palettes
 *   (16 linear colours) in a storage buffer, kept for good (an engine sends each once);
 * - per frame, an instance per character in view (bounding sphere): its skin matrices (rigid
 *   skinning; every instance's in one storage buffer - a gib's one), palette, opacity (dithered)
 *   and tint (a hit's flash). A mesh's instances are drawn in one call, each finding its record by
 *   `instance_index` (firstInstance);
 * - decals, drawn before them: soft blob shadows under the people on their feet, blocky blood
 *   stains on the world; voxel bits after them: the blood drops in flight, small lit cubes.
 *
 * Group 0 is the frame's (shared with the world pipelines); group 1 holds the character storage
 * buffers.
 */
import type { CharacterMesh, Vec3 } from '../engine/protocol.ts';
import { BLOOD_DROP_STRIDE, BLOOD_STAIN_STRIDE, CHAR_VERTEX_STRIDE, CHARACTER_PALETTE_SLOTS, CHARACTER_SKIN_FLOATS } from '../engine/protocol.ts';
import { sphereVisible } from './math.ts';

/** A character to draw this frame (game/people.ts poses them). */
export interface CharacterDraw {
  mesh: number;
  palette: number;
  /** Its bones' matrices, rest model space -> world. */
  skin: Float32Array;
  /** The matrices that count: CHARACTER_BONES, a gib's first alone. */
  bones: number;
  /** Bounding sphere (world). */
  centre: Vec3;
  radius: number;
  /** 0..1, dithered. */
  opacity: number;
  /** 0..1: a hit's flash. */
  flash: number;
  /** On its feet: where its blob shadow goes, on the ground under it (null: none). */
  shadow: Vec3 | null;
  /** Its prop's mesh (0: none), drawn at `prop` (16 floats) as its bone 0. */
  propMesh: number;
  prop: Float32Array | null;
}

export interface CharacterDrawStats {
  /** Characters this frame, and those drawn (in view). */
  characters: number;
  drawn: number;
  triangles: number;
  /** Blood drops in flight and stains. */
  drops: number;
  stains: number;
}

interface GpuMesh {
  vbuf: GPUBuffer;
  ibuf: GPUBuffer;
  indexCount: number;
  bytes: number;
}

interface Pending {
  draw: CharacterDraw;
  mesh: GpuMesh;
  meshId: number;
  place: number;
  /** The prop (its matrix as the one bone), not the body. */
  prop: boolean;
}

interface Batch {
  mesh: GpuMesh;
  first: number;
  count: number;
}

const INSTANCE_BYTES = 32;
const DECAL_FLOATS = 12;
const BIT_FLOATS = 12;
/** A hit's flash: this red, at most this much of the colour. */
const FLASH_TINT: readonly number[] = [1, 0.15, 0.1];
const FLASH_AMOUNT = 0.45;
const SHADOW_RADIUS = 0.4;
const SHADOW_STRENGTH = 0.5;
/** Stains: the blood's colour (the palettes' 0x5c0808, linear) a little darker on the ground. */
const STAIN_COLOR: readonly number[] = [0.107 * 0.8, 0.0024 * 0.8, 0.0024 * 0.8];
const UP: Vec3 = [0, 0, 1];

function growBuffer(device: GPUDevice, old: GPUBuffer | null, bytes: number, usage: number, label: string): GPUBuffer {
  if (old && old.size >= bytes) return old;
  old?.destroy();
  let size = 1024;
  while (size < bytes) size *= 2;
  return device.createBuffer({ label, size, usage });
}

function within(p: ArrayLike<number>, o: number, eye: Vec3, d: number): boolean {
  const dx = p[o]! - eye[0];
  const dy = p[o + 1]! - eye[1];
  const dz = p[o + 2]! - eye[2];
  return dx * dx + dy * dy + dz * dz <= d * d;
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
  private paletteBuffer: GPUBuffer | null = null;
  private decalBuffer: GPUBuffer | null = null;
  private bitBuffer: GPUBuffer | null = null;
  private readonly meshes = new Map<number, GpuMesh>();
  /** Palette ids -> their places in the palette buffer. */
  private readonly palettes = new Map<number, number>();
  private paletteData = new Float32Array(64 * CHARACTER_PALETTE_SLOTS * 4);
  private paletteDirty = false;
  private bones = new Float32Array(64 * CHARACTER_SKIN_FLOATS);
  private boneCount = 0;
  private instances = new ArrayBuffer(64 * INSTANCE_BYTES);
  private readonly pending: Pending[] = [];
  private readonly batches: Batch[] = [];
  private decals = new Float32Array(256 * DECAL_FLOATS);
  private decalCount = 0;
  private bits = new Float32Array(256 * BIT_FLOATS);
  private bitCount = 0;
  /** The engine's blood (BLOOD_DROP_STRIDE / BLOOD_STAIN_STRIDE floats each). */
  private drops: Float32Array = new Float32Array(0);
  private stains: Float32Array = new Float32Array(0);
  private listed = 0;
  private bodies = 0;
  private meshBytes = 0;

  /** `module`: character.wgsl after frame.wgsl. */
  constructor(device: GPUDevice, frameLayout: GPUBindGroupLayout, module: GPUShaderModule, format: GPUTextureFormat, samples: number, depthFormat: GPUTextureFormat) {
    this.device = device;
    this.groupLayout = device.createBindGroupLayout({
      label: 'characters',
      entries: [
        { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: 'read-only-storage' } },
        { binding: 1, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } },
        { binding: 2, visibility: GPUShaderStage.VERTEX, buffer: { type: 'read-only-storage' } },
      ],
    });
    const layout = device.createPipelineLayout({ label: 'frame+characters', bindGroupLayouts: [frameLayout, this.groupLayout] });
    const frameOnly = device.createPipelineLayout({ label: 'frame only (character decals, bits)', bindGroupLayouts: [frameLayout] });
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
    const instanced = (stride: number): GPUVertexBufferLayout => ({
      arrayStride: stride,
      stepMode: 'instance',
      attributes: [
        { shaderLocation: 0, offset: 0, format: 'float32x4' },
        { shaderLocation: 1, offset: 16, format: 'float32x4' },
        { shaderLocation: 2, offset: 32, format: 'float32x4' },
      ],
    });
    const premultiplied: GPUBlendComponent = { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' };
    this.decalPipeline = device.createRenderPipeline({
      label: 'character decals',
      layout: frameOnly,
      vertex: { module, entryPoint: 'vsDecal', buffers: [instanced(DECAL_FLOATS * 4)] },
      fragment: { module, entryPoint: 'fsDecal', targets: [{ format, blend: { color: premultiplied, alpha: premultiplied } }] },
      primitive: { topology: 'triangle-list', cullMode: 'none' },
      depthStencil: { format: depthFormat, depthWriteEnabled: false, depthCompare: 'greater' },
      multisample,
    });
    this.bitPipeline = device.createRenderPipeline({
      label: 'voxel bits',
      layout: frameOnly,
      vertex: { module, entryPoint: 'vsBit', buffers: [instanced(BIT_FLOATS * 4)] },
      fragment: { module, entryPoint: 'fsBit', targets: [{ format }] },
      primitive: { topology: 'triangle-list', cullMode: 'back', frontFace: 'ccw' },
      depthStencil: { format: depthFormat, depthWriteEnabled: true, depthCompare: 'greater' },
      multisample,
    });
  }

  get meshCount(): number {
    return this.meshes.size;
  }

  get gpuBytes(): number {
    return this.meshBytes;
  }

  /** A mesh from the engine (replacing one with its id). */
  addMesh(m: CharacterMesh): void {
    this.removeMesh(m.id);
    if (m.indexCount === 0) return;
    const vbytes = m.vertexCount * CHAR_VERTEX_STRIDE;
    const ibytes = m.indexCount * 4;
    const vbuf = this.device.createBuffer({ label: `character mesh ${m.id} vertices`, size: vbytes, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    const ibuf = this.device.createBuffer({ label: `character mesh ${m.id} indices`, size: ibytes, usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    this.device.queue.writeBuffer(vbuf, 0, m.vertices, 0, vbytes);
    this.device.queue.writeBuffer(ibuf, 0, m.indices, 0, ibytes);
    this.meshes.set(m.id, { vbuf, ibuf, indexCount: m.indexCount, bytes: vbytes + ibytes });
    this.meshBytes += vbytes + ibytes;
  }

  removeMesh(id: number): void {
    const g = this.meshes.get(id);
    if (!g) return;
    g.vbuf.destroy();
    g.ibuf.destroy();
    this.meshBytes -= g.bytes;
    this.meshes.delete(id);
  }

  /** A palette from the engine: 16 linear rgb colours. */
  setPalette(id: number, rgb: Float32Array): void {
    let place = this.palettes.get(id);
    if (place === undefined) {
      place = this.palettes.size;
      this.palettes.set(id, place);
      const need = (place + 1) * CHARACTER_PALETTE_SLOTS * 4;
      if (need > this.paletteData.length) {
        const next = new Float32Array(Math.max(need, this.paletteData.length * 2));
        next.set(this.paletteData);
        this.paletteData = next;
      }
    }
    for (let s = 0; s < CHARACTER_PALETTE_SLOTS; s++) {
      const o = (place * CHARACTER_PALETTE_SLOTS + s) * 4;
      this.paletteData[o] = rgb[s * 3] ?? 1;
      this.paletteData[o + 1] = rgb[s * 3 + 1] ?? 0;
      this.paletteData[o + 2] = rgb[s * 3 + 2] ?? 1;
      this.paletteData[o + 3] = 1;
    }
    this.paletteDirty = true;
  }

  /** The engine's blood now: drops in flight, stains on the world. */
  setBlood(drops: Float32Array, stains: Float32Array): void {
    this.drops = drops;
    this.stains = stains;
  }

  /** Drops the meshes and the blood (another world); the palettes stay (the engine does not send them again). */
  clear(): void {
    for (const id of [...this.meshes.keys()]) this.removeMesh(id);
    this.drops = new Float32Array(0);
    this.stains = new Float32Array(0);
    this.pending.length = 0;
    this.batches.length = 0;
    this.decalCount = 0;
    this.bitCount = 0;
    this.listed = 0;
    this.bodies = 0;
  }

  /**
   * This frame's characters: those in view (and within `maxDistance`) become instances, a shadow
   * under each on its feet; the blood within reach becomes decals and bits. Everything is uploaded
   * (call before the render pass).
   */
  prepare(list: readonly CharacterDraw[], planes: Float32Array, eye: Vec3, maxDistance: number): void {
    const pending = this.pending;
    pending.length = 0;
    this.batches.length = 0;
    this.boneCount = 0;
    this.decalCount = 0;
    this.bitCount = 0;
    this.listed = list.length;
    for (const c of list) {
      if (c.opacity <= 0) continue;
      const place = this.palettes.get(c.palette);
      const body = this.meshes.get(c.mesh);
      if (place === undefined || !body) continue;
      if (!within(c.centre, 0, eye, maxDistance + c.radius)) continue;
      if (!sphereVisible(planes, c.centre, c.radius)) continue;
      pending.push({ draw: c, mesh: body, meshId: c.mesh, place, prop: false });
      const prop = c.propMesh !== 0 && c.prop ? this.meshes.get(c.propMesh) : undefined;
      if (prop) pending.push({ draw: c, mesh: prop, meshId: c.propMesh, place, prop: true });
      if (c.shadow) this.decal(c.shadow, 0, UP, 0, SHADOW_RADIUS, 0, 0, 0, SHADOW_STRENGTH * c.opacity, 0);
    }
    // (the blood: stains and drops within reach; the GPU clips the rest)
    const st = this.stains;
    for (let o = 0; o + BLOOD_STAIN_STRIDE <= st.length; o += BLOOD_STAIN_STRIDE) {
      if (!within(st, o, eye, maxDistance)) continue;
      const strength = Math.min(0.92, 0.5 + st[o + 7]!);
      this.decal(st, o, st, o + 3, Math.max(0.045, st[o + 6]! * 2.4), STAIN_COLOR[0]!, STAIN_COLOR[1]!, STAIN_COLOR[2]!, strength, 1);
    }
    const dr = this.drops;
    for (let o = 0; o + BLOOD_DROP_STRIDE <= dr.length; o += BLOOD_DROP_STRIDE) if (within(dr, o, eye, maxDistance)) this.bit(dr, o);
    // (by mesh: one call draws a mesh's instances)
    pending.sort((a, b) => a.meshId - b.meshId);
    const n = pending.length;
    if (this.instances.byteLength < n * INSTANCE_BYTES) this.instances = new ArrayBuffer(Math.max(n, (2 * this.instances.byteLength) / INSTANCE_BYTES) * INSTANCE_BYTES);
    const u = new Uint32Array(this.instances);
    const f = new Float32Array(this.instances);
    let bodies = 0;
    for (let i = 0; i < n; i++) {
      const p = pending[i]!;
      const c = p.draw;
      const bones = p.prop ? 1 : c.bones;
      const need = (this.boneCount + bones) * 16;
      if (need > this.bones.length) {
        const next = new Float32Array(Math.max(need, this.bones.length * 2));
        next.set(this.bones.subarray(0, this.boneCount * 16));
        this.bones = next;
      }
      this.bones.set((p.prop ? c.prop! : c.skin).subarray(0, bones * 16), this.boneCount * 16);
      const o = i * (INSTANCE_BYTES / 4);
      u[o] = this.boneCount;
      u[o + 1] = p.place;
      f[o + 2] = 1; // (sector light: procedural worlds are lit alike)
      f[o + 3] = c.opacity;
      f[o + 4] = FLASH_TINT[0]!;
      f[o + 5] = FLASH_TINT[1]!;
      f[o + 6] = FLASH_TINT[2]!;
      f[o + 7] = Math.max(0, Math.min(1, c.flash)) * FLASH_AMOUNT;
      this.boneCount += bones;
      if (!p.prop) bodies++;
      const last = this.batches[this.batches.length - 1];
      if (last && last.mesh === p.mesh) last.count++;
      else this.batches.push({ mesh: p.mesh, first: i, count: 1 });
    }
    this.bodies = bodies;
    this.upload(n);
  }

  /** Draws the decals, the characters, then the voxel bits (group 0, the frame's, must be bound). */
  draw(pass: GPURenderPassEncoder): CharacterDrawStats {
    let triangles = 0;
    if (this.decalCount > 0 && this.decalBuffer) {
      pass.setPipeline(this.decalPipeline);
      pass.setVertexBuffer(0, this.decalBuffer);
      pass.draw(6, this.decalCount);
      triangles += this.decalCount * 2;
    }
    if (this.batches.length > 0 && this.bindGroup) {
      pass.setPipeline(this.charPipeline);
      pass.setBindGroup(1, this.bindGroup);
      for (const b of this.batches) {
        pass.setVertexBuffer(0, b.mesh.vbuf);
        pass.setIndexBuffer(b.mesh.ibuf, 'uint32');
        pass.drawIndexed(b.mesh.indexCount, b.count, 0, 0, b.first);
        triangles += (b.mesh.indexCount / 3) * b.count;
      }
    }
    if (this.bitCount > 0 && this.bitBuffer) {
      pass.setPipeline(this.bitPipeline);
      pass.setVertexBuffer(0, this.bitBuffer);
      pass.draw(36, this.bitCount);
      triangles += this.bitCount * 12;
    }
    return {
      characters: this.listed,
      drawn: this.bodies,
      triangles,
      drops: this.drops.length / BLOOD_DROP_STRIDE,
      stains: this.stains.length / BLOOD_STAIN_STRIDE,
    };
  }

  /** A decal quad on a surface (`kind` 0 a soft blob, 1 a blocky splat): colour linear rgb and strength. */
  private decal(pos: ArrayLike<number>, po: number, normal: ArrayLike<number>, no: number, radius: number, r: number, g: number, b: number, a: number, kind: number): void {
    if ((this.decalCount + 1) * DECAL_FLOATS > this.decals.length) {
      const next = new Float32Array(this.decals.length * 2);
      next.set(this.decals);
      this.decals = next;
    }
    const o = this.decalCount * DECAL_FLOATS;
    const d = this.decals;
    d[o] = pos[po]!;
    d[o + 1] = pos[po + 1]!;
    d[o + 2] = pos[po + 2]!;
    d[o + 3] = radius;
    d[o + 4] = normal[no]!;
    d[o + 5] = normal[no + 1]!;
    d[o + 6] = normal[no + 2]!;
    d[o + 7] = kind;
    d[o + 8] = r;
    d[o + 9] = g;
    d[o + 10] = b;
    d[o + 11] = a;
    this.decalCount++;
  }

  /** A blood drop (BLOOD_DROP_STRIDE floats at `o`): a small cube, lit, unturned. */
  private bit(drops: Float32Array, o: number): void {
    if ((this.bitCount + 1) * BIT_FLOATS > this.bits.length) {
      const next = new Float32Array(this.bits.length * 2);
      next.set(this.bits);
      this.bits = next;
    }
    const k = this.bitCount * BIT_FLOATS;
    const b = this.bits;
    b[k] = drops[o]!;
    b[k + 1] = drops[o + 1]!;
    b[k + 2] = drops[o + 2]!;
    b[k + 3] = drops[o + 3]!;
    b[k + 4] = 0;
    b[k + 5] = 0;
    b[k + 6] = 0;
    b[k + 7] = 1;
    b[k + 8] = drops[o + 4]!;
    b[k + 9] = drops[o + 5]!;
    b[k + 10] = drops[o + 6]!;
    b[k + 11] = 1;
    this.bitCount++;
  }

  private upload(instances: number): void {
    const d = this.device;
    if (this.decalCount > 0) {
      this.decalBuffer = growBuffer(d, this.decalBuffer, this.decalCount * DECAL_FLOATS * 4, GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST, 'character decals');
      d.queue.writeBuffer(this.decalBuffer, 0, this.decals, 0, this.decalCount * DECAL_FLOATS);
    }
    if (this.bitCount > 0) {
      this.bitBuffer = growBuffer(d, this.bitBuffer, this.bitCount * BIT_FLOATS * 4, GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST, 'voxel bits');
      d.queue.writeBuffer(this.bitBuffer, 0, this.bits, 0, this.bitCount * BIT_FLOATS);
    }
    if (instances === 0) return; // (the palettes wait for someone to draw)
    const usage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;
    const bones = this.boneBuffer;
    const inst = this.instanceBuffer;
    const pal = this.paletteBuffer;
    this.boneBuffer = growBuffer(d, bones, this.boneCount * 64, usage, 'character bones');
    this.instanceBuffer = growBuffer(d, inst, instances * INSTANCE_BYTES, usage, 'character instances');
    this.paletteBuffer = growBuffer(d, pal, this.paletteData.byteLength, usage, 'character palettes');
    if (this.paletteBuffer !== pal) this.paletteDirty = true;
    if (!this.bindGroup || bones !== this.boneBuffer || inst !== this.instanceBuffer || pal !== this.paletteBuffer) {
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
    d.queue.writeBuffer(this.boneBuffer, 0, this.bones, 0, this.boneCount * 16);
    d.queue.writeBuffer(this.instanceBuffer, 0, this.instances, 0, instances * INSTANCE_BYTES);
    if (this.paletteDirty) {
      d.queue.writeBuffer(this.paletteBuffer, 0, this.paletteData);
      this.paletteDirty = false;
    }
  }
}
