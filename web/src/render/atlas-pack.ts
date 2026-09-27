/**
 * Texture atlas packing (pure; no GPU). Textures of any size go into square pages of a
 * 2D array texture. Vertex texcoords are in texels and repeat, so the fragment shader
 * wraps them manually; to keep filtering and mipmapping correct across the wrap:
 * - every texture is surrounded by a gutter filled with its own wrapped texels;
 * - blocks (texture + gutters) are aligned to 2^(mips-1) texels, so box-filtered mips of
 *   one block never mix with a neighbour;
 * - the sampler clamps LOD to mips-1, where a gutter texel still covers the filter footprint.
 */

export const ATLAS_GUTTER = 8;
export const ATLAS_MIP_LEVELS = 4; // levels 0..3: 1 level-3 texel = 8 base texels = gutter
const ALIGN = 1 << (ATLAS_MIP_LEVELS - 1);

export interface AtlasSource {
  id: number;
  width: number;
  height: number;
  rgba: ArrayBuffer;
}

export interface AtlasEntry {
  layer: number;
  /** Atlas texel position of the texture's texel (0,0). */
  x: number;
  y: number;
  /** Texture size in its own texels (the wrap period of vertex texcoords). */
  width: number;
  height: number;
  /** Atlas texels per texture texel (< 1 only for textures too large for a page). */
  scale: number;
}

export interface PackedAtlas {
  pageSize: number;
  layers: number;
  /** Indexed by texture id; null where no texture has that id. */
  entries: (AtlasEntry | null)[];
  /** Level-0 RGBA8 (sRGB) data per page. */
  pages: Uint8Array<ArrayBuffer>[];
}

function align(v: number): number {
  return Math.ceil(v / ALIGN) * ALIGN;
}

function nextPow2(v: number): number {
  let p = 1;
  while (p < v) p *= 2;
  return p;
}

/** Nearest-neighbour downscale by an integer power-of-two factor. */
function downscale(src: Uint8Array, w: number, h: number, factor: number): { data: Uint8Array; w: number; h: number } {
  const nw = Math.max(1, Math.floor(w / factor));
  const nh = Math.max(1, Math.floor(h / factor));
  const out = new Uint8Array(nw * nh * 4);
  for (let y = 0; y < nh; y++)
    for (let x = 0; x < nw; x++) {
      const s = (Math.min(h - 1, y * factor) * w + Math.min(w - 1, x * factor)) * 4;
      out.set(src.subarray(s, s + 4), (y * nw + x) * 4);
    }
  return { data: out, w: nw, h: nh };
}

