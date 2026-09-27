/**
 * WASM engine worker: serves the engine protocol (docs/API.md) with the structvox C++ core
 * compiled to WebAssembly. The module (web/src/wasm/svx_web.{js,wasm}) is produced by
 *   cmake --preset wasm-release-threads && cmake --build --preset wasm-release-threads --target svx_web
 * and exposes the flat C ABI of core/include/svx/api/svx_api.h. Selected with ?engine=wasm.
 *
 * The worker owns the simulation clock (fixed 60 Hz tick). After every tick it forwards
 * changed chunk meshes, removed chunks and events; stats go out at ~4 Hz.
 */
import type {
  ChunkMesh,
  ChunkOccupancy,
  DisplacementField,
  EngineCommand,
  EngineEvent,
  EngineParams,
  EngineStats,
  InitConfig,
  ProceduralKind,
  TextureInfo,
  Vec3,
  WadOptions,
  WorldInfo,
} from '../engine/protocol.ts';
import { DEBRIS_STRIDE, DEFAULT_PARAMS, FLAME_STRIDE, SMOKE_STRIDE, TIMELINE_STRIDE, VERTEX_STRIDE } from '../engine/protocol.ts';
import { postToMain, reportError, serveCommands } from './host.ts';
// Generated Emscripten ES module (see the header comment); typed by SvxModule below.
import createSvxModule from '../wasm/svx_web.js';

interface SvxModule {
  HEAPU8: Uint8Array;
  HEAP32: Int32Array;
  HEAPF32: Float32Array;
  HEAPF64: Float64Array;
  UTF8ToString(ptr: number): string;
  stringToUTF8(str: string, ptr: number, max: number): void;
  lengthBytesUTF8(str: string): number;
  _malloc(n: number): number;
  _free(ptr: number): void;
  _svx_create(h: number): number;
  _svx_destroy(e: number): void;
  _svx_set_threads(n: number): void;
  _svx_set_params(e: number, fragility: number, impact: number, dif: number, reserved: number, dbg: number, paused: number): void;
  _svx_load_procedural(e: number, kind: number, seed: number): number;
  _svx_load_wad(e: number, data: number, size: number, map: number, mode: number, shell: number): number;
  _svx_last_error(e: number): number;
  _svx_save_delta(e: number, outSize: number): number;
  _svx_load_delta(e: number, data: number, size: number): number;
  _svx_modified(e: number): number;
  _svx_texture_count(e: number): number;
  _svx_texture_size(e: number, i: number, out2: number): void;
  _svx_texture_name(e: number, i: number): number;
  _svx_texture_rgba(e: number, i: number): number;
  _svx_bake(e: number): number;
  _svx_world_info(e: number, out: number): void;
  _svx_tick(e: number): void;
  _svx_viewer(e: number, x: number, y: number, z: number): void;
  _svx_carve(e: number, x: number, y: number, z: number, r: number): void;
  _svx_blast(e: number, x: number, y: number, z: number, r: number, energy: number): void;
  _svx_ignite(e: number, x: number, y: number, z: number, r: number): void;
  _svx_extinguish(e: number, x: number, y: number, z: number, r: number): void;
  _svx_poll_env(e: number, maxFlames: number, maxSmoke: number): number;
  _svx_env_flames(e: number): number;
  _svx_env_smoke_count(e: number): number;
  _svx_env_smoke(e: number): number;
  _svx_use(e: number, ox: number, oy: number, oz: number, dx: number, dy: number, dz: number): number;
  _svx_raycast(e: number, ox: number, oy: number, oz: number, dx: number, dy: number, dz: number, max: number, out: number): number;
  _svx_collide(e: number, a: number, b: number, c: number, d: number, f: number, g: number, mx: number, my: number, mz: number, out: number): void;
  _svx_poll_meshes(e: number): number;
  _svx_mesh_info(e: number, i: number, out: number): void;
  _svx_mesh_vertices(e: number, i: number): number;
  _svx_mesh_indices(e: number, i: number): number;
  _svx_poll_removed(e: number): number;
  _svx_removed_chunk(e: number, i: number, out3: number): void;
  _svx_chunk_occupancy(e: number, cx: number, cy: number, cz: number, out: number): number;
  _svx_poll_far(e: number): number;
  _svx_far_info(e: number, i: number, out: number): void;
  _svx_far_vertices(e: number, i: number): number;
  _svx_far_indices(e: number, i: number): number;
  _svx_poll_far_removed(e: number): number;
  _svx_far_removed(e: number, i: number, out2: number): void;
  _svx_poll_events(e: number): number;
  _svx_event_info(e: number, i: number, out: number): void;
  _svx_event_vertices(e: number, i: number): number;
  _svx_event_indices(e: number, i: number): number;
  _svx_stats(e: number, out: number): void;
  _svx_debris(e: number): number;
  _svx_debris_data(e: number): number;
  _svx_set_debris(e: number, enabled: number): void;
  _svx_set_gpu_displacement(e: number, enabled: number): void;
  _svx_poll_fields(e: number): number;
  _svx_field_info(e: number, i: number, out: number): void;
  _svx_field_data(e: number, i: number): number;
}

