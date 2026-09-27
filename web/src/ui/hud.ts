/**
 * Heads-up display: frame timing, renderer counters, engine `stats` (known fields plus any
 * engine-specific extras), weapon bar, crosshair and muzzle flash.
 */
import type { DebugView, EngineStats, Vec3 } from '../engine/protocol.ts';
import { DEBUG_VIEW_NAMES } from '../engine/protocol.ts';
import type { WeaponDef } from '../game/weapons.ts';
import type { RenderStats } from '../render/renderer.ts';
import { formatCount, h } from './dom.ts';
import { Timeline } from './timeline.ts';

export interface HudState {
  fps: number;
  frameMs: number;
  render: RenderStats;
  engineKind: string;
  engine: EngineStats | null;
  gpu: string;
  pendingRequests: number;
  weapon: WeaponDef;
  weapons: readonly WeaponDef[];
  player: { pos: Vec3; onGround: boolean; noclip: boolean };
  debugView: DebugView;
  rockets: number;
}

const KNOWN = new Set(['tickMs', 'structuralMs', 'activeBubbles', 'activeNodes', 'voxels', 'chunks', 'memoryMB', 'events']);

function fmt(v: number | string | boolean): string {
  if (typeof v !== 'number') return String(v);
  if (Number.isInteger(v)) return formatCount(v);
  return v.toFixed(2);
}

export class Hud {
  readonly root: HTMLElement;
  private readonly stats: HTMLElement;
  private readonly weaponBar: HTMLElement;
  private readonly flash: HTMLElement;
  private readonly timeline = new Timeline();
  private lastWeapon: WeaponDef | null = null;

  constructor(parent: HTMLElement) {
    this.stats = h('pre', { class: 'hud-stats' });
    this.weaponBar = h('div', { class: 'hud-weapons' });
    this.flash = h('div', { class: 'hud-flash' });
    this.root = h(
      'div',
      { class: 'hud' },
      this.stats,
      this.timeline.root,
      h('div', { class: 'crosshair' }),
      this.flash,
      this.weaponBar,
    );
    parent.append(this.root);
  }

  /** The worker's per-tick samples since the last stats message (engines that send them). */
  pushTimeline(samples: Float32Array): void {
    this.timeline.push(samples);
    this.timeline.draw();
  }

  clearTimeline(): void {
    this.timeline.clear();
    this.timeline.draw();
  }

  setVisible(v: boolean): void {
    this.root.classList.toggle('hidden', !v);
  }

  setMuzzleFlash(on: boolean): void {
    this.flash.classList.toggle('on', on);
  }

  update(s: HudState): void {
    const r = s.render;
    const e = s.engine;
    const lines = [
      `${s.fps.toFixed(0).padStart(3)} fps  ${s.frameMs.toFixed(1)} ms   ${r.width}x${r.height}`,
      `gpu    ${s.gpu}`,
      `draw   ${r.chunksDrawn}/${r.chunksTotal} chunks  ${formatCount(r.triangles)} tris  ${r.gpuMB.toFixed(0)} MB`,
      `fx     ${r.particles} particles  ${r.islands} islands  ${s.rockets} rockets`,
      `engine ${s.engineKind}  ${s.pendingRequests} pending`,
    ];
    if (e) {
      lines.push(
        `tick   ${e.tickMs.toFixed(2)} ms  structural ${e.structuralMs.toFixed(2)} ms`,
        `world  ${formatCount(e.voxels)} voxels  ${formatCount(e.chunks)} chunks  ${e.memoryMB.toFixed(1)} MB`,
        `bubble ${e.activeBubbles} active  ${formatCount(e.activeNodes)} nodes  ${e.events} events`,
      );
      const extras = Object.entries(e).filter(([k]) => !KNOWN.has(k));
      for (let i = 0; i < extras.length; i += 3) {
        const chunk = extras
          .slice(i, i + 3)
          .map(([k, v]) => `${k} ${fmt(v)}`)
          .join('  ');
        lines.push(`${i === 0 ? 'extra ' : '      '} ${chunk}`);
      }
    } else {
      lines.push('engine stats: waiting');
    }
    const p = s.player.pos;
    lines.push(
      `player ${p.map((v) => v.toFixed(1)).join(' ')}${s.player.onGround ? '  ground' : ''}${s.player.noclip ? '  NOCLIP' : ''}`,
      `view   ${DEBUG_VIEW_NAMES[s.debugView]}`,
    );
    this.stats.textContent = lines.join('\n');

    if (s.weapon !== this.lastWeapon) {
      this.lastWeapon = s.weapon;
      this.weaponBar.replaceChildren(
        ...s.weapons.map((w, i) => h('span', { class: w === s.weapon ? 'weapon active' : 'weapon' }, `${i + 1} ${w.name}`)),
      );
    }
  }
}
