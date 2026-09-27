/**
 * MockEngine: a pure-TypeScript implementation of the engine protocol (docs/API.md).
 *
 * It is the reference implementation of the message contract for the front end, not a
 * physics engine. What it fakes, and how:
 * - structure: a voxel grid with anchored bedrock; after edits, unsupported pieces are
 *   found by flood fill and emitted as `detached` events (plan §B7 "vanish" semantics);
 * - bubbles: every blast opens a short-lived debug bubble that drives the bubble-level
 *   debug view and a decaying "wobble" displacement of nearby chunks (re-sent meshes, as
 *   the real engine does for displaced chunks, at most once per tick);
 * - utilization: a heuristic (load above x slenderness, plus damage around craters);
 * - Doom WADs: not voxelized; `loadWad` reports an error and loads a procedural world.
 *
 * The engine is environment-neutral: it talks to the outside only through `post`, and
 * time comes in through `tick(nowMs)`, so tests can drive it synchronously.
 */
import type {
  BubbleEvent,
  ChunkMesh,
  DetachedEvent,
  EngineCommand,
  EngineEvent,
  EngineParams,
  EngineStats,
  InitConfig,
  MeshData,
  ProceduralKind,
  Vec3,
  WadOptions,
  WorkerMessage,
} from '../../engine/protocol.ts';
import { DEFAULT_PARAMS, DOOM_TEXELS_PER_METRE, DebugView, VERTEX_STRIDE } from '../../engine/protocol.ts';
import { MeshBuilder } from '../../engine/vertex.ts';
import { isIndestructible, resolveFaceTextures } from './blocks.ts';
import { collideAabb } from './collide.ts';
import { findIslands, type Island } from './connectivity.ts';
import { fillPaddedChunk, meshBlock, PADDED_CHUNK_VOLUME, type MeshStyle } from './mesher.ts';
import { generateWorld, type GeneratedWorld } from './procgen.ts';
import { raycast, traceVoxels } from './raycast.ts';
import { buildMockTextures } from './textures.ts';
import { AIR, CHUNK_SIZE, type VoxelWorld } from './world.ts';
import { inspectWad } from './wad.ts';

export type PostFn = (msg: WorkerMessage) => void;

const GRAVITY = 9.81;
/** kg/m^3, used for island masses and virtual impact energies. */
const DENSITY = 2400;
/** A component larger than this is assumed supported (bounds the flood fill). */
const MAX_ISLAND_SEARCH = 1_500_000;
const STATS_INTERVAL_MS = 250;
const BUBBLE_DURATION_MS = 2200;
/** Wobble (displacement) updates: 20 Hz, i.e. at most once per tick. */
const WOBBLE_INTERVAL_MS = 50;
/** Meshing budget per tick; larger while the initial world streams in. */
const MESH_BUDGET_MS = 8;
const MESH_BUDGET_LOADING_MS = 40;
/** Chunk meshes per message (keeps single messages small). */
const MESHES_PER_MESSAGE = 48;

interface Bubble {
  id: number;
  center: Vec3;
  radius: number;
  start: number;
  /** Peak displacement in metres (already amplified). */
  amplitude: number;
  omega: number;
  zeta: number;
  /** Chunks inside the bubble radius. */
  chunks: number[];
}

interface Cause {
  kind: 'carve' | 'blast';
  pos: Vec3;
  energy: number;
}

interface Scheduled {
  at: number;
  event: EngineEvent;
}

function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function hashVoxel(i: number): number {
  let h = Math.imul(i, 0x9e3779b1);
  h = Math.imul(h ^ (h >>> 16), 0x85ebca6b);
  return ((h ^ (h >>> 13)) >>> 0) / 4294967296;
}

function sanitizeParams(p: EngineParams): EngineParams {
  const num = (v: number, lo: number, hi: number, dflt: number): number =>
    Number.isFinite(v) ? Math.min(hi, Math.max(lo, v)) : dflt;
  const dv = p.debugView;
  return {
    compliance: num(p.compliance, 0.1, 100, DEFAULT_PARAMS.compliance),
    amplification: num(p.amplification, 0, 100, DEFAULT_PARAMS.amplification),
    fragility: num(p.fragility, 0.05, 20, DEFAULT_PARAMS.fragility),
    damping: num(p.damping, 0, 1, DEFAULT_PARAMS.damping),
    debugView: dv === DebugView.Utilization || dv === DebugView.BubbleLevel ? dv : DebugView.None,
    paused: p.paused === true,
  };
}

