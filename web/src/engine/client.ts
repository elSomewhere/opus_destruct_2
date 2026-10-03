/**
 * EngineClient: owns the engine worker, sends typed commands and dispatches replies.
 *
 * - Fire-and-forget commands (carve, blast, viewer, ...) are plain methods.
 * - `raycast` and `collide` return promises matched to replies by request id.
 * - Every other worker message is delivered to subscribers registered with `on`.
 * - A worker that fails to load or crashes surfaces as a fatal `error` message, and all
 *   pending requests are rejected.
 */
import type {
  EngineCommand,
  EngineParams,
  InitConfig,
  PedestrianSettings,
  ProceduralKind,
  RaycastHit,
  TrafficSettings,
  Vec3,
  WadOptions,
  WorkerMessage,
  WorkerMessageOf,
  WorkerMessageType,
} from './protocol.ts';
import { commandTransferables, isWorkerMessage } from './protocol.ts';
import type { EngineKind } from './select.ts';

export interface CollideResult {
  move: Vec3;
  onGround: boolean;
  /** (onGround) the grid it stands on (0 the world grid) or the piece, and its velocity under the box. */
  ground?: number;
  groundPiece?: number;
  groundVelocity?: Vec3;
}

interface Pending<T> {
  resolve: (value: T) => void;
  reject: (reason: Error) => void;
}

/** Answers to the client's own requests, and its failures: these belong to no world. */
const CROSSES_LOADS: ReadonlySet<WorkerMessageType> = new Set<WorkerMessageType>(['loading', 'error', 'raycastResult', 'collideResult']);

type AnyHandler = (msg: WorkerMessage) => void;

export class EngineClient {
  readonly kind: EngineKind;
  private readonly worker: Worker;
  private readonly handlers = new Map<WorkerMessageType, Set<AnyHandler>>();
  private readonly raycasts = new Map<number, Pending<RaycastHit | null>>();
  private readonly collides = new Map<number, Pending<CollideResult>>();
  private nextId = 1;
  private dead: Error | null = null;
  /** Loads asked for, and the last the engine reported starting (`loading`). */
  private loadsSent = 0;
  private loadsBegun = 0;

  constructor(worker: Worker, kind: EngineKind) {
    this.worker = worker;
    this.kind = kind;
    worker.addEventListener('message', (ev: MessageEvent<unknown>) => this.receive(ev.data));
    worker.addEventListener('error', (ev: ErrorEvent) => {
      // Fires when the worker script fails to load/evaluate or throws uncaught.
      ev.preventDefault();
      const detail = ev.message ? `: ${ev.message}` : '';
      this.fail(new Error(`The ${kind} engine worker failed${detail}. Is it built? See web/README.md.`));
    });
    worker.addEventListener('messageerror', () => {
      this.emit({ type: 'error', fatal: false, message: 'A message from the engine could not be deserialized.' });
    });
  }

  /** Subscribes to a worker message type; returns an unsubscribe function. */
  on<T extends WorkerMessageType>(type: T, handler: (msg: WorkerMessageOf<T>) => void): () => void {
    let set = this.handlers.get(type);
    if (!set) {
      set = new Set();
      this.handlers.set(type, set);
    }
    // Stored type-erased; `emit` only calls it with messages of this `type`.
    const h = handler as AnyHandler;
    set.add(h);
    return () => set.delete(h);
  }

  get alive(): boolean {
    return this.dead === null;
  }

  /** Requests awaiting a reply (raycast + collide). */
  get pendingRequests(): number {
    return this.raycasts.size + this.collides.size;
  }

  send(cmd: EngineCommand): void {
    if (this.dead) return;
    this.worker.postMessage(cmd, commandTransferables(cmd));
  }

  init(config: InitConfig): void {
    this.send({ type: 'init', config });
  }

  loadProcedural(kind: ProceduralKind, seed: number): void {
    this.loadsSent++;
    this.send({ type: 'loadProcedural', kind, seed });
  }

  /** A preset by id (docs/PRESETS.md; `seed` 0: the preset's own). */
  loadPreset(id: string, seed: number): void {
    this.loadsSent++;
    this.send({ type: 'loadPreset', id, seed });
  }

  /** Transfers `buffer`: it is detached (unusable) afterwards. */
  loadWad(buffer: ArrayBuffer, map: string, options: WadOptions): void {
    this.loadsSent++;
    this.send({ type: 'loadWad', buffer, map, options });
  }

  viewer(pos: Vec3, dir: Vec3): void {
    this.send({ type: 'viewer', pos, dir });
  }

  carve(pos: Vec3, radius: number): void {
    this.send({ type: 'carve', pos, radius });
  }

  /** Doors, lifts and switches (engines without movers ignore it). */
  use(pos: Vec3, dir: Vec3): void {
    this.send({ type: 'use', pos, dir });
  }

  blast(pos: Vec3, radius: number, energy: number): void {
    this.send({ type: 'blast', pos, radius, energy });
  }

  /** Sets fire to what burns in the sphere (engines without fire ignore it). */
  ignite(pos: Vec3, radius: number): void {
    this.send({ type: 'ignite', pos, radius });
  }

  /** Fills the air in the sphere with water (engines without water ignore it). */
  pour(pos: Vec3, radius: number): void {
    this.send({ type: 'pour', pos, radius });
  }

  /** Removes the water in the sphere. */
  drain(pos: Vec3, radius: number): void {
    this.send({ type: 'drain', pos, radius });
  }

  /** Brings the solids in the sphere to (at least) `celsius`. */
  heat(pos: Vec3, radius: number, celsius: number): void {
    this.send({ type: 'heat', pos, radius, celsius });
  }

  /** An environment setting by name ("fire.flame_reach", ...): recorded in replays. */
  setEnv(name: string, value: number): void {
    this.send({ type: 'setEnv', name, value });
  }

