/**
 * WebGPU renderer: sky, chunk meshes, detached islands, characters, water and particles in one 4x
 * MSAA pass with a reversed-Z depth buffer (depth32float, clear 0, compare 'greater').
 *
 * Bind groups: group 0 = frame uniforms + texture atlas + displacement fields (shared by all
 * pipelines); group 1 = per-object uniforms (model matrix, opacity, displaced flag) with
 * dynamic offsets: slot 0 is the identity used by chunks (displaced by the fields), slots
 * 1..MAX_ISLANDS belong to islands, the next MAX_GRIDS to the oriented grids (each keeps its
 * slot; only changed slots are uploaded) and the next MAX_WHEELS to vehicles' wheels (every
 * frame). The characters (skinned, render/characters.ts) have a group 1 of their own and come
 * after the world pipeline's draws. Skid marks are drawn translucent after the opaque world.
 */
import type { GridFrames } from '../engine/gridframes.ts';
import type { TextureInfo, Vec3 } from '../engine/protocol.ts';
import { DebugView, Material, Paint, PAINT_SLOT_BASE, VERTEX_STRIDE } from '../engine/protocol.ts';
import { GpuAtlas } from './atlas.ts';
import { CharacterRenderer, type CharacterDraw } from './characters.ts';
import { ChunkStore } from './chunks.ts';
import { FIELD_UNIFORM_FLOATS, FieldStore, MAX_FIELDS } from './fields.ts';
import { initWebGpu, type GpuContext } from './gpu.ts';
import { GridRenderer } from './grids.ts';
import { IslandRenderer } from './islands.ts';
import { cross, frustumPlanes, mat4, mat4LookDir, mat4Multiply, mat4PerspectiveReversedInfinite, normalize } from './math.ts';
import { ParticleSystem } from './particles.ts';
import { RopeRenderer } from './ropes.ts';
import { SkidMarks } from './skids.ts';
import { RIM_BLUR_SLOT, WheelRenderer, type WheelDraw } from './wheels.ts';
import characterWgsl from './shaders/character.wgsl?raw';
import frameWgsl from './shaders/frame.wgsl?raw';
import particlesWgsl from './shaders/particles.wgsl?raw';
import skidsWgsl from './shaders/skids.wgsl?raw';
import skyWgsl from './shaders/sky.wgsl?raw';
import waterWgsl from './shaders/water.wgsl?raw';
import worldWgsl from './shaders/world.wgsl?raw';

const SAMPLES = 4;
const DEPTH_FORMAT: GPUTextureFormat = 'depth32float';
/** Palette entries (see shaders/frame.wgsl): materials, the default, paints. */
const PALETTE_SIZE = 64;
/** Floats in the Frame uniform (see shaders/frame.wgsl). */
const FRAME_FLOATS = 16 + 8 * 4 + PALETTE_SIZE * 4;
const OBJECT_BYTES = 80; // mat4 + vec4
/**
 * Detached pieces drawn at once: the engine keeps up to max_bodies rigid pieces (the settings
 * allow 6000) plus the culled ones fading out.
 */
const MAX_ISLANDS = 8192;
/** Oriented grids drawn at once (docs/GRIDS.md): each has a slot of its own. */
const MAX_GRIDS = 1024;
/** Vehicles' wheels drawn at once. */
const MAX_WHEELS = 512;
const MAX_VIEW_DISTANCE = 600;

export interface Camera {
  eye: Vec3;
  /** Unit view direction. */
  forward: Vec3;
  /** Vertical field of view, radians. */
  fovY: number;
  near: number;
}

export interface FrameInputs {
  camera: Camera;
  timeS: number;
  debugView: DebugView;
  flashPos: Vec3;
  flashIntensity: number;
  /** Voxel pitch in metres (drives the untextured per-voxel variation). */
  voxelSize: number;
  /** The oriented grids' frames (their chunks are drawn with them), placed for this frame. */
  gridFrames?: GridFrames;
  /** Vehicles' wheels, posed for this frame. */
  wheels?: readonly WheelDraw[];
  /** The characters (people), posed for this frame. */
  characters?: readonly CharacterDraw[];
}

