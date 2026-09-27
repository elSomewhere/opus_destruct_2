/**
 * The "Characters" section of the settings panel: populate the world with civilians,
 * soldiers and thugs, AI on/off, god mode, and the animation presentation (smooth, or retro: baked
 * whole-body voxel frames stepped in Doom tics with 8-way facing, fine or chunky voxels).
 */
import { h } from './dom.ts';

export interface CharacterPanelState {
  ai: boolean;
  god: boolean;
  style: 'smooth' | 'retro' | 'retro-chunky';
}

export interface CharacterPanelCallbacks {
  onChange(s: CharacterPanelState): void;
  onSpawn(kind: 'civilian' | 'soldier' | 'thug', count: number): void;
  onClear(): void;
}

export class CharacterPanel {
  readonly root: HTMLElement;
  private readonly state: CharacterPanelState;
  private readonly info: HTMLElement;

  constructor(initial: CharacterPanelState, cb: CharacterPanelCallbacks) {
    this.state = { ...initial };
    const ai = h('input', { type: 'checkbox' });
    ai.checked = initial.ai;
    ai.addEventListener('change', () => {
      this.state.ai = ai.checked;
      cb.onChange({ ...this.state });
    });
    const god = h('input', { type: 'checkbox' });
    god.checked = initial.god;
    god.addEventListener('change', () => {
      this.state.god = god.checked;
      cb.onChange({ ...this.state });
    });
    const style = h(
      'select',
      {},
      h('option', { value: 'smooth' }, 'smooth'),
      h('option', { value: 'retro' }, 'retro (Voxel Doom)'),
      h('option', { value: 'retro-chunky' }, 'retro, chunky voxels'),
    );
    style.value = initial.style;
    style.addEventListener('change', () => {
      this.state.style = style.value as CharacterPanelState['style'];
      cb.onChange({ ...this.state });
    });
    const btn = (label: string, fn: () => void): HTMLButtonElement => {
      const b = h('button', { type: 'button' }, label);
      b.addEventListener('click', fn);
      return b;
    };
    this.info = h('p', { class: 'help' }, '');
    this.root = h(
      'section',
      {},
      h('h2', {}, 'Characters'),
      h('div', { class: 'row' }, btn('+6 civilians', () => cb.onSpawn('civilian', 6)), btn('+4 soldiers', () => cb.onSpawn('soldier', 4)), btn('+3 thugs', () => cb.onSpawn('thug', 3)), btn('clear', cb.onClear)),
      h('label', { class: 'row' }, h('span', {}, 'Animation'), style),
      h('label', { class: 'row' }, h('span', {}, 'AI'), ai, h('span', {}, 'God mode'), god),
      this.info,
    );
  }

  setInfo(text: string): void {
    this.info.textContent = text;
  }
}
