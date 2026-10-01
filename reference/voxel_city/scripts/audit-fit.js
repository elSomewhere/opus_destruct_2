#!/usr/bin/env node
/**
 * Fit report: how terrain, roads, lots, buildings and plants fit together
 * around a point (validate/fit.js).
 *   node scripts/audit-fit.js [--cx m] [--cy m] [--size m] [--preset id] [--variant v] [--lod 1] [--thr 4] [--top 10]
 * Ground steps are listed by what meets what (road, lot, space, nature,
 * field) with their worst spots (m), then road level jumps per cell,
 * buildings standing on road surface and plants off the ground.
 */
import { createWorld } from "../src/engine/world/createWorld.js";
import { worldConfigFromArgs } from "./lib/world.js";
import { groundSteps, roadJumps, buildingsOnRoads, plantFit } from "../src/engine/validate/fit.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const world = createWorld(worldConfigFromArgs(args));
const cx = Number(args.cx ?? 0) * 8;
const cy = Number(args.cy ?? 0) * 8;
const half = (Number(args.size ?? 300) * 8) / 2;
const rect = { x0: cx - half, y0: cy - half, x1: cx + half, y1: cy + half };
const top = Number(args.top ?? 10);
const m = (v) => (v / 8).toFixed(1);
const t0 = performance.now();

const steps = groundSteps(world, rect, { lod: Number(args.lod ?? 1), thr: Number(args.thr ?? 4) });
console.log("ground steps (voxels) by what meets what:");
for (const [pair, e] of [...steps.pairs].sort((a, b) => b[1].n - a[1].n)) console.log(`  ${pair.padEnd(14)} n ${String(e.n).padStart(6)}  max ${e.max}`);
const shown = [];
for (const q of steps.worst) {
  if (shown.some((p) => Math.hypot(p.x - q.x, p.y - q.y) < 120)) continue;
  shown.push(q);
  if (shown.length >= top) break;
}
for (const q of shown) console.log(`  ${q.dz} ${q.pair} at (${m(q.x)}, ${m(q.y)})`);

const cells = new Map();
for (const c of world.cellsOverlapping(rect)) cells.set(`${c.i},${c.j}`, c);
console.log("road level jumps (largest per cell, voxels over 2 voxels along):");
for (const c of cells.values()) {
  const r = roadJumps(world, c.i, c.j);
  console.log(`  C${c.i}_${c.j}: ${r.jump.toFixed(2)}${r.road ? ` on ${r.road} at ${m(r.along)} m` : ""}`);
}
const onRoads = buildingsOnRoads(world, rect);
console.log(`buildings on road surface: ${onRoads.length}`, onRoads.slice(0, top).map((b) => `${b.id} (${b.n})`).join(" "));
const plants = plantFit(world, rect);
console.log(`plants off the ground: ${plants.off.length} of ${plants.checked}`);
for (const p of plants.off.slice(0, top)) console.log(`  ${p.what} ${p.kind} ${p.dz > 0 ? "floats" : "buried"} ${Math.abs(p.dz)} at (${m(p.x)}, ${m(p.y)})`);
console.log(`(${((performance.now() - t0) / 1000).toFixed(1)} s)`);