export class MockEngine {
  private readonly post: PostFn;
  private config: InitConfig = { voxelSize: 0.125, threads: 1, memoryMB: 512, params: { ...DEFAULT_PARAMS } };
  private params: EngineParams = { ...DEFAULT_PARAMS };
  private gen: GeneratedWorld | null = null;
  private faceTextures: Uint16Array = new Uint16Array(0);
  private rand: () => number = mulberry32(1);
  private readonly builder = new MeshBuilder(1 << 14);
  private readonly padded = new Uint8Array(PADDED_CHUNK_VOLUME);

  /** Chunks needing a regular (greedy, undisplaced) remesh. */
  private readonly dirty = new Set<number>();
  /** Chunks the front end currently has a mesh for. */
  private readonly meshed = new Set<number>();
  private initialTotal = 0;
  private loading = false;

  private readonly pendingSeeds = new Set<number>();
  private pendingCause: Cause | null = null;
  private events: EngineEvent[] = [];
  private scheduled: Scheduled[] = [];
  private bubbles: Bubble[] = [];
  /** Chunks currently shown displaced -> their undisplaced culled-face mesh. */
  private readonly wobbleBase = new Map<number, MeshData | null>();
  private lastWobble = 0;
  /** Sparse damage 0..1 per voxel (monotone), for the utilization view. */
  private readonly damage = new Map<number, number>();
  private viewer: Vec3 = [0, 0, 0];
  private nextId = 1;

  // Stats accumulators.
  private lastStats = 0;
  private tickMsEma = 0;
  private structuralMsAcc = 0;
  private meshMsAcc = 0;
  private meshedAcc = 0;
  private ticksAcc = 0;
  private eventsAcc = 0;
  private islandsTotal = 0;
  private detachedVoxelsTotal = 0;

  constructor(post: PostFn) {
    this.post = post;
  }

  get world(): VoxelWorld | null {
    return this.gen?.world ?? null;
  }

  // -------------------------------------------------------------------------------------
  // Commands
  // -------------------------------------------------------------------------------------

  handle(cmd: EngineCommand, now = performance.now()): void {
    switch (cmd.type) {
      case 'init':
        this.config = { ...cmd.config, params: sanitizeParams(cmd.config.params) };
        this.params = this.config.params;
        break;
      case 'loadProcedural':
        this.loadProcedural(cmd.kind, cmd.seed, now);
        break;
      case 'loadWad':
        this.loadWad(cmd.buffer, cmd.map, cmd.options, now);
        break;
      case 'viewer':
        this.viewer = [cmd.pos[0], cmd.pos[1], cmd.pos[2]];
        break;
      case 'blast':
        this.blast(cmd.pos, cmd.radius, cmd.energy, now);
        break;
      case 'carve':
        this.carve(cmd.pos, cmd.radius);
        break;
      case 'raycast': {
        const w = this.world;
        this.post({ type: 'raycastResult', id: cmd.id, hit: w ? raycast(w, cmd.origin, cmd.dir, cmd.maxDist) : null });
        break;
      }
      case 'collide': {
        const w = this.world;
        const r = w ? collideAabb(w, cmd.min, cmd.max, cmd.move) : { move: cmd.move, onGround: false };
        this.post({ type: 'collideResult', id: cmd.id, move: r.move, onGround: r.onGround });
        break;
      }
      case 'setParams':
        this.setParams(cmd.params);
        break;
    }
  }

  private setParams(next: EngineParams): void {
    const p = sanitizeParams(next);
    const prev = this.params;
    this.params = p;
    if (p.debugView !== prev.debugView) {
      // The debug byte is baked into vertices: re-send everything.
      for (const ci of this.meshed) this.dirty.add(ci);
    }
    if ((p.amplification === 0 || p.paused) && this.wobbleBase.size > 0) this.endWobble([...this.wobbleBase.keys()]);
  }

  private resetWorldState(): void {
    if (this.meshed.size > 0 && this.gen) {
      const w = this.gen.world;
      this.post({ type: 'chunkRemoved', keys: [...this.meshed].map((ci) => w.chunkKey(ci)) });
    }
    this.meshed.clear();
    this.dirty.clear();
    this.pendingSeeds.clear();
    this.pendingCause = null;
    this.events = [];
    this.scheduled = [];
    this.bubbles = [];
    this.wobbleBase.clear();
    this.damage.clear();
  }

