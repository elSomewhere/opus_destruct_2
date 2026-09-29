/**
 * Game orchestration: wires engine messages to the renderer and effects, runs the frame
 * loop (input -> player or the car they drive -> weapons -> effects -> render -> HUD) and owns
 * the UI.
 */
import type { EngineClient } from '../engine/client.ts';
import { GridFrames } from '../engine/gridframes.ts';
import type { DebugView, EngineEvent, EngineParams, EngineStats, ProceduralKind, TrafficSettings, Vec3, WorldInfo } from '../engine/protocol.ts';
import { CAR_PAINTS, DEFAULT_TRAFFIC, VEHICLE_KINDS } from '../engine/protocol.ts';
import type { Camera, Renderer, RenderStats } from '../render/renderer.ts';
import type { WheelDraw } from '../render/wheels.ts';
import { DriveHud } from '../ui/drivehud.ts';
import { Hud } from '../ui/hud.ts';
import type { Overlay } from '../ui/overlay.ts';
import { SettingsPanel } from '../ui/settings.ts';
import { Effects } from './effects.ts';
import { Input } from './input.ts';
import { PieceBodies } from '../engine/pieces.ts';
import { OccupancyStore } from './occupancy.ts';
import { Driving, padPressed, type CameraMode } from './driving.ts';
import { PLAYER, Player } from './player.ts';
import { VehicleEffects } from './vehicle-effects.ts';
import { kindName, paintName, rotate, VehicleTracker } from './vehicles.ts';
import { WEAPONS, Weapons, type WeaponId } from './weapons.ts';

export interface GameOptions {
  canvas: HTMLCanvasElement;
  uiRoot: HTMLElement;
  overlay: Overlay;
  renderer: Renderer;
  engine: EngineClient;
  world: { kind: ProceduralKind; seed: number };
  params: EngineParams;
  voxelSize: number;
}

const FOV_Y = (70 * Math.PI) / 180;
const NEAR = 0.05;
const HUD_INTERVAL_MS = 200;
/** How near (m, from the player's middle to a car's box) the player takes a car. */
const ENTER_REACH = 2.2;

export class Game {
  private readonly renderer: Renderer;
  private readonly engine: EngineClient;
  private readonly overlay: Overlay;
  private readonly input: Input;
  private readonly player = new Player();
  private readonly occupancy = new OccupancyStore();
  /** The oriented grids' places: drawn and felt where they are (interpolated when they move). */
  private readonly gridFrames = new GridFrames();
  /** The rigid pieces as the player's collision feels them (a lift's car, a turntable, rubble). */
  private readonly pieces = new PieceBodies();
  private readonly effects: Effects;
  private readonly weapons: Weapons;
  private readonly hud: Hud;
  private readonly driveHud: DriveHud;
  /** The vehicles (`vehicles` messages): their bodies are pieces, their wheels drawn from this. */
  private readonly tracker = new VehicleTracker();
  private readonly driving = new Driving();
  private readonly vehicleFx = new VehicleEffects();
  /** The next kind of car B drops in front of the player. */
  private spawnKind = 1;
  private traffic: TrafficSettings = { ...DEFAULT_TRAFFIC };
  /** The last pose message (debris, vehicles) handled, acknowledged to the worker every frame. */
  private poseSeq = 0;
  private ackedSeq = 0;
  private readonly settings: SettingsPanel;
  private readonly voxelSize: number;
  private readonly world: { kind: ProceduralKind; seed: number };
  private params: EngineParams;
  private info: WorldInfo | null = null;
  private engineStats: EngineStats | null = null;
  private lastRender: RenderStats | null = null;
  private running = false;
  private lastFrame = 0;
  private frameMsEma = 16;
  private lastHud = 0;
  private frameCount = 0;
  private hudVisible = true;
  private meshesSinceReady = 0;
  private eventsSeen = 0;
  private statsSeq = 0;