export interface RenderStats {
  chunksDrawn: number;
  chunksTotal: number;
  /** The oriented grids' chunks drawn and held. */
  gridChunksDrawn: number;
  gridChunks: number;
  triangles: number;
  islands: number;
  islandsDrawn: number;
  particles: number;
  wheels: number;
  skidMarks: number;
  /** The characters posed this frame, and those drawn (in view); the blood's drops and stains. */
  characters: number;
  charactersDrawn: number;
  bloodDrops: number;
  bloodStains: number;
  gpuMB: number;
  width: number;
  height: number;
}

/**
 * Linear-space colours (vec4 each): materials by id (0..20), [31] the untextured default, paints
 * at PAINT_SLOT_BASE + paint (cars, facades, the road's markings), [59] a wheel's blurred spokes.
 */
const PALETTE: Float32Array = (() => {
  const p = new Float32Array(PALETTE_SIZE * 4).fill(1);
  const set = (i: number, r: number, g: number, b: number): void => p.set([r, g, b, 1], i * 4);
  for (let i = 0; i < PALETTE_SIZE; i++) set(i, 0.3, 0.3, 0.3);
  set(Material.Rc, 0.3, 0.31, 0.33);
  set(Material.Concrete, 0.36, 0.35, 0.33);
  set(Material.Steel, 0.2, 0.27, 0.36);
  set(Material.Masonry, 0.42, 0.16, 0.1);
  set(Material.Soil, 0.24, 0.15, 0.08);
  set(Material.Rock, 0.2, 0.18, 0.16);
  set(Material.Bedrock, 0.07, 0.065, 0.06);
  set(Material.Wood, 0.34, 0.2, 0.09);
  set(Material.Stone, 0.4, 0.38, 0.33);
  set(Material.Glass, 0.55, 0.7, 0.72);
  set(Material.Rebar, 0.16, 0.13, 0.11);
  set(Material.SteelSection, 0.22, 0.24, 0.27);
  set(Material.Sheet, 0.34, 0.35, 0.37);
  set(Material.CarFrame, 0.05, 0.05, 0.055);
  set(Material.Engine, 0.1, 0.1, 0.105);
  set(Material.Window, 0.035, 0.045, 0.055);
  set(Material.Tyre, 0.03, 0.03, 0.032);
  set(Material.Plastic, 0.035, 0.035, 0.038);
  set(Material.Asphalt, 0.065, 0.065, 0.07);
  set(Material.Paint, 0.62, 0.62, 0.6);
  set(Material.Lamp, 0.85, 0.85, 0.8);
  const paint = (k: number, r: number, g: number, b: number): void => set(PAINT_SLOT_BASE + k, r, g, b);
  paint(Paint.White, 0.78, 0.79, 0.78);
  paint(Paint.Silver, 0.42, 0.44, 0.47);
  paint(Paint.Black, 0.018, 0.018, 0.02);
  paint(Paint.Red, 0.5, 0.02, 0.025);
  paint(Paint.Blue, 0.02, 0.09, 0.38);
  paint(Paint.Green, 0.04, 0.2, 0.07);
  paint(Paint.Yellow, 0.75, 0.55, 0.03);
  paint(Paint.Orange, 0.75, 0.22, 0.02);
  paint(Paint.TaxiYellow, 0.8, 0.55, 0.02);
  paint(Paint.NavyBlue, 0.015, 0.03, 0.12);
  paint(Paint.Maroon, 0.17, 0.015, 0.025);
  paint(Paint.Beige, 0.5, 0.43, 0.3);
  paint(Paint.Graphite, 0.085, 0.09, 0.095);
  paint(Paint.Teal, 0.02, 0.25, 0.25);
  paint(Paint.Trim, 0.03, 0.03, 0.033);
  paint(Paint.TailRed, 0.55, 0.015, 0.015);
  paint(Paint.Amber, 0.85, 0.35, 0.02);
  paint(Paint.Plaster, 0.6, 0.58, 0.53);
  paint(Paint.Cream, 0.66, 0.58, 0.42);
  paint(Paint.Terracotta, 0.45, 0.17, 0.08);
  paint(Paint.Sand, 0.58, 0.48, 0.31);
  paint(Paint.Slate, 0.19, 0.21, 0.25);
  paint(Paint.Ochre, 0.55, 0.36, 0.09);
  paint(Paint.Mint, 0.38, 0.56, 0.47);
  paint(Paint.LineWhite, 0.72, 0.72, 0.7);
  paint(Paint.LineYellow, 0.72, 0.48, 0.03);
  paint(Paint.Kerb, 0.42, 0.42, 0.4);
  set(RIM_BLUR_SLOT, 0.24, 0.245, 0.25);
  return p;
})();

