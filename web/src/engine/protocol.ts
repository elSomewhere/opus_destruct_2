/**
 * structvox engine <-> front-end message contract (docs/API.md, v1) as TypeScript types.
 *
 * This module is shared by the main thread and by every engine worker implementation
 * (mock-worker.ts today, wasm-worker.ts later). It must stay free of DOM-only and
 * worker-only APIs.
 *
 * Conventions:
 * - Every message is a plain object with a string `type` discriminator. (`kind` is taken:
 *   `loadProcedural.kind` and the event union use it.)
 * - Coordinates are metres, z is up, right-handed.
 * - Bulk data (meshes, textures, WAD files) travels as transferred ArrayBuffers; use
 *   `workerMessageTransferables` / `commandTransferables` to build the transfer list.
 *
 * Extensions beyond docs/API.md v1 are marked "(front-end extension)". They are optional
 * for engines: a worker that never sends them still works with this front end.
 */

export type Vec3 = [number, number, number];

export interface Aabb {
  min: Vec3;
  max: Vec3;
}

// ---------------------------------------------------------------------------------------
// Shared enums
// ---------------------------------------------------------------------------------------

/** Material ids, mirroring `svx::MaterialId` in core/include/svx/mech/material.hpp. */
export const Material = {
  Rc: 0,
  Concrete: 1,
  Steel: 2,
  Masonry: 3,
  Soil: 4,
  Rock: 5,
  Bedrock: 6,
} as const;
export type MaterialId = number;

export const MATERIAL_NAMES: readonly string[] = [
  'rc',
  'concrete',
  'steel',
  'masonry',
  'soil',
  'rock',
  'bedrock',
];

export function materialName(id: MaterialId): string {
  return MATERIAL_NAMES[id] ?? `material#${id}`;
}

/**
 * What the per-vertex debug byte means (`setParams.debugView`). The engine writes the
 * byte according to the current view and re-sends affected meshes when it changes.
 * - None: the byte is ignored.
 * - Utilization: 0..255 maps to bond utilization 0..1 (heat map).
 * - BubbleLevel: 0 = not in a bubble; 1 + L for bubble level L (fine = level 0).
 */
export const DebugView = {
  None: 0,
  Utilization: 1,
  BubbleLevel: 2,
} as const;
export type DebugView = (typeof DebugView)[keyof typeof DebugView];

export const DEBUG_VIEW_NAMES: Readonly<Record<DebugView, string>> = {
  [DebugView.None]: 'none',
  [DebugView.Utilization]: 'utilization',
  [DebugView.BubbleLevel]: 'bubble level',
};

export type ProceduralKind = 'city' | 'rooms' | 'tower';
export const PROCEDURAL_KINDS: readonly ProceduralKind[] = ['rooms', 'city', 'tower'];

// ---------------------------------------------------------------------------------------
// Chunk mesh vertex format (28 bytes, interleaved, little-endian)
// ---------------------------------------------------------------------------------------

export const VERTEX_STRIDE = 28;
/** float32x3: world position, metres. */
export const VERTEX_OFFSET_POSITION = 0;
/** snorm8x4: normal xyz, w = ambient occlusion (-1..1 maps to 0..1). */
export const VERTEX_OFFSET_NORMAL_AO = 12;
/** float32x2: texture coordinates in texels (the shader wraps by texture size). */
export const VERTEX_OFFSET_UV = 16;
/** uint16: texture id; see TEXTURE_NONE and TEXTURE_MATERIAL_BASE. */
export const VERTEX_OFFSET_TEXTURE = 24;
/** uint8: light level 0..255 (Doom sector light). */
export const VERTEX_OFFSET_LIGHT = 26;
/** uint8: debug value 0..255 (see DebugView). */
export const VERTEX_OFFSET_DEBUG = 27;

/** Untextured: flat default material colour. */
export const TEXTURE_NONE = 0xffff;
/**
 * (front-end extension) Ids `TEXTURE_MATERIAL_BASE + m` (m < 255) are untextured and
 * tinted with material m's palette colour. An engine that only ever writes TEXTURE_NONE
 * gets one neutral colour for all untextured faces. Real texture ids must stay below this.
 */