  private loadProcedural(kind: ProceduralKind, seed: number, now: number): void {
    this.resetWorldState();
    const t0 = performance.now();
    this.post({ type: 'progress', stage: `generating ${kind}`, done: 0, total: 1 });
    const gen = generateWorld(kind, seed | 0, this.config.voxelSize);
    this.gen = gen;
    this.rand = mulberry32(seed ^ 0x5eed);

    const textures = buildMockTextures(seed | 0);
    const ids = new Map<string, number>();
    for (const t of textures) ids.set(t.name, t.id);
    this.faceTextures = resolveFaceTextures(ids);
    this.post({ type: 'textures', list: textures });

    const w = gen.world;
    for (let ci = 0; ci < w.chunkCount; ci++) if (w.chunkSolid(ci) > 0) this.dirty.add(ci);
    this.initialTotal = this.dirty.size;
    this.loading = true;
    this.viewer = [...gen.spawn.pos];
    this.post({
      type: 'ready',
      info: { bounds: w.bounds, voxelCount: w.solidCount, spawn: gen.spawn, textures: true },
    });
    this.lastStats = now;
    const ms = performance.now() - t0;
    console.info(`[mock] ${kind} #${seed}: ${w.solidCount} voxels, ${this.initialTotal} chunks, generated in ${ms.toFixed(0)} ms`);
  }

  private loadWad(buffer: ArrayBuffer, map: string, options: WadOptions, now: number): void {
    const info = inspectWad(buffer, map);
    const what = info.ok
      ? `${info.kind} with ${info.lumps} lumps, map ${info.map} found (${info.textureLumps} texture lumps)`
      : info.error;
    this.post({
      type: 'error',
      fatal: false,
      command: 'loadWad',
      message:
        `The mock engine cannot voxelize Doom maps (${what}; mode ${options.mode}, shell ${options.shellVoxels}). ` +
        `Loaded the procedural "rooms" world instead. Use ?engine=wasm for WAD levels.`,
    });
    this.loadProcedural('rooms', 1, now);
  }

  // -------------------------------------------------------------------------------------
  // Edits
  // -------------------------------------------------------------------------------------

  /**
   * Removes destructible voxels within a (roughened) sphere. Returns removed voxel count.
   * Neighbours of removed voxels become support-check seeds; a damage halo is recorded.
   */
  private removeSphere(pos: Vec3, radius: number, roughness: number): number {
    const w = this.world;
    if (!w || !(radius > 0)) return 0;
    const c = w.toGrid(pos);
    const r = radius / w.h;
    const halo = r * 1.6;
    const x0 = Math.floor(c[0] - halo);
    const x1 = Math.ceil(c[0] + halo);
    const y0 = Math.floor(c[1] - halo);
    const y1 = Math.ceil(c[1] + halo);
    const z0 = Math.max(0, Math.floor(c[2] - halo));
    const z1 = Math.ceil(c[2] + halo);
    let removed = 0;
    for (let z = z0; z <= z1; z++) {
      for (let y = y0; y <= y1; y++) {
        for (let x = x0; x <= x1; x++) {
          const b = w.get(x, y, z);
          if (b === AIR || !w.inBounds(x, y, z)) continue;
          const dx = x + 0.5 - c[0];
          const dy = y + 0.5 - c[1];
          const dz = z + 0.5 - c[2];
          const d = Math.sqrt(dx * dx + dy * dy + dz * dz);
          const id = w.pack(x, y, z);
          const rough = r * (1 + roughness * (hashVoxel(id) - 0.5));
          if (d <= rough && !isIndestructible(b)) {
            w.set(x, y, z, AIR);
            w.affectedChunks(x, y, z, this.dirty);
            this.damage.delete(id);
            this.invalidateWobble(x, y, z);
            removed++;
            if (x > 0) this.pendingSeeds.add(id - 1);
            if (x + 1 < w.nx) this.pendingSeeds.add(id + 1);
            if (y > 0) this.pendingSeeds.add(id - w.nx);
            if (y + 1 < w.ny) this.pendingSeeds.add(id + w.nx);
            if (z > 0) this.pendingSeeds.add(id - w.nx * w.ny);
            if (z + 1 < w.nz) this.pendingSeeds.add(id + w.nx * w.ny);
          } else if (d <= halo) {
            const dmg = Math.min(1, (1 - (d - r) / (halo - r)) * Math.min(1, this.params.fragility));
            if (dmg > (this.damage.get(id) ?? 0)) {
              this.damage.set(id, dmg);
              if (this.params.debugView === DebugView.Utilization) w.affectedChunks(x, y, z, this.dirty);
            }
          }
        }
      }
    }
    return removed;
  }

