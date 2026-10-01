#!/usr/bin/env node
/**
 * Vertical cross-section through the voxel world (caves, bases, subway,
 * sewers, tunnels): a plane along x (or y with --axis y) through a point.
 *   node scripts/render-slice.js --x m --y m [--axis x] [--width m] [--top m] [--bottom m] [--step 1] [--parts 1] [--out file]
 * z range is absolute meters; --step voxels per pixel (0.25: four pixels a
 * voxel, where a pitched road part shows its plane and the world grid its
 * steps). --parts 1 draws the angled world's parts in their own lattices.
 */
import { createWorld } from "../src/engine/world/createWorld.js";
import { worldConfigFromArgs } from "./lib/world.js";
import { buildChunk, groundTile } from "../src/engine/voxel/compose.js";
import { MATERIALS } from "../src/engine/voxel/materials.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { Canvas } from "./lib/png.js";
import { partsIn, partsAt } from "./lib/parts.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const world = createWorld(worldConfigFromArgs(args));
const cx = Math.round(Number(args.x ?? 0) * 8);
const cy = Math.round(Number(args.y ?? 0) * 8);
const alongX = (args.axis ?? "x") === "x";
const half = Math.round((Number(args.width ?? 120) * 8) / 2);
const z1 = Math.round(Number(args.top ?? 60) * 8);
const z0 = Math.round(Number(args.bottom ?? -40) * 8);
const step = Number(args.step ?? 1);
const W = Math.floor((2 * half) / step);
const H = Math.floor((z1 - z0) / step);
const canvas = new Canvas(W, H, [184, 201, 217]);
const cache = new Map();
const at = (x, y, z) => {
  const kx = Math.floor(x / 32);
  const ky = Math.floor(y / 32);
  const kz = Math.floor(z / 32);
  const key = `${kx},${ky},${kz}`;
  let c = cache.get(key);
  if (!c) {
    c = buildChunk(world, 0, kx, ky, kz, groundTile(world, 0, kx, ky));
    cache.set(key, c);
  }
  return c.data[x - kx * 32 + 1 + (y - ky * 32 + 1) * P + (z - kz * 32 + 1) * P2];
};
const parts = world.config.world.angles?.partsMode === "separate" ? partsIn(world, alongX ? { x0: cx - half, y0: cy - 1, x1: cx + half, y1: cy + 1 } : { x0: cx - 1, y0: cy - half, x1: cx + 1, y1: cy + half }) : [];
const t0 = performance.now();
for (let py = 0; py < H; py += 1) {
  // (the middle of the pixel: a voxel's own at one voxel a pixel)
  const z = z1 - py * step + step / 2;
  for (let px = 0; px < W; px += 1) {
    const a = -half + px * step + step / 2;
    const [x, y] = alongX ? [cx + a, cy + 0.5] : [cx + 0.5, cy + a];
    const m = (parts.length && partsAt(world, parts, x, y, z)) || at(Math.floor(x), Math.floor(y), Math.floor(z));
    if (!m) continue;
    const mat = MATERIALS[m];
    const shade = mat.transparent ? 0.8 : 1;
    canvas.set(px, py, mat.rgb.map((v) => Math.min(255, v * shade + mat.emissive * 40)));
  }
}
canvas.save(args.out ?? "slice.png");
console.log(`slice ${W}x${H} (${(2 * half) / 8} m x ${(z1 - z0) / 8} m) in ${(performance.now() - t0).toFixed(0)} ms, ${cache.size} chunks, ${parts.length} parts`);
