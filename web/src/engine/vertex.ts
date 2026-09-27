/**
 * Read/write helpers for the 28-byte chunk vertex format (protocol.ts, docs/API.md).
 * Environment-neutral: used by the mock worker (writing), the renderer (bounds) and tests.
 *
 * Layout per vertex, with the typed-array indices used below:
 *   bytes  0..11  float32x3 position   -> f32[i*7 + 0..2]
 *   bytes 12..15  snorm8x4  normal+AO  -> i8[i*28 + 12..15]
 *   bytes 16..23  float32x2 uv (texels)-> f32[i*7 + 4..5]
 *   bytes 24..25  uint16    texture id -> u16[i*14 + 12]
 *   byte  26      uint8     light      -> u8[i*28 + 26]
 *   byte  27      uint8     debug      -> u8[i*28 + 27]
 */
import type { Aabb, MeshData, Vec3 } from './protocol.ts';
import { VERTEX_STRIDE } from './protocol.ts';

const F32_PER_VERTEX = VERTEX_STRIDE / 4; // 7
const U16_PER_VERTEX = VERTEX_STRIDE / 2; // 14

function snorm8(x: number): number {
  const c = x < -1 ? -1 : x > 1 ? 1 : x;
  return Math.round(c * 127);
}

/** Growable interleaved vertex + uint32 index buffers producing exact-size MeshData. */
export class MeshBuilder {
  private vbuf: ArrayBuffer;
  private f32: Float32Array<ArrayBuffer>;
  private i8: Int8Array<ArrayBuffer>;
  private u16: Uint16Array<ArrayBuffer>;
  private u8: Uint8Array<ArrayBuffer>;
  private idx: Uint32Array<ArrayBuffer>;
  vertexCount = 0;
  indexCount = 0;

  constructor(vertexCapacity = 1024) {
    const cap = Math.max(4, vertexCapacity);
    this.vbuf = new ArrayBuffer(cap * VERTEX_STRIDE);
    this.f32 = new Float32Array(this.vbuf);
    this.i8 = new Int8Array(this.vbuf);
    this.u16 = new Uint16Array(this.vbuf);
    this.u8 = new Uint8Array(this.vbuf);
    this.idx = new Uint32Array(Math.ceil(cap * 1.5));
  }

  private growVertices(min: number): void {
    const cap = Math.max(min, (this.vbuf.byteLength / VERTEX_STRIDE) * 2);
    const next = new ArrayBuffer(cap * VERTEX_STRIDE);
    new Uint8Array(next).set(new Uint8Array(this.vbuf, 0, this.vertexCount * VERTEX_STRIDE));
    this.vbuf = next;
    this.f32 = new Float32Array(next);
    this.i8 = new Int8Array(next);
    this.u16 = new Uint16Array(next);
    this.u8 = new Uint8Array(next);
  }

  private growIndices(min: number): void {
    const next = new Uint32Array(Math.max(min, this.idx.length * 2));
    next.set(this.idx.subarray(0, this.indexCount));
    this.idx = next;
  }

  /**
   * Appends a vertex and returns its index. `ao` is 0 (fully occluded) .. 1 (open);
   * the normal must be unit length.
   */
  vertex(
    px: number,
    py: number,
    pz: number,
    nx: number,
    ny: number,
    nz: number,
    ao: number,
    u: number,
    v: number,
    texture: number,
    light: number,
    debug: number,
  ): number {
    const i = this.vertexCount;
    if ((i + 1) * VERTEX_STRIDE > this.vbuf.byteLength) this.growVertices(i + 1);
    const f = i * F32_PER_VERTEX;
    this.f32[f] = px;
    this.f32[f + 1] = py;
    this.f32[f + 2] = pz;
    this.f32[f + 4] = u;
    this.f32[f + 5] = v;
    const b = i * VERTEX_STRIDE;
    this.i8[b + 12] = snorm8(nx);
    this.i8[b + 13] = snorm8(ny);
    this.i8[b + 14] = snorm8(nz);
    this.i8[b + 15] = snorm8(ao * 2 - 1);
    this.u16[i * U16_PER_VERTEX + 12] = texture & 0xffff;
    this.u8[b + 26] = light & 0xff;
    this.u8[b + 27] = debug & 0xff;
    this.vertexCount = i + 1;
    return i;
  }

  triangle(a: number, b: number, c: number): void {
    const n = this.indexCount;
    if (n + 3 > this.idx.length) this.growIndices(n + 3);
    this.idx[n] = a;
    this.idx[n + 1] = b;
    this.idx[n + 2] = c;
    this.indexCount = n + 3;
  }

  /** Copies out exact-size buffers (safe to transfer); the builder can be reset and reused. */
  finish(): MeshData {
    return {
      vertices: this.vbuf.slice(0, this.vertexCount * VERTEX_STRIDE),
      vertexCount: this.vertexCount,
      indices: this.idx.buffer.slice(0, this.indexCount * 4),
      indexCount: this.indexCount,
    };
  }

  reset(): void {
    this.vertexCount = 0;
    this.indexCount = 0;
  }
}

/** Decoded vertex, for tests and debugging (slow path). */
export interface DecodedVertex {
  pos: Vec3;
  normal: Vec3;
  ao: number;
  uv: [number, number];
  texture: number;
  light: number;
  debug: number;
}

export function decodeVertex(vertices: ArrayBuffer, i: number): DecodedVertex {
  const dv = new DataView(vertices, i * VERTEX_STRIDE, VERTEX_STRIDE);
  const sn = (o: number): number => Math.max(dv.getInt8(o) / 127, -1);
  return {
    pos: [dv.getFloat32(0, true), dv.getFloat32(4, true), dv.getFloat32(8, true)],
    normal: [sn(12), sn(13), sn(14)],
    ao: sn(15) * 0.5 + 0.5,
    uv: [dv.getFloat32(16, true), dv.getFloat32(20, true)],
    texture: dv.getUint16(24, true),
    light: dv.getUint8(26),
    debug: dv.getUint8(27),
  };
}

/** Axis-aligned bounds of the vertex positions; null for an empty mesh. */
export function vertexBounds(vertices: ArrayBuffer, vertexCount: number): Aabb | null {
  if (vertexCount <= 0) return null;
  const f = new Float32Array(vertices, 0, vertexCount * F32_PER_VERTEX);
  let x0 = Infinity;
  let y0 = Infinity;
  let z0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  let z1 = -Infinity;
  for (let o = 0; o < f.length; o += F32_PER_VERTEX) {
    const x = f[o]!;
    const y = f[o + 1]!;
    const z = f[o + 2]!;
    if (x < x0) x0 = x;
    if (x > x1) x1 = x;
    if (y < y0) y0 = y;
    if (y > y1) y1 = y;
    if (z < z0) z0 = z;
    if (z > z1) z1 = z;
  }
  return { min: [x0, y0, z0], max: [x1, y1, z1] };
}
