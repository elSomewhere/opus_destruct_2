/**
 * MockEngine: a pure-TypeScript implementation of the engine protocol (docs/API.md).
 *
 * It is the reference implementation of the message contract for the front end, not a
 * physics engine. What it fakes, and how:
 * - structure: a voxel grid with anchored bedrock; after edits, unsupported pieces are
 *   found by flood fill and emitted as rigid `detached` events;
 * - pieces: each falls without collision (gravity, a spin) until its centre reaches the
 *   surface below its detachment point, then rests there as rubble; poses go out as packed
 *   `debris` messages, a landing emits an `impact`, and beyond MAX_PIECES the oldest resting
 *   pieces fade out (as the real engine does over its budget). Pieces never split;
 * - utilization: a heuristic (load above x slenderness, plus damage around craters);
 * - fragments: 0.5 m cells with hashed ids stand in for the engine's pre-scored fragments;
 * - Doom WADs: not voxelized; `loadWad` reports an error and loads a procedural world.
 *
 * The engine is environment-neutral: it talks to the outside only through `post`, and
 * time comes in through `tick(nowMs)`, so tests can drive it synchronously.
 */
import type {
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
import { DEBRIS_STRIDE, DEFAULT_PARAMS, DOOM_TEXELS_PER_METRE, DebugView, emptyEngineStats } from '../../engine/protocol.ts';
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
/** Rigid pieces kept (the engine's max_bodies); beyond, the oldest resting ones fade out. */
const MAX_PIECES = 3000;
const PIECE_FADE_MS = 1000;
/** Pieces falling this far below the world are removed. */
const KILL_DEPTH = 30;
/** Landings slower than this (m/s, a 0.2 m drop) emit no impact. */
const MIN_IMPACT_SPEED = Math.sqrt(2 * GRAVITY * 0.2);
/** Meshing budget per tick; larger while the initial world streams in. */
const MESH_BUDGET_MS = 8;
const MESH_BUDGET_LOADING_MS = 40;
/** Chunk meshes per message (keeps single messages small). */
const MESHES_PER_MESSAGE = 48;

interface Cause {
  kind: 'carve' | 'blast';
  pos: Vec3;
  energy: number;
}

/** A detached piece: falling (awake) until it reaches `restZ`, then resting rubble. */
interface Piece {
  id: number;
  /** Centre of mass (the pivot of the pose rotation is the centre at detachment). */
  pos: Vec3;
  vel: Vec3;
  axis: Vec3;
  spin: number;
  angle: number;
  mass: number;
  /** Centre height at which it lands (-Infinity: nothing below, it falls out of the world). */
  restZ: number;
  awake: boolean;
  /** Time it started fading out (over the budget), or -1. */
  fadeStart: number;
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

/** Fragment id (1..254) of a voxel: 4^3-voxel cells with hashed ids. */
function fragmentId(gx: number, gy: number, gz: number): number {
  const cell = Math.imul(gx >> 2, 73856093) ^ Math.imul(gy >> 2, 19349663) ^ Math.imul(gz >> 2, 83492791);
  return 1 + Math.floor(hashVoxel(cell) * 254);
}

/** (dif, the dynamic increase factor, has no counterpart in the mock.) */
function sanitizeParams(p: EngineParams): EngineParams {
  const num = (v: number, lo: number, hi: number, dflt: number): number =>
    Number.isFinite(v) ? Math.min(hi, Math.max(lo, v)) : dflt;
  const dv = p.debugView;
  return {
    fragility: num(p.fragility, 0.05, 20, DEFAULT_PARAMS.fragility),
    impact: num(p.impact, 0, 20, DEFAULT_PARAMS.impact),
    dif: num(p.dif, 1, 5, DEFAULT_PARAMS.dif),
    debugView: dv === DebugView.Utilization || dv === DebugView.Fragments ? dv : DebugView.None,
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
  /** Rigid pieces, oldest first. */
  private pieces: Piece[] = [];
  /** Pieces were added or removed since the last `debris` message. */
  private piecesChanged = false;
  private lastPieceStep = -1;
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
  private landingsTotal = 0;
  private ticksTotal = 0;

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
        this.blast(cmd.pos, cmd.radius, cmd.energy);
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
    this.pieces = []; // (the front end drops the old world's pieces when it requests a load)
    this.piecesChanged = false;
    this.lastPieceStep = -1;
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

  private blast(pos: Vec3, radius: number, energy: number): void {
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
  // Meshing
  // -------------------------------------------------------------------------------------

  private debugFunction(): MeshStyle['debug'] {
    const w = this.world;
    if (!w) return null;
    const view = this.params.debugView;
    if (view === DebugView.Fragments) {
      // every destructible voxel belongs to a fragment (pieces keep their fragments' colours)
      return (gx, gy, gz) => (isIndestructible(w.get(gx, gy, gz)) ? 0 : fragmentId(gx, gy, gz));
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

  private checkSupport(): void {
    const w = this.world;
    if (!w || this.pendingSeeds.size === 0 || this.params.paused) return;
    const t0 = performance.now();
    const seeds = [...this.pendingSeeds];
    this.pendingSeeds.clear();
    const cause = this.pendingCause ?? { kind: 'carve' as const, pos: this.viewer, energy: 0 };
    this.pendingCause = null;
    const { islands } = findIslands(w, seeds, MAX_ISLAND_SEARCH);
    for (const island of islands) this.events.push(this.detach(island, cause));
    this.structuralMsAcc += performance.now() - t0;
  }

  private detach(island: Island, cause: Cause): DetachedEvent {
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
      {
        faceTextures: this.faceTextures,
        texelsPerMetre: DOOM_TEXELS_PER_METRE,
        light: gen.light,
        debug: this.params.debugView === DebugView.Fragments ? this.debugFunction() : null,
        greedy: true,
      },
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

    // It lands where its bottom meets the surface below its detachment point.
    const below = traceVoxels(w, centroid, [0, 0, -1], 200);
    const bottomOffset = centroid[2] - (w.origin[2] + z0 * h);
    const restZ = below ? centroid[2] - Math.max(0, below.distance - bottomOffset) : -Infinity;
    const id = this.nextId++;
    this.pieces.push({
      id,
      pos: [...centroid],
      vel: [...velocity],
      axis: [angular[0] / spin, angular[1] / spin, angular[2] / spin],
      spin,
      angle: 0,
      mass,
      restZ,
      awake: true,
      fadeStart: -1,
    });
    this.piecesChanged = true;

    this.islandsTotal++;
    this.detachedVoxelsTotal += n;
    return { kind: 'detached', id, voxels: n, centroid, velocity, angular, mesh, rigid: true };
  }

  /**
   * Moves the falling pieces (no collision: each lands on the surface below its detachment
   * point, with an impact scaled by the `impact` knob), fades the oldest resting ones beyond
   * MAX_PIECES, and posts the poses.
   */
  private stepPieces(now: number): void {
    const w = this.world;
    if (!w) return;
    const dt = this.lastPieceStep < 0 ? 0 : Math.min(0.1, Math.max(0, (now - this.lastPieceStep) / 1000));
    this.lastPieceStep = now;
    if (this.params.paused || this.pieces.length === 0) {
      this.postPoses(false);
      return;
    }
    const killZ = w.bounds.min[2] - KILL_DEPTH;
    let over = this.pieces.length - MAX_PIECES;
    let moving = false;
    for (const p of this.pieces) {
      if (p.fadeStart >= 0) {
        moving = true;
        over--;
      } else if (over > 0 && !p.awake) {
        p.fadeStart = now; // oldest resting pieces first
        moving = true;
        over--;
      }
      if (!p.awake) continue;
      moving = true;
      p.vel[2] -= GRAVITY * dt;
      for (let a = 0; a < 3; a++) p.pos[a] = p.pos[a]! + p.vel[a]! * dt;
      p.angle += p.spin * dt;
      if (p.pos[2] <= p.restZ) {
        p.pos[2] = p.restZ;
        p.awake = false;
        this.landingsTotal++;
        const speed = Math.hypot(p.vel[0], p.vel[1], p.vel[2]);
        if (speed > MIN_IMPACT_SPEED) {
          const energy = 0.5 * p.mass * speed * speed * this.params.impact;
          this.events.push({ kind: 'impact', pos: [p.pos[0], p.pos[1], p.pos[2]], energy });
        }
      }
    }
    const before = this.pieces.length;
    this.pieces = this.pieces.filter((p) => p.pos[2] > killZ && (p.fadeStart < 0 || now - p.fadeStart < PIECE_FADE_MS));
    if (this.pieces.length !== before) this.piecesChanged = true;
    this.postPoses(moving);
  }

  /** Packed poses, when anything moved or the set changed (resting rubble is not re-sent). */
  private postPoses(moving: boolean): void {
    if (!moving && !this.piecesChanged) return;
    this.piecesChanged = false;
    const poses = new Float64Array(this.pieces.length * DEBRIS_STRIDE);
    const now = this.lastPieceStep;
    this.pieces.forEach((p, i) => {
      const s = Math.sin(p.angle / 2);
      const opacity = p.fadeStart < 0 ? 1 : Math.max(0, 1 - (now - p.fadeStart) / PIECE_FADE_MS);
      poses.set([p.id, p.pos[0], p.pos[1], p.pos[2], p.axis[0] * s, p.axis[1] * s, p.axis[2] * s, Math.cos(p.angle / 2), opacity], i * DEBRIS_STRIDE);
    });
    this.post({ type: 'debris', poses });
  }

  // -------------------------------------------------------------------------------------
  // Tick
  // -------------------------------------------------------------------------------------

  tick(now: number): void {
    const w = this.world;
    if (!w) return;
    const t0 = performance.now();

    this.ticksTotal++;
    this.checkSupport();
    this.stepPieces(now);

    const meshes: ChunkMesh[] = [];
    const removed: string[] = [];
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
    const ticks = Math.max(1, this.ticksAcc);
    let awake = 0;
    for (const p of this.pieces) if (p.awake) awake++;
    // (the structure and solver counters have no counterpart in the mock: zero)
    const stats: EngineStats = {
      ...emptyEngineStats(),
      tickMs: this.tickMsEma,
      structuralMs: this.structuralMsAcc / ticks,
      meshMs: this.meshMsAcc / ticks,
      voxels: w.solidCount,
      chunks: this.meshed.size,
      memoryMB: w.allocatedBytes / (1024 * 1024),
      events: this.eventsAcc,
      ticks: this.ticksTotal,
      detachedVoxels: this.detachedVoxelsTotal,
      detachedPieces: this.islandsTotal,
      pieces: this.pieces.length,
      awakePieces: awake,
      impactLoads: this.landingsTotal,
      residentChunks: w.chunkCount,
      engine: 'mock',
      meshQueue: this.dirty.size,
      meshMsPerChunk: this.meshedAcc > 0 ? this.meshMsAcc / this.meshedAcc : 0,
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