  /** A world tunable by name ("rigid.gravity", ...): recorded in replays. */
  setTunable(name: string, value: number): void {
    this.send({ type: 'setTunable', name, value });
  }

  /** Puts out and cools the sphere. */
  extinguish(pos: Vec3, radius: number): void {
    this.send({ type: 'extinguish', pos, radius });
  }

  setParams(params: EngineParams): void {
    this.send({ type: 'setParams', params: { ...params } });
  }

  /** A bullet's hit: holes what its energy (J) gets through (engines without it carve). */
  shoot(pos: Vec3, radius: number, energy: number): void {
    this.send({ type: 'shoot', pos, radius, energy });
  }

  /** A vehicle dropped into the world (kind: VEHICLE_KINDS index; paint: `Paint`). */
  spawnVehicle(kind: number, paint: number, pos: Vec3, yaw: number): void {
    this.send({ type: 'spawnVehicle', kind, paint, pos, yaw });
  }

  enterVehicle(id: number): void {
    this.send({ type: 'enterVehicle', id });
  }

  exitVehicle(): void {
    this.send({ type: 'exitVehicle' });
  }

  /** The player's vehicle's controls (until changed). */
  drive(throttle: number, brake: number, steer: number, handbrake: boolean): void {
    this.send({ type: 'drive', throttle, brake, steer, handbrake });
  }

  setTraffic(traffic: TrafficSettings): void {
    this.send({ type: 'setTraffic', traffic: { ...traffic } });
  }

  setPedestrians(pedestrians: PedestrianSettings): void {
    this.send({ type: 'setPedestrians', pedestrians: { ...pedestrians } });
  }

  /** A round into a character (its id from a raycast with `characters`), where the ray found it. */
  woundCharacter(id: number, pos: Vec3, radius: number, energy: number): void {
    this.send({ type: 'woundCharacter', id, pos, radius, energy });
  }

  attachProp(id:number,archetype:string,point=0,socket='primary',style=0):void {this.send({type:'attachProp',id,archetype,point,socket,style});}
  detachProp(id:number,point=0,reason=0):void {this.send({type:'detachProp',id,point,reason});}
  pedestrianLoadouts(armed:number,carrying:number):void {this.send({type:'pedestrianLoadouts',armed,carrying});}

  damageCharacter(id: number, pos: Vec3, direction: Vec3, mass: number, speed: number, diameter: number, construction = 0, impactScale = construction === 2 ? 1 : 30): void {
    this.send({type: 'damageCharacter', id, kind: 0, pos, direction, mass, speed, diameter, construction, impactScale});
  }

  /** The last pose message handled (once per frame). */
  frameAck(seq: number): void {
    this.send({ type: 'frameAck', seq });
  }

  /** `characters`: a shot's line - the characters' bodies are hit too (`RaycastHit.character`). */
  raycast(origin: Vec3, dir: Vec3, maxDist: number, characters = false): Promise<RaycastHit | null> {
    return new Promise((resolve, reject) => {
      if (this.dead) return reject(this.dead);
      const id = this.nextId++;
      this.raycasts.set(id, { resolve, reject });
      this.send(characters ? { type: 'raycast', id, origin, dir, maxDist, characters } : { type: 'raycast', id, origin, dir, maxDist });
    });
  }

  collide(min: Vec3, max: Vec3, move: Vec3): Promise<CollideResult> {
    return new Promise((resolve, reject) => {
      if (this.dead) return reject(this.dead);
      const id = this.nextId++;
      this.collides.set(id, { resolve, reject });
      this.send({ type: 'collide', id, min, max, move });
    });
  }

  terminate(): void {
    this.worker.terminate();
    this.rejectAll(new Error('engine terminated'));
    this.dead ??= new Error('engine terminated');
  }

  private receive(data: unknown): void {
    if (!isWorkerMessage(data)) {
      console.warn('[engine] ignoring unknown message', data);
      return;
    }
    if (data.type === 'raycastResult') {
      const p = this.raycasts.get(data.id);
      this.raycasts.delete(data.id);
      p?.resolve(data.hit);
    } else if (data.type === 'collideResult') {
      const p = this.collides.get(data.id);
      this.collides.delete(data.id);
      const r: CollideResult = { move: data.move, onGround: data.onGround };
      if (data.ground !== undefined) r.ground = data.ground;
      if (data.groundPiece) r.groundPiece = data.groundPiece;
      if (data.groundVelocity) r.groundVelocity = data.groundVelocity;
      p?.resolve(r);
    } else if (data.type === 'error' && data.fatal) {
      this.fail(new Error(data.message), data);
      return;
    } else if (data.type === 'loading') {
      this.loadsBegun = data.generation;
    }
    // Between asking for a world and the engine starting it, what arrives describes the world
    // being replaced: a piece detached then would outlive it (its poses never come).
    if (this.loadsBegun !== this.loadsSent && !CROSSES_LOADS.has(data.type)) return;
    this.emit(data);
  }

  private emit(msg: WorkerMessage): void {
    const set = this.handlers.get(msg.type);
    if (!set) return;
    for (const h of set) {
      try {
        h(msg);
      } catch (err) {
        console.error(`[engine] ${msg.type} handler failed`, err);
      }
    }
  }

  private rejectAll(err: Error): void {
    for (const p of this.raycasts.values()) p.reject(err);
    for (const p of this.collides.values()) p.reject(err);
    this.raycasts.clear();
    this.collides.clear();
  }

  private fail(err: Error, original?: WorkerMessage): void {
    if (this.dead) return;
    this.dead = err;
    this.rejectAll(err);
    this.emit(original ?? { type: 'error', fatal: true, message: err.message });
  }
}
