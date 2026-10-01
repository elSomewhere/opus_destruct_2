#!/usr/bin/env node
/**
 * "Satellite" plan view: the ground tiles' top materials at a LOD matching
 * the pixel size (roads, lots, parks, fields, forest canopy, snow, seasonal
 * colours), building roofs from the cell plans (shaded by height) and trees.
 *   node scripts/render-sat.js [--cx m] [--cy m] [--size m] [--px 1000] [--preset id] [--variant v] [--season s] [--out file]
 */
import { createWorld } from "../src/engine/world/createWorld.js";
import { worldConfigFromArgs } from "./lib/world.js";
import { groundTile } from "../src/engine/voxel/compose.js";
import { MATERIALS } from "../src/engine/voxel/materials.js";
import { CHUNK } from "../src/engine/core/units.js";
import { P } from "../src/engine/voxel/chunk.js";
import { Canvas } from "./lib/png.js";
import { buildingLook } from "../src/engine/buildings/facade.js";
import { frameOf, turnedFrame } from "../src/engine/buildings/frame.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const cxm = Number(args.cx ?? 0);
const cym = Number(args.cy ?? 0);
const sizeM = Number(args.size ?? 1200);
const px = Number(args.px ?? 1000);
const out = args.out ?? "sat.png";
const world = createWorld(worldConfigFromArgs(args));
const vPerPx = (sizeM * 8) / px;
const lod = Math.max(0, Math.min(6, Math.floor(Math.log2(Math.max(1, vPerPx)))));
const s = 1 << lod;
const x0 = cxm * 8 - (px / 2) * vPerPx;
const y0 = cym * 8 - (px / 2) * vPerPx;
const canvas = new Canvas(px, px);
const t0 = performance.now();
const tiles = new Map();
const tileAt = (cx, cy) => {
  const k = `${cx},${cy}`;
  let t = tiles.get(k);
  if (!t) {
    t = groundTile(world, lod, cx, cy);
    tiles.set(k, t);
  }
  return t;
};
const zs = new Float32Array(px * px);
for (let py = 0; py < px; py += 1)
  for (let pxi = 0; pxi < px; pxi += 1) {
    const wx = x0 + (pxi + 0.5) * vPerPx;
    const wy = y0 + (py + 0.5) * vPerPx;
    const lx = Math.floor(wx / s);
    const ly = Math.floor(wy / s);
    const cx = Math.floor(lx / CHUNK);
    const cy = Math.floor(ly / CHUNK);
    const t = tileAt(cx, cy);
    const idx = lx - cx * CHUNK + 1 + (ly - cy * CHUNK + 1) * P;
    const water = t.water[idx] > t.z[idx];
    const m = water ? (t.ice?.[idx] ? MATERIALS.findIndex((q) => q.name === "ICE") : MATERIALS.findIndex((q) => q.name === "WATER")) : t.top[idx];
    const c = MATERIALS[m].rgb;
    zs[pxi + py * px] = Math.max(t.z[idx], water ? t.water[idx] : -1e9);
    canvas.set(pxi, py, water ? [c[0] * 0.8, c[1] * 0.85, c[2]] : c);
  }
// hillshade from the NW
for (let py = px - 1; py > 0; py -= 1)
  for (let pxi = px - 1; pxi > 0; pxi -= 1) {
    const d = zs[pxi + py * px] - zs[pxi - 1 + (py - 1) * px];
    const k = Math.max(0.7, Math.min(1.3, 1 + (d / vPerPx) * 0.6));
    const i = (py * px + pxi) * 3;
    for (let c = 0; c < 3; c += 1) canvas.px[i + c] = Math.min(255, canvas.px[i + c] * k);
  }
// roofs
const rect = { x0, y0, x1: x0 + px * vPerPx, y1: y0 + px * vPerPx };
const toPx = (x, y) => [(x - x0) / vPerPx, (y - y0) / vPerPx];
for (const env of world.envelopesIn(rect)) {
  const look = buildingLook(env, world.seed);
  const col = MATERIALS[env.roof.type === "flat" ? look.style.roof : look.style.pitched].rgb;
  const wall = MATERIALS[look.style.wall].rgb;
  const snowy = env._snow > 0.3;
  const f = [col, wall];
  for (const t of env.tiers) {
    for (const r0 of t.rects) {
      const r = frameRect(env, r0);
      const [a, b] = toPx(r.x0, r.y0);
      const [c, d] = toPx(r.x1 + 1, r.y1 + 1);
      const shade = 0.75 + Math.min(0.4, (t.f1 + 1) * 0.03);
      const fr = env._frame;
      for (let y = Math.floor(b); y < Math.ceil(d); y += 1)
        for (let x = Math.floor(a); x < Math.ceil(c); x += 1) {
          let edge = x === Math.floor(a) || y === Math.floor(b) || x === Math.ceil(c) - 1 || y === Math.ceil(d) - 1;
          if (fr.turned) {
            // a turned building: the pixels whose centre lies on its roof, the eaves a pixel wide
            const [u, v] = fr.pointFromWorld(x0 + (x + 0.5) * vPerPx, y0 + (y + 0.5) * vPerPx);
            if (u < r0.x0 || u > r0.x1 + 1 || v < r0.y0 || v > r0.y1 + 1) continue;
            edge = u < r0.x0 + vPerPx || u > r0.x1 + 1 - vPerPx || v < r0.y0 + vPerPx || v > r0.y1 + 1 - vPerPx;
          }
          const cc = snowy && !edge ? [236, 240, 244] : edge ? f[1].map((v) => v * 0.6) : f[0].map((v) => v * shade);
          canvas.set(x, y, cc);
        }
    }
  }
  // wings, corner and canted bays (the angled world, S5): their flat roofs in their own axes
  for (const w of env.wings ?? []) {
    const wf = turnedFrame(w.turn, w.U, w.V);
    const roof = MATERIALS[look.style.roof].rgb;
    const [a, b] = toPx(w.bounds.x0, w.bounds.y0);
    const [c, d] = toPx(w.bounds.x1 + 1, w.bounds.y1 + 1);
    for (let y = Math.floor(b); y < Math.ceil(d); y += 1)
      for (let x = Math.floor(a); x < Math.ceil(c); x += 1) {
        const [u, v] = wf.pointFromWorld(x0 + (x + 0.5) * vPerPx, y0 + (y + 0.5) * vPerPx);
        if (u < 0 || u > w.U || v < 0 || v > w.V) continue;
        const edge = u < vPerPx || u > w.U - vPerPx || v < vPerPx;
        canvas.set(x, y, snowy && !edge ? [236, 240, 244] : edge ? f[1].map((q) => q * 0.6) : roof.map((q) => q * 0.8));
      }
  }
}
canvas.save(out);
console.log(`sat ${px}px lod ${lod} ${sizeM} m around (${cxm}, ${cym}) in ${(performance.now() - t0).toFixed(0)} ms -> ${out}`);

function frameRect(env, r) {
  // canonical building frame -> world rect
  env._frame ??= frameOf(env);
  return env._frame.rectToWorld(r);
}
