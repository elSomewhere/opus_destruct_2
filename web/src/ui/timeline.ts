/**
 * Job and budget timeline (plan §B9 debug overlay): the worker's last ticks as stacked bars,
 * one colour per part of the tick (structure solves, rigid pieces, event processing,
 * streaming, the rest of the tick, and the worker's flush of meshes / events / poses after
 * it), against the 8 ms budget and the 16.7 ms frame, with the awake pieces as a line
 * (scaled to the window's peak).
 */
import { TIMELINE_FIELDS, TIMELINE_STRIDE } from '../engine/protocol.ts';
import { h } from './dom.ts';

const TICKS = 240;         // 4 s at 60 Hz
const MS_TOP = 33.3;        // full height: two frames
const AWAKE_TOP_MIN = 50;   // the awake line's full height is at least this many pieces
const COLORS: Record<(typeof TIMELINE_FIELDS)[number], string> = {
  env: '#e8703a',
  structural: '#e0a040',
  rigid: '#a070e0',
  events: '#e05050',
  stream: '#50a0e0',
  other: '#5a5a5a',
  flush: '#909090',
  awake: '#f0f0f0',
};
const BARS = TIMELINE_FIELDS.filter((f) => f !== 'awake');
const AWAKE = TIMELINE_FIELDS.indexOf('awake');

export class Timeline {
  readonly root: HTMLElement;
  private readonly canvas: HTMLCanvasElement;
  private readonly samples = new Float32Array(TICKS * TIMELINE_STRIDE);
  private count = 0;  // samples held (<= TICKS)
  private head = 0;   // next slot

  constructor() {
    this.canvas = h('canvas', { class: 'timeline-canvas', width: TICKS * 2, height: 120 });
    const legend = h(
      'div',
      { class: 'timeline-legend' },
      ...BARS.map((f) => h('span', {}, h('i', { style: `background:${COLORS[f]}` }), f)),
      h('span', {}, h('i', { style: `background:${COLORS.awake}` }), 'awake pieces'),
    );
    this.root = h('div', { class: 'timeline' }, this.canvas, legend);
  }

  push(samples: Float32Array): void {
    for (let o = 0; o + TIMELINE_STRIDE <= samples.length; o += TIMELINE_STRIDE) {
      this.samples.set(samples.subarray(o, o + TIMELINE_STRIDE), this.head * TIMELINE_STRIDE);
      this.head = (this.head + 1) % TICKS;
      this.count = Math.min(TICKS, this.count + 1);
    }
  }

  clear(): void {
    this.count = 0;
    this.head = 0;
  }

  draw(): void {
    const ctx = this.canvas.getContext('2d');
    if (!ctx) return;
    const W = this.canvas.width, H = this.canvas.height;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = 'rgba(0, 0, 0, 0.45)';
    ctx.fillRect(0, 0, W, H);
    const y = (ms: number): number => H - (Math.min(ms, MS_TOP) / MS_TOP) * H;
    const bw = W / TICKS;
    for (let i = 0; i < this.count; i++) {
      const slot = (this.head - this.count + i + TICKS) % TICKS;
      const s = this.samples.subarray(slot * TIMELINE_STRIDE, (slot + 1) * TIMELINE_STRIDE);
      let acc = 0;
      const x = (TICKS - this.count + i) * bw;
      BARS.forEach((f, k) => {
        const v = s[k] ?? 0;
        if (v <= 0) return;
        ctx.fillStyle = COLORS[f];
        ctx.fillRect(x, y(acc + v), Math.max(1, bw - 0.5), y(acc) - y(acc + v));
        acc += v;
      });
    }
    // budgets: 8 ms of engine work per tick, 16.7 ms frame
    ctx.strokeStyle = 'rgba(255, 220, 120, 0.8)';
    ctx.setLineDash([4, 3]);
    for (const ms of [8, 16.7]) {
      ctx.beginPath();
      ctx.moveTo(0, y(ms) + 0.5);
      ctx.lineTo(W, y(ms) + 0.5);
      ctx.stroke();
    }
    ctx.setLineDash([]);
    // awake pieces (full height = the window's peak, rounded up to 50s)
    const awake = (i: number): number => this.samples[((this.head - this.count + i + TICKS) % TICKS) * TIMELINE_STRIDE + AWAKE] ?? 0;
    let peak = 0;
    for (let i = 0; i < this.count; i++) peak = Math.max(peak, awake(i));
    const top = Math.max(AWAKE_TOP_MIN, Math.ceil(peak / 50) * 50);
    ctx.strokeStyle = COLORS.awake;
    ctx.beginPath();
    for (let i = 0; i < this.count; i++) {
      const px = (TICKS - this.count + i + 0.5) * bw, py = H - (awake(i) / top) * H;
      if (i === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.stroke();
    ctx.fillStyle = 'rgba(255, 255, 255, 0.75)';
    ctx.font = '10px monospace';
    ctx.fillText('8 ms', 2, y(8) - 2);
    ctx.fillText('16.7 ms', 2, y(16.7) - 2);
    const label = `${top} awake`;
    ctx.fillText(label, W - 2 - ctx.measureText(label).width, 10);
  }
}