const TICK_MS = 1000 / 60;
const STATS_MS = 250;
// job and budget timeline: per-tick samples since the last stats message (TIMELINE_FIELDS)
let timeline: number[] = [];
const AUTOSAVE_MS = 5000;
/** svx_stats fills out[0..47]. */
const STATS_COUNT = 48;
/** Flames and smoke cells sent to the renderer at most, and how often (ticks: their step). */
const MAX_FLAMES = 4096;
const MAX_SMOKE = 4096;
const ENV_TICKS = 6;

let mod: SvxModule | null = null;
let eng = 0;
let scratch = 0; // 64 doubles of call scratch space
let config: InitConfig = { voxelSize: 0.125, threads: 1, memoryMB: 1024, params: { ...DEFAULT_PARAMS } };
let params: EngineParams = { ...DEFAULT_PARAMS };
let loaded = false;
let lastStats = 0;
let eventsSinceStats = 0;
const knownChunks = new Set<string>();
let worldId = '';
let lastSave = 0;
let saving = false;
let debrisLive = 0;
let debrisSent = new Float64Array(0); // poses of the last debris message (resting rubble is not re-sent)
let envLive = false;
let envTick = 0;
let fieldsLive = 0;
let fieldsKey = ''; // (id:version of each field last sent: an unchanged set is not sent again)
let occBuf = 0; // 4096-byte scratch for chunk occupancy

// ---- persistence (OPFS deltas; opt-in with InitConfig.persist) -------------------------

async function deltaFile(create: boolean): Promise<FileSystemFileHandle | null> {
  if (!config.persist || !worldId || !('storage' in navigator)) return null;
  try {
    const root = await navigator.storage.getDirectory();
    const dir = await root.getDirectoryHandle('structvox', { create: true });
    const name = `${worldId.replace(/[^A-Za-z0-9_.-]/g, '_')}.svxd`;
    return await dir.getFileHandle(name, { create });
  } catch {
    return null;
  }
}

async function restoreDelta(): Promise<void> {
  const m = mod as SvxModule;
  const fh = await deltaFile(false);
  if (!fh) return;
  const bytes = new Uint8Array(await (await fh.getFile()).arrayBuffer());
  if (bytes.length === 0) return;
  const ptr = m._malloc(bytes.length);
  m.HEAPU8.set(bytes, ptr);
  const rc = m._svx_load_delta(eng, ptr, bytes.length);
  m._free(ptr);
  console.info(`[wasm] restored ${worldId}: ${rc === 0 ? `${bytes.length} bytes` : 'invalid delta ignored'}`);
}

async function saveDelta(): Promise<void> {
  const m = mod as SvxModule;
  if (saving || m._svx_modified(eng) !== 1) return;
  saving = true;
  try {
    const fh = await deltaFile(true);
    if (!fh) return;
    const ptr = m._svx_save_delta(eng, scratch);
    const bytes = m.HEAPU8.slice(ptr, ptr + f64(0));
    const w = await fh.createWritable();
    await w.write(bytes);
    await w.close();
  } catch (err) {
    console.warn('[wasm] autosave failed', err);
  } finally {
    saving = false;
  }
}

function chunkKey(x: number, y: number, z: number): string {
  return `${x},${y},${z}`;
}