const FOG_COLOR: Vec3 = [0.55, 0.6, 0.66];
const ZENITH_COLOR: Vec3 = [0.16, 0.3, 0.58];
const SUN_DIR: Vec3 = normalize([0.45, 0.28, 0.85]);
const FOG_DENSITY = 0.0065;

export class Renderer {
  readonly gpu: GpuContext;
  readonly chunks: ChunkStore;
  /** Water surface meshes (the engine's water), drawn translucent after the opaque world. */
  readonly water: ChunkStore;
  readonly islands: IslandRenderer;
  readonly grids: GridRenderer;
  readonly ropes: RopeRenderer;
  readonly wheels: WheelRenderer;
  readonly skids: SkidMarks;
  /** The people: character meshes and palettes (as the engine sends them), drawn skinned. */
  readonly characters: CharacterRenderer;
  readonly particles: ParticleSystem;
  readonly fields: FieldStore;
  private readonly canvas: HTMLCanvasElement;
  private readonly fieldBuffer: GPUBuffer;
  private readonly fieldSampler: GPUSampler;
  private boundFieldsVersion = -1;
  private readonly device: GPUDevice;
  private readonly frameBuffer: GPUBuffer;
  private readonly frameData = new Float32Array(FRAME_FLOATS);
  private readonly objectBuffer: GPUBuffer;
  private readonly objectStride: number;
  private readonly objectData: Float32Array<ArrayBuffer>;
  private readonly frameLayout: GPUBindGroupLayout;
  private readonly objectBindGroup: GPUBindGroup;
  private readonly sampler: GPUSampler;
  private frameBindGroup!: GPUBindGroup;
  private atlas: GpuAtlas;
  private readonly skyPipeline: GPURenderPipeline;
  private readonly worldPipeline: GPURenderPipeline;
  private readonly particlePipeline: GPURenderPipeline;
  private readonly waterPipeline: GPURenderPipeline;
  private readonly skidPipeline: GPURenderPipeline;
  private colorTarget: GPUTexture | null = null;
  private depthTarget: GPUTexture | null = null;
  private readonly view = mat4();
  private readonly proj = mat4();
  private readonly viewProj = mat4();
  private readonly planes = new Float32Array(20);
  /** Render resolution scale relative to device pixels (<= 1 trades sharpness for speed). */
  renderScale = 1;
  /**
   * Draw one frame in this many (the rest are simulated, not drawn): a software GPU on a small
   * machine takes the cores the page and the engine need (the browser checks draw only for their
   * screenshots).
   */
  drawEvery = 1;

  static async create(canvas: HTMLCanvasElement, onDeviceLost: (reason: string) => void): Promise<Renderer> {
    const gpu = await initWebGpu(canvas, onDeviceLost);
    return new Renderer(canvas, gpu);
  }