export const TEXTURE_MATERIAL_BASE = 0xff00;

/** Doom scale: 1 texel per map unit, 32 map units per metre (plan §A4). */
export const DOOM_TEXELS_PER_METRE = 32;

// ---------------------------------------------------------------------------------------
// Main -> worker commands
// ---------------------------------------------------------------------------------------

/** Tunables (plan §B11). `setParams` always carries the complete set. */
export interface EngineParams {
  /** Physical compliance S_p (plan §B3). */
  compliance: number;
  /** Render amplification A on displacements (plan §B3). */
  amplification: number;
  /** Fragility scale F: how easily things snap. */
  fragility: number;
  /** Damping (fraction of critical, 0..1). */
  damping: number;
  debugView: DebugView;
  paused: boolean;
}

/**
 * Calibrated against the prototype's XPBD feel captures (docs/phase5/FEEL_SPEC.md): visible
 * event response S_p x A ~ 9 x the stiff response, time to quiet best at S_p 9 and light damping.
 */
export const DEFAULT_PARAMS: Readonly<EngineParams> = {
  compliance: 9,
  amplification: 1,
  fragility: 1,
  damping: 0.05,
  debugView: DebugView.None,
  paused: false,
};

export interface InitConfig {
  /** Voxel pitch h in metres (plan default 0.125). */
  voxelSize: number;
  /** Worker thread count the engine may use (WASM pthreads). */
  threads: number;
  /** Fixed WASM heap budget. */
  memoryMB: number;
  params: EngineParams;
  /**
   * (front-end extension) Persist gameplay changes per world (OPFS deltas, plan §B8): the
   * engine autosaves while playing and restores them when the same world is loaded again.
   */
  persist?: boolean;
  /**
   * (front-end extension) Move chunks inside physics bubbles on the GPU with displacement
   * fields (`ChunkMeshesMessage.fields`) instead of re-meshing them every tick. Default on for
   * engines that support it.
   */
  gpuDisplacement?: boolean;
}

export interface InitCommand {
  type: 'init';
  config: InitConfig;
}

export interface LoadProceduralCommand {
  type: 'loadProcedural';
  seed: number;
  kind: ProceduralKind;
}

export interface WadOptions {
  /** How void outside the playable space is filled (plan §B10). */
  mode: 'rock' | 'air';
  /** Thickness of the structural wall shell, in voxels. */
  shellVoxels: number;
  /** Run the bake (baselines, auto-strengthening) before play. */
  bake: boolean;
}

export interface LoadWadCommand {
  type: 'loadWad';
  /** Whole WAD file; transferred. */
  buffer: ArrayBuffer;
  /** Map lump name, e.g. "MAP01" or "E1M1". */
  map: string;
  options: WadOptions;
}

/** Streaming / LOD focus; sent every frame or two. */
export interface ViewerCommand {
  type: 'viewer';
  pos: Vec3;
  dir: Vec3;
}

export interface BlastCommand {
  type: 'blast';
  pos: Vec3;
  radius: number;
  /** Joules. The front end's rocket uses ROCKET_ENERGY_J (game/weapons.ts). */
  energy: number;
}

export interface CarveCommand {
  type: 'carve';
  pos: Vec3;
  radius: number;
}

export interface RaycastCommand {
  type: 'raycast';
  id: number;
  origin: Vec3;
  /** Unit direction. */
  dir: Vec3;
  maxDist: number;
}

/** Player AABB sweep: returns the corrected move. */
export interface CollideCommand {
  type: 'collide';
  id: number;
  min: Vec3;
  max: Vec3;
  move: Vec3;
}

/** (front-end extension) The player's "use" key: doors, lifts and switches in reach. */
export interface UseCommand {
  type: 'use';
  pos: Vec3;
  /** Unit view direction. */
  dir: Vec3;
}