  constructor(opts: GameOptions) {
    this.renderer = opts.renderer;
    this.engine = opts.engine;
    this.overlay = opts.overlay;
    this.voxelSize = opts.voxelSize;
    this.world = { ...opts.world };
    this.params = { ...opts.params };
    this.effects = new Effects(this.renderer.particles);
    this.weapons = new Weapons(this.engine, this.effects);
    this.hud = new Hud(opts.uiRoot);
    this.driveHud = new DriveHud(opts.uiRoot);
    this.settings = new SettingsPanel(opts.uiRoot, this.params, this.world, this.traffic, {
      onParams: (p) => {
        this.params = p;
        this.engine.setParams(p);
      },
      onSetting: (kind, name, value) => {
        if (kind === 'env') this.engine.setEnv(name, value);
        else this.engine.setTunable(name, value);
      },
      onLoadProcedural: (kind, seed) => this.loadProcedural(kind, seed),
      onTraffic: (t) => {
        this.traffic = { ...t };
        this.engine.setTraffic(t);
      },
      onLoadWad: (file, map, options) => {
        this.beginLoad(`Reading ${file.name}`);
        file
          .arrayBuffer()
          .then((buf) => {
            this.overlay.setLoading(`Loading ${map} from ${file.name}`, 0.1);
            this.engine.loadWad(buf, map, options);
          })
          .catch((err: unknown) => this.overlay.toast(`Could not read ${file.name}: ${String(err)}`, 'error'));
      },
    });
    this.occupancy.setFrames(this.gridFrames);
    this.occupancy.setPieces(this.pieces);
    this.input = new Input(opts.canvas, (locked) => {
      this.overlay.setPrompt(!locked && this.info !== null);
      this.settings.setVisible(!locked);
    });
    this.wireEngine();
  }

  start(): void {
    this.engine.init({
      voxelSize: this.voxelSize,
      threads: Math.max(1, Math.min(16, navigator.hardwareConcurrency || 4)),
      memoryMB: 1024,
      params: this.params,
      persist: new URLSearchParams(location.search).get('persist') === '1',
      gpuDisplacement: new URLSearchParams(location.search).get('gpudisp') !== '0',
    });
    this.loadProcedural(this.world.kind, this.world.seed);
    this.running = true;
    requestAnimationFrame(this.frame);
  }

  stop(): void {
    this.running = false;
  }

  private beginLoad(text: string): void {
    this.info = null;
    // the old world's stats must not describe the new one (fresh stats follow its first ticks)
    this.engineStats = null;
    this.hud.clearTimeline();
    this.player.active = false;
    this.driving.vehicle = 0;
    this.driving.scripted = null;
    this.tracker.clear();
    this.vehicleFx.clear();
    this.meshesSinceReady = 0;
    // Engines remove the old world's chunks with chunkRemoved; clearing here as well
    // keeps the view clean if one does not.
    this.renderer.clearWorld();
    this.occupancy.clear();
    this.gridFrames.clear();
    this.pieces.clear();
    this.effects.setFlames(new Float32Array(0));
    this.effects.setSmoke(new Float32Array(0));
    this.overlay.setLoading(text, 0.05);
    this.overlay.setPrompt(false);
  }

  private loadProcedural(kind: ProceduralKind, seed: number): void {
    this.world.kind = kind;
    this.world.seed = seed;
    this.beginLoad(`Generating ${kind} #${seed}`);
    this.engine.loadProcedural(kind, seed);
  }