async function ensureModule(): Promise<SvxModule> {
  if (mod) return mod;
  const m = (await createSvxModule()) as unknown as SvxModule;
  mod = m;
  scratch = m._malloc(Math.max(64, STATS_COUNT) * 8);
  occBuf = m._malloc(4096);
  eng = m._svx_create(config.voxelSize);
  m._svx_set_threads(1);
  m._svx_set_gpu_displacement(eng, config.gpuDisplacement === false ? 0 : 1);
  applyParams(params);
  return m;
}

function f64(i: number): number {
  return (mod as SvxModule).HEAPF64[(scratch >> 3) + i] ?? 0;
}

function withString<T>(s: string, fn: (ptr: number) => T): T {
  const m = mod as SvxModule;
  const n = m.lengthBytesUTF8(s) + 1;
  const p = m._malloc(n);
  m.stringToUTF8(s, p, n);
  try {
    return fn(p);
  } finally {
    m._free(p);
  }
}

function applyParams(p: EngineParams): void {
  params = p;
  if (!mod || !eng) return;
  mod._svx_set_params(eng, p.fragility, p.impact, p.dif, 0, p.debugView, p.paused ? 1 : 0);
}

function copyOut(ptr: number, bytes: number): ArrayBuffer {
  const m = mod as SvxModule;
  return m.HEAPU8.slice(ptr, ptr + bytes).buffer;
}

function worldInfo(texturesSent: boolean): WorldInfo {
  const m = mod as SvxModule;
  m._svx_world_info(eng, scratch);
  const v = (i: number): number => f64(i);
  return {
    bounds: { min: [v(0), v(1), v(2)], max: [v(3), v(4), v(5)] },
    voxelCount: v(6),
    spawn: { pos: [v(7), v(8), v(9)], dir: [v(10), v(11), v(12)] },
    textures: texturesSent,
  };
}

function sendTextures(): boolean {
  const m = mod as SvxModule;
  const n = m._svx_texture_count(eng);
  if (n === 0) return false;
  const list: TextureInfo[] = [];
  const size = scratch;
  for (let i = 0; i < n; i++) {
    m._svx_texture_size(eng, i, size);
    const w = m.HEAP32[size >> 2] ?? 0;
    const h = m.HEAP32[(size >> 2) + 1] ?? 0;
    const name = m.UTF8ToString(m._svx_texture_name(eng, i));
    list.push({ id: i, name, width: w, height: h, rgba: copyOut(m._svx_texture_rgba(eng, i), w * h * 4) });
  }
  postToMain({ type: 'textures', list });
  return true;
}

function occupancyOf(cx: number, cy: number, cz: number): ChunkOccupancy {
  const m = mod as SvxModule;
  const state = m._svx_chunk_occupancy(eng, cx, cy, cz, occBuf) as 0 | 1 | 2;
  return state === 2 ? { chunk: [cx, cy, cz], state, bits: copyOut(occBuf, 4096) } : { chunk: [cx, cy, cz], state };
}