export interface SetParamsCommand {
  type: 'setParams';
  params: EngineParams;
}

export type EngineCommand =
  | InitCommand
  | LoadProceduralCommand
  | LoadWadCommand
  | ViewerCommand
  | BlastCommand
  | CarveCommand
  | RaycastCommand
  | CollideCommand
  | SetParamsCommand
  | UseCommand;

export type EngineCommandType = EngineCommand['type'];

// ---------------------------------------------------------------------------------------
// Worker -> main messages
// ---------------------------------------------------------------------------------------

export interface WorldInfo {
  bounds: Aabb;
  voxelCount: number;
  /** `pos` is the player's feet position (standing on the floor); `dir` the view direction. */
  spawn: { pos: Vec3; dir: Vec3 };
  /** True when a `textures` message was (or will be) sent for this world. */
  textures: boolean;
}

export interface ReadyMessage {
  type: 'ready';
  info: WorldInfo;
}

export interface TextureInfo {
  /** Texture id as used by vertices (0-based, dense; below TEXTURE_MATERIAL_BASE). */
  id: number;
  name: string;
  width: number;
  height: number;
  /** width*height*4 bytes, RGBA8, row 0 = top of the texture; transferred. */
  rgba: ArrayBuffer;
}

export interface TexturesMessage {
  type: 'textures';
  list: TextureInfo[];
}

/** Index/vertex buffers of one mesh. Buffers are sized exactly (no slack). */
export interface MeshData {
  /** vertexCount * VERTEX_STRIDE bytes. */
  vertices: ArrayBuffer;
  vertexCount: number;
  /** indexCount * 4 bytes, uint32, CCW triangles seen from outside. */
  indices: ArrayBuffer;
  indexCount: number;
}

export interface ChunkMesh extends MeshData {
  /** Opaque, stable chunk key. A new mesh with the same key replaces the old one. */
  key: string;
  /** World position of the chunk's minimum corner. Vertex positions are world, not local. */
  origin: Vec3;
}

/**
 * (front-end extension) Displacement of the chunks inside one physics bubble: a voxel grid of
 * rgba16float texels (x fastest, then y, then z) holding (w dx, w dy, w dz, w), with w = 1 for
 * solid voxels and 0 for air. The renderer samples it trilinearly at each chunk vertex (the
 * texel centres are voxel centres) and adds xyz / w, which is the displacement averaged over the
 * solid voxels around the vertex. Displacements are metres, already amplified.
 */
export interface DisplacementField {
  /** Bubble id. */
  id: number;
  /** World position of the centre of texel (0, 0, 0). */
  origin: Vec3;
  /** Texels along x, y, z. */
  size: [number, number, number];
  /** Texel pitch (the voxel size). */
  voxelSize: number;
  /** Largest displacement magnitude in the field (for culling bounds). */
  maxDisp: number;
  /** size[0] * size[1] * size[2] * 8 bytes; transferred. */
  data: ArrayBuffer;
}

export interface ChunkMeshesMessage {
  type: 'chunkMeshes';
  meshes: ChunkMesh[];
  /**
   * (front-end extension) When present, the complete set of displacement fields, replacing the
   * previous set; applied together with `meshes` (no pop when a bubble ends and its chunks get
   * their final static meshes). Absent: the fields are unchanged.
   */
  fields?: DisplacementField[];
}

/**
 * (front-end extension) Solid occupancy of one 32^3 chunk, for client-side collision (the
 * player's sweeps never wait for a busy worker). `state`: 0 all air, 1 all solid, 2 mixed with
 * `bits` (4096 bytes: bit v of byte v >> 3 is voxel v = (x * 32 + y) * 32 + z, local).
 */
export interface ChunkOccupancy {
  /** Chunk coordinates (voxel = 32 * chunk + local). */
  chunk: [number, number, number];
  state: 0 | 1 | 2;
  bits?: ArrayBuffer;
}

