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
  Wood: 7,
  Stone: 8,
  Glass: 9,
  Rebar: 10,
  // smeared sections (docs/VEHICLES.md): a member's real section baked into a voxel material
  SteelSection: 11,
  Sheet: 12,
  CarFrame: 13,
  Engine: 14,
  Window: 15,
  Tyre: 16,
  Plastic: 17,
  Asphalt: 18,
  Paint: 19,
  Lamp: 20,
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
  'wood',
  'stone',
  'glass',
  'rebar',
  'steel section',
  'sheet metal',
  'car frame',
  'engine',
  'window',
  'tyre',
  'plastic',
  'asphalt',
  'road paint',
  'lamp',
];

/**
 * (front-end extension) The engine's paint layer (`svx::Paint`, game/include/svx/game/vehicles.hpp):
 * a voxel's colour over its material's. Painted faces carry texture `TEXTURE_PAINT_BASE + paint`
 * (palette slot PAINT_SLOT_BASE + paint): cars' paints, facades' plasters, the road's markings.
 */
export const Paint = {
  None: 0,
  White: 1,
  Silver: 2,
  Black: 3,
  Red: 4,
  Blue: 5,
  Green: 6,
  Yellow: 7,
  Orange: 8,
  TaxiYellow: 9,
  NavyBlue: 10,
  Maroon: 11,
  Beige: 12,
  Graphite: 13,
  Teal: 14,
  Trim: 15,
  TailRed: 16,
  Amber: 17,
  Plaster: 18,
  Cream: 19,
  Terracotta: 20,
  Sand: 21,
  Slate: 22,
  Ochre: 23,
  Mint: 24,
  LineWhite: 25,
  LineYellow: 26,
  Kerb: 27,
} as const;
export const PAINT_SLOT_BASE = 31;
/** The paints a car is sprayed in. */
export const CAR_PAINTS: readonly number[] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14];

export function materialName(id: MaterialId): string {
  return MATERIAL_NAMES[id] ?? `material#${id}`;
}

/**
 * What the per-vertex debug byte means (`setParams.debugView`). The engine writes the
 * byte according to the current view and re-sends affected meshes when it changes.
 * - None: the byte is ignored.
 * - Utilization: 0..255 maps to bond utilization 0..1 (heat map).
 * - Fragments: 0 = not part of a rubble fragment; 1..254 a pseudo-random value per fragment
 *   (the pre-scored pieces a structure breaks into), drawn as distinct colours.
 */
export const DebugView = {
  None: 0,
  Utilization: 1,
  Fragments: 2,
} as const;
export type DebugView = (typeof DebugView)[keyof typeof DebugView];

export const DEBUG_VIEW_NAMES: Readonly<Record<DebugView, string>> = {
  [DebugView.None]: 'none',
  [DebugView.Utilization]: 'utilization',
  [DebugView.Fragments]: 'fragments',
};

/** `drive`: the endless city with roads and traffic (docs/VEHICLES.md); engines without it load their city. */
export type ProceduralKind = 'drive' | 'city' | 'rooms' | 'tower' | 'yard' | 'angles' | 'machines';
export const PROCEDURAL_KINDS: readonly ProceduralKind[] = ['drive', 'rooms', 'city', 'tower', 'yard', 'angles', 'machines'];

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
/**
 * (front-end extension) Ids `TEXTURE_GLOW_BASE + m`: untextured, material m's colour, glowing
 * (burning or red-hot voxels; the engine's fire).
 */
export const TEXTURE_GLOW_BASE = 0xfe00;
/** (front-end extension) Water surfaces (the `water` meshes). */
export const TEXTURE_WATER = 0xfffe;
/** (front-end extension) Ids `TEXTURE_PAINT_BASE + p`: untextured, painted in paint p (`Paint`). */
export const TEXTURE_PAINT_BASE = TEXTURE_MATERIAL_BASE + PAINT_SLOT_BASE;

/** Doom scale: 1 texel per map unit, 32 map units per metre (plan §A4). */
export const DOOM_TEXELS_PER_METRE = 32;