function flushMeshes(): void {
  const m = mod as SvxModule;
  const n = m._svx_poll_meshes(eng);
  const meshes: ChunkMesh[] = [];
  const occupancy: ChunkOccupancy[] = [];
  for (let i = 0; i < n; i++) {
    m._svx_mesh_info(eng, i, scratch);
    const vc = f64(6);
    const ic = f64(7);
    occupancy.push(occupancyOf(f64(0), f64(1), f64(2)));
    const key = chunkKey(f64(0), f64(1), f64(2));
    knownChunks.add(key);
    meshes.push({
      key,
      origin: [f64(3), f64(4), f64(5)],
      vertices: copyOut(m._svx_mesh_vertices(eng, i), vc * VERTEX_STRIDE),
      vertexCount: vc,
      indices: copyOut(m._svx_mesh_indices(eng, i), ic * 4),
      indexCount: ic,
    });
  }
  // far render tier (streamed worlds): coarse tile meshes under "far:x,y" keys
  const nfar = m._svx_poll_far(eng);
  for (let i = 0; i < nfar; i++) {
    m._svx_far_info(eng, i, scratch);
    const vc = f64(6);
    const ic = f64(7);
    const key = `far:${f64(0)},${f64(1)}`;
    knownChunks.add(key);
    meshes.push({
      key,
      origin: [f64(3), f64(4), f64(5)],
      vertices: copyOut(m._svx_far_vertices(eng, i), vc * VERTEX_STRIDE),
      vertexCount: vc,
      indices: copyOut(m._svx_far_indices(eng, i), ic * 4),
      indexCount: ic,
    });
  }
  const nfr = m._svx_poll_far_removed(eng);
  if (nfr > 0) {
    const keys: string[] = [];
    for (let i = 0; i < nfr; i++) {
      m._svx_far_removed(eng, i, scratch);
      const key = `far:${m.HEAP32[scratch >> 2] ?? 0},${m.HEAP32[(scratch >> 2) + 1] ?? 0}`;
      if (knownChunks.delete(key)) keys.push(key);
    }
    if (keys.length > 0) postToMain({ type: 'chunkRemoved', keys });
  }
  // displacement fields of the running bubbles, sent with the last mesh batch of the tick
  let fields: DisplacementField[] | undefined;
  const nf = m._svx_poll_fields(eng);
  if (nf > 0 || fieldsLive > 0) {
    let key = '';
    for (let i = 0; i < nf; i++) {
      m._svx_field_info(eng, i, scratch);
      key += `${f64(0)}:${f64(9)},`;
    }
    if (key !== fieldsKey) {
      fields = [];
      for (let i = 0; i < nf; i++) {
        m._svx_field_info(eng, i, scratch);
        const size: [number, number, number] = [f64(4), f64(5), f64(6)];
        fields.push({
          id: f64(0),
          origin: [f64(1), f64(2), f64(3)],
          size,
          voxelSize: config.voxelSize * Math.max(1, f64(8)),
          maxDisp: f64(7),
          data: copyOut(m._svx_field_data(eng, i), size[0] * size[1] * size[2] * 8),
        });
      }
      fieldsKey = key;
    }
    fieldsLive = nf;
  }
  if (meshes.length > 0) {
    // keep messages moderate: batches of 64 chunks
    for (let k = 0; k < meshes.length; k += 64) {
      const last = k + 64 >= meshes.length;
      postToMain({ type: 'chunkMeshes', meshes: meshes.slice(k, k + 64), ...(last && fields ? { fields } : {}) });
    }
  } else if (fields) {
    postToMain({ type: 'chunkMeshes', meshes: [], fields });
  }
  const r = m._svx_poll_removed(eng);
  if (r > 0) {
    const keys: string[] = [];
    for (let i = 0; i < r; i++) {
      m._svx_removed_chunk(eng, i, scratch);
      const b = scratch >> 2;
      const c: [number, number, number] = [m.HEAP32[b] ?? 0, m.HEAP32[b + 1] ?? 0, m.HEAP32[b + 2] ?? 0];
      // a chunk without visible faces may still be solid inside (rock)
      occupancy.push(occupancyOf(c[0], c[1], c[2]));
      const key = chunkKey(c[0], c[1], c[2]);
      if (knownChunks.delete(key)) keys.push(key);
    }
    if (keys.length > 0) postToMain({ type: 'chunkRemoved', keys });
  }
  if (occupancy.length > 0) postToMain({ type: 'occupancy', voxelSize: config.voxelSize, chunks: occupancy });
}

function flushEvents(): void {
  const m = mod as SvxModule;
  const n = m._svx_poll_events(eng);
  if (n === 0) return;
  const list: EngineEvent[] = [];
  for (let i = 0; i < n; i++) {
    m._svx_event_info(eng, i, scratch);
    const kind = f64(0);
    const pos: Vec3 = [f64(2), f64(3), f64(4)];
    if (kind === 0) {
      const vc = f64(18);
      const ic = f64(19);
      list.push({
        kind: 'detached',
        id: f64(1),
        voxels: f64(16),
        centroid: pos,
        velocity: [f64(5), f64(6), f64(7)],
        angular: [f64(8), f64(9), f64(10)],
        rigid: f64(20) > 0,
        mesh: {
          vertices: copyOut(m._svx_event_vertices(eng, i), vc * VERTEX_STRIDE),
          vertexCount: vc,
          indices: copyOut(m._svx_event_indices(eng, i), ic * 4),
          indexCount: ic,
        },
      });
    } else if (kind === 1) {
      const voxels = f64(16);
      list.push(
        voxels > 0
          ? { kind: 'crack', pos, normal: [f64(11), f64(12), f64(13)], strength: f64(15), voxels, velocity: [f64(5), f64(6), f64(7)], radius: f64(14) }
          : { kind: 'crack', pos, normal: [f64(11), f64(12), f64(13)], strength: f64(15) },
      );
    } else if (kind === 2) {
      list.push({ kind: 'impact', pos, energy: f64(15) });
    }
    // (kind 3, the v1 bubble debug event, is not emitted by v2 engines)
  }
  eventsSinceStats += list.length;
  if (list.length > 0) postToMain({ type: 'events', list });
}

