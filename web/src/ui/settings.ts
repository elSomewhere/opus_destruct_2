/**
 * Settings panel: engine tunables (sent as `setParams`), the debug view, and world
 * loading (procedural worlds, or a WAD file + map name -> `loadWad`).
 */
import type { EngineParams, ProceduralKind, WadOptions } from '../engine/protocol.ts';
import { DEBUG_VIEW_NAMES, DebugView, PROCEDURAL_KINDS } from '../engine/protocol.ts';
import { h } from './dom.ts';

export interface SettingsCallbacks {
  onParams(params: EngineParams): void;
  onLoadProcedural(kind: ProceduralKind, seed: number): void;
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
  private readonly sliderInputs = new Map<SliderDef['key'], { input: HTMLInputElement; value: HTMLElement }>();

  constructor(parent: HTMLElement, initial: EngineParams, world: { kind: ProceduralKind; seed: number }, callbacks: SettingsCallbacks) {
    this.params = { ...initial };
    this.callbacks = callbacks;

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

    // World loading.
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
      h('h2', {}, 'World'),
      h('div', { class: 'row' }, kindSelect, h('span', {}, 'seed'), seedInput, loadProc),
      h('h2', {}, 'Doom WAD'),
      h('div', { class: 'row' }, fileInput),
      h('div', { class: 'row' }, h('span', {}, 'map'), mapInput, h('span', {}, 'void'), modeSelect),
      h('div', { class: 'row' }, h('span', {}, 'shell'), shellInput, h('span', {}, 'bake'), bakeBox, loadWad),
      h(
        'p',
        { class: 'help' },
        'Click the view to play. WASD move, mouse look, Space jump, Shift run, 1/2/3/4 or wheel weapons (4: knife), ' +
          'click fire, E use (doors, lifts, switches), G debug view, V noclip, R respawn, H hud, Esc menu.',
      ),
    );
    parent.append(this.root);
  }

  get current(): EngineParams {
    return { ...this.params };
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