// ---------------------------------------------------------------------------------------
// Main -> worker commands
// ---------------------------------------------------------------------------------------

/** Tunables (docs/V2_DESIGN.md §6). `setParams` always carries the complete set. */
export interface EngineParams {
  /** Divides every bond strength: above 1 weaker bonds, more collapse. */
  fragility: number;
  /** Scale on the contact loads of pieces (impulse over the impact duration): how hard landings hit. */
  impact: number;
  /** Dynamic increase factor: overshoot of sudden load changes (1 = quasi-static). */
  dif: number;
  debugView: DebugView;
  paused: boolean;
}

export const DEFAULT_PARAMS: Readonly<EngineParams> = {
  fragility: 1,
  impact: 1,
  dif: 1.5,
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
   * engines that support it (v1 engines; v2 has no displacement and sends no fields).
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

/** (front-end extension) Sets fire to what burns in the sphere (engines without fire ignore it). */
export interface IgniteCommand {
  type: 'ignite';
  pos: Vec3;
  radius: number;
}

/** (front-end extension) Fills the air in the sphere with water (engines without water ignore it). */
export interface PourCommand {
  type: 'pour';
  pos: Vec3;
  radius: number;
}

/** (front-end extension) Removes the water in the sphere. */
export interface DrainCommand {
  type: 'drain';
  pos: Vec3;
  radius: number;
}

/** (front-end extension) Brings the solids in the sphere to (at least) `celsius`. */
export interface HeatCommand {
  type: 'heat';
  pos: Vec3;
  radius: number;
  celsius: number;
}

/**
 * (front-end extension) A setting by name, recorded in replays: the environment's
 * ("fire.flame_reach", "smoke.wind_x", "water.loads", ...: `svx_env_param_*`) or the world's
 * ("rigid.gravity", "max_bodies", ...: `svx_tunable_*`). Unknown names are ignored.
 */
export interface SetEnvCommand {
  type: 'setEnv';
  name: string;
  value: number;
}
export interface SetTunableCommand {
  type: 'setTunable';
  name: string;
  value: number;
}

/** (front-end extension) Puts out and cools the sphere (a fire extinguisher). */
export interface ExtinguishCommand {
  type: 'extinguish';
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
  /**
   * (front-end extension) A shot's line: the characters' bodies are hit too (`RaycastHit.character`).
   * Engines without characters cast against the world alone.
   */
  characters?: boolean;
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

/**
 * (front-end extension) A bullet's hit: holes what its energy (J) gets through within the radius
 * (sheet metal and glass of cars, brittle materials; not armour). Engines without it carve.
 */
export interface ShootCommand {
  type: 'shoot';
  pos: Vec3;
  radius: number;
  energy: number;
}

/** Vehicle kinds (`svx::VehicleKind`). */
export const VEHICLE_KINDS = ['compact', 'sedan', 'van', 'pickup', 'truck'] as const;

/** (front-end extension, docs/VEHICLES.md) A vehicle dropped into the world (kind: VEHICLE_KINDS index; paint: `Paint`). */
export interface SpawnVehicleCommand {
  type: 'spawnVehicle';
  kind: number;
  paint: number;
  /** Where its frame's origin goes (on the ground under the middle between its axles). */
  pos: Vec3;
  /** Heading about z (radians; 0 = +x). */
  yaw: number;
}

/** (front-end extension) The player takes the wheel of a vehicle (its id from `vehicles`). */
export interface EnterVehicleCommand {
  type: 'enterVehicle';
  id: number;
}

/** (front-end extension) The player gets out (the vehicle keeps its handbrake on). */
export interface ExitVehicleCommand {
  type: 'exitVehicle';
}

/** (front-end extension) The player's vehicle's controls, until changed (send changes only). */
export interface DriveCommand {
  type: 'drive';
  /** -1..1: backwards reverses, or brakes while rolling forward. */
  throttle: number;
  /** 0..1 */
  brake: number;
  /** -1 (right) .. 1 (left). */
  steer: number;
  handbrake: boolean;
}

/** Traffic of a world with roads (the `drive` city). */
export interface TrafficSettings {
  enabled: boolean;
  /** Cars driving around the viewer, and parked at the kerbs. */
  cars: number;
  parked: number;
  /** Metres: spawned beyond nearRadius (out of sight) and within radius; removed beyond (untouched ones). */
  nearRadius: number;
  radius: number;
  /** x the roads' speed limits. */
  speedScale: number;
}

export const DEFAULT_TRAFFIC: Readonly<TrafficSettings> = { enabled: true, cars: 14, parked: 18, nearRadius: 45, radius: 110, speedScale: 1 };

/**
 * (front-end extension) The last pose message (`debris`, `vehicles`: their `seq`) the page has
 * handled, sent once per frame: an engine holds back further pose messages while the page is far
 * behind (a slow frame, a busy main thread), so it gets the latest poses rather than a backlog.
 */
export interface FrameAckCommand {
  type: 'frameAck';
  seq: number;
}

/** (front-end extension) Traffic settings (recorded in replays). */
export interface SetTrafficCommand {
  type: 'setTraffic';
  traffic: TrafficSettings;
}

/**
 * Pedestrians' bodies: deep (every physical body an articulation of the world: a car that hits one
 * hits a body), shallow (bodies of their own), hybrid (deep near the viewer and near moving
 * pieces, shallow further, on their plans alone far).
 */
export const PedestrianBodies = { Deep: 0, Shallow: 1, Hybrid: 2 } as const;
export type PedestrianBodies = (typeof PedestrianBodies)[keyof typeof PedestrianBodies];

export const PEDESTRIAN_BODY_NAMES: Readonly<Record<PedestrianBodies, string>> = {
  [PedestrianBodies.Deep]: 'deep',
  [PedestrianBodies.Shallow]: 'shallow',
  [PedestrianBodies.Hybrid]: 'hybrid',
};

/** Pedestrians of a world with walkways (the `drive` city): svx_anim characters on its sidewalks. */
export interface PedestrianSettings {
  enabled: boolean;
  /** People about the viewer. */
  count: number;
  /** Metres: spawned beyond nearRadius (out of sight) and within radius; the living removed beyond. */
  nearRadius: number;
  radius: number;
  bodies: PedestrianBodies;
  /** (hybrid) The most deep bodies: the nearest. */
  maxDeep: number;
}

/** The engine's defaults (`svx::PedestrianConfig`). */
export const DEFAULT_PEDESTRIANS: Readonly<PedestrianSettings> = { enabled: true, count: 24, nearRadius: 30, radius: 70, bodies: PedestrianBodies.Hybrid, maxDeep: 24 };

/** (front-end extension) Pedestrians settings (recorded in replays; kept across loads). */
export interface SetPedestriansCommand {
  type: 'setPedestrians';
  pedestrians: PedestrianSettings;
}

/**
 * (front-end extension) A round into a character (its id from a raycast with `characters`) where
 * the ray found its body, fired from the viewer; `energy` (J) and `radius` as `shoot`'s. Recorded
 * in replays; engines without characters ignore it.
 */
export interface WoundCharacterCommand {
  type: 'woundCharacter';
  id: number;
  pos: Vec3;
  radius: number;
  energy: number;
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
  | UseCommand
  | IgniteCommand
  | ExtinguishCommand
  | PourCommand
  | DrainCommand
  | HeatCommand
  | SetEnvCommand
  | SetTunableCommand
  | ShootCommand
  | SpawnVehicleCommand
  | EnterVehicleCommand
  | ExitVehicleCommand
  | DriveCommand
  | SetTrafficCommand
  | SetPedestriansCommand
  | WoundCharacterCommand
  | FrameAckCommand;

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
  /**
   * (front-end extension) An oriented grid's chunk (docs/GRIDS.md): its vertices and origin are
   * in the grid's lattice (metres), drawn where the grid's frame (`grids` messages) places it.
   */
  grid?: number;
}

/**
 * (front-end extension, v1 engines only: v2 sends none) Displacement of the chunks inside one
 * physics bubble: a voxel grid of
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
  /**
   * (front-end extension) An oriented grid's chunk, in its lattice (placed by its frame: `grids`
   * messages); absent: the world grid's.
   */
  grid?: number;
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

/**
 * A piece that lost support. The physics already removed it from the world; the front end
 * draws it. A rigid piece that later splits disappears from the `debris` poses and its parts
 * are announced as new detached events.
 */
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
   * (front-end extension) The engine simulates this piece as a rigid body and sends its pose in
   * `debris` messages until it is gone (pieces at rest persist as rubble; the engine fades the
   * oldest ones through the pose opacity when over its budget); `centroid` is then its centre
   * of mass (the pivot). Absent or false: the front end animates the piece itself (ballistic
   * fade).
   */
  rigid?: boolean;
  /**
   * (front-end extension) A new mesh for a piece already shown (its charring or glow changed):
   * it replaces the old one, at the piece's pose now, without the effects of a detachment.
   */
  remesh?: boolean;
  /**
   * (front-end extension) The rigid piece's voxels for the client's collision, at the pose the
   * mesh is in: u32 its shapes, then per shape its lattice (origin xyz, rotation xyzw, voxel size:
   * f64), its voxel box (lo xyz, dims xyz: i32) and a bit per cell of the box (little-endian;
   * engine/pieces.ts). Transferred.
   */
  occupancy?: ArrayBuffer;
}

/** Bond ruptures (decals and particles); `strength` is the utilization, about 1..2. */
export interface CrackEvent {
  kind: 'crack';
  pos: Vec3;
  normal: Vec3;
  strength: number;
  /**
   * (ext) Dust: material crushed or broken off too small to be a piece. Its voxel count, velocity
   * (m/s) and size (radius, m). Absent for a plain crack.
   */
  voxels?: number;
  velocity?: Vec3;
  radius?: number;
  /** (ext) Dust: what was crushed or shattered (a car's glass, a wall's brick; absent: unknown). */
  material?: MaterialId;
}

/** Blast or virtual debris impact (camera shake and particles). Energy in joules. */
export interface ImpactEvent {
  kind: 'impact';
  pos: Vec3;
  energy: number;
}

/** (front-end extension) A piece hit the water hard; `strength` is its momentum into it (kg m/s). */
export interface SplashEvent {
  kind: 'splash';
  pos: Vec3;
  strength: number;
}

export type EngineEvent = DetachedEvent | CrackEvent | ImpactEvent | SplashEvent;

export interface EventsMessage {
  type: 'events';
  list: EngineEvent[];
}

export interface RaycastHit {
  pos: Vec3;
  normal: Vec3;
  distance: number;
  /** (-1: a character) */
  material: MaterialId;
  /** (front-end extension; raycasts with `characters`) The character hit (absent: the world) and its bone. */
  character?: number;
  bone?: number;
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
  /**
   * (front-end extension) What the box stands on (onGround): the grid (0 the world grid) or the
   * piece (a lift's car, a turntable) and its velocity under the box (a rider adds it x dt).
   */
  ground?: number;
  groundPiece?: number;
  groundVelocity?: Vec3;
}

/**
 * Engine counters, in the order of `svx_stats` (core/include/svx/api/svx_api.h). Times are ms
 * of the last tick; "total" counters are cumulative since the world was loaded.
 */
export interface EngineStats {
  tickMs: number;
  structuralMs: number;
  eventMs: number;
  rigidMs: number;
  meshMs: number;
  voxels: number;
  chunks: number;
  memoryMB: number;
  /** Events emitted since the previous stats message. */
  events: number;
  ticks: number;
  /** Structures registered / being solved now, and the nodes of those being solved. */
  structures: number;
  structuresSolving: number;
  solvingNodes: number;
  /** Totals: structure extractions, converged solves, PCG iterations, broken bonds. */
  extractions: number;
  convergedSolves: number;
  pcgIterations: number;
  bondsBroken: number;
  /** Totals of detached voxels and pieces. */
  detachedVoxels: number;
  detachedPieces: number;
  /** Largest bond utilization of the last judged round. */
  maxUtilization: number;
  /** Rigid pieces alive (moving or resting rubble), awake ones, and their contacts. */
  pieces: number;
  awakePieces: number;
  contacts: number;
  /** Totals: piece stress checks, piece splits, impact load cases. */
  pieceChecks: number;
  pieceSplits: number;
  impactLoads: number;
  residentChunks: number;
  archivedChunks: number;
  streamMs: number;
  evictedChunks: number;
  movers: number;
  /** Bake (design pass) results. */
  designMaxUtilization: number;
  strengthenedVoxels: number;
  floatingVoxelsRemoved: number;
  bakeMs: number;
  /**
   * Memory the world holds (MB): all of it, and its fragment caches, structures and pieces; the
   * change archive of a streamed world (used / its fixed capacity) and what it forgot; pieces
   * culled by the budgets.
   */
  worldMemoryMB: number;
  fragmentCacheMB: number;
  structureMemoryMB: number;
  pieceMemoryMB: number;
  archiveMB: number;
  archiveCapacityMB: number;
  forgottenRegions: number;
  culledPieces: number;
  /** Fire: voxels with heat, voxels burning; the environment systems' step (ms). */
  fireHot: number;
  fireBurning: number;
  envMs: number;
  /** Smoke: cells with smoke, blocks (chunks) holding them. */
  smokeCells: number;
  smokeBlocks: number;
  /** Water: voxels moving, loads on structures, pieces in water. */
  waterActive: number;
  waterLoads: number;
  floating: number;
  /**
   * Characters (pedestrians), and how their bodies are: deep (articulations of the world),
   * shallow (their own), on their plans alone, at rest (the dead that stopped moving); their
   * step (ms).
   */
  characters: number;
  charactersDeep: number;
  charactersShallow: number;
  charactersPlanOnly: number;
  charactersAtRest: number;
  charactersMs: number;
  /** Engine-specific extras are shown generically by the HUD. */
  [extra: string]: number | string | boolean;
}

/** All-zero stats, for engines that fill only part of them. */
export function emptyEngineStats(): EngineStats {
  return {
    tickMs: 0,
    structuralMs: 0,
    eventMs: 0,
    rigidMs: 0,
    meshMs: 0,
    voxels: 0,
    chunks: 0,
    memoryMB: 0,
    events: 0,
    ticks: 0,
    structures: 0,
    structuresSolving: 0,
    solvingNodes: 0,
    extractions: 0,
    convergedSolves: 0,
    pcgIterations: 0,
    bondsBroken: 0,
    detachedVoxels: 0,
    detachedPieces: 0,
    maxUtilization: 0,
    pieces: 0,
    awakePieces: 0,
    contacts: 0,
    pieceChecks: 0,
    pieceSplits: 0,
    impactLoads: 0,
    residentChunks: 0,
    archivedChunks: 0,
    streamMs: 0,
    evictedChunks: 0,
    movers: 0,
    designMaxUtilization: 0,
    strengthenedVoxels: 0,
    floatingVoxelsRemoved: 0,
    bakeMs: 0,
    worldMemoryMB: 0,
    fragmentCacheMB: 0,
    structureMemoryMB: 0,
    pieceMemoryMB: 0,
    archiveMB: 0,
    archiveCapacityMB: 0,
    forgottenRegions: 0,
    culledPieces: 0,
    fireHot: 0,
    fireBurning: 0,
    envMs: 0,
    smokeCells: 0,
    smokeBlocks: 0,
    waterActive: 0,
    waterLoads: 0,
    floating: 0,
    characters: 0,
    charactersDeep: 0,
    charactersShallow: 0,
    charactersPlanOnly: 0,
    charactersAtRest: 0,
    charactersMs: 0,
  };
}

/**
 * Doubles per rigid piece in `DebrisMessage.poses` (the `svx_debris_data` layout): id of its
 * detached event, centre of mass xyz, rotation since detachment as a unit quaternion xyzw (the
 * pivot is the event centroid), opacity 0..1 (below 1 while the engine fades it out), velocity
 * of its centre xyz, angular velocity xyz (what rides on it is carried so).
 */
export const DEBRIS_STRIDE = 15;

/**
 * (front-end extension) Poses of all live rigid pieces, sent after an engine tick while any
 * exist (plus one empty set when the last one is gone). A rigid piece missing from the set has
 * been removed by the engine (split, faded, or fell out of the world). Packed, because there
 * can be thousands of pieces every tick.
 */
export interface DebrisMessage {
  type: 'debris';
  /** DEBRIS_STRIDE doubles per piece; transferred. */
  poses: Float64Array<ArrayBuffer>;
  /** (front-end extension) Pose message sequence number (see FrameAckCommand). */
  seq?: number;
}

/**
 * (front-end extension) Water surface meshes (the engine's water: translucent, drawn after the
 * opaque world) of chunks whose water changed, replacing earlier ones with the same key, and
 * the keys of chunks whose water is gone (to be applied first: a key is never in both).
 */
export interface WaterMessage {
  type: 'water';
  meshes: ChunkMesh[];
  removed: string[];
}

/** Floats per flame in `EnvMessage.flames`: world position xyz, temperature (degC). */
export const FLAME_STRIDE = 4;
/** Floats per smoke cell in `EnvMessage.smoke`: its centre xyz, density (about 1: thick). */
export const SMOKE_STRIDE = 4;
/** Edge of a smoke cell, in voxels. */
export const SMOKE_CELL_VOXELS = 4;

/**
 * (front-end extension) The environment's state for the renderer, sent about ten times a second
 * while anything burns or smokes, plus one empty set when all is clear (and on a load):
 * the flames (burning voxels, an even sample of at most a few thousand) and the smoke (the
 * densest cells of the smoke field).
 */
export interface EnvMessage {
  type: 'env';
  /** FLAME_STRIDE floats per flame; transferred. */
  flames: Float32Array<ArrayBuffer>;
  /** SMOKE_STRIDE floats per cell; transferred. */
  smoke: Float32Array<ArrayBuffer>;
}

/** Doubles per grid in `GridsMessage.frames`: id, origin xyz, rotation xyzw (lattice -> world), voxel size. */
export const GRID_STRIDE = 9;

/**
 * (front-end extension) The oriented grids' places (docs/GRIDS.md): the grids that came or were
 * placed anew, and the grids gone. Their chunk meshes and occupancy are in their lattices.
 */
export interface GridsMessage {
  type: 'grids';
  /** GRID_STRIDE doubles per grid; transferred. */
  frames: Float64Array<ArrayBuffer>;
  removed: number[];
}

/**
 * Doubles per joint in `JointsMessage.joints`: id, type (0 ball, 1 hinge, 2 slider, 3 fixed,
 * 4 distance: a rope or a rod), end a xyz, end b xyz (world).
 */
export const JOINT_STRIDE = 8;

/** (front-end extension) The joints (to draw ropes), sent after a tick while any exist (and once empty). */
export interface JointsMessage {
  type: 'joints';
  /** JOINT_STRIDE doubles per joint; transferred. */
  joints: Float64Array<ArrayBuffer>;
}

/**
 * Doubles per vehicle in `VehiclesMessage.vehicles` (the `svx_vehicles_data` layout, extended): id,
 * chassis (its piece: the id of its detached event and debris poses; 0: none yet), kind, paint,
 * centre of mass xyz, frame rotation xyzw (x forward, y left, z up), velocity xyz, speed (m/s
 * forward), engine rpm, gear (-1 reverse, 0 neutral, 1..), controls (throttle, brake, steer,
 * handbrake), flags (VehicleFlag), the driver's seat xyz, half extent xyz (its box about its
 * frame's origin, z up from the ground), wheels on, damage 0..1, frame origin xyz, redline, parts
 * still on (doors, bonnet, bumpers, ...: pieces of their own, on joints to the chassis), parts built.
 */
export const VEHICLE_STRIDE = 36;
export const VehicleFlag = { Player: 1, Npc: 2, Parked: 4, Wreck: 8 } as const;

/**
 * Doubles per wheel in `VehiclesMessage.wheels`: vehicle id, wheel id, centre xyz, rotation xyzw
 * (x the way it rolls, y its axle; turned and spun), radius, width, on the ground (1/0), slip
 * (m/s: skids, smoke), the material under it (-1 none), suspension compression (m).
 */
export const WHEEL_STRIDE = 15;

/**
 * (front-end extension) The vehicles and their wheels, sent after every tick while any exist (and
 * once empty after the last is gone). A vehicle's body is its chassis piece (a detached event
 * and debris poses, like any piece); its wheels are drawn from this.
 */
export interface VehiclesMessage {
  type: 'vehicles';
  /** VEHICLE_STRIDE doubles per vehicle; transferred. */
  vehicles: Float64Array<ArrayBuffer>;
  /** WHEEL_STRIDE doubles per wheel; transferred. */
  wheels: Float64Array<ArrayBuffer>;
  /** The player's vehicle (0: on foot). */
  player: number;
  /** Pose message sequence number (see FrameAckCommand). */
  seq?: number;
}

/**
 * Bytes per character vertex, the svx_anim character vertex format
 * (anim/include/svx/anim/voxel/mesh.hpp): float32x3 position in rest model space (m); snorm8x4
 * normal xyz and ambient occlusion (-1..1 = 0..1); uint32 bone | palette slot << 8 | shade << 12
 * (128 = 1.0). Indices are uint32, CCW seen from outside.
 */
export const CHAR_VERTEX_STRIDE = 20;
/**
 * Bones of a character (the humanoid rig, anim/src/rig.cpp; bone 0 the root: between the feet of a
 * posed body, with the pelvis of a physical one).
 */
export const CHARACTER_BONES = 23;
/** Floats per character in `CharactersMessage.skin`. */
export const CHARACTER_SKIN_FLOATS = CHARACTER_BONES * 16;
/** Colours of a character palette (the slots of the vertices). */
export const CHARACTER_PALETTE_SLOTS = 16;

/** A character mesh: its vertices in rest model space, drawn skinned. */
export interface CharacterMesh {
  id: number;
  /** vertexCount * CHAR_VERTEX_STRIDE bytes. */
  vertices: ArrayBuffer;
  vertexCount: number;
  /** indexCount * 4 bytes. */
  indices: ArrayBuffer;
  indexCount: number;
}

export interface CharacterPalette {
  id: number;
  /** CHARACTER_PALETTE_SLOTS linear rgb colours (48 floats). */
  rgb: Float32Array<ArrayBuffer>;
}

/**
 * (front-end extension; svx_api.h: svx_poll_character_meshes) Character meshes and palettes new
 * since the last one, and the meshes no character draws any more (apply the removed first), sent
 * before the `characters` message that draws them. A damaged character gets a mesh of its own. An
 * engine sends a palette once, even across loads: keep them.
 */
export interface CharacterMeshesMessage {
  type: 'characterMeshes';
  /** Buffers transferred. */
  meshes: CharacterMesh[];
  removed: number[];
  /** Transferred. */
  palettes: CharacterPalette[];
}

/**
 * Doubles per character in `CharactersMessage.characters` (the `svx_characters_data` layout): id,
 * mesh, palette, flags (CharacterFlag), bounding sphere centre xyz and radius (world), hit flash
 * 0..1 (a tint), its prop's mesh (0: none), health 0..1 (0 from engines without it), 1 reserved.
 */
export const CHARACTER_STRIDE = 12;
/**
 * Alive; deep (its body an articulation of the world); physical (a body, not the plan alone);
 * asleep (a body at rest); down; a gib (a piece of one - a limb shot off, what a blast tore apart:
 * its mesh's vertices are all bone 0's, its first matrix the one that counts).
 */
export const CharacterFlag = { Alive: 1, Deep: 2, Physical: 4, Asleep: 8, Down: 16, Gib: 32 } as const;

/**
 * (front-end extension) The characters (the drive city's people), sent after every tick while any
 * exist (and once empty after the last is gone): a pose message like `debris` and `vehicles` (its
 * `seq`: FrameAckCommand). Rigid skinning: a vertex of a character's mesh is drawn at
 * skin[its bone] x its position, so voxels stay cubes.
 */
export interface CharactersMessage {
  type: 'characters';
  /** CHARACTER_STRIDE doubles per character; transferred. */
  characters: Float64Array<ArrayBuffer>;
  /** CHARACTER_SKIN_FLOATS per character: its bones' column-major 4x4 matrices, rest model space -> world; transferred. */
  skin: Float32Array<ArrayBuffer>;
  /** 16 floats per character, while any holds a prop: its prop's matrix (the prop's mesh drawn as bone 0); transferred. */
  props?: Float32Array<ArrayBuffer>;
  /** Pose message sequence number (see FrameAckCommand). */
  seq?: number;
}

/** Floats per drop in `BloodMessage.drops`: position xyz, radius (m), linear rgb. */
export const BLOOD_DROP_STRIDE = 7;
/** Floats per stain in `BloodMessage.stains`: position xyz, the surface's normal xyz, radius (m), age (s). */
export const BLOOD_STAIN_STRIDE = 8;

/**
 * (front-end extension) The people's blood: drops in flight and the stains they left on the
 * world, sent after every tick while there are any (and once empty after), held back with the pose
 * messages (each carries all of it).
 */
export interface BloodMessage {
  type: 'blood';
  /** BLOOD_DROP_STRIDE floats per drop; transferred. */
  drops: Float32Array<ArrayBuffer>;
  /** BLOOD_STAIN_STRIDE floats per stain; transferred. */
  stains: Float32Array<ArrayBuffer>;
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

/**
 * Per-tick timeline sample: ms per part of the engine tick (structure solves, rigid pieces,
 * event processing, streaming, the environment systems, the rest), the worker's flush after it
 * (meshes, events, poses), then the awake pieces.
 */
export const TIMELINE_FIELDS = ['structural', 'rigid', 'events', 'stream', 'env', 'other', 'flush', 'awake'] as const;
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
  | OccupancyMessage
  | EnvMessage
  | WaterMessage
  | GridsMessage
  | JointsMessage
  | VehiclesMessage
  | CharacterMeshesMessage
  | CharactersMessage
  | BloodMessage;

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
  'env',
  'water',
  'grids',
  'joints',
  'vehicles',
  'characterMeshes',
  'characters',
  'blood',
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
  'ignite',
  'extinguish',
  'pour',
  'drain',
  'heat',
  'setEnv',
  'setTunable',
  'shoot',
  'spawnVehicle',
  'enterVehicle',
  'exitVehicle',
  'drive',
  'setTraffic',
  'setPedestrians',
  'woundCharacter',
  'frameAck',
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
    case 'debris':
      pushUnique(out, seen, msg.poses.buffer);
      break;
    case 'grids':
      pushUnique(out, seen, msg.frames.buffer);
      break;
    case 'joints':
      pushUnique(out, seen, msg.joints.buffer);
      break;
    case 'vehicles':
      pushUnique(out, seen, msg.vehicles.buffer);
      pushUnique(out, seen, msg.wheels.buffer);
      break;
    case 'characterMeshes':
      for (const m of msg.meshes) {
        pushUnique(out, seen, m.vertices);
        pushUnique(out, seen, m.indices);
      }
      for (const p of msg.palettes) pushUnique(out, seen, p.rgb.buffer);
      break;
    case 'characters':
      pushUnique(out, seen, msg.characters.buffer);
      pushUnique(out, seen, msg.skin.buffer);
      if (msg.props) pushUnique(out, seen, msg.props.buffer);
      break;
    case 'blood':
      pushUnique(out, seen, msg.drops.buffer);
      pushUnique(out, seen, msg.stains.buffer);
      break;
    case 'env':
      pushUnique(out, seen, msg.flames.buffer);
      pushUnique(out, seen, msg.smoke.buffer);
      break;
    case 'water':
      for (const m of msg.meshes) {
        pushUnique(out, seen, m.vertices);
        pushUnique(out, seen, m.indices);
      }
      break;
    case 'events':
      for (const e of msg.list) {
        if (e.kind === 'detached') {
          pushUnique(out, seen, e.mesh.vertices);
          pushUnique(out, seen, e.mesh.indices);
          if (e.occupancy) pushUnique(out, seen, e.occupancy);
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