  private wireEngine(): void {
    const e = this.engine;
    e.on('textures', (msg) => {
      try {
        this.renderer.setTextures(msg.list);
      } catch (err) {
        this.overlay.toast(`Texture upload failed: ${String(err)}`, 'error');
      }
    });
    e.on('ready', (msg) => {
      this.info = msg.info;
      this.player.spawn(msg.info.spawn.pos, msg.info.spawn.dir);
      this.overlay.setLoading('Streaming chunk meshes', 0.5);
      this.overlay.setPrompt(!this.input.locked);
    });
    e.on('chunkMeshes', (msg) => {
      for (const m of msg.meshes) {
        if (m.grid !== undefined) this.renderer.grids.upsert(m);
        else this.renderer.chunks.upsert(m);
      }
      if (msg.fields) this.renderer.fields.set(msg.fields);
      if (this.info && this.meshesSinceReady === 0) this.overlay.setLoading(null);
      this.meshesSinceReady += msg.meshes.length;
    });
    e.on('chunkRemoved', (msg) => {
      for (const k of msg.keys) if (!this.renderer.grids.remove(k)) this.renderer.chunks.remove(k);
    });
    e.on('grids', (msg) => {
      this.gridFrames.apply(msg.frames, msg.removed);
      for (const id of msg.removed) {
        this.renderer.grids.removeGrid(id);
        this.occupancy.removeGrid(id);
      }
    });
    e.on('joints', (msg) => this.renderer.ropes.set(msg.joints));
    e.on('vehicles', (msg) => {
      const now = performance.now() / 1000;
      this.tracker.apply(msg.vehicles, msg.wheels, msg.player, now);
      // (the car the player drives is gone - all its wheels torn off - or no longer theirs)
      if (!this.driving.confirm(this.tracker)) this.leaveVehicle(false);
      this.poseSeq = msg.seq ?? this.poseSeq;
    });
    e.on('events', (msg) => this.handleEvents(msg.list));
    e.on('occupancy', (msg) => this.occupancy.apply(msg));
    e.on('debris', (msg) => {
      const now = performance.now() / 1000;
      this.renderer.islands.applyDebris(msg.poses, now);
      this.pieces.applyDebris(msg.poses, now);
      this.poseSeq = msg.seq ?? this.poseSeq;
    });
    e.on('water', (msg) => {
      for (const k of msg.removed) this.renderer.water.remove(k);
      for (const m of msg.meshes) this.renderer.water.upsert(m);
    });
    e.on('env', (msg) => {
      this.effects.setFlames(msg.flames);
      this.effects.setSmoke(msg.smoke);
    });
    e.on('stats', (msg) => {
      if (this.info === null) return; // posted before the current world's ready (worker messages are ordered)
      this.engineStats = msg.stats;
      this.statsSeq++;
      if (msg.timeline) this.hud.pushTimeline(msg.timeline);
    });
    e.on('progress', (msg) => {
      if (this.info === null) this.overlay.setLoading(`${msg.stage}`, msg.total > 0 ? msg.done / msg.total : 0);
    });
    e.on('error', (msg) => {
      if (msg.fatal) {
        this.stop();
        this.overlay.fatal(
          `The ${e.kind} engine stopped`,
          msg.message,
          e.kind === 'wasm' ? [{ href: '?engine=mock', label: 'Run with the mock engine instead' }] : [],
        );
      } else {
        this.overlay.toast(msg.message, 'error', 9000);
      }
    });
  }

  private handleEvents(list: readonly EngineEvent[]): void {
    const now = performance.now() / 1000;
    const eye = this.player.eye();
    for (const ev of list) {
      this.eventsSeen++;
      switch (ev.kind) {
        case 'detached':
          this.renderer.islands.add(ev, now);
          this.pieces.add(ev);
          if (!ev.remesh) this.effects.detached(ev);
          break;
        case 'crack':
          this.effects.crack(ev);
          break;
        case 'impact':
          this.effects.impact(ev, eye);
          break;
        case 'splash':
          this.effects.splash(ev);
          break;
      }
    }
  }

