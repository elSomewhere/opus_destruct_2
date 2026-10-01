/**
 * Settings panel: engine tunables (sent as `setParams`), the debug view, the drive city's traffic
 * and people, and world loading (procedural worlds, or a WAD file + map name -> `loadWad`).
 */
import type { EngineParams, PedestrianSettings, PresetInfo, ProceduralKind, TrafficSettings, WadOptions } from '../engine/protocol.ts';
import { DEBUG_VIEW_NAMES, DebugView, PEDESTRIAN_BODY_NAMES, PedestrianBodies, PROCEDURAL_KINDS } from '../engine/protocol.ts';
import { h } from './dom.ts';

export interface SettingsCallbacks {
  onParams(params: EngineParams): void;
  /** An environment setting (`setEnv`) or a world tunable (`setTunable`) by name. */
  onSetting(kind: 'env' | 'tunable', name: string, value: number): void;
  onLoadProcedural(kind: ProceduralKind, seed: number): void;
  /** A preset by id (docs/PRESETS.md; seed 0: the preset's own). */
  onLoadPreset(id: string, seed: number): void;
  /** Traffic of a world with roads (the `drive` city). */
  onTraffic(traffic: TrafficSettings): void;
  /** Pedestrians of a world with walkways (the `drive` city). */
  onPedestrians(pedestrians: PedestrianSettings): void;
  onLoadWad(file: File, map: string, options: WadOptions): void;
}

interface SliderDef {
  key: 'fragility' | 'impact' | 'dif';
  label: string;
  min: number;
  max: number;
  step: number;
  /** Logarithmic slider (equal travel per factor of two), for scale factors around 1. */
  log?: boolean;
  hint: string;
}

const SLIDERS: readonly SliderDef[] = [
  { key: 'fragility', label: 'Fragility', min: 0.25, max: 4, step: 0.01, log: true, hint: 'Fragility — weaker bonds, more collapse' },
  { key: 'impact', label: 'Impact', min: 0.25, max: 4, step: 0.01, log: true, hint: 'Impact — how hard landings hit' },
  { key: 'dif', label: 'Dynamic factor', min: 1, max: 2.5, step: 0.05, hint: 'Dynamic factor — overshoot of sudden load changes' },
];

/**
 * Settings by name (`setEnv` / `setTunable`): the engine's defaults are the initial values, and
 * the engine keeps them across level loads.
 */
interface NamedSetting {
  kind: 'env' | 'tunable';
  name: string;
  label: string;
  /** A checkbox (0 / 1), or a slider over [min, max]. */
  toggle?: boolean;
  min?: number;
  max?: number;
  step?: number;
  value: number;
  hint: string;
}

const ENVIRONMENT: readonly NamedSetting[] = [
  { kind: 'env', name: 'fire.enabled', label: 'Fire', toggle: true, value: 1, hint: 'Fire: burning, heat, charring' },
  { kind: 'env', name: 'fire.flame_reach', label: 'Fire spread', min: 0.02, max: 0.6, step: 0.01, value: 0.15, hint: 'How fast flames heat what is above them (1/s)' },
  { kind: 'env', name: 'fire.wood.burn_s', label: 'Wood burn time', min: 5, max: 120, step: 1, value: 40, hint: 'Seconds a voxel of wood burns before it is gone' },
  { kind: 'env', name: 'smoke.enabled', label: 'Smoke', toggle: true, value: 1, hint: 'Smoke: rises, fills rooms, drifts, thins out' },
  { kind: 'env', name: 'smoke.lifetime', label: 'Smoke lifetime', min: 2, max: 60, step: 1, value: 20, hint: 'Seconds smoke takes to thin out' },
  { kind: 'env', name: 'smoke.wind_x', label: 'Wind x', min: -10, max: 10, step: 0.5, value: 0, hint: 'Wind (m/s) along x' },
  { kind: 'env', name: 'smoke.wind_y', label: 'Wind y', min: -10, max: 10, step: 0.5, value: 0, hint: 'Wind (m/s) along y' },
  { kind: 'env', name: 'water.enabled', label: 'Water flow', toggle: true, value: 1, hint: 'Water flows, spreads and settles' },
  { kind: 'env', name: 'water.loads', label: 'Water pressure', toggle: true, value: 1, hint: 'Water presses on the walls that hold it' },
  { kind: 'env', name: 'water.buoyancy', label: 'Buoyancy', toggle: true, value: 1, hint: 'Pieces float or sink' },
];