export function packAtlas(textures: readonly AtlasSource[], maxPageSize = 2048): PackedAtlas {
  const G = ATLAS_GUTTER;
  interface Item {
    src: AtlasSource;
    data: Uint8Array;
    w: number;
    h: number;
    scale: number;
    bw: number;
    bh: number;
  }
  const items: Item[] = [];
  let area = 0;
  let largest = 0;
  for (const t of textures) {
    if (t.width <= 0 || t.height <= 0 || t.rgba.byteLength < t.width * t.height * 4) continue;
    let data: Uint8Array = new Uint8Array(t.rgba, 0, t.width * t.height * 4);
    let w = t.width;
    let h = t.height;
    let factor = 1;
    while (align(w / factor + 2 * G) > maxPageSize || align(h / factor + 2 * G) > maxPageSize) factor *= 2;
    if (factor > 1) ({ data, w, h } = downscale(data, w, h, factor));
    const bw = align(w + 2 * G);
    const bh = align(h + 2 * G);
    items.push({ src: t, data, w, h, scale: 1 / factor, bw, bh });
    area += bw * bh;
    largest = Math.max(largest, bw, bh);
  }
  const pageSize = Math.min(maxPageSize, nextPow2(Math.max(64, largest, Math.ceil(Math.sqrt(area * 1.3)))));

  // Shelf packing, tallest first.
  items.sort((a, b) => b.bh - a.bh || b.bw - a.bw);
  const placed: { item: Item; layer: number; x: number; y: number }[] = [];
  let layer = 0;
  let x = 0;
  let y = 0;
  let shelf = 0;
  for (const item of items) {
    if (x + item.bw > pageSize) {
      y += shelf;
      x = 0;
      shelf = 0;
    }
    if (y + item.bh > pageSize) {
      layer++;
      x = 0;
      y = 0;
      shelf = 0;
    }
    placed.push({ item, layer, x, y });
    x += item.bw;
    shelf = Math.max(shelf, item.bh);
  }
  const layers = items.length > 0 ? layer + 1 : 1;
  const pages: Uint8Array<ArrayBuffer>[] = [];
  for (let l = 0; l < layers; l++) pages.push(new Uint8Array(pageSize * pageSize * 4));

  const maxId = textures.reduce((m, t) => Math.max(m, t.id), -1);
  const entries: (AtlasEntry | null)[] = new Array<AtlasEntry | null>(maxId + 1).fill(null);
  for (const { item, layer: l, x: bx, y: by } of placed) {
    const page = pages[l]!;
    const { w, h, data } = item;
    for (let j = 0; j < item.bh; j++) {
      const sy = (((j - G) % h) + h) % h;
      for (let i = 0; i < item.bw; i++) {
        const sx = (((i - G) % w) + w) % w;
        const s = (sy * w + sx) * 4;
        const d = ((by + j) * pageSize + bx + i) * 4;
        page[d] = data[s]!;
        page[d + 1] = data[s + 1]!;
        page[d + 2] = data[s + 2]!;
        page[d + 3] = data[s + 3]!;
      }
    }
    entries[item.src.id] = {
      layer: l,
      x: bx + G,
      y: by + G,
      width: item.src.width,
      height: item.src.height,
      scale: item.scale,
    };
  }
  return { pageSize, layers, entries, pages };
}

const SRGB_TO_LINEAR = new Float32Array(256).map((_, i) => {
  const c = i / 255;
  return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
});

function linearToSrgbByte(v: number): number {
  const c = v <= 0.0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - 0.055;
  return Math.max(0, Math.min(255, Math.round(c * 255)));
}

/** Next mip level of an sRGB RGBA8 square image (2x2 box filter in linear space). */
export function downsampleSrgb(src: Uint8Array, size: number): Uint8Array<ArrayBuffer> {
  const n = size >> 1;
  const out = new Uint8Array(n * n * 4);
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      const a = ((2 * y) * size + 2 * x) * 4;
      const b = a + 4;
      const c = a + size * 4;
      const d = c + 4;
      const o = (y * n + x) * 4;
      for (let k = 0; k < 3; k++) {
        const v = (SRGB_TO_LINEAR[src[a + k]!]! + SRGB_TO_LINEAR[src[b + k]!]! + SRGB_TO_LINEAR[src[c + k]!]! + SRGB_TO_LINEAR[src[d + k]!]!) / 4;
        out[o + k] = linearToSrgbByte(v);
      }
      out[o + 3] = (src[a + 3]! + src[b + 3]! + src[c + 3]! + src[d + 3]! + 2) >> 2;
    }
  }
  return out;
}

export function mipLevelCount(pageSize: number): number {
  return Math.min(ATLAS_MIP_LEVELS, Math.floor(Math.log2(pageSize)) + 1);
}

/**
 * Per-texture shader records: 8 floats per id = rect (x, y, width, height) and
 * (layer, scale, valid, 0). Invalid ids have valid = 0 and render untextured.
 */
export function atlasInfoRecords(atlas: PackedAtlas): Float32Array<ArrayBuffer> {
  const out = new Float32Array(Math.max(1, atlas.entries.length) * 8);
  atlas.entries.forEach((e, id) => {
    if (!e) return;
    out.set([e.x, e.y, e.width, e.height, e.layer, e.scale, 1, 0], id * 8);
  });
  return out;
}