/**
 * Poses of the rigid pieces, packed (DEBRIS_STRIDE doubles each; one final empty set after the
 * last one is gone). A set identical to the previous one (all pieces resting) is not re-sent.
 */
function flushDebris(): void {
  const m = mod as SvxModule;
  const n = m._svx_debris(eng);
  if (n === 0 && debrisLive === 0) return;
  debrisLive = n;
  const b = m._svx_debris_data(eng) >> 3;
  const poses = m.HEAPF64.slice(b, b + n * DEBRIS_STRIDE);
  if (poses.length > 0 && poses.length === debrisSent.length && poses.every((v, i) => v === debrisSent[i])) return;
  debrisSent = poses.slice();
  postToMain({ type: 'debris', poses });
}

/** The environment for the renderer: flames and smoke (one final empty set when all is clear). */
function flushEnv(): void {
  if (++envTick < ENV_TICKS) return;
  envTick = 0;
  const m = mod as SvxModule;
  const n = m._svx_poll_env(eng, MAX_FLAMES, MAX_SMOKE);
  const ns = m._svx_env_smoke_count(eng);
  if (n === 0 && ns === 0 && !envLive) return;
  envLive = n > 0 || ns > 0;
  const b = m._svx_env_flames(eng) >> 2;
  const bs = m._svx_env_smoke(eng) >> 2;
  postToMain({ type: 'env', flames: m.HEAPF32.slice(b, b + n * FLAME_STRIDE), smoke: m.HEAPF32.slice(bs, bs + ns * SMOKE_STRIDE) });
}

/** One timeline sample: the tick's parts (engine timings of that tick) and the flush after it. */
function sampleTimeline(tickMs: number, flushMs: number): void {
  (mod as SvxModule)._svx_stats(eng, scratch);
  // a paused tick only streams: the other timings are those of the last running tick
  const running = !params.paused;
  const structural = running ? f64(1) : 0;
  const events = running ? f64(2) : 0;
  const rigid = running ? f64(3) : 0;
  const stream = f64(28);
  const other = Math.max(0, tickMs - structural - events - rigid - stream);
  timeline.push(structural, rigid, events, stream, other, flushMs, f64(21));
  if (timeline.length > 600 * TIMELINE_STRIDE) timeline = timeline.slice(-300 * TIMELINE_STRIDE);
}

function sendStats(now: number): void {
  const m = mod as SvxModule;
  m._svx_stats(eng, scratch);
  const r2 = (i: number): number => Number(f64(i).toFixed(2)); // (two decimals)
  const stats: EngineStats = {
    tickMs: f64(0),
    structuralMs: f64(1),
    eventMs: r2(2),
    rigidMs: r2(3),
    meshMs: r2(4),
    voxels: f64(5),
    chunks: f64(6),
    memoryMB: f64(7),
    events: eventsSinceStats, // (8 is the engine's running total)
    ticks: f64(9),
    structures: f64(10),
    structuresSolving: f64(11),
    solvingNodes: f64(12),
    extractions: f64(13),
    convergedSolves: f64(14),
    pcgIterations: f64(15),
    bondsBroken: f64(16),
    detachedVoxels: f64(17),
    detachedPieces: f64(18),
    maxUtilization: r2(19),
    pieces: f64(20),
    awakePieces: f64(21),
    contacts: f64(22),
    pieceChecks: f64(23),
    pieceSplits: f64(24),
    impactLoads: f64(25),
    residentChunks: f64(26),
    archivedChunks: f64(27),
    streamMs: r2(28),
    evictedChunks: f64(29),
    movers: f64(30),
    designMaxUtilization: r2(31),
    strengthenedVoxels: f64(32),
    floatingVoxelsRemoved: f64(33),
    bakeMs: Math.round(f64(34)),
    worldMemoryMB: r2(35),
    fragmentCacheMB: r2(36),
    structureMemoryMB: r2(37),
    pieceMemoryMB: r2(38),
    archiveMB: r2(39),
    archiveCapacityMB: r2(40),
    forgottenRegions: f64(41),
    culledPieces: f64(42),
    fireHot: f64(43),
    fireBurning: f64(44),
    envMs: r2(45),
    smokeCells: f64(46),
    smokeBlocks: f64(47),
  };
  eventsSinceStats = 0;
  lastStats = now;
  postToMain({ type: 'stats', stats, timeline: Float32Array.from(timeline) });
  timeline = [];
}