  private handleKeys(nowS: number): void {
    const i = this.input;
    const driving = this.driving.driving;
    if (!driving) for (const w of WEAPONS) if (i.wasPressed(w.key)) this.weapons.select(w.id);
    const wheel = i.consumeWheel();
    if (wheel !== 0 && !driving) this.weapons.cycle(wheel);
    if (i.wasPressed('KeyV') && !driving) this.player.noclip = !this.player.noclip;
    if (i.wasPressed('KeyE')) this.useKey(nowS);
    if (i.wasPressed('KeyC') && driving) this.driving.cycleCamera();
    if (i.wasPressed('KeyB') && !driving) this.spawnCar();
    if (i.wasPressed('KeyR')) {
      if (driving) this.leaveVehicle(true);
      this.player.respawn();
    }
    if (i.wasPressed('KeyG')) this.settings.cycleDebugView();
    if (i.wasPressed('KeyH')) {
      this.hudVisible = !this.hudVisible;
      this.hud.setVisible(this.hudVisible);
    }
  }

  /** E (or a gamepad's Y): out of the car; into one near by; else "use" (doors, lifts). */
  private useKey(nowS: number): void {
    if (this.driving.driving) {
      this.leaveVehicle(true);
      return;
    }
    if (!this.player.active) return;
    const v = this.tracker.nearest(this.playerMiddle(), ENTER_REACH, nowS);
    if (v) {
      this.driving.enter(v, this.tracker.pose(v, nowS), this.engine);
      this.player.active = false;
      this.player.vel = [0, 0, 0];
      return;
    }
    this.engine.use(this.player.eye(), this.player.forward());
  }

  private playerMiddle(): Vec3 {
    return [this.player.pos[0], this.player.pos[1], this.player.pos[2] + PLAYER.height / 2];
  }

  /** Out of the car: beside its door (or wherever there is room), on foot again. */
  private leaveVehicle(tellEngine: boolean): void {
    if (!this.driving.driving && this.player.active) return;
    const v = this.tracker.vehicles.get(this.driving.vehicle);
    const spot = this.driving.exitSpot(v, this.occupancy, PLAYER.width, PLAYER.height);
    if (tellEngine) this.driving.exit(this.engine);
    else this.driving.vehicle = 0;
    if (spot) {
      this.player.pos = spot.pos;
      this.player.yaw = spot.yaw;
      this.player.pitch = -0.1;
    }
    this.player.vel = [0, 0, 0];
    this.player.active = this.info !== null;
  }

  /** B: a car dropped on the ground a few metres in front of the player, facing their way. */
  private spawnCar(): void {
    if (!this.player.active) return;
    const f: Vec3 = [Math.cos(this.player.yaw), Math.sin(this.player.yaw), 0];
    const p: Vec3 = [this.player.pos[0] + f[0] * 7, this.player.pos[1] + f[1] * 7, this.player.pos[2]];
    // (on the ground there: the first solid voxel under free space, from above the player's head)
    const h = this.voxelSize;
    const ix = Math.floor(p[0] / h + 0.5);
    const iy = Math.floor(p[1] / h + 0.5);
    let z = p[2];
    if (this.occupancy.ready) {
      for (let iz = Math.floor((p[2] + 2.5) / h + 0.5); iz > Math.floor((p[2] - 6) / h); iz--) {
        if (this.occupancy.solid(ix, iy, iz) && !this.occupancy.solid(ix, iy, iz + 1)) {
          z = (iz + 0.5) * h;
          break;
        }
      }
    }
    const kind = this.spawnKind;
    this.spawnKind = (this.spawnKind + 1) % VEHICLE_KINDS.length;
    const paint = CAR_PAINTS[Math.floor(Math.random() * CAR_PAINTS.length)]!;
    this.engine.spawnVehicle(kind, paint, [p[0], p[1], z + 0.05], this.player.yaw);
  }