const WORLD: readonly NamedSetting[] = [
  { kind: 'tunable', name: 'rigid.gravity', label: 'Gravity', min: 1, max: 20, step: 0.1, value: 9.81, hint: 'm/s^2' },
  { kind: 'tunable', name: 'max_bodies', label: 'Max pieces', min: 500, max: 6000, step: 100, value: 3000, hint: 'Pieces simulated at once (beyond: the smallest are culled)' },
];

function namedRow(d: NamedSetting, onChange: (v: number) => void): HTMLElement {
  if (d.toggle) {
    const box = h('input', { type: 'checkbox' });
    box.checked = d.value !== 0;
    box.addEventListener('change', () => onChange(box.checked ? 1 : 0));
    return h('label', { class: 'row', title: d.hint }, h('span', {}, d.label), box);
  }
  const input = h('input', { type: 'range', min: d.min ?? 0, max: d.max ?? 1, step: d.step ?? 0.01, value: d.value });
  const value = h('span', { class: 'value' }, String(d.value));
  input.addEventListener('input', () => {
    value.textContent = input.value;
    onChange(Number(input.value));
  });
  return h('label', { class: 'row', title: d.hint }, h('span', {}, d.label), input, value);
}

/** Log sliders run over 0..LOG_STEPS. */
const LOG_STEPS = 1000;

function toSlider(d: SliderDef, v: number): number {
  if (!d.log) return v;
  return Math.round((LOG_STEPS * Math.log(v / d.min)) / Math.log(d.max / d.min));
}

function fromSlider(d: SliderDef, x: number): number {
  if (!d.log) return x;
  const v = d.min * Math.pow(d.max / d.min, x / LOG_STEPS);
  return Number((Math.round(v / d.step) * d.step).toFixed(6));
}

export class SettingsPanel {
  readonly root: HTMLElement;
  private params: EngineParams;
  private readonly callbacks: SettingsCallbacks;
  private readonly debugSelect: HTMLSelectElement;
  private readonly pausedBox: HTMLInputElement;
  private readonly presetSelect: HTMLSelectElement;
  private readonly sliderInputs = new Map<SliderDef['key'], { input: HTMLInputElement; value: HTMLElement }>();

