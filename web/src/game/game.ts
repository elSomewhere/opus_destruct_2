/**
 * Game orchestration: wires engine messages to the renderer and effects, runs the frame
 * loop (input -> player -> weapons -> effects -> render -> HUD) and owns the UI.
 */
import type { EngineClient } from '../engine/client.ts';
import type { DebugView, EngineEvent, EngineParams, EngineStats, ProceduralKind, Vec3, WorldInfo } from '../engine/protocol.ts';
import type { Renderer, RenderStats } from '../render/renderer.ts';
import { Hud } from '../ui/hud.ts';
import type { Overlay } from '../ui/overlay.ts';
import { SettingsPanel } from '../ui/settings.ts';
import { Effects } from './effects.ts';
import { Input } from './input.ts';
import { OccupancyStore } from './occupancy.ts';
import { Player } from './player.ts';
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

export class Game {
  private readonly renderer: Renderer;
  private readonly engine: EngineClient;
  private readonly overlay: Overlay;
  private readonly input: Input;
  private readonly player = new Player();
  private readonly occupancy = new OccupancyStore();
  private readonly effects: Effects;
  private readonly weapons: Weapons;
  private readonly hud: Hud;
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
    this.settings = new SettingsPanel(opts.uiRoot, this.params, this.world, {
      onParams: (p) => {
        this.params = p;
        this.engine.setParams(p);
      },
      onLoadProcedural: (kind, seed) => this.loadProcedural(kind, seed),
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
    this.meshesSinceReady = 0;
    // Engines remove the old world's chunks with chunkRemoved; clearing here as well
    // keeps the view clean if one does not.
    this.renderer.clearWorld();
    this.occupancy.clear();
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
      for (const m of msg.meshes) this.renderer.chunks.upsert(m);
      if (msg.fields) this.renderer.fields.set(msg.fields);
      if (this.info && this.meshesSinceReady === 0) this.overlay.setLoading(null);
      this.meshesSinceReady += msg.meshes.length;
    });
    e.on('chunkRemoved', (msg) => {
      for (const k of msg.keys) this.renderer.chunks.remove(k);
    });
    e.on('events', (msg) => this.handleEvents(msg.list));
    e.on('occupancy', (msg) => this.occupancy.apply(msg));
    e.on('debris', (msg) => this.renderer.islands.applyDebris(msg.poses, performance.now() / 1000));
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
          this.effects.detached(ev);
          break;
        case 'crack':
          this.effects.crack(ev);
          break;
        case 'impact':
          this.effects.impact(ev, eye);
          break;
        case 'bubble':
          // Debug only; shown through the bubble-level view and the HUD counters.
          break;
      }
    }
  }

  private handleKeys(): void {
    const i = this.input;
    for (const w of WEAPONS) if (i.wasPressed(w.key)) this.weapons.select(w.id);
    const wheel = i.consumeWheel();
    if (wheel !== 0) this.weapons.cycle(wheel);
    if (i.wasPressed('KeyV')) this.player.noclip = !this.player.noclip;
    if (i.wasPressed('KeyE') && this.player.active) this.engine.use(this.player.eye(), this.player.forward());
    if (i.wasPressed('KeyR')) this.player.respawn();
    if (i.wasPressed('KeyG')) this.settings.cycleDebugView();
    if (i.wasPressed('KeyH')) {
      this.hudVisible = !this.hudVisible;
      this.hud.setVisible(this.hudVisible);
    }
  }

  private readonly frame = (t: number): void => {
    if (!this.running) return;
    const dt = this.lastFrame === 0 ? 1 / 60 : Math.min(0.1, Math.max(0, (t - this.lastFrame) / 1000));
    this.lastFrame = t;
    this.frameMsEma = this.frameMsEma * 0.95 + dt * 1000 * 0.05;
    this.frameCount++;

    if (this.input.locked) {
      const [dx, dy] = this.input.consumeMouse();
      this.player.look(dx, dy);
      this.handleKeys();
    }
    this.player.update(dt, this.input, this.engine, this.occupancy);
    if (this.info && this.player.pos[2] < this.info.bounds.min[2] - 30) this.player.respawn(); // kill plane

    const eye = this.player.eye();
    const forward = this.player.forward();
    if (this.player.active) {
      this.weapons.update(dt, this.input.locked && this.input.fireHeld, this.input.locked && this.input.fireClicked, eye, forward);
    }
    // streaming / bake focus follows the camera whether or not the player is in control
    if (this.frameCount % 2 === 0) this.engine.viewer(eye, forward);
    this.effects.update(dt);
    this.renderer.particles.update(dt);

    // Camera shake perturbs only the rendered view, not aiming.
    const shake = this.effects.shake(t / 1000);
    const yaw = this.player.yaw + shake.yaw;
    const pitch = this.player.pitch + shake.pitch;
    const cp = Math.cos(pitch);
    const viewDir: Vec3 = [cp * Math.cos(yaw), cp * Math.sin(yaw), Math.sin(pitch)];
    const viewEye: Vec3 = [eye[0] + shake.offset[0], eye[1] + shake.offset[1], eye[2] + shake.offset[2]];

    this.lastRender = this.renderer.render({
      camera: { eye: viewEye, forward: viewDir, fovY: FOV_Y, near: NEAR },
      timeS: t / 1000,
      debugView: this.params.debugView,
      flashPos: this.effects.flashPos,
      flashIntensity: this.effects.flashIntensity,
      voxelSize: this.voxelSize,
    });
    this.hud.setMuzzleFlash(this.effects.muzzle > 0);
    if (t - this.lastHud > HUD_INTERVAL_MS) {
      this.lastHud = t;
      this.updateHud();
    }
    this.input.endFrame();
    requestAnimationFrame(this.frame);
  };

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
      state: () => ({
        ready: this.info !== null,
        player: [...this.player.pos],
        onGround: this.player.onGround,
        weapon: this.weapons.current.id,
        render: this.lastRender,
        engine: this.engineStats,
        statsSeq: this.statsSeq,
        events: this.eventsSeen,
        fps: 1000 / Math.max(1e-3, this.frameMsEma),
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
    fps: number;
  };
}