  /** The wheels to draw this frame (interpolated), and the vehicles' effects. */
  private vehicleFrame(dt: number, nowS: number): WheelDraw[] {
    const wheels: WheelDraw[] = [];
    for (const w of this.tracker.wheels.values()) {
      const v = this.tracker.vehicles.get(w.vehicle);
      if (!v) continue;
      const p = this.tracker.wheelPose(w, nowS);
      wheels.push({ centre: p.centre, rot: p.rot, radius: w.radius, width: w.width, side: w.side, spin: v.speed / Math.max(0.1, w.radius) });
    }
    this.vehicleFx.update(dt, nowS, this.tracker, this.renderer.skids, this.effects);
    return wheels;
  }

  private readonly frame = (t: number): void => {
    if (!this.running) return;
    const dt = this.lastFrame === 0 ? 1 / 60 : Math.min(0.1, Math.max(0, (t - this.lastFrame) / 1000));
    this.lastFrame = t;
    this.frameMsEma = this.frameMsEma * 0.95 + dt * 1000 * 0.05;
    this.frameCount++;
    const nowS = t / 1000;

    if (this.input.locked) {
      const [dx, dy] = this.input.consumeMouse();
      if (this.driving.driving) this.driving.look(dx, dy, nowS);
      else this.player.look(dx, dy);
      this.handleKeys(nowS);
    } else if (this.driving.driving) {
      this.driving.look(0, 0, nowS); // (a gamepad's stick)
    }
    if (padPressed(3)) this.useKey(nowS); // (a gamepad's Y: in or out)
    // (the pieces where they are drawn: the player feels them there too)
    this.pieces.advance(nowS);

    const shake = this.effects.shake(nowS);
    let camera: Camera;
    let focus: Vec3;
    let focusDir: Vec3;
    const car = this.driving.driving ? this.tracker.vehicles.get(this.driving.vehicle) : undefined;
    if (this.driving.driving) {
      this.driving.steer(this.input, this.engine);
      if (car && car.chassis !== 0) {
        const pose = this.tracker.pose(car, nowS);
        const { cam, jolt } = this.driving.camera(car, pose, dt, nowS, this.occupancy, this.voxelSize);
        if (jolt > 0) this.effects.addTrauma(jolt);
        camera = { eye: [cam.eye[0] + shake.offset[0], cam.eye[1] + shake.offset[1], cam.eye[2] + shake.offset[2]], forward: cam.forward, fovY: cam.fovY, near: NEAR };
        focus = pose.pos;
        focusDir = rotate(pose.rot, [1, 0, 0]);
        // (the player rides in it: where they are, for streaming and the HUD)
        this.player.pos = [pose.pos[0], pose.pos[1], pose.pos[2] - PLAYER.eye / 2];
      } else {
        const eye = this.player.eye();
        camera = { eye, forward: this.player.forward(), fovY: FOV_Y, near: NEAR };
        focus = eye;
        focusDir = this.player.forward();
      }
    } else {
      this.player.update(dt, this.input, this.engine, this.occupancy);
      if (this.info && this.player.pos[2] < this.info.bounds.min[2] - 30) this.player.respawn(); // kill plane
      const eye = this.player.eye();
      const forward = this.player.forward();
      if (this.player.active) {
        this.weapons.update(dt, this.input.locked && this.input.fireHeld, this.input.locked && this.input.fireClicked, eye, forward);
      }
      // Camera shake perturbs only the rendered view, not aiming.
      const yaw = this.player.yaw + shake.yaw;
      const pitch = this.player.pitch + shake.pitch;
      const cp = Math.cos(pitch);
      camera = {
        eye: [eye[0] + shake.offset[0], eye[1] + shake.offset[1], eye[2] + shake.offset[2]],
        forward: [cp * Math.cos(yaw), cp * Math.sin(yaw), Math.sin(pitch)],
        fovY: FOV_Y,
        near: NEAR,
      };
      focus = eye;
      focusDir = forward;
    }
    // streaming / bake focus follows the player (or their car) whether or not they are in control
    if (this.frameCount % 2 === 0) this.engine.viewer(focus, focusDir);
    this.effects.update(dt);
    this.effects.fire(dt, camera.eye, nowS);
    this.effects.smokeField(nowS, this.voxelSize, camera.eye);
    const wheels = this.vehicleFrame(dt, nowS);
    this.renderer.particles.update(dt);

    this.lastRender = this.renderer.render({
      camera,
      timeS: nowS,
      debugView: this.params.debugView,
      flashPos: this.effects.flashPos,
      flashIntensity: this.effects.flashIntensity,
      voxelSize: this.voxelSize,
      gridFrames: this.gridFrames,
      wheels,
    });
    this.hud.setMuzzleFlash(this.effects.muzzle > 0 && !this.driving.driving);
    this.hud.setDriving(this.driving.driving);
    this.updateDriveHud(car, nowS);
    if (t - this.lastHud > HUD_INTERVAL_MS) {
      this.lastHud = t;
      this.updateHud();
    }
    this.input.endFrame();
    // (the worker holds back poses while the page is far behind: it gets the latest instead of a backlog)
    if (this.poseSeq !== this.ackedSeq) {
      this.ackedSeq = this.poseSeq;
      this.engine.frameAck(this.poseSeq);
    }
    requestAnimationFrame(this.frame);
  };