  constructor(
    parent: HTMLElement,
    initial: EngineParams,
    world: { kind: ProceduralKind; seed: number; preset?: string },
    trafficInitial: TrafficSettings,
    pedestriansInitial: PedestrianSettings,
    callbacks: SettingsCallbacks,
  ) {
    this.params = { ...initial };
    this.callbacks = callbacks;

    // Traffic (the drive city): cars driving and parked around the player.
    const traffic = { ...trafficInitial };
    const trafficRows = [
      namedRow({ kind: 'env', name: 'traffic', label: 'Traffic', toggle: true, value: traffic.enabled ? 1 : 0, hint: 'Cars driving the roads and parked at the kerbs' }, (v) => {
        traffic.enabled = v !== 0;
        callbacks.onTraffic({ ...traffic });
      }),
      namedRow({ kind: 'env', name: 'cars', label: 'Cars driving', min: 0, max: 40, step: 1, value: traffic.cars, hint: 'Cars driving around the player' }, (v) => {
        traffic.cars = v;
        callbacks.onTraffic({ ...traffic });
      }),
      namedRow({ kind: 'env', name: 'parked', label: 'Cars parked', min: 0, max: 60, step: 1, value: traffic.parked, hint: 'Cars parked at the kerbs around the player' }, (v) => {
        traffic.parked = v;
        callbacks.onTraffic({ ...traffic });
      }),
      namedRow({ kind: 'env', name: 'speed', label: 'Traffic speed', min: 0.3, max: 2, step: 0.05, value: traffic.speedScale, hint: 'x the roads\' speed limits' }, (v) => {
        traffic.speedScale = v;
        callbacks.onTraffic({ ...traffic });
      }),
    ];

    // People (the drive city): pedestrians on the sidewalks around the player.
    const people = { ...pedestriansInitial };
    const bodiesSelect = h('select', {}, ...Object.values(PedestrianBodies).map((b) => h('option', { value: b }, PEDESTRIAN_BODY_NAMES[b])));
    bodiesSelect.value = String(people.bodies);
    bodiesSelect.addEventListener('change', () => {
      people.bodies = Number(bodiesSelect.value) as PedestrianBodies;
      callbacks.onPedestrians({ ...people });
    });
    const peopleRows = [
      namedRow({ kind: 'env', name: 'people', label: 'People', toggle: true, value: people.enabled ? 1 : 0, hint: 'Pedestrians on the sidewalks, crossing at the zebras' }, (v) => {
        people.enabled = v !== 0;
        callbacks.onPedestrians({ ...people });
      }),
      namedRow({ kind: 'env', name: 'count', label: 'People about', min: 0, max: 96, step: 1, value: people.count, hint: 'Pedestrians around the player' }, (v) => {
        people.count = v;
        callbacks.onPedestrians({ ...people });
      }),
      h(
        'label',
        { class: 'row', title: 'Their bodies: deep (articulations of the world: a car hits a body), shallow (their own), hybrid (deep near the player and moving pieces)' },
        h('span', {}, 'Bodies'),
        bodiesSelect,
      ),
      namedRow({ kind: 'env', name: 'deep', label: 'Deep bodies', min: 0, max: 64, step: 1, value: people.maxDeep, hint: '(hybrid) The most deep bodies: the nearest' }, (v) => {
        people.maxDeep = v;
        callbacks.onPedestrians({ ...people });
      }),
    ];

    const sliders = SLIDERS.map((d) => {
      const range = d.log ? { min: 0, max: LOG_STEPS, step: 1 } : { min: d.min, max: d.max, step: d.step };
      const input = h('input', { type: 'range', ...range, value: toSlider(d, this.params[d.key]) });
      const value = h('span', { class: 'value' }, this.params[d.key].toFixed(2));
      input.addEventListener('input', () => {
        this.params[d.key] = fromSlider(d, Number(input.value));
        value.textContent = this.params[d.key].toFixed(2);
        this.emit();
      });
      this.sliderInputs.set(d.key, { input, value });
      return h('label', { class: 'row', title: d.hint }, h('span', {}, d.label), input, value);
    });

    this.debugSelect = h(
      'select',
      {},
      ...Object.values(DebugView).map((v) => h('option', { value: v }, DEBUG_VIEW_NAMES[v])),
    );
    this.debugSelect.value = String(this.params.debugView);
    this.debugSelect.addEventListener('change', () => this.setDebugView(Number(this.debugSelect.value) as DebugView));

    this.pausedBox = h('input', { type: 'checkbox' });
    this.pausedBox.checked = this.params.paused;
    this.pausedBox.addEventListener('change', () => {
      this.params.paused = this.pausedBox.checked;
      this.emit();
    });

    // World loading: presets (listed by the engine: setPresets), procedural kinds, WADs.
    this.presetSelect = h('select', {});
    const presetSeed = h('input', { type: 'number', value: 0, min: 0, step: 1, class: 'narrow', title: 'Seed (0: the preset\'s own)' });
    const loadPresetButton = h('button', { type: 'button' }, 'Load preset');
    loadPresetButton.addEventListener('click', () => {
      if (!this.presetSelect.value) return;
      callbacks.onLoadPreset(this.presetSelect.value, Math.max(0, Math.floor(Number(presetSeed.value) || 0)));
    });
    const kindSelect = h('select', {}, ...PROCEDURAL_KINDS.map((k) => h('option', { value: k }, k)));
    kindSelect.value = world.kind;
    const seedInput = h('input', { type: 'number', value: world.seed, min: 0, step: 1, class: 'narrow' });
    const loadProc = h('button', { type: 'button' }, 'Load world');
    loadProc.addEventListener('click', () => {
      callbacks.onLoadProcedural(kindSelect.value as ProceduralKind, Math.max(0, Math.floor(Number(seedInput.value) || 0)));
    });

    const fileInput = h('input', { type: 'file', accept: '.wad,.WAD' });
    const mapInput = h('input', { type: 'text', value: 'MAP01', class: 'narrow', spellcheck: false });
    const modeSelect = h('select', {}, h('option', { value: 'rock' }, 'rock'), h('option', { value: 'air' }, 'air'));
    const shellInput = h('input', { type: 'number', value: 8, min: 1, max: 64, class: 'narrow' });
    const bakeBox = h('input', { type: 'checkbox' });
    bakeBox.checked = true;
    const loadWad = h('button', { type: 'button' }, 'Load WAD');
    loadWad.addEventListener('click', () => {
      const file = fileInput.files?.[0];
      if (!file) {
        fileInput.click();
        return;
      }
      callbacks.onLoadWad(file, mapInput.value.trim().toUpperCase() || 'MAP01', {
        mode: modeSelect.value === 'air' ? 'air' : 'rock',
        shellVoxels: Math.max(1, Math.floor(Number(shellInput.value) || 8)),
        bake: bakeBox.checked,
      });
    });

    this.root = h(
      'aside',
      { class: 'panel settings' },
      h('h2', {}, 'Engine'),
      ...sliders,
      h('label', { class: 'row' }, h('span', {}, 'Debug view'), this.debugSelect),
      h('label', { class: 'row' }, h('span', {}, 'Paused'), this.pausedBox),
      ...WORLD.map((d) => namedRow(d, (v) => callbacks.onSetting(d.kind, d.name, v))),
      h('h2', {}, 'Environment'),
      ...ENVIRONMENT.map((d) => namedRow(d, (v) => callbacks.onSetting(d.kind, d.name, v))),
      h('h2', {}, 'Traffic'),
      ...trafficRows,
      h('h2', {}, 'People'),
      ...peopleRows,
      h('h2', {}, 'World'),
      h('div', { class: 'row' }, this.presetSelect, h('span', {}, 'seed'), presetSeed, loadPresetButton),
      h('div', { class: 'row' }, kindSelect, h('span', {}, 'seed'), seedInput, loadProc),
      h('h2', {}, 'Doom WAD'),
      h('div', { class: 'row' }, fileInput),
      h('div', { class: 'row' }, h('span', {}, 'map'), mapInput, h('span', {}, 'void'), modeSelect),
      h('div', { class: 'row' }, h('span', {}, 'shell'), shellInput, h('span', {}, 'bake'), bakeBox, loadWad),
      h(
        'p',
        { class: 'help' },
        'Click the view to play. WASD move, mouse look, Space jump, Shift run, 1-5 or wheel weapons ' +
          '(pistol, shotgun, rockets, flamethrower, water hose), click fire, E use (doors, lifts, switches) ' +
          'or get in and out of a car, B drop a car, G debug view, V noclip, R respawn, H hud, Esc menu. ' +
          'Driving: W/S throttle and brake/reverse, A/D steer, Space handbrake, C camera, mouse look ' +
          '(or a gamepad: RT/LT, left stick, A handbrake, Y in/out).',
      ),
    );
    parent.append(this.root);
  }

  get current(): EngineParams {
    return { ...this.params };
  }

  /** The engine's presets (its `presets` message): the loadable ones, the current one selected. */
  setPresets(presets: readonly PresetInfo[], current: string): void {
    this.presetSelect.replaceChildren(
      ...presets
        .filter((p) => p.available)
        .map((p) => h('option', { value: p.id, title: p.description }, p.experimental ? `${p.label} (experimental)` : p.label)),
    );
    if (presets.some((p) => p.id === current && p.available)) this.presetSelect.value = current;
  }

  setVisible(v: boolean): void {
    this.root.classList.toggle('hidden', !v);
  }

  setDebugView(v: DebugView): void {
    this.params.debugView = v;
    this.debugSelect.value = String(v);
    this.emit();
  }

  cycleDebugView(): void {
    const views = Object.values(DebugView);
    const i = views.indexOf(this.params.debugView);
    this.setDebugView(views[(i + 1) % views.length]!);
  }

  private emit(): void {
    this.callbacks.onParams({ ...this.params });
  }
}
