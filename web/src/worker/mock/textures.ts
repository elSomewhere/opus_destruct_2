/**
 * Procedural, seamlessly tiling RGBA8 textures for the mock worlds. They exercise the
 * texture path of the protocol the same way Doom textures and flats will (mixed sizes,
 * non-square, sRGB bytes, row 0 = top).
 */
import type { TextureInfo } from '../../engine/protocol.ts';

type Rgb = [number, number, number];

/** Integer hash -> [0,1). */
function hash3(x: number, y: number, s: number): number {
  let h = Math.imul(x | 0, 0x27d4eb2d) ^ Math.imul(y | 0, 0x165667b1) ^ Math.imul(s | 0, 0x9e3779b9);
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}

function mod(a: number, n: number): number {
  return ((a % n) + n) % n;
}

/** Periodic value noise: `period` lattice cells per texture edge, so it tiles. */
function valueNoise(u: number, v: number, period: number, seed: number): number {
  const x = u * period;
  const y = v * period;
  const x0 = Math.floor(x);
  const y0 = Math.floor(y);
  const fx = x - x0;
  const fy = y - y0;
  const sx = fx * fx * (3 - 2 * fx);
  const sy = fy * fy * (3 - 2 * fy);
  const a = hash3(mod(x0, period), mod(y0, period), seed);
  const b = hash3(mod(x0 + 1, period), mod(y0, period), seed);
  const c = hash3(mod(x0, period), mod(y0 + 1, period), seed);
  const d = hash3(mod(x0 + 1, period), mod(y0 + 1, period), seed);
  return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

/** Tiling fractal noise in [0,1). */
function fbm(u: number, v: number, basePeriod: number, octaves: number, seed: number): number {
  let sum = 0;
  let amp = 0.5;
  let norm = 0;
  let period = basePeriod;
  for (let o = 0; o < octaves; o++) {
    sum += amp * valueNoise(u, v, period, seed + o * 101);
    norm += amp;
    amp *= 0.5;
    period *= 2;
  }
  return sum / norm;
}

function clamp255(x: number): number {
  return x < 0 ? 0 : x > 255 ? 255 : Math.round(x);
}

function makeTexture(
  name: string,
  width: number,
  height: number,
  shade: (x: number, y: number) => Rgb,
): Omit<TextureInfo, 'id'> {
  const rgba = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const [r, g, b] = shade(x, y);
      const o = (y * width + x) * 4;
      rgba[o] = clamp255(r);
      rgba[o + 1] = clamp255(g);
      rgba[o + 2] = clamp255(b);
      rgba[o + 3] = 255;
    }
  }
  return { name, width, height, rgba: rgba.buffer };
}

function mix(a: Rgb, b: Rgb, t: number): Rgb {
  return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
}

function scale(c: Rgb, k: number): Rgb {
  return [c[0] * k, c[1] * k, c[2] * k];
}

function brick(seed: number): Omit<TextureInfo, 'id'> {
  const W = 64;
  const H = 64;
  const bw = 16;
  const bh = 8;
  const mortar: Rgb = [150, 140, 128];
  return makeTexture('BRICK', W, H, (x, y) => {
    const row = Math.floor(y / bh);
    const off = row % 2 === 0 ? 0 : bw / 2;
    const col = Math.floor(mod(x + off, W) / bw);
    const inX = mod(x + off, bw);
    const inY = y % bh;
    const n = fbm(x / W, y / H, 8, 3, seed);
    if (inX === 0 || inY === 0) return scale(mortar, 0.85 + 0.3 * n);
    const tint = hash3(col, row, seed + 7);
    const base: Rgb = mix([150, 62, 44], [110, 48, 38], tint);
    const edge = inX === 1 || inY === 1 ? 0.85 : inY === bh - 1 || inX === bw - 1 ? 0.92 : 1;
    return scale(base, (0.8 + 0.4 * n) * edge);
  });
}