  private updateDriveHud(car: ReturnType<VehicleTracker['vehicles']['get']>, nowS: number): void {
    if (this.driving.driving) {
      this.driveHud.setPrompt(null);
      this.driveHud.update(
        car
          ? {
              speed: car.speed,
              gear: car.gear,
              rpm: car.rpm,
              redline: car.redline,
              damage: car.damage,
              wheels: car.wheels,
              wheelSlots: Math.max(car.wheels, car.kind === 4 ? 6 : 4),
              handbrake: this.driving.controls.handbrake,
              camera: this.driving.mode,
              kind: kindName(car.kind),
              gamepad: this.driving.gamepad,
            }
          : null,
      );
      return;
    }
    this.driveHud.update(null);
    const near = this.player.active && this.info ? this.tracker.nearest(this.playerMiddle(), ENTER_REACH, nowS) : null;
    this.driveHud.setPrompt(near ? `E  drive the ${paintName(near.paint)} ${kindName(near.kind)}` : null);
  }

  private updateHud(): void {
    if (!this.lastRender) return;
    this.hud.update({
      fps: 1000 / Math.max(1e-3, this.frameMsEma),
      frameMs: this.frameMsEma,
      render: this.lastRender,
      engineKind: this.engine.kind,
      engine: this.engineStats,
      gpu: this.renderer.description,
      pendingRequests: this.engine.pendingRequests,
      weapon: this.weapons.current,
      weapons: WEAPONS,
      player: { pos: this.player.pos, onGround: this.player.onGround, noclip: this.player.noclip },
      vehicles: this.tracker.size,
      debugView: this.params.debugView,
      rockets: this.weapons.liveRockets,
    });
  }