  private constructor(canvas: HTMLCanvasElement, gpu: GpuContext) {
    this.canvas = canvas;
    this.gpu = gpu;
    const device = gpu.device;
    this.device = device;

    this.frameBuffer = device.createBuffer({
      label: 'frame uniforms',
      size: FRAME_FLOATS * 4,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    // one slot per object at the dynamic-offset alignment: (1 + MAX_ISLANDS + MAX_GRIDS + MAX_WHEELS) x 256 B
    this.objectStride = Math.max(256, device.limits.minUniformBufferOffsetAlignment);
    this.objectData = new Float32Array(((1 + MAX_ISLANDS + MAX_GRIDS + MAX_WHEELS) * this.objectStride) / 4);
    this.objectBuffer = device.createBuffer({
      label: 'object uniforms',
      size: this.objectData.byteLength,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    // slot 0: identity for chunks, which move by the displacement fields
    const o = this.objectData;
    o[0] = o[5] = o[10] = o[15] = 1;
    o[16] = 1; // opacity
    o[17] = 1; // displaced
    device.queue.writeBuffer(this.objectBuffer, 0, o, 0, this.objectStride / 4);
    this.sampler = device.createSampler({
      label: 'atlas sampler',
      magFilter: 'nearest', // crisp Doom texels up close
      minFilter: 'linear',
      mipmapFilter: 'linear',
      addressModeU: 'clamp-to-edge', // wrapping is manual (see atlas-pack.ts)
      addressModeV: 'clamp-to-edge',
      lodMaxClamp: 3,
    });

    this.fields = new FieldStore(device);
    this.fieldBuffer = device.createBuffer({
      label: 'displacement fields',
      size: FIELD_UNIFORM_FLOATS * 4,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    this.fieldSampler = device.createSampler({
      label: 'displacement field sampler',
      magFilter: 'linear',
      minFilter: 'linear',
      addressModeU: 'clamp-to-edge',
      addressModeV: 'clamp-to-edge',
      addressModeW: 'clamp-to-edge',
    });
    const VF = GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT;
    const V = GPUShaderStage.VERTEX;
    const fieldEntries: GPUBindGroupLayoutEntry[] = [];
    for (let k = 0; k < MAX_FIELDS; k++)
      fieldEntries.push({ binding: 4 + k, visibility: V, texture: { sampleType: 'float', viewDimension: '3d' } });
    this.frameLayout = device.createBindGroupLayout({
      label: 'frame',
      entries: [
        { binding: 0, visibility: VF, buffer: { type: 'uniform' } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d-array' } },
        { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
        { binding: 3, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } },
        ...fieldEntries,
        { binding: 4 + MAX_FIELDS, visibility: V, sampler: { type: 'filtering' } },
        { binding: 5 + MAX_FIELDS, visibility: V, buffer: { type: 'uniform' } },
      ],
    });
    const objectLayout = device.createBindGroupLayout({
      label: 'object',
      entries: [{ binding: 0, visibility: VF, buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: OBJECT_BYTES } }],
    });
    this.objectBindGroup = device.createBindGroup({
      label: 'object',
      layout: objectLayout,
      entries: [{ binding: 0, resource: { buffer: this.objectBuffer, size: OBJECT_BYTES } }],
    });

    this.atlas = GpuAtlas.empty(device);
    this.rebuildFrameBindGroup();

    const module = (label: string, code: string): GPUShaderModule => {
      const m = device.createShaderModule({ label, code: `${frameWgsl}\n${code}` });
      void m.getCompilationInfo().then((info) => {
        for (const msg of info.messages) {
          const log = msg.type === 'error' ? console.error : console.warn;
          log(`[wgsl ${label}] ${msg.type} ${msg.lineNum}:${msg.linePos} ${msg.message}`);
        }
      });
      return m;
    };
    const format = gpu.renderFormat;
    const multisample: GPUMultisampleState = { count: SAMPLES };
    const frameOnly = device.createPipelineLayout({ label: 'frame only', bindGroupLayouts: [this.frameLayout] });
    const frameAndObject = device.createPipelineLayout({ label: 'frame+object', bindGroupLayouts: [this.frameLayout, objectLayout] });

    const sky = module('sky', skyWgsl);
    this.skyPipeline = device.createRenderPipeline({
      label: 'sky',
      layout: frameOnly,
      vertex: { module: sky, entryPoint: 'vs' },
      fragment: { module: sky, entryPoint: 'fs', targets: [{ format }] },
      depthStencil: { format: DEPTH_FORMAT, depthWriteEnabled: false, depthCompare: 'always' },
      multisample,
    });

    const world = module('world', worldWgsl);
    this.worldPipeline = device.createRenderPipeline({
      label: 'world',
      layout: frameAndObject,
      vertex: {
        module: world,
        entryPoint: 'vs',
        buffers: [
          {
            arrayStride: VERTEX_STRIDE,
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x3' },
              { shaderLocation: 1, offset: 12, format: 'snorm8x4' },
              { shaderLocation: 2, offset: 16, format: 'float32x2' },
              // uint16 texture id + uint8 light + uint8 debug, fetched as one uint32.
              { shaderLocation: 3, offset: 24, format: 'uint32' },
            ],
          },
        ],
      },
      fragment: { module: world, entryPoint: 'fs', targets: [{ format }] },
      primitive: { topology: 'triangle-list', cullMode: 'back', frontFace: 'ccw' },
      depthStencil: { format: DEPTH_FORMAT, depthWriteEnabled: true, depthCompare: 'greater' },
      multisample,
    });

    const premultiplied: GPUBlendComponent = { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' };
    const waterModule = module('water', waterWgsl);
    this.waterPipeline = device.createRenderPipeline({
      label: 'water',
      layout: frameOnly,
      vertex: {
        module: waterModule,
        entryPoint: 'vs',
        buffers: [
          {
            arrayStride: VERTEX_STRIDE,
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x3' },
              { shaderLocation: 1, offset: 12, format: 'snorm8x4' },
              { shaderLocation: 2, offset: 16, format: 'float32x2' },
              { shaderLocation: 3, offset: 24, format: 'uint32' },
            ],
          },
        ],
      },
      fragment: { module: waterModule, entryPoint: 'fs', targets: [{ format, blend: { color: premultiplied, alpha: premultiplied } }] },
      // (both sides: seen from under water too)
      primitive: { topology: 'triangle-list', cullMode: 'none' },
      depthStencil: { format: DEPTH_FORMAT, depthWriteEnabled: false, depthCompare: 'greater' },
      multisample,
    });

    const skids = module('skids', skidsWgsl);
    this.skidPipeline = device.createRenderPipeline({
      label: 'skid marks',
      layout: frameOnly,
      vertex: {
        module: skids,
        entryPoint: 'vs',
        buffers: [{ arrayStride: 16, attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x4' }] }],
      },
      fragment: { module: skids, entryPoint: 'fs', targets: [{ format, blend: { color: premultiplied, alpha: premultiplied } }] },
      primitive: { topology: 'triangle-list', cullMode: 'none' },
      depthStencil: { format: DEPTH_FORMAT, depthWriteEnabled: false, depthCompare: 'greater' },
      multisample,
    });

    const particles = module('particles', particlesWgsl);
    this.particlePipeline = device.createRenderPipeline({
      label: 'particles',
      layout: frameOnly,
      vertex: {
        module: particles,
        entryPoint: 'vs',
        buffers: [
          {
            arrayStride: 32,
            stepMode: 'instance',
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x4' },
              { shaderLocation: 1, offset: 16, format: 'float32x4' },
            ],
          },
        ],
      },
      fragment: { module: particles, entryPoint: 'fs', targets: [{ format, blend: { color: premultiplied, alpha: premultiplied } }] },
      primitive: { topology: 'triangle-list', cullMode: 'none' },
      depthStencil: { format: DEPTH_FORMAT, depthWriteEnabled: false, depthCompare: 'greater' },
      multisample,
    });

    this.chunks = new ChunkStore(device);
    this.water = new ChunkStore(device);
    this.islands = new IslandRenderer(device, MAX_ISLANDS);
    this.grids = new GridRenderer(device, 1 + MAX_ISLANDS, MAX_GRIDS);
    this.ropes = new RopeRenderer(device);
    this.wheels = new WheelRenderer(device, 1 + MAX_ISLANDS + MAX_GRIDS, MAX_WHEELS);
    this.skids = new SkidMarks(device);
    this.characters = new CharacterRenderer(device, this.frameLayout, module('characters', characterWgsl), format, SAMPLES, DEPTH_FORMAT);
    this.particles = new ParticleSystem(device);
  }

  get description(): string {
    return this.gpu.description;
  }

  /** Replaces the texture set (from a `textures` message). */
  setTextures(list: readonly TextureInfo[]): void {
    const next = GpuAtlas.create(this.device, list);
    this.atlas.destroy();
    this.atlas = next;
    this.rebuildFrameBindGroup();
  }

  get textureBytes(): number {
    return this.atlas.bytes;
  }

  /** Drops all world geometry (before loading another world). */
  clearWorld(): void {
    this.chunks.clear();
    this.water.clear();
    this.islands.clear();
    this.grids.clear();
    this.ropes.clear();
    this.wheels.clear();
    this.skids.clear();
    this.characters.clear();
    this.particles.clear();
    this.fields.clear();
  }

  private rebuildFrameBindGroup(): void {
    const views = this.fields.views();
    this.frameBindGroup = this.device.createBindGroup({
      label: 'frame',
      layout: this.frameLayout,
      entries: [
        { binding: 0, resource: { buffer: this.frameBuffer } },
        { binding: 1, resource: this.atlas.view },
        { binding: 2, resource: this.sampler },
        { binding: 3, resource: { buffer: this.atlas.info } },
        ...views.map((view, k) => ({ binding: 4 + k, resource: view })),
        { binding: 4 + MAX_FIELDS, resource: this.fieldSampler },
        { binding: 5 + MAX_FIELDS, resource: { buffer: this.fieldBuffer } },
      ],
    });
    this.boundFieldsVersion = this.fields.version;
  }

  /** Keeps the canvas backing store matched to its CSS size and recreates targets. */
  private ensureTargets(): { width: number; height: number } {
    const dpr = Math.min(window.devicePixelRatio || 1, 2) * this.renderScale;
    const maxDim = this.device.limits.maxTextureDimension2D;
    const width = Math.max(1, Math.min(maxDim, Math.round(this.canvas.clientWidth * dpr)));
    const height = Math.max(1, Math.min(maxDim, Math.round(this.canvas.clientHeight * dpr)));
    if (this.canvas.width !== width || this.canvas.height !== height || !this.colorTarget || !this.depthTarget) {
      this.canvas.width = width;
      this.canvas.height = height;
      this.colorTarget?.destroy();
      this.depthTarget?.destroy();
      this.colorTarget = this.device.createTexture({
        label: 'msaa color',
        size: [width, height],
        format: this.gpu.renderFormat,
        sampleCount: SAMPLES,
        usage: GPUTextureUsage.RENDER_ATTACHMENT,
      });
      this.depthTarget = this.device.createTexture({
        label: 'depth',
        size: [width, height],
        format: DEPTH_FORMAT,
        sampleCount: SAMPLES,
        usage: GPUTextureUsage.RENDER_ATTACHMENT,
      });
    }
    return { width, height };
  }

  render(input: FrameInputs): RenderStats {
    const { width, height } = this.ensureTargets();
    const cam = input.camera;
    const aspect = width / height;
    const up: Vec3 = [0, 0, 1];
    mat4LookDir(this.view, cam.eye, cam.forward, up);
    mat4PerspectiveReversedInfinite(this.proj, cam.fovY, aspect, cam.near);
    mat4Multiply(this.viewProj, this.proj, this.view);
    frustumPlanes(this.viewProj, this.planes);

    const right = normalize(cross(cam.forward, up));
    const camUp = cross(right, cam.forward);
    const tanY = Math.tan(cam.fovY / 2);
    const f = this.frameData;
    f.set(this.viewProj, 0);
    f.set([cam.eye[0], cam.eye[1], cam.eye[2], input.timeS], 16);
    f.set([right[0], right[1], right[2], tanY * aspect], 20);
    f.set([camUp[0], camUp[1], camUp[2], tanY], 24);
    f.set([cam.forward[0], cam.forward[1], cam.forward[2], input.debugView], 28);
    f.set([...FOG_COLOR, FOG_DENSITY], 32);
    f.set([...ZENITH_COLOR, input.voxelSize], 36);
    f.set([...SUN_DIR, this.atlas.count], 40);
    f.set([...input.flashPos, input.flashIntensity], 44);
    f.set(PALETTE, 48);
    this.device.queue.writeBuffer(this.frameBuffer, 0, f);

    // Object slots of the islands: upload the range spanning the changed ones (resting rubble
    // does not change, so a quiet frame uploads nothing).
    this.islands.update(input.timeS);
    const o = this.objectData;
    const stride = this.objectStride / 4;
    let lo = Infinity;
    let hi = -1;
    for (const isl of this.islands.list) {
      if (!isl.dirty) continue;
      isl.dirty = false;
      const base = isl.slot * stride;
      o.set(isl.model, base);
      o[base + 16] = isl.opacity;
      o[base + 17] = 0;
      lo = Math.min(lo, isl.slot);
      hi = Math.max(hi, isl.slot);
    }
    // (the grids' slots: their frames, and their voxel size for the per-voxel variation)
    if (input.gridFrames) this.grids.update(input.gridFrames);
    for (const g of this.grids.list) {
      if (!g.dirty) continue;
      g.dirty = false;
      const base = g.slot * stride;
      o.set(g.model, base);
      o[base + 16] = 1;
      o[base + 17] = 0;
      o[base + 18] = g.h;
      lo = Math.min(lo, g.slot);
      hi = Math.max(hi, g.slot);
    }
    if (hi >= lo) this.device.queue.writeBuffer(this.objectBuffer, lo * this.objectStride, o, lo * stride, (hi - lo + 1) * stride);
    // (the wheels' slots: every frame, a range of their own)
    const wr = this.wheels.set(input.wheels ?? [], o, stride);
    if (wr.hi >= wr.lo) this.device.queue.writeBuffer(this.objectBuffer, wr.lo * this.objectStride, o, wr.lo * stride, (wr.hi - wr.lo + 1) * stride);
    this.skids.upload();
    this.characters.prepare(input.characters ?? [], this.planes, cam.eye, MAX_VIEW_DISTANCE);

    const particleCount = this.particles.upload();
    if (this.fields.version !== this.boundFieldsVersion) this.rebuildFrameBindGroup();
    this.device.queue.writeBuffer(this.fieldBuffer, 0, this.fields.uniform);

    const encoder = this.device.createCommandEncoder({ label: 'frame' });
    const pass = encoder.beginRenderPass({
      label: 'main',
      colorAttachments: [
        {
          view: this.colorTarget!.createView(),
          resolveTarget: this.gpu.context.getCurrentTexture().createView({ format: this.gpu.renderFormat }),
          clearValue: { r: FOG_COLOR[0], g: FOG_COLOR[1], b: FOG_COLOR[2], a: 1 },
          loadOp: 'clear',
          storeOp: 'discard',
        },
      ],
      depthStencilAttachment: {
        view: this.depthTarget!.createView(),
        depthClearValue: 0,
        depthLoadOp: 'clear',
        depthStoreOp: 'discard',
      },
    });

    pass.setBindGroup(0, this.frameBindGroup);
    pass.setPipeline(this.skyPipeline);
    pass.draw(3);

    pass.setPipeline(this.worldPipeline);
    pass.setBindGroup(1, this.objectBindGroup, [0]);
    const fields = this.fields;
    const drawn = this.chunks.draw(pass, this.planes, cam.eye, MAX_VIEW_DISTANCE, fields.count > 0 ? (mn, mx) => fields.inflation(mn, mx) : undefined);
    const pieces = this.islands.draw(pass, this.objectBindGroup, this.objectStride, this.planes, cam.eye, MAX_VIEW_DISTANCE);
    const grids = this.grids.draw(pass, this.objectBindGroup, this.objectStride, this.planes, cam.eye, MAX_VIEW_DISTANCE);
    const wheels = this.wheels.draw(pass, this.objectBindGroup, this.objectStride, this.planes, cam.eye, MAX_VIEW_DISTANCE);
    let triangles = drawn.triangles + pieces.triangles + grids.triangles + wheels.triangles;
    if (this.ropes.count > 0) {
      pass.setBindGroup(1, this.objectBindGroup, [0]);
      triangles += this.ropes.draw(pass);
    }
    // (their shadows and the blood's stains, them, the blood's drops: after the world pipeline's
    // draws - they bind a group 1 of their own)
    const people = this.characters.draw(pass);
    triangles += people.triangles;
    if (this.skids.count > 0) {
      pass.setPipeline(this.skidPipeline);
      triangles += this.skids.draw(pass);
    }
    if (this.water.count > 0) {
      pass.setPipeline(this.waterPipeline);
      triangles += this.water.draw(pass, this.planes, cam.eye, MAX_VIEW_DISTANCE, undefined, true).triangles;
    }

    if (particleCount > 0) {
      pass.setPipeline(this.particlePipeline);
      pass.setVertexBuffer(0, this.particles.buffer);
      pass.draw(6, particleCount);
    }
    pass.end();
    this.device.queue.submit([encoder.finish()]);

    return {
      chunksDrawn: drawn.drawn,
      chunksTotal: this.chunks.count,
      gridChunksDrawn: grids.drawn,
      gridChunks: this.grids.count,
      triangles,
      islands: this.islands.count,
      islandsDrawn: pieces.drawn,
      particles: particleCount,
      wheels: wheels.drawn,
      skidMarks: this.skids.count,
      characters: people.characters,
      charactersDrawn: people.drawn,
      bloodDrops: people.drops,
      bloodStains: people.stains,
      gpuMB: (this.chunks.bytes + this.grids.bytes + this.water.bytes + this.atlas.bytes + this.characters.gpuBytes) / (1024 * 1024),
      width,
      height,
    };
  }
}