  private carve(pos: Vec3, radius: number): void {
    const w = this.world;
    if (!w) return;
    const r = radius * (0.75 + 0.25 * this.params.fragility);
    const removed = this.removeSphere(pos, r, 0.2);
    if (removed === 0) return;
    this.pendingCause ??= { kind: 'carve', pos, energy: 0 };
    if (this.rand() < 0.5 * Math.min(1, this.params.fragility)) {
      this.events.push({ kind: 'crack', pos: [...pos], normal: this.surfaceNormal(pos), strength: 0.2 + 0.3 * this.rand() });
    }
  }

  private blast(pos: Vec3, radius: number, energy: number, now: number): void {
    const w = this.world;
    if (!w) return;
    const r = radius * (0.75 + 0.25 * this.params.fragility);
    this.removeSphere(pos, r, 0.35);
    this.pendingCause = { kind: 'blast', pos: [...pos], energy };
    this.events.push({ kind: 'impact', pos: [...pos], energy });
    // Cracks on the crater rim.
    const n = 4 + Math.floor(4 * Math.min(2, this.params.fragility));
    for (let k = 0; k < n; k++) {
      const u = this.rand() * 2 - 1;
      const phi = this.rand() * Math.PI * 2;
      const s = Math.sqrt(1 - u * u);
      const dir: Vec3 = [s * Math.cos(phi), s * Math.sin(phi), u];
      const hit = traceVoxels(w, pos, dir, r * 1.5);
      if (!hit) continue;
      const p: Vec3 = [pos[0] + dir[0] * hit.distance, pos[1] + dir[1] * hit.distance, pos[2] + dir[2] * hit.distance];
      this.events.push({ kind: 'crack', pos: p, normal: hit.normal, strength: 0.4 + 0.6 * this.rand() });
    }
    if (!this.params.paused) this.openBubble(pos, r, energy, now);
  }

  /** Outward normal estimate at a surface point: away from nearby solid voxels. */
  private surfaceNormal(pos: Vec3): Vec3 {
    const w = this.world;
    if (!w) return [0, 0, 1];
    const g = w.toGrid(pos);
    const bx = Math.floor(g[0]);
    const by = Math.floor(g[1]);
    const bz = Math.floor(g[2]);
    let nx = 0;
    let ny = 0;
    let nz = 0;
    for (let dz = -1; dz <= 1; dz++)
      for (let dy = -1; dy <= 1; dy++)
        for (let dx = -1; dx <= 1; dx++) {
          if (w.get(bx + dx, by + dy, bz + dz) === AIR) continue;
          nx -= dx;
          ny -= dy;
          nz -= dz;
        }
    const len = Math.hypot(nx, ny, nz);
    return len > 0 ? [nx / len, ny / len, nz / len] : [0, 0, 1];
  }

  // -------------------------------------------------------------------------------------
  // Bubbles and wobble
  // -------------------------------------------------------------------------------------

  private openBubble(pos: Vec3, radius: number, energy: number, now: number): void {
    const w = this.world;
    if (!w) return;
    const p = this.params;
    const bubbleRadius = Math.max(2, radius * 2.5);
    const amplitude = Math.min(0.2, 0.04 * Math.sqrt(Math.max(0, energy) / 1e6) * (p.compliance / 6) * (p.amplification / 3));
    const chunks: number[] = [];
    const s = CHUNK_SIZE * w.h;
    const c = pos;
    for (let ci = 0; ci < w.chunkCount; ci++) {
      if (w.chunkSolid(ci) === 0) continue;
      const o = w.chunkOrigin(ci);
      // Sphere vs chunk box.
      let d2 = 0;
      for (let a = 0; a < 3; a++) {
        const v = c[a]! < o[a]! ? o[a]! - c[a]! : c[a]! > o[a]! + s ? c[a]! - o[a]! - s : 0;
        d2 += v * v;
      }
      if (d2 <= bubbleRadius * bubbleRadius) chunks.push(ci);
    }
    const bubble: Bubble = {
      id: this.nextId++,
      center: [...pos],
      radius: bubbleRadius,
      start: now,
      amplitude,
      omega: (2 * Math.PI * 1.6) / Math.sqrt(p.compliance / 6),
      zeta: Math.max(0.03, p.damping),
      chunks,
    };
    this.bubbles.push(bubble);
    const ev: BubbleEvent = { kind: 'bubble', id: bubble.id, center: bubble.center, radius: bubbleRadius, level: 0 };
    this.events.push(ev);
    if (p.debugView === DebugView.BubbleLevel) for (const ci of chunks) this.dirty.add(ci);
  }

