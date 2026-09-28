/**
 * Game orchestration: wires engine messages to the renderer and effects, runs the frame
 * loop (input -> player -> weapons -> effects -> render -> HUD) and owns the UI.
 */
import { ActorWorld, type PlayerView } from '../actors/actors.ts';
import { WorldAccess } from '../actors/env.ts';
import type { EngineClient } from '../engine/client.ts';
import type { DebugView, EngineEvent, EngineParams, EngineStats, ProceduralKind, Vec3, WorldInfo } from '../engine/protocol.ts';
import { Material } from '../engine/protocol.ts';
import type { Renderer, RenderStats } from '../render/renderer.ts';
import { CharacterPanel, type CharacterPanelState } from '../ui/characters.ts';
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
const PLAYER_HEALTH = 100;

/**
 * Characters placed when a world loads: `?civilians=N&soldiers=M&thugs=K` override, `?actors=0`
 * none.
 */
function populationFor(kind: ProceduralKind | 'wad'): { civilians: number; soldiers: number; thugs: number; rMin: number; rMax: number } {
  const url = new URLSearchParams(location.search);
  const base = { rooms: [5, 3, 1, 2, 12], city: [18, 8, 3, 6, 34], tower: [12, 6, 2, 6, 30], wad: [8, 6, 2, 3, 24] }[kind];
  const off = url.get('actors') === '0';
  const num = (k: string, d: number): number => (off ? 0 : Math.max(0, Math.min(80, Number(url.get(k) ?? d) || 0)));
  return { civilians: num('civilians', base[0]!), soldiers: num('soldiers', base[1]!), thugs: num('thugs', base[2]!), rMin: base[3]!, rMax: base[4]! };
}
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
  /** Soldiers and civilians (svx_anim characters driven by the actors layer). */
  readonly actors: ActorWorld;
  private readonly charPanel: CharacterPanel;
  private charState: CharacterPanelState;
  private playerHealth = PLAYER_HEALTH;
  private hurt = 0;
  private respawnAt = 0;
  private populateAt = 0;
  private worldKind: ProceduralKind | 'wad' = 'rooms';
  private loadingWad = false;
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
            this.loadingWad = true;
            this.engine.loadWad(buf, map, options);
          })
          .catch((err: unknown) => this.overlay.toast(`Could not read ${file.name}: ${String(err)}`, 'error'));
      },
    });
    // characters
    const env = new WorldAccess(this.occupancy, this.voxelSize);
    const player = this.player;
    const self = this;
    const playerView: PlayerView = {
      get feet() {
        return player.pos;
      },
      get forward() {
        return player.forward();
      },
      get alive() {
        return player.active && self.playerHealth > 0;
      },
      get threat() {
        return !player.noclip;
      },
      eye: () => player.eye(),
      chest: () => {
        const e = player.eye();
        return [e[0], e[1], e[2] - 0.35];
      },
    };
    const particles = this.renderer.particles;
    this.actors = new ActorWorld(this.renderer.characters, env, playerView, {
      carve: (pos, r) => this.engine.carve(pos, r),
      impact: (pos, normal) => this.effects.bulletImpact({ pos, normal, distance: 0, material: Material.Concrete }),
      hurtPlayer: (d) => this.hurtPlayer(d),
      pushPlayer: (v) => {
        if (!player.active || player.noclip) return;
        player.vel[0] += v[0];
        player.vel[1] += v[1];
        player.vel[2] += v[2];
      },
      muzzleFlash: (pos, dir) => {
        this.effects.light(pos, 1.1);
        for (let k = 0; k < 4; k++) {
          const v = 1.5 + Math.random() * 3;
          particles.spawn({ pos, vel: [dir[0] * v, dir[1] * v, dir[2] * v], life: 0.05, size: 0.04 + Math.random() * 0.03, color: [3, 2, 0.8, 1], additive: true, gravity: 0 });
        }
      },
      tracer: (from, to) => {
        const d = [to[0] - from[0], to[1] - from[1], to[2] - from[2]];
        const len = Math.hypot(d[0]!, d[1]!, d[2]!);
        const n = Math.min(40, Math.ceil(len / 0.6));
        for (let k = 1; k <= n; k++) {
          const t = k / (n + 1);
          particles.spawn({ pos: [from[0] + d[0]! * t, from[1] + d[1]! * t, from[2] + d[2]! * t], vel: [0, 0, 0], life: 0.035 + 0.05 * t, size: 0.012, color: [4, 3, 1.4, 1], additive: true, gravity: 0 });
        }
      },
      bloodMist: (pos, dir, amount) => {
        for (let k = 0; k < 6 * amount; k++) {
          const v = 0.4 + Math.random() * 1.6;
          particles.spawn({
            pos,
            vel: [dir[0] * v + (Math.random() - 0.5), dir[1] * v + (Math.random() - 0.5), dir[2] * v + Math.random() * 0.5],
            life: 0.4 + Math.random() * 0.6,
            size: 0.05 + Math.random() * 0.08,
            grow: 0.25,
            color: [0.28, 0.01, 0.01, 0.75],
            drag: 3,
            gravity: 0.3,
          });
        }
      },
    }, this.renderer.islands);
    this.charState = { ai: true, god: new URLSearchParams(location.search).get('god') === '1', style: (new URLSearchParams(location.search).get('anim') as CharacterPanelState['style']) ?? 'smooth' };
    if (!['smooth', 'retro', 'retro-chunky'].includes(this.charState.style)) this.charState.style = 'smooth';
    this.applyCharState();
    this.charPanel = new CharacterPanel(this.charState, {
      onChange: (st) => {
        this.charState = st;
        this.applyCharState();
      },
      onSpawn: (kind, n) => {
        if (!this.info) return;
        const placed = this.actors.populate(this.player.pos, kind === 'civilian' ? n : 0, kind === 'soldier' ? n : 0, 8, 30, kind === 'thug' ? n : 0);
        this.overlay.toast(`${placed} ${kind === 'civilian' ? 'civilians' : kind === 'soldier' ? 'soldiers' : 'thugs'} placed`, 'info', 2500);
      },
      onClear: () => this.actors.clear(),
    });
    this.settings.root.insertBefore(this.charPanel.root, this.settings.root.lastChild);
    this.weapons.targets = {
      hit: (origin, dir, maxDist) => {
        const r = this.actors.raycast(origin, dir, maxDist);
        if (!r) return null;
        return { distance: r.hit.distance, apply: (damage, radius) => void this.actors.wound(r.actor, r.hit, [...dir], damage, [...this.player.pos], radius) };
      },
      blast: (pos, radius) => this.actors.blast([...pos], radius, 1),
      fired: (pos) => this.actors.noise({ pos: [...pos], radius: 55, kind: 'shot', source: null }),
      melee: (origin, dir) => this.actors.playerMelee([...origin], [...dir]),
    };

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
    this.actors.clear();
    this.populateAt = 0;
    this.overlay.setLoading(text, 0.05);
    this.overlay.setPrompt(false);
  }

  private loadProcedural(kind: ProceduralKind, seed: number): void {
    this.world.kind = kind;
    this.world.seed = seed;
    this.loadingWad = false;
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
      // (an engine that cannot load a WAD falls back to a procedural world: count that as one)
      this.worldKind = this.loadingWad && msg.info.textures ? 'wad' : this.world.kind;
      // Doom sectors are darker than the procedural worlds' daylight
      this.actors.settings.light = this.worldKind === 'wad' ? 0.8 : 1;
      this.playerHealth = PLAYER_HEALTH;
      this.populateAt = performance.now() / 1000 + 1.0;
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
    this.updateActors(dt, t / 1000);

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
    this.hud.setHurt(this.hurt);
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
      extra: [this.actorLine()],
    });
  }

  private actorLine(): string {
    const s = this.actors.stats();
    return `actors ${s.civilians} civilians  ${s.soldiers} soldiers  ${s.thugs} thugs  ${s.dead} dead  ${s.gibs} gibs  ${s.stains} stains  ${this.actors.bodies} bodies  ${this.actors.updateMs.toFixed(1)} ms  ${this.actors.shotsFired} rounds fired  health ${Math.max(0, Math.round(this.playerHealth))}${this.charState.god ? ' (god)' : ''}${s.bakes > 0 ? `  baking ${s.bakes}` : ''}`;
  }

  private applyCharState(): void {
    const st = this.actors.settings;
    st.ai = this.charState.ai;
    st.playerDamage = !this.charState.god;
    st.retro = this.charState.style !== 'smooth';
    st.retroVoxel = this.charState.style === 'retro-chunky' ? 1 / 16 : 1 / 32;
  }

  private hurtPlayer(damage: number): void {
    if (this.playerHealth <= 0 || !this.player.active) return;
    this.playerHealth -= damage;
    this.hurt = Math.min(1, this.hurt + 0.3 + damage / 40);
    this.effects.addTrauma(0.12);
    if (this.playerHealth <= 0) {
      this.playerHealth = 0;
      this.respawnAt = performance.now() / 1000 + 2.5;
      this.overlay.toast('You were killed', 'error', 2500);
    }
  }

  private updateActors(dt: number, now: number): void {
    this.hurt = Math.max(0, this.hurt - dt * 1.2);
    if (this.playerHealth <= 0 && this.respawnAt > 0 && now >= this.respawnAt) {
      this.respawnAt = 0;
      this.playerHealth = PLAYER_HEALTH;
      this.player.respawn();
    }
    if (this.info && this.populateAt > 0 && now >= this.populateAt && (this.occupancy.ready || this.engine.kind === 'mock')) {
      this.populateAt = 0;
      const pop = populationFor(this.worldKind);
      this.actors.populate(this.player.pos, pop.civilians, pop.soldiers, pop.rMin, pop.rMax, pop.thugs);
    }
    this.actors.update(dt);
    this.actors.draw();
    this.hud.setHealth(this.info ? this.playerHealth / PLAYER_HEALTH : null);
    if (this.frameCount % 15 === 0) {
      const s = this.actors.stats();
      this.charPanel.setInfo(`${s.civilians} civilians, ${s.soldiers} soldiers, ${s.thugs} thugs alive, ${s.dead} dead`);
    }
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
      spawn: (kind, n, rMin = 6, rMax = 30) => this.actors.populate(this.player.pos, kind === 'civilian' ? n : 0, kind === 'soldier' ? n : 0, rMin, rMax, kind === 'thug' ? n : 0),
      spawnAt: (kind, x, y, z, yaw = 0) => this.actors.spawn(kind, [x, y, z], yaw).id,
      actors: () =>
        this.actors.actors.map((a) => ({ id: a.id, faction: a.faction, alive: a.char.alive, pos: [...a.pos], state: a.brain.state, health: a.char.health })),
      characters: (st) => {
        this.charState = { ...this.charState, ...st };
        this.applyCharState();
      },
      actorWorld: () => this.actors,
      brawl: (a, b) => {
        const x = this.actors.actors.find((t) => t.id === a);
        const y = this.actors.actors.find((t) => t.id === b);
        if (!x || !y || !x.char.alive || !y.char.alive) return false;
        this.actors.startBrawl(x, y);
        return true;
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
  /** Places n civilians or soldiers around the player; returns how many were placed. */
  /** Places n civilians, soldiers or thugs around the player; returns how many were placed. */
  spawn(kind: 'civilian' | 'soldier' | 'thug', n: number, rMin?: number, rMax?: number): number;
  spawnAt(kind: 'civilian' | 'soldier' | 'thug', x: number, y: number, z: number, yaw?: number): number;
  actors(): { id: number; faction: string; alive: boolean; pos: number[]; state: string; health: number }[];
  /** Character settings: AI, god mode, animation style. */
  characters(st: Partial<CharacterPanelState>): void;
  actorWorld(): ActorWorld;
  /** Starts a fist (or knife) fight between two characters by id. */
  brawl(a: number, b: number): boolean;
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
