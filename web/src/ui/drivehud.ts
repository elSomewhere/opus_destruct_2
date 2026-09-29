/**
 * The driving HUD (docs/VEHICLES.md): speed, gear and revs, the car's damage and wheels, the
 * handbrake light, the camera and the controls - and, on foot, the prompt to take a car near by.
 */
import { h } from './dom.ts';

export interface DashState {
  /** m/s along the car's forward. */
  speed: number;
  gear: number;
  rpm: number;
  redline: number;
  damage: number;
  /** Wheels still on, of how many. */
  wheels: number;
  wheelSlots: number;
  handbrake: boolean;
  camera: string;
  kind: string;
  gamepad: boolean;
}

export class DriveHud {
  readonly root: HTMLElement;
  private readonly dash: HTMLElement;
  private readonly speed: HTMLElement;
  private readonly unit: HTMLElement;
  private readonly gear: HTMLElement;
  private readonly revs: HTMLElement;
  private readonly revsFill: HTMLElement;
  private readonly damage: HTMLElement;
  private readonly damageFill: HTMLElement;
  private readonly wheelDots: HTMLElement;
  private readonly brake: HTMLElement;
  private readonly hint: HTMLElement;
  private readonly prompt: HTMLElement;
  private shown = '';

  constructor(parent: HTMLElement) {
    this.speed = h('span', { class: 'dash-speed' }, '0');
    this.unit = h('span', { class: 'dash-unit' }, 'km/h');
    this.gear = h('span', { class: 'dash-gear' }, 'N');
    this.revsFill = h('div', { class: 'dash-bar-fill' });
    this.revs = h('div', { class: 'dash-bar dash-revs' }, this.revsFill);
    this.damageFill = h('div', { class: 'dash-bar-fill' });
    this.damage = h('div', { class: 'dash-bar dash-damage' }, this.damageFill);
    this.wheelDots = h('span', { class: 'dash-wheels' });
    this.brake = h('span', { class: 'dash-brake' }, 'P');
    this.hint = h('div', { class: 'dash-hint' });
    this.dash = h(
      'div',
      { class: 'dash hidden' },
      h('div', { class: 'dash-top' }, h('span', { class: 'dash-readout' }, this.speed, this.unit), this.gear),
      this.revs,
      h('div', { class: 'dash-row' }, h('span', { class: 'dash-label' }, 'body'), this.damage, this.wheelDots, this.brake),
      this.hint,
    );
    this.prompt = h('div', { class: 'drive-prompt hidden' });
    this.root = h('div', { class: 'drive-hud' }, this.dash, this.prompt);
    parent.append(this.root);
  }

  /** The dashboard while driving (null: on foot). */
  update(s: DashState | null): void {
    this.dash.classList.toggle('hidden', s === null);
    if (!s) return;
    const kmh = Math.round(Math.abs(s.speed) * 3.6);
    this.set(this.speed, String(kmh));
    this.set(this.gear, s.gear < 0 ? 'R' : s.gear === 0 ? 'N' : String(s.gear));
    const rev = Math.max(0, Math.min(1, s.rpm / Math.max(1000, s.redline)));
    this.revsFill.style.width = `${(rev * 100).toFixed(1)}%`;
    this.revs.classList.toggle('high', rev > 0.9);
    this.damageFill.style.width = `${(Math.max(0, Math.min(1, s.damage)) * 100).toFixed(1)}%`;
    this.set(this.wheelDots, '●'.repeat(Math.max(0, s.wheels)) + '○'.repeat(Math.max(0, s.wheelSlots - s.wheels)));
    this.wheelDots.classList.toggle('lost', s.wheels < s.wheelSlots);
    this.brake.classList.toggle('on', s.handbrake);
    const keys = s.gamepad
      ? `RT/LT drive · stick steer · A handbrake · Y get out`
      : `W/S drive · A/D steer · Space handbrake · C camera (${s.camera}) · E get out`;
    this.set(this.hint, `${s.kind} — ${keys}`);
  }

  /** The prompt to take a car (null: none near). */
  setPrompt(text: string | null): void {
    if (text === this.shown) return;
    this.shown = text ?? '';
    this.prompt.classList.toggle('hidden', text === null);
    this.prompt.textContent = text ?? '';
  }

  setVisible(v: boolean): void {
    this.root.classList.toggle('hidden', !v);
  }

  private set(el: HTMLElement, text: string): void {
    if (el.textContent !== text) el.textContent = text;
  }
}