  private updateBubbles(now: number): void {
    if (this.bubbles.length === 0) return;
    const alive: Bubble[] = [];
    const ended: number[] = [];
    for (const b of this.bubbles) {
      if (now - b.start < BUBBLE_DURATION_MS) alive.push(b);
      else ended.push(...b.chunks);
    }
    if (ended.length === 0) return;
    this.bubbles = alive;
    const still = new Set<number>();
    for (const b of alive) for (const ci of b.chunks) still.add(ci);
    const done = ended.filter((ci) => !still.has(ci));
    this.endWobble(done.filter((ci) => this.wobbleBase.has(ci)));
    if (this.params.debugView === DebugView.BubbleLevel) for (const ci of done) this.dirty.add(ci);
  }

  private endWobble(chunks: number[]): void {
    for (const ci of chunks) {
      this.wobbleBase.delete(ci);
      this.dirty.add(ci); // back to the regular undisplaced mesh
    }
  }

  private invalidateWobble(x: number, y: number, z: number): void {
    if (this.wobbleBase.size === 0 || !this.gen) return;
    const touched = new Set<number>();
    this.gen.world.affectedChunks(x, y, z, touched);
    for (const ci of touched) if (this.wobbleBase.has(ci)) this.wobbleBase.set(ci, null);
  }

  /** Displacement at a world point (metres): a damped sag-and-sway around each bubble. */
  private displacement(px: number, py: number, pz: number, now: number, out: number[]): void {
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    for (const b of this.bubbles) {
      const dx = px - b.center[0];
      const dy = py - b.center[1];
      const dz = pz - b.center[2];
      const r2 = (dx * dx + dy * dy + dz * dz) / (b.radius * b.radius);
      if (r2 >= 1) continue;
      const t = (now - b.start) / 1000;
      const falloff = (1 - r2) * (1 - r2);
      const osc = Math.exp(-b.zeta * b.omega * t * 4) * Math.sin(b.omega * t);
      const a = b.amplitude * falloff * osc;
      const horiz = Math.hypot(dx, dy) || 1;
      out[0] += (a * 0.35 * dx) / horiz;
      out[1] += (a * 0.35 * dy) / horiz;
      out[2] -= a;
    }
  }

  private wobble(now: number, out: ChunkMesh[]): void {
    const w = this.world;
    const p = this.params;
    if (!w || this.bubbles.length === 0 || p.amplification === 0 || p.paused) return;
    if (now - this.lastWobble < WOBBLE_INTERVAL_MS) return;
    this.lastWobble = now;
    const chunks = new Set<number>();
    for (const b of this.bubbles) if (b.amplitude > 0) for (const ci of b.chunks) chunks.add(ci);
    const d = [0, 0, 0];
    for (const ci of chunks) {
      let base = this.wobbleBase.get(ci) ?? null;
      if (!base) {
        base = this.buildChunkMesh(ci, false);
        if (!base) continue;
        this.wobbleBase.set(ci, base);
      }
      this.dirty.delete(ci); // the displaced mesh below supersedes a regular remesh
      const vertices = base.vertices.slice(0);
      const f = new Float32Array(vertices);
      const stride = VERTEX_STRIDE / 4;
      for (let o = 0; o < f.length; o += stride) {
        this.displacement(f[o]!, f[o + 1]!, f[o + 2]!, now, d);
        f[o] = f[o]! + d[0]!;
        f[o + 1] = f[o + 1]! + d[1]!;
        f[o + 2] = f[o + 2]! + d[2]!;
      }
      out.push({
        key: w.chunkKey(ci),
        origin: w.chunkOrigin(ci),
        vertices,
        vertexCount: base.vertexCount,
        indices: base.indices.slice(0),
        indexCount: base.indexCount,
      });
      this.meshed.add(ci);
    }
  }

