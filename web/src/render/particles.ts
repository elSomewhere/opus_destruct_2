/**
 * CPU-simulated particles drawn as instanced camera-facing sprites: dust, chips, sparks,
 * rocket flame and smoke. Storage is structure-of-arrays with swap-remove, so the live
 * set is always dense in [0, count).
 */
import type { Vec3 } from '../engine/protocol.ts';

export interface ParticleSpec {
  pos: Vec3;
  vel: Vec3;
  /** Seconds. */
  life: number;
  /** Sprite radius in metres at birth, and its growth in m/s. */
  size: number;
  grow?: number;
  /** Linear RGB + alpha. */
  color: [number, number, number, number];
  /** Velocity damping per second (0 = none). */
  drag?: number;
  /** Multiplier on gravity (1 = ballistic, negative = rises). */
  gravity?: number;
  additive?: boolean;
}

const FLOATS_PER_INSTANCE = 8;
const GRAVITY = 9.81;

export class ParticleSystem {
  readonly capacity: number;
  private count = 0;
  private readonly pos: Float32Array;
  private readonly vel: Float32Array;
  private readonly age: Float32Array;
  private readonly life: Float32Array;
  private readonly size: Float32Array;
  private readonly grow: Float32Array;
  private readonly color: Float32Array;
  private readonly drag: Float32Array;
  private readonly gravity: Float32Array;
  private readonly additive: Uint8Array;
  private readonly instances: Float32Array<ArrayBuffer>;
  readonly buffer: GPUBuffer;
  private readonly device: GPUDevice;

  constructor(device: GPUDevice, capacity = 16384) {
    this.device = device;
    this.capacity = capacity;
    this.pos = new Float32Array(capacity * 3);
    this.vel = new Float32Array(capacity * 3);
    this.age = new Float32Array(capacity);
    this.life = new Float32Array(capacity);
    this.size = new Float32Array(capacity);
    this.grow = new Float32Array(capacity);
    this.color = new Float32Array(capacity * 4);
    this.drag = new Float32Array(capacity);
    this.gravity = new Float32Array(capacity);
    this.additive = new Uint8Array(capacity);
    this.instances = new Float32Array(capacity * FLOATS_PER_INSTANCE);
    this.buffer = device.createBuffer({
      label: 'particle instances',
      size: this.instances.byteLength,
      usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
    });
  }

  get live(): number {
    return this.count;
  }

  spawn(p: ParticleSpec): void {
    if (this.count >= this.capacity) return;
    const i = this.count++;
    this.pos.set(p.pos, i * 3);
    this.vel.set(p.vel, i * 3);
    this.age[i] = 0;
    this.life[i] = Math.max(0.01, p.life);
    this.size[i] = p.size;
    this.grow[i] = p.grow ?? 0;
    this.color.set(p.color, i * 4);
    this.drag[i] = p.drag ?? 0;
    this.gravity[i] = p.gravity ?? 1;
    this.additive[i] = p.additive ? 1 : 0;
  }

  private moveSlot(from: number, to: number): void {
    this.pos.copyWithin(to * 3, from * 3, from * 3 + 3);
    this.vel.copyWithin(to * 3, from * 3, from * 3 + 3);
    this.color.copyWithin(to * 4, from * 4, from * 4 + 4);
    this.age[to] = this.age[from]!;
    this.life[to] = this.life[from]!;
    this.size[to] = this.size[from]!;
    this.grow[to] = this.grow[from]!;
    this.drag[to] = this.drag[from]!;
    this.gravity[to] = this.gravity[from]!;
    this.additive[to] = this.additive[from]!;
  }

  update(dt: number): void {
    let i = 0;
    while (i < this.count) {
      const age = this.age[i]! + dt;
      if (age >= this.life[i]!) {
        this.moveSlot(--this.count, i);
        continue;
      }
      this.age[i] = age;
      const k = Math.exp(-this.drag[i]! * dt);
      const o = i * 3;
      this.vel[o] = this.vel[o]! * k;
      this.vel[o + 1] = this.vel[o + 1]! * k;
      this.vel[o + 2] = this.vel[o + 2]! * k - GRAVITY * this.gravity[i]! * dt;
      this.pos[o] = this.pos[o]! + this.vel[o]! * dt;
      this.pos[o + 1] = this.pos[o + 1]! + this.vel[o + 1]! * dt;
      this.pos[o + 2] = this.pos[o + 2]! + this.vel[o + 2]! * dt;
      this.size[i] = Math.max(0.001, this.size[i]! + this.grow[i]! * dt);
      i++;
    }
  }

  /** Writes the instance buffer; returns the instance count to draw. */
  upload(): number {
    const n = this.count;
    const out = this.instances;
    for (let i = 0; i < n; i++) {
      const o = i * FLOATS_PER_INSTANCE;
      const t = this.age[i]! / this.life[i]!;
      // Quick fade-in, quadratic fade-out.
      const fade = Math.min(1, t * 12) * (1 - t) * (1 - t);
      out[o] = this.pos[i * 3]!;
      out[o + 1] = this.pos[i * 3 + 1]!;
      out[o + 2] = this.pos[i * 3 + 2]!;
      out[o + 3] = this.additive[i] ? -this.size[i]! : this.size[i]!;
      out[o + 4] = this.color[i * 4]!;
      out[o + 5] = this.color[i * 4 + 1]!;
      out[o + 6] = this.color[i * 4 + 2]!;
      out[o + 7] = this.color[i * 4 + 3]! * fade;
    }
    if (n > 0) this.device.queue.writeBuffer(this.buffer, 0, out, 0, n * FLOATS_PER_INSTANCE);
    return n;
  }

  clear(): void {
    this.count = 0;
  }
}