function concrete(seed: number): Omit<TextureInfo, 'id'> {
  const S = 64;
  return makeTexture('CONCRETE', S, S, (x, y) => {
    const n = fbm(x / S, y / S, 4, 4, seed);
    const pore = hash3(x, y, seed + 3) < 0.025 ? 0.7 : 1;
    const g = (118 + 60 * n) * pore;
    return [g, g * 0.99, g * 0.96];
  });
}

function tile(seed: number): Omit<TextureInfo, 'id'> {
  const S = 64;
  const t = 16;
  return makeTexture('TILE', S, S, (x, y) => {
    const tx = Math.floor(x / t);
    const ty = Math.floor(y / t);
    const n = fbm(x / S, y / S, 8, 2, seed);
    if (x % t === 0 || y % t === 0) return [70 + 20 * n, 66 + 20 * n, 60 + 20 * n];
    const checker = (tx + ty) % 2 === 0;
    const base: Rgb = checker ? [196, 188, 170] : [150, 142, 128];
    return scale(base, 0.9 + 0.15 * n + 0.08 * hash3(tx, ty, seed));
  });
}

function dirt(seed: number): Omit<TextureInfo, 'id'> {
  const S = 64;
  return makeTexture('DIRT', S, S, (x, y) => {
    const n = fbm(x / S, y / S, 4, 4, seed);
    const pebble = hash3(x >> 1, y >> 1, seed + 5) < 0.04 ? 1.25 : 1;
    return scale([112, 84, 58], (0.65 + 0.6 * n) * pebble);
  });
}

function grass(seed: number): Omit<TextureInfo, 'id'> {
  const S = 64;
  return makeTexture('GRASS', S, S, (x, y) => {
    const n = fbm(x / S, y / S, 8, 3, seed);
    const blade = hash3(x, y, seed + 9);
    return mix([60, 92, 40], [96, 128, 56], 0.5 * n + 0.5 * blade);
  });
}

function rock(seed: number): Omit<TextureInfo, 'id'> {
  const S = 64;
  return makeTexture('ROCK', S, S, (x, y) => {
    const n = fbm(x / S, y / S, 4, 5, seed);
    const ridge = 1 - Math.abs(2 * fbm(x / S, y / S, 2, 3, seed + 31) - 1);
    const crack = ridge > 0.93 ? 0.55 : 1;
    return scale([96, 90, 84], (0.6 + 0.7 * n) * crack);
  });
}

/** Deliberately non-square to exercise per-axis wrapping. */
function metal(seed: number): Omit<TextureInfo, 'id'> {
  const W = 64;
  const H = 32;
  return makeTexture('METAL', W, H, (x, y) => {
    const n = fbm(x / W, y / H, 4, 3, seed);
    const px = x % 32;
    const py = y % 32;
    const seam = px === 0 || py === 0 ? 0.6 : 1;
    const rivet = (px === 3 || px === 28) && (py === 3 || py === 28) ? 1.4 : 1;
    const brushed = 0.92 + 0.08 * hash3(0, y, seed + 2);
    return scale([104, 116, 128], (0.75 + 0.4 * n) * seam * rivet * brushed);
  });
}

/** Deliberately small to exercise a tiny texture in the atlas. */
function plaster(seed: number): Omit<TextureInfo, 'id'> {
  const S = 32;
  return makeTexture('PLASTER', S, S, (x, y) => {
    const n = fbm(x / S, y / S, 4, 3, seed);
    return scale([214, 206, 188], 0.85 + 0.2 * n);
  });
}

function hazard(): Omit<TextureInfo, 'id'> {
  const S = 32;
  return makeTexture('HAZARD', S, S, (x, y) => {
    const stripe = Math.floor(mod(x + y, S) / 8) % 2 === 0;
    return stripe ? [210, 170, 30] : [30, 28, 26];
  });
}

/** All mock textures; ids are assigned in list order. */
export function buildMockTextures(seed: number): TextureInfo[] {
  const list = [
    brick(seed),
    concrete(seed),
    tile(seed),
    dirt(seed),
    grass(seed),
    rock(seed),
    metal(seed),
    plaster(seed),
    hazard(),
  ];
  return list.map((t, id) => ({ id, ...t }));
}