/**
 * Threads for loading and baking (the module is built with pthreads; the pool's threads come
 * from a pre-spawned set of 16). Gameplay leaves two cores to the page and the renderer.
 */
function loadThreads(): number {
  return typeof SharedArrayBuffer !== 'undefined' && self.crossOriginIsolated ? Math.max(1, Math.min(8, config.threads)) : 1;
}

function playThreads(): number {
  return Math.max(1, Math.min(8, loadThreads(), config.threads - 2));
}

async function finishLoad(texturesSent: boolean, label: string): Promise<void> {
  const m = mod as SvxModule;
  postToMain({ type: 'progress', stage: `baking ${label}`, done: 1, total: 3 });
  const t0 = performance.now();
  m._svx_set_threads(loadThreads());
  const baked = m._svx_bake(eng) === 1;
  // the saved changes go onto the designed world (a delta carries its chunks' design classes;
  // baked after it, damaged members would be designed again as if built that way)
  await restoreDelta();
  // gameplay: structure solves and pieces
  m._svx_set_threads(playThreads());
  console.info(`[wasm] ${label}: bake ${baked ? 'done' : 'skipped (world too large)'} in ${(performance.now() - t0).toFixed(0)} ms`);
  postToMain({ type: 'progress', stage: `meshing ${label}`, done: 2, total: 3 });
  postToMain({ type: 'ready', info: worldInfo(texturesSent) });
  flushMeshes();
  loaded = true;
  postToMain({ type: 'progress', stage: 'ready', done: 3, total: 3 });
}

function clearChunks(): void {
  if (knownChunks.size > 0) postToMain({ type: 'chunkRemoved', keys: [...knownChunks] });
  knownChunks.clear();
  fieldsLive = 0;
  fieldsKey = '';
  debrisLive = 0; // (the front end drops the old world's pieces when it requests a load)
  debrisSent = new Float64Array(0);
  envLive = false;
}

async function loadProcedural(kind: ProceduralKind, seed: number): Promise<void> {
  const m = await ensureModule();
  loaded = false;
  clearChunks();
  postToMain({ type: 'progress', stage: `generating ${kind}`, done: 0, total: 3 });
  withString(kind, (p) => m._svx_load_procedural(eng, p, seed >>> 0));
  worldId = `proc-${kind}-${seed >>> 0}`;
  await finishLoad(false, kind);
}

async function loadWad(buffer: ArrayBuffer, map: string, options: WadOptions): Promise<void> {
  const m = await ensureModule();
  loaded = false;
  clearChunks();
  postToMain({ type: 'progress', stage: `voxelizing ${map}`, done: 0, total: 3 });
  const bytes = new Uint8Array(buffer);
  const ptr = m._malloc(bytes.length);
  m.HEAPU8.set(bytes, ptr);
  const rc = withString(map, (mp) =>
    m._svx_load_wad(eng, ptr, bytes.length, mp, options.mode === 'air' ? 1 : 0, options.shellVoxels | 0),
  );
  m._free(ptr);
  if (rc !== 0) {
    const msg = m.UTF8ToString(m._svx_last_error(eng));
    postToMain({
      type: 'error',
      fatal: false,
      command: 'loadWad',
      message: `cannot load ${map}: ${msg}. Loaded the procedural "rooms" world instead.`,
    });
    await loadProcedural('rooms', 1); // as the mock does: never leave the view without a world
    return;
  }
  const tex = sendTextures();
  // world identity: map name + a cheap content fingerprint of the WAD
  let fp = bytes.length >>> 0;
  for (let i = 0; i < bytes.length; i += 4099) fp = (Math.imul(fp, 31) + (bytes[i] ?? 0)) >>> 0;
  worldId = `wad-${map}-${fp.toString(16)}`;
  await finishLoad(tex, map);
}