/**
 * (front-end extension) Occupancy of every chunk whose voxels changed (sent after the mesh
 * flush of the same tick). A front end that receives it may resolve player collision locally
 * with the same rules as `collide`.
 */
export interface OccupancyMessage {
  type: 'occupancy';
  voxelSize: number;
  chunks: ChunkOccupancy[];
}

export interface ChunkRemovedMessage {
  type: 'chunkRemoved';
  keys: string[];
}

/** A piece that lost support. The physics already removed it; the front end animates it. */
export interface DetachedEvent {
  kind: 'detached';
  id: number;
  voxels: number;
  centroid: Vec3;
  /** m/s. */
  velocity: Vec3;
  /** rad/s, world frame, about the centroid. */
  angular: Vec3;
  /** World-space mesh at the moment of detachment. */
  mesh: MeshData;
  /**
   * (front-end extension) The engine simulates this piece as rigid debris and sends its pose
   * in `debris` messages until it is gone; `centroid` is then its centre of mass (the pivot).
   * Absent or false: the front end animates the piece itself (ballistic fade).
   */
  rigid?: boolean;
}

/** Bond ruptures (decals and particles). */
export interface CrackEvent {
  kind: 'crack';
  pos: Vec3;
  normal: Vec3;
  strength: number;
}

/** Blast or virtual debris impact (camera shake and particles). Energy in joules. */
export interface ImpactEvent {
  kind: 'impact';
  pos: Vec3;
  energy: number;
}

/** Debug: an active structural bubble. */
export interface BubbleEvent {
  kind: 'bubble';
  id: number;
  center: Vec3;
  radius: number;
  level: number;
}

export type EngineEvent = DetachedEvent | CrackEvent | ImpactEvent | BubbleEvent;

export interface EventsMessage {
  type: 'events';
  list: EngineEvent[];
}

export interface RaycastHit {
  pos: Vec3;
  normal: Vec3;
  distance: number;
  material: MaterialId;
}

export interface RaycastResultMessage {
  type: 'raycastResult';
  id: number;
  hit: RaycastHit | null;
}

export interface CollideResultMessage {
  type: 'collideResult';
  id: number;
  move: Vec3;
  onGround: boolean;
}

export interface EngineStats {
  tickMs: number;
  structuralMs: number;
  activeBubbles: number;
  activeNodes: number;
  voxels: number;
  chunks: number;
  memoryMB: number;
  /** Events emitted since the previous stats message. */
  events: number;
  /** Engine-specific extras are shown generically by the HUD. */
  [extra: string]: number | string | boolean;
}

/** (front-end extension) Pose of one rigid debris piece (see DetachedEvent.rigid). */
export interface DebrisPose {
  /** Id of the piece's detached event. */
  id: number;
  /** Current centre of mass. */
  pos: Vec3;
  /** Rotation since detachment, unit quaternion [x, y, z, w]; the pivot is the event centroid. */
  rot: [number, number, number, number];
  /** 1 while visible, falling to 0 as the piece fades out. */
  opacity: number;
}

/**
 * (front-end extension) Poses of all live rigid debris pieces, sent after every engine tick
 * while any exist (plus one empty list when the last one is gone). A rigid piece missing from
 * the list has been removed by the engine.
 */
export interface DebrisMessage {
  type: 'debris';
  poses: DebrisPose[];
}

export interface StatsMessage {
  type: 'stats';
  stats: EngineStats;
  /**
   * (front-end extension) The ticks since the previous stats message, TIMELINE_STRIDE values
   * each (see TIMELINE_FIELDS): the job and budget timeline of the debug overlay.
   */
  timeline?: Float32Array;
}

/** Per-tick timeline sample: ms per part of the worker's tick, then the active bubble nodes. */
export const TIMELINE_FIELDS = ['structural', 'events', 'stream', 'debris', 'verify', 'mesh', 'other', 'nodes'] as const;
export const TIMELINE_STRIDE = TIMELINE_FIELDS.length;

