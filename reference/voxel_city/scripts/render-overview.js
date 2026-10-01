#!/usr/bin/env node
/**
 * World overview (biomes, relief, settlements, sites, highways).
 *   node scripts/render-overview.js [--cx m] [--cy m] [--size m] [--px 800] [--seed n] [--face f] [--out file]
 *   --face f renders one face of the cube-sphere planet (0-3 equator, 4 north, 5 south)
 *   --preset id [--variant size] renders a world preset (e.g. --preset nordicIsland --variant large)
 */
import { createWorld } from "../src/engine/world/createWorld.js";
import { overview } from "../src/engine/stream/queries.js";
import { Canvas } from "./lib/png.js";
import { worldConfigFromArgs } from "./lib/world.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const cx = Number(args.cx ?? 0) * 8;
const cy = Number(args.cy ?? 0) * 8;
const size = Number(args.size ?? 60000) * 8;
const px = Number(args.px ?? 800);
const planet = args.face !== undefined ? { chart: "cube", planet: { radius: Number(args.radius ?? 240000), face: Number(args.face) } } : {};
const world = createWorld(worldConfigFromArgs(args, planet));
const t0 = performance.now();
const rect = { x0: cx - size / 2, y0: cy - size / 2, x1: cx + size / 2, y1: cy + size / 2 };
const ov = overview(world, rect, px, px);
const canvas = new Canvas(px, px);
for (let j = 0; j < px; j += 1)
  for (let i = 0; i < px; i += 1) {
    const o = (i + j * px) * 4;
    canvas.set(i, j, [ov.rgba[o], ov.rgba[o + 1], ov.rgba[o + 2]]);
  }
const toPx = (x, y) => [Math.round(((x - rect.x0) / (rect.x1 - rect.x0)) * px), Math.round(((y - rect.y0) / (rect.y1 - rect.y0)) * px)];
const overlays = size <= 120000 * 8;
for (const h of overlays ? ov.highways : []) {
  for (let k = 0; k + 1 < h.pts.length; k += 1) {
    const [ax, ay] = toPx(...h.pts[k]);
    const [bx, by] = toPx(...h.pts[k + 1]);
    const n = Math.max(Math.abs(bx - ax), Math.abs(by - ay), 1);
    for (let s = 0; s <= n; s += 1) canvas.set(Math.round(ax + ((bx - ax) * s) / n), Math.round(ay + ((by - ay) * s) / n), [240, 190, 70]);
  }
}
for (const st of overlays ? ov.sites : []) {
  const [ax, ay] = toPx(st.rect.x0, st.rect.y0);
  const [bx, by] = toPx(st.rect.x1, st.rect.y1);
  for (let x = ax - 2; x <= bx + 2; x += 1) for (const y of [ay - 2, by + 2]) canvas.set(x, y, [220, 60, 60]);
  for (let y = ay - 2; y <= by + 2; y += 1) for (const x of [ax - 2, bx + 2]) canvas.set(x, y, [220, 60, 60]);
}
for (const c of overlays ? [...ov.cities, ...ov.villages] : []) {
  const [ax, ay] = toPx(c.x, c.y);
  const r = Math.max(2, (c.r / (rect.x1 - rect.x0)) * px);
  for (let a = 0; a < 64; a += 1) canvas.set(Math.round(ax + Math.cos((a / 64) * Math.PI * 2) * r), Math.round(ay + Math.sin((a / 64) * Math.PI * 2) * r), c.flavor ? [255, 255, 255] : [250, 220, 120]);
}
canvas.save(args.out ?? "overview.png");
console.log(`overview ${px}px ${(size / 8000).toFixed(0)} km in ${(performance.now() - t0).toFixed(0)} ms; biomes: ${ov.legend.map((b) => b.id).join(", ")}; cities ${ov.cities.length}`);