  /** Scriptable hooks for automated browser checks and console debugging. */
  debugApi(): StructvoxDebugApi {
    return {
      fire: () => this.weapons.fire(this.player.eye(), this.player.forward()),
      select: (id) => this.weapons.select(id),
      look: (yawDeg, pitchDeg) => {
        this.player.yaw = (yawDeg * Math.PI) / 180;
        this.player.pitch = (pitchDeg * Math.PI) / 180;
      },
      collideLocal: (min, max, move) => (this.occupancy.ready ? this.occupancy.collide(min, max, move) : null),
      teleport: (x, y, z) => {
        this.player.pos = [x, y, z];
        this.player.vel = [0, 0, 0];
      },
      noclip: (on) => {
        this.player.noclip = on;
        this.player.vel = [0, 0, 0];
      },
      setDebugView: (v) => this.settings.setDebugView(v),
      load: (kind, seed) => this.loadProcedural(kind, seed),
      vehicles: () =>
        [...this.tracker.vehicles.values()].map((v) => ({
          id: v.id,
          kind: kindName(v.kind),
          pos: [...v.cur.pos],
          yaw: (Math.atan2(rotate(v.cur.rot, [1, 0, 0])[1], rotate(v.cur.rot, [1, 0, 0])[0]) * 180) / Math.PI,
          speed: v.speed,
          gear: v.gear,
          rpm: v.rpm,
          flags: v.flags,
          damage: v.damage,
          wheels: v.wheels,
          chassis: v.chassis,
        })),
      spawnVehicle: (kind, paint, x, y, z, yawDeg) => this.engine.spawnVehicle(kind, paint, [x, y, z], (yawDeg * Math.PI) / 180),
      enterVehicle: (id) => {
        const now = performance.now() / 1000;
        const v = id !== undefined ? this.tracker.vehicles.get(id) : this.tracker.nearest(this.playerMiddle(), 50, now);
        if (!v) return false;
        this.driving.enter(v, this.tracker.pose(v, now), this.engine);
        this.player.active = false;
        return true;
      },
      exitVehicle: () => this.leaveVehicle(true),
      drive: (throttle, steer, handbrake = false, brake = 0) => {
        this.driving.scripted = { throttle, steer, handbrake, brake };
      },
      camera: (mode) => {
        this.driving.mode = mode;
      },
      setTraffic: (t) => {
        this.traffic = { ...this.traffic, ...t };
        this.engine.setTraffic(this.traffic);
      },
      state: () => ({
        ready: this.info !== null,
        player: [...this.player.pos],
        onGround: this.player.onGround,
        weapon: this.weapons.current.id,
        render: this.lastRender,
        engine: this.engineStats,
        statsSeq: this.statsSeq,
        events: this.eventsSeen,
        pieceBodies: this.pieces.size,
        fps: 1000 / Math.max(1e-3, this.frameMsEma),
        driving: this.driving.vehicle,
        engineDriving: this.tracker.player,
        camera: this.driving.mode,
      }),
    };
  }
}

export interface StructvoxDebugApi {
  fire(): void;
  select(id: WeaponId): void;
  look(yawDeg: number, pitchDeg: number): void;
  teleport(x: number, y: number, z: number): void;
  /** Free flight without collision or gravity (the V key; keys need pointer lock). */
  noclip(on: boolean): void;
  /** Client-side collision (occupancy), or null before the engine sent any. */
  collideLocal(min: Vec3, max: Vec3, move: Vec3): { move: Vec3; onGround: boolean } | null;
  setDebugView(v: DebugView): void;
  load(kind: ProceduralKind, seed: number): void;
  /** The vehicles (as the last `vehicles` message had them). */
  vehicles(): { id: number; kind: string; pos: number[]; yaw: number; speed: number; gear: number; rpm: number; flags: number; damage: number; wheels: number; chassis: number }[];
  spawnVehicle(kind: number, paint: number, x: number, y: number, z: number, yawDeg: number): void;
  /** Takes the wheel of a vehicle (the nearest within 50 m if no id); false if there is none. */
  enterVehicle(id?: number): boolean;
  exitVehicle(): void;
  /** Scripted controls of the player's vehicle (instead of the keys, until getting out). */
  drive(throttle: number, steer: number, handbrake?: boolean, brake?: number): void;
  camera(mode: CameraMode): void;
  setTraffic(t: Partial<TrafficSettings>): void;
  state(): {
    ready: boolean;
    player: number[];
    onGround: boolean;
    weapon: WeaponId;
    render: RenderStats | null;
    engine: EngineStats | null;
    /** Number of `stats` messages received (to wait for fresh stats after an action). */
    statsSeq: number;
    events: number;
    /** Pieces whose voxels the client's collision holds. */
    pieceBodies: number;
    fps: number;
    /** The vehicle the player drives (0: on foot), and the engine's word on it. */
    driving: number;
    engineDriving: number;
    camera: CameraMode;
  };
}
