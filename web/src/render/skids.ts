/**
 * Skid marks: the rubber sliding tyres lay down on the road (docs/VEHICLES.md). A ring of quads
 * on the ground (the oldest overwritten), each a stretch of one tyre's track between two frames,
 * drawn translucent over the opaque world. The game feeds each wheel's contact point while it
 * slides (`track`); a wheel that stops sliding or leaves the ground ends its mark.
 */
import type { Vec3 } from '../engine/protocol.ts';

/** Quads kept (the oldest are overwritten). */
const CAPACITY = 6000;
const FLOATS_PER_VERTEX = 4;
/** Height above the contact point (no fighting with the road's depth). */
const LIFT = 0.012;

interface Track {
  pos: Vec3;
  alpha: number;
  t: number;
}

export class SkidMarks {
  private readonly device: GPUDevice;
  private readonly vbuf: GPUBuffer;
  private readonly ibuf: GPUBuffer;
  private readonly data = new Float32Array(CAPACITY * 4 * FLOATS_PER_VERTEX);
  private head = 0;
  private filled = 0;
  private dirtyLo = Infinity;
  private dirtyHi = -1;
  private readonly tracks = new Map<number, Track>();

  constructor(device: GPUDevice) {
    this.device = device;
    this.vbuf = device.createBuffer({ label: 'skid marks', size: this.data.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    const idx = new Uint32Array(CAPACITY * 6);
    for (let q = 0; q < CAPACITY; q++) {
      const v = q * 4;
      idx.set([v, v + 1, v + 2, v, v + 2, v + 3], q * 6);
    }
    this.ibuf = device.createBuffer({ label: 'skid mark indices', size: idx.byteLength, usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(this.ibuf, 0, idx);
  }

  get count(): number {
    return this.filled;
  }

  /**
   * A wheel's contact point this frame: `intensity` 0..1 how hard it slides (0: it rolls, or is off
   * the ground - its mark ends), `width` of its tread (m), `up` the ground's normal.
   */
  track(wheel: number, contact: Vec3, up: Vec3, width: number, intensity: number, nowS: number): void {
    const last = this.tracks.get(wheel);
    if (intensity <= 0) {
      this.tracks.delete(wheel);
      return;
    }
    const alpha = Math.min(0.85, 0.25 + 0.6 * intensity);
    const pos: Vec3 = [contact[0] + up[0] * LIFT, contact[1] + up[1] * LIFT, contact[2] + up[2] * LIFT];
    if (!last || nowS - last.t > 0.2) {
      this.tracks.set(wheel, { pos, alpha: 0, t: nowS });
      return;
    }
    const d: Vec3 = [pos[0] - last.pos[0], pos[1] - last.pos[1], pos[2] - last.pos[2]];
    const len = Math.hypot(d[0], d[1], d[2]);
    if (len < 0.12) return; // (too short a stretch yet: wait for more)
    if (len > 3) {
      this.tracks.set(wheel, { pos, alpha: 0, t: nowS });
      return;
    }
    // across the track, in the ground's plane
    let s: Vec3 = [up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2], up[0] * d[1] - up[1] * d[0]];
    const sl = Math.hypot(s[0], s[1], s[2]) || 1;
    const hw = width * 0.45;
    s = [(s[0] / sl) * hw, (s[1] / sl) * hw, (s[2] / sl) * hw];
    this.quad(
      [last.pos[0] - s[0], last.pos[1] - s[1], last.pos[2] - s[2]],
      [last.pos[0] + s[0], last.pos[1] + s[1], last.pos[2] + s[2]],
      [pos[0] + s[0], pos[1] + s[1], pos[2] + s[2]],
      [pos[0] - s[0], pos[1] - s[1], pos[2] - s[2]],
      last.alpha,
      alpha,
    );
    this.tracks.set(wheel, { pos, alpha, t: nowS });
  }

  /** Forgets wheels not fed for a while (gone). */
  prune(nowS: number): void {
    for (const [id, t] of this.tracks) if (nowS - t.t > 1) this.tracks.delete(id);
  }

  private quad(a: Vec3, b: Vec3, c: Vec3, d: Vec3, alphaFrom: number, alphaTo: number): void {
    const q = this.head;
    const o = q * 4 * FLOATS_PER_VERTEX;
    this.data.set([a[0], a[1], a[2], alphaFrom, b[0], b[1], b[2], alphaFrom, c[0], c[1], c[2], alphaTo, d[0], d[1], d[2], alphaTo], o);
    this.dirtyLo = Math.min(this.dirtyLo, q);
    this.dirtyHi = Math.max(this.dirtyHi, q);
    this.head = (q + 1) % CAPACITY;
    this.filled = Math.min(CAPACITY, this.filled + 1);
    // (wrapped: the dirty range is the whole ring - rare)
    if (this.head === 0) {
      this.dirtyLo = 0;
      this.dirtyHi = CAPACITY - 1;
    }
  }

  /** Uploads what changed; call once per frame before drawing. */
  upload(): void {
    if (this.dirtyHi < this.dirtyLo) return;
    const f = FLOATS_PER_VERTEX * 4;
    this.device.queue.writeBuffer(this.vbuf, this.dirtyLo * f * 4, this.data, this.dirtyLo * f, (this.dirtyHi - this.dirtyLo + 1) * f);
    this.dirtyLo = Infinity;
    this.dirtyHi = -1;
  }

  /** Draws the marks (the skid pipeline must be set). */
  draw(pass: GPURenderPassEncoder): number {
    if (this.filled === 0) return 0;
    pass.setVertexBuffer(0, this.vbuf);
    pass.setIndexBuffer(this.ibuf, 'uint32');
    pass.drawIndexed(this.filled * 6);
    return this.filled * 2;
  }

  clear(): void {
    this.data.fill(0);
    this.head = 0;
    this.filled = 0;
    this.tracks.clear();
    this.dirtyLo = 0;
    this.dirtyHi = CAPACITY - 1;
  }
}
