/**
 * Texture atlas packing: placement, wrapped gutters (so filtering across the manual wrap is
 * seamless), alignment for mips, and linear-space mip generation.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ATLAS_GUTTER, atlasInfoRecords, downsampleSrgb, packAtlas, type AtlasSource } from '../src/render/atlas-pack.ts';

function solidTexture(id: number, w: number, h: number, texel: (x: number, y: number) => [number, number, number]): AtlasSource {
  const rgba = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const [r, g, b] = texel(x, y);
      rgba.set([r, g, b, 255], (y * w + x) * 4);
    }
  return { id, width: w, height: h, rgba: rgba.buffer };
}

test('atlas: entries are placed, aligned and do not overlap', () => {
  const texs = [
    solidTexture(0, 64, 64, () => [200, 0, 0]),
    solidTexture(1, 64, 32, () => [0, 200, 0]),
    solidTexture(2, 32, 32, () => [0, 0, 200]),
    solidTexture(4, 3, 5, () => [9, 9, 9]), // odd size, sparse id
  ];
  const atlas = packAtlas(texs);
  assert.equal(atlas.entries.length, 5);
  assert.equal(atlas.entries[3], null, 'unused id has no entry');
  const rects = atlas.entries.flatMap((e) => (e ? [e] : []));
  for (const e of rects) {
    assert.equal((e.x - ATLAS_GUTTER) % 8, 0, 'block origin is 8-aligned (mip-safe)');
    assert.ok(e.x - ATLAS_GUTTER >= 0 && e.y - ATLAS_GUTTER >= 0);
    assert.ok(e.x + e.width + ATLAS_GUTTER <= atlas.pageSize && e.y + e.height + ATLAS_GUTTER <= atlas.pageSize);
  }
  for (let i = 0; i < rects.length; i++)
    for (let j = i + 1; j < rects.length; j++) {
      const a = rects[i]!;
      const b = rects[j]!;
      if (a.layer !== b.layer) continue;
      const G = ATLAS_GUTTER;
      const sep = a.x + a.width + G <= b.x - G || b.x + b.width + G <= a.x - G || a.y + a.height + G <= b.y - G || b.y + b.height + G <= a.y - G;
      assert.ok(sep, 'blocks including gutters are disjoint');
    }
  const records = atlasInfoRecords(atlas);
  assert.equal(records.length, 5 * 8);
  assert.equal(records[3 * 8 + 6], 0, 'invalid id flagged');
  assert.equal(records[4 * 8 + 6], 1);
  assert.equal(records[4 * 8 + 2], 3);
  assert.equal(records[4 * 8 + 3], 5);
});

test('atlas: gutters hold the wrapped texture', () => {
  const tex = solidTexture(0, 4, 4, (x, y) => [x * 50, y * 50, 7]);
  const atlas = packAtlas([tex]);
  const e = atlas.entries[0]!;
  const page = atlas.pages[e.layer]!;
  const at = (px: number, py: number): number[] => {
    const o = (py * atlas.pageSize + px) * 4;
    return [page[o]!, page[o + 1]!, page[o + 2]!];
  };
  assert.deepEqual(at(e.x, e.y), [0, 0, 7]);
  assert.deepEqual(at(e.x - 1, e.y), [150, 0, 7], 'left gutter = last column');
  assert.deepEqual(at(e.x + 4, e.y + 1), [0, 50, 7], 'right gutter = first column');
  assert.deepEqual(at(e.x - 1, e.y - 1), [150, 150, 7], 'corner wraps both axes');
  assert.deepEqual(at(e.x + 2 - 4 * 2, e.y + 3), [100, 150, 7], 'deep gutter keeps tiling');
});

test('atlas: sRGB mips average in linear space', () => {
  const size = 4;
  const img = new Uint8Array(size * size * 4);
  for (let i = 0; i < size * size; i++) img.set(i % 2 === 0 ? [255, 255, 255, 255] : [0, 0, 0, 255], i * 4);
  const mip = downsampleSrgb(img, size);
  assert.equal(mip.length, 2 * 2 * 4);
  // 50% linear grey encodes to ~188 in sRGB (not 128).
  assert.ok(Math.abs(mip[0]! - 188) <= 1, `got ${mip[0]}`);
  const flat = new Uint8Array(size * size * 4).fill(90);
  assert.ok(downsampleSrgb(flat, size).every((v) => Math.abs(v - 90) <= 1), 'uniform stays uniform');
});