async function handle(cmd: EngineCommand): Promise<void> {
  switch (cmd.type) {
    case 'init':
      config = cmd.config;
      params = { ...cmd.config.params };
      await ensureModule();
      break;
    case 'loadProcedural':
      await loadProcedural(cmd.kind, cmd.seed);
      break;
    case 'loadWad':
      await loadWad(cmd.buffer, cmd.map, cmd.options);
      break;
    case 'viewer':
      if (mod && loaded) mod._svx_viewer(eng, cmd.pos[0], cmd.pos[1], cmd.pos[2]);
      break;
    case 'blast':
      if (mod && loaded) mod._svx_blast(eng, cmd.pos[0], cmd.pos[1], cmd.pos[2], cmd.radius, cmd.energy);
      break;
    case 'carve':
      if (mod && loaded) mod._svx_carve(eng, cmd.pos[0], cmd.pos[1], cmd.pos[2], cmd.radius);
      break;
    case 'ignite':
      if (mod && loaded) mod._svx_ignite(eng, cmd.pos[0], cmd.pos[1], cmd.pos[2], cmd.radius);
      break;
    case 'extinguish':
      if (mod && loaded) mod._svx_extinguish(eng, cmd.pos[0], cmd.pos[1], cmd.pos[2], cmd.radius);
      break;
    case 'use':
      if (mod && loaded) mod._svx_use(eng, cmd.pos[0], cmd.pos[1], cmd.pos[2], cmd.dir[0], cmd.dir[1], cmd.dir[2]);
      break;
    case 'raycast': {
      let hit = null;
      if (mod && loaded) {
        const d = cmd.dir;
        if (mod._svx_raycast(eng, cmd.origin[0], cmd.origin[1], cmd.origin[2], d[0], d[1], d[2], cmd.maxDist, scratch) === 1)
          hit = {
            pos: [f64(0), f64(1), f64(2)] as Vec3,
            normal: [f64(3), f64(4), f64(5)] as Vec3,
            distance: f64(6),
            material: f64(7),
          };
      }
      postToMain({ type: 'raycastResult', id: cmd.id, hit });
      break;
    }
    case 'collide': {
      let move: Vec3 = [...cmd.move];
      let onGround = false;
      if (mod && loaded) {
        mod._svx_collide(eng, cmd.min[0], cmd.min[1], cmd.min[2], cmd.max[0], cmd.max[1], cmd.max[2], cmd.move[0], cmd.move[1], cmd.move[2], scratch);
        move = [f64(0), f64(1), f64(2)];
        onGround = f64(3) > 0;
      }
      postToMain({ type: 'collideResult', id: cmd.id, move, onGround });
      break;
    }
    case 'setParams':
      applyParams(cmd.params);
      break;
  }
}

serveCommands(handle);

// Fixed-rate simulation clock, independent of rendering.
let next = performance.now();
function loop(): void {
  const now = performance.now();
  try {
    if (mod && loaded) {
      const t0 = performance.now();
      mod._svx_tick(eng);
      const t1 = performance.now();
      flushEvents();
      flushDebris();
      flushEnv();
      flushMeshes();
      sampleTimeline(t1 - t0, performance.now() - t1);
      if (now - lastStats >= STATS_MS) sendStats(now);
      if (config.persist && now - lastSave >= AUTOSAVE_MS) {
        lastSave = now;
        void saveDelta();
      }
    }
  } catch (err) {
    reportError(err);
  }
  next += TICK_MS;
  if (next < now) next = now + TICK_MS; // fell behind: skip, don't burst
  setTimeout(loop, Math.max(0, next - performance.now()));
}
loop();