/** (front-end extension) Something failed. `fatal` means the engine cannot continue. */
export interface ErrorMessage {
  type: 'error';
  message: string;
  fatal: boolean;
  /** The command type that failed, if any. */
  command?: EngineCommandType;
}

/** (front-end extension) Load progress for the loading screen. */
export interface ProgressMessage {
  type: 'progress';
  stage: string;
  done: number;
  total: number;
}

export type WorkerMessage =
  | ReadyMessage
  | TexturesMessage
  | ChunkMeshesMessage
  | ChunkRemovedMessage
  | EventsMessage
  | RaycastResultMessage
  | CollideResultMessage
  | StatsMessage
  | ErrorMessage
  | ProgressMessage
  | DebrisMessage
  | OccupancyMessage;

export type WorkerMessageType = WorkerMessage['type'];
export type WorkerMessageOf<T extends WorkerMessageType> = Extract<WorkerMessage, { type: T }>;
export type EngineCommandOf<T extends EngineCommandType> = Extract<EngineCommand, { type: T }>;

// ---------------------------------------------------------------------------------------
// Runtime helpers
// ---------------------------------------------------------------------------------------

const WORKER_MESSAGE_TYPES: ReadonlySet<string> = new Set<WorkerMessageType>([
  'ready',
  'textures',
  'chunkMeshes',
  'chunkRemoved',
  'events',
  'raycastResult',
  'collideResult',
  'stats',
  'error',
  'progress',
  'debris',
  'occupancy',
]);

const ENGINE_COMMAND_TYPES: ReadonlySet<string> = new Set<EngineCommandType>([
  'init',
  'loadProcedural',
  'loadWad',
  'viewer',
  'blast',
  'carve',
  'raycast',
  'collide',
  'setParams',
  'use',
]);

function typeField(data: unknown): string | undefined {
  if (typeof data !== 'object' || data === null) return undefined;
  const t: unknown = (data as { type?: unknown }).type;
  return typeof t === 'string' ? t : undefined;
}

/** Shallow guard: checks the discriminator only (payloads come from trusted code). */
export function isWorkerMessage(data: unknown): data is WorkerMessage {
  const t = typeField(data);
  return t !== undefined && WORKER_MESSAGE_TYPES.has(t);
}

/** Shallow guard: checks the discriminator only. */
export function isEngineCommand(data: unknown): data is EngineCommand {
  const t = typeField(data);
  return t !== undefined && ENGINE_COMMAND_TYPES.has(t);
}

function pushUnique(out: ArrayBuffer[], seen: Set<ArrayBuffer>, buf: ArrayBuffer): void {
  // A duplicate entry in a transfer list throws DataCloneError, and engines may pack
  // several arrays into one buffer.
  if (!seen.has(buf)) {
    seen.add(buf);
    out.push(buf);
  }
}

/** ArrayBuffers a worker should transfer (not copy) when posting `msg`. */
export function workerMessageTransferables(msg: WorkerMessage): ArrayBuffer[] {
  const out: ArrayBuffer[] = [];
  const seen = new Set<ArrayBuffer>();
  switch (msg.type) {
    case 'chunkMeshes':
      for (const m of msg.meshes) {
        pushUnique(out, seen, m.vertices);
        pushUnique(out, seen, m.indices);
      }
      for (const f of msg.fields ?? []) pushUnique(out, seen, f.data);
      break;
    case 'textures':
      for (const t of msg.list) pushUnique(out, seen, t.rgba);
      break;
    case 'occupancy':
      for (const c of msg.chunks) if (c.bits) pushUnique(out, seen, c.bits);
      break;
    case 'events':
      for (const e of msg.list) {
        if (e.kind === 'detached') {
          pushUnique(out, seen, e.mesh.vertices);
          pushUnique(out, seen, e.mesh.indices);
        }
      }
      break;
    default:
      break;
  }
  return out;
}

/** ArrayBuffers the main thread transfers when sending `cmd`. */
export function commandTransferables(cmd: EngineCommand): ArrayBuffer[] {
  return cmd.type === 'loadWad' ? [cmd.buffer] : [];
}