  // -------------------------------------------------------------------------------------
  // Meshing
  // -------------------------------------------------------------------------------------

  private debugFunction(): MeshStyle['debug'] {
    const w = this.world;
    if (!w) return null;
    const view = this.params.debugView;
    if (view === DebugView.BubbleLevel) {
      if (this.bubbles.length === 0) return null;
      const h = w.h;
      const o = w.origin;
      return (gx, gy, gz) => {
        let best = 0;
        for (const b of this.bubbles) {
          const r0 = b.radius / 4;
          const d = Math.hypot(o[0] + (gx + 0.5) * h - b.center[0], o[1] + (gy + 0.5) * h - b.center[1], o[2] + (gz + 0.5) * h - b.center[2]);
          const level = d < r0 ? 1 : d < 2 * r0 ? 2 : d < 4 * r0 ? 3 : 0;
          if (level > 0 && (best === 0 || level < best)) best = level;
        }
        return best;
      };
    }
    if (view === DebugView.Utilization) {
      // Heuristic: solid voxels stacked above (up to 8 m) x slenderness, or crater damage.
      const cache = new Map<number, number>();
      const reach = Math.round(8 / w.h);
      return (gx, gy, gz) => {
        const key = w.pack(gx, gy, gz);
        let load = cache.get(key);
        if (load === undefined) {
          load = 0;
          const top = Math.min(w.columnTop(gx, gy), gz + 1 + reach);
          for (let z = gz + 1; z < top; z++) if (w.get(gx, gy, z) !== AIR) load++;
          cache.set(key, load);
        }
        let around = 0;
        for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) if (w.get(gx + dx, gy + dy, gz) !== AIR) around++;
        const slender = 1 + 2 * (1 - around / 25);
        const u = Math.max(Math.min(1, (load / reach) * slender), this.damage.get(key) ?? 0);
        return Math.round(u * 255);
      };
    }
    return null;
  }

  private buildChunkMesh(ci: number, greedy: boolean): MeshData | null {
    const gen = this.gen;
    if (!gen || gen.world.chunkSolid(ci) === 0) return null;
    const block = fillPaddedChunk(gen.world, ci, this.padded);
    const style: MeshStyle = {
      faceTextures: this.faceTextures,
      texelsPerMetre: DOOM_TEXELS_PER_METRE,
      light: gen.light,
      debug: this.debugFunction(),
      greedy,
    };
    this.builder.reset();
    meshBlock(block, style, this.builder);
    if (this.builder.indexCount === 0) return null;
    return this.builder.finish();
  }

  private meshDirty(budgetMs: number, out: ChunkMesh[], removed: string[]): void {
    const w = this.world;
    if (!w || this.dirty.size === 0) return;
    // Nearest chunks first.
    const s = CHUNK_SIZE * w.h;
    const v = this.viewer;
    const order = [...this.dirty].map((ci) => {
      const o = w.chunkOrigin(ci);
      const dx = o[0] + s / 2 - v[0];
      const dy = o[1] + s / 2 - v[1];
      const dz = o[2] + s / 2 - v[2];
      return { ci, d: dx * dx + dy * dy + dz * dz };
    });
    order.sort((a, b) => a.d - b.d);
    const t0 = performance.now();
    for (const { ci } of order) {
      if (this.wobbleBase.has(ci)) {
        this.dirty.delete(ci); // being displaced; the wobble pass re-sends it
        continue;
      }
      const m = this.buildChunkMesh(ci, true);
      this.dirty.delete(ci);
      this.meshedAcc++;
      if (m) {
        out.push({ key: w.chunkKey(ci), origin: w.chunkOrigin(ci), ...m });
        this.meshed.add(ci);
      } else if (this.meshed.delete(ci)) {
        removed.push(w.chunkKey(ci));
      }
      if (performance.now() - t0 > budgetMs) break;
    }
    this.meshMsAcc += performance.now() - t0;
  }

  // -------------------------------------------------------------------------------------
  // Support checks and detached islands
  // -------------------------------------------------------------------------------------

  private checkSupport(now: number): void {
    const w = this.world;
    if (!w || this.pendingSeeds.size === 0 || this.params.paused) return;
    const t0 = performance.now();
    const seeds = [...this.pendingSeeds];
    this.pendingSeeds.clear();
    const cause = this.pendingCause ?? { kind: 'carve' as const, pos: this.viewer, energy: 0 };
    this.pendingCause = null;
    const { islands } = findIslands(w, seeds, MAX_ISLAND_SEARCH);
    for (const island of islands) this.events.push(this.detach(island, cause, now));
    this.structuralMsAcc += performance.now() - t0;
  }

  private detach(island: Island, cause: Cause, now: number): DetachedEvent {
    const gen = this.gen!;
    const w = gen.world;
    const { nx, ny } = w;
    const plane = nx * ny;
    const n = island.voxels.length;
    let x0 = Infinity;
    let y0 = Infinity;
    let z0 = Infinity;
    let x1 = -Infinity;
    let y1 = -Infinity;
    let z1 = -Infinity;
    let sx = 0;
    let sy = 0;
    let sz = 0;
    for (let k = 0; k < n; k++) {
      const v = island.voxels[k]!;
      const x = v % nx;
      const y = Math.floor(v / nx) % ny;
      const z = Math.floor(v / plane);
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
      if (z < z0) z0 = z;
      if (z > z1) z1 = z;
      sx += x;
      sy += y;
      sz += z;
    }
    const h = w.h;
    const centroid: Vec3 = [
      w.origin[0] + (sx / n + 0.5) * h,
      w.origin[1] + (sy / n + 0.5) * h,
      w.origin[2] + (sz / n + 0.5) * h,
    ];

    // Mesh the island from its own padded bounding box (world-space positions).
    const bx = x1 - x0 + 1;
    const by = y1 - y0 + 1;
    const bz = z1 - z0 + 1;
    const px = bx + 2;
    const py = by + 2;
    const blocks = new Uint8Array(px * py * (bz + 2));
    for (let k = 0; k < n; k++) {
      const v = island.voxels[k]!;
      const x = (v % nx) - x0 + 1;
      const y = (Math.floor(v / nx) % ny) - y0 + 1;
      const z = Math.floor(v / plane) - z0 + 1;
      blocks[x + px * (y + py * z)] = island.blocks[k]!;
    }
    const builder = new MeshBuilder(Math.min(1 << 20, 64 + n));
    meshBlock(
      {
        blocks,
        sx: bx,
        sy: by,
        sz: bz,
        origin: [w.origin[0] + x0 * h, w.origin[1] + y0 * h, w.origin[2] + z0 * h],
        grid: [x0, y0, z0],
        h,
      },
      { faceTextures: this.faceTextures, texelsPerMetre: DOOM_TEXELS_PER_METRE, light: gen.light, debug: null, greedy: true },
      builder,
    );
    const mesh = builder.finish();

    // Remove it from the world (the physics "already removed it").
    for (let k = 0; k < n; k++) {
      const v = island.voxels[k]!;
      const x = v % nx;
      const y = Math.floor(v / nx) % ny;
      const z = Math.floor(v / plane);
      w.set(x, y, z, AIR);
      w.affectedChunks(x, y, z, this.dirty);
      this.invalidateWobble(x, y, z);
      this.damage.delete(v);
    }

    // Motion: blasts throw pieces away from the centre; carves just let them drop.
    const mass = n * h * h * h * DENSITY;
    const velocity: Vec3 = [0, 0, 0];
    if (cause.kind === 'blast') {
      const dx = centroid[0] - cause.pos[0];
      const dy = centroid[1] - cause.pos[1];
      const dz = centroid[2] - cause.pos[2];
      const dist = Math.max(0.5, Math.hypot(dx, dy, dz));
      const speed = Math.min(12, Math.sqrt((2 * 0.02 * cause.energy) / mass) / dist);
      velocity[0] = (dx / dist) * speed;
      velocity[1] = (dy / dist) * speed;
      velocity[2] = (dz / dist) * speed + speed * 0.3;
    } else {
      velocity[0] = (this.rand() - 0.5) * 0.4;
      velocity[1] = (this.rand() - 0.5) * 0.4;
    }
    const spin = Math.min(3, 3 / (1 + Math.sqrt(n) / 10));
    const axis: Vec3 = [this.rand() - 0.5, this.rand() - 0.5, (this.rand() - 0.5) * 0.3];
    const al = Math.hypot(axis[0], axis[1], axis[2]) || 1;
    const angular: Vec3 = [(axis[0] / al) * spin, (axis[1] / al) * spin, (axis[2] / al) * spin];

    // Virtual impact where it lands (plan §B7): mass x fall.
    const below = traceVoxels(w, centroid, [0, 0, -1], 200);
    if (below) {
      const bottomOffset = centroid[2] - (w.origin[2] + z0 * h);
      const fall = Math.max(0, below.distance - bottomOffset);
      const v0 = velocity[2];
      const t = (v0 + Math.sqrt(v0 * v0 + 2 * GRAVITY * fall)) / GRAVITY;
      const landing: Vec3 = [centroid[0] + velocity[0] * t, centroid[1] + velocity[1] * t, centroid[2] - below.distance];
      const energy = mass * (GRAVITY * fall + 0.5 * (velocity[0] ** 2 + velocity[1] ** 2 + v0 * v0));
      if (fall > 0.2) this.scheduled.push({ at: now + t * 1000, event: { kind: 'impact', pos: landing, energy } });
    }

    this.islandsTotal++;
    this.detachedVoxelsTotal += n;
    return { kind: 'detached', id: this.nextId++, voxels: n, centroid, velocity, angular, mesh };
  }

  // -------------------------------------------------------------------------------------
  // Tick
  // -------------------------------------------------------------------------------------

  tick(now: number): void {
    const w = this.world;
    if (!w) return;
    const t0 = performance.now();

    this.checkSupport(now);
    this.updateBubbles(now);

    const meshes: ChunkMesh[] = [];
    const removed: string[] = [];
    this.wobble(now, meshes);
    this.meshDirty(this.loading ? MESH_BUDGET_LOADING_MS : MESH_BUDGET_MS, meshes, removed);
    for (let k = 0; k < meshes.length; k += MESHES_PER_MESSAGE) {
      this.post({ type: 'chunkMeshes', meshes: meshes.slice(k, k + MESHES_PER_MESSAGE) });
    }
    if (removed.length > 0) this.post({ type: 'chunkRemoved', keys: removed });
    if (this.loading) {
      const done = this.initialTotal - this.dirty.size;
      this.post({ type: 'progress', stage: 'meshing', done, total: this.initialTotal });
      if (this.dirty.size === 0) this.loading = false;
    }

    if (this.scheduled.length > 0) {
      const due = this.scheduled.filter((s) => s.at <= now);
      if (due.length > 0) {
        this.scheduled = this.scheduled.filter((s) => s.at > now);
        for (const s of due) this.events.push(s.event);
      }
    }
    if (this.events.length > 0) {
      const list = this.events;
      this.events = [];
      this.eventsAcc += list.length;
      this.post({ type: 'events', list });
    }

    const tickMs = performance.now() - t0;
    this.tickMsEma = this.tickMsEma === 0 ? tickMs : this.tickMsEma * 0.9 + tickMs * 0.1;
    this.ticksAcc++;
    if (now - this.lastStats >= STATS_INTERVAL_MS) this.postStats(now, w);
  }

  private postStats(now: number, w: VoxelWorld): void {
    let activeNodes = 0;
    for (const b of this.bubbles) for (const ci of b.chunks) activeNodes += w.chunkSolid(ci);
    const ticks = Math.max(1, this.ticksAcc);
    const stats: EngineStats = {
      tickMs: this.tickMsEma,
      structuralMs: this.structuralMsAcc / ticks,
      activeBubbles: this.bubbles.length,
      activeNodes,
      voxels: w.solidCount,
      chunks: this.meshed.size,
      memoryMB: w.allocatedBytes / (1024 * 1024),
      events: this.eventsAcc,
      engine: 'mock',
      meshQueue: this.dirty.size,
      meshMsPerChunk: this.meshedAcc > 0 ? this.meshMsAcc / this.meshedAcc : 0,
      displacedChunks: this.wobbleBase.size,
      islands: this.islandsTotal,
      detachedVoxels: this.detachedVoxelsTotal,
      damagedVoxels: this.damage.size,
    };
    this.post({ type: 'stats', stats });
    this.lastStats = now;
    this.structuralMsAcc = 0;
    this.meshMsAcc = 0;
    this.meshedAcc = 0;
    this.ticksAcc = 0;
    this.eventsAcc = 0;
  }
}

