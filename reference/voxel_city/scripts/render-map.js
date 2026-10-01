#!/usr/bin/env node
/**
 * Top-down debug map. Usage:
 *   node scripts/render-map.js [--cx m] [--cy m] [--size m] [--px 1024] [--seed n] [--out file]
 */
import { createWorld } from "../src/engine/world/createWorld.js";
import { worldConfigFromArgs } from "./lib/world.js";
import { DISTRICTS } from "../src/engine/world/registry.js";
import { insideCuts } from "../src/engine/city/blockPoly.js";
import { MATERIALS } from "../src/engine/voxel/materials.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../src/engine/network/roadSurface.js";
import { Canvas, hexToRgb } from "./lib/png.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const cxm = Number(args.cx ?? 0);
const cym = Number(args.cy ?? 0);
const sizeM = Number(args.size ?? 1600);
const px = Number(args.px ?? 1024);
const seed = Number(args.seed ?? 1337);
const out = args.out ?? "map.png";

const world = createWorld(worldConfigFromArgs(args, args.mode === "infinite" ? { world: { mode: "infiniteCity" } } : {}));
const canvas = new Canvas(px, px);
const vPerPx = (sizeM * 8) / px;
const x0 = cxm * 8 - (px / 2) * vPerPx;
const y0 = cym * 8 - (px / 2) * vPerPx;

const t0 = performance.now();
const sample = makeRoadSample();
const cands = [];
const blockColor = new Map();
for (const d of DISTRICTS.all()) blockColor.set(d.id, hexToRgb(d.color));

for (let py = 0; py < px; py += 1) {
  for (let pxi = 0; pxi < px; pxi += 1) {
    const wx = x0 + (pxi + 0.5) * vPerPx;
    const wy = y0 + (py + 0.5) * vPerPx;
    const { i, j } = world.cellAt(wx, wy);
    const view = world.roadView(i, j);
    cands.length = 0;
    view.near({ x0: wx - 1, y0: wy - 1, x1: wx + 1, y1: wy + 1 }, cands);
    sampleRoadSurface(cands, Math.floor(wx) + 0.5, Math.floor(wy) + 0.5, sample, seed);
    let c;
    const hwEdges = world.highways ? world.highways.edgesNear({ x0: wx - 1, y0: wy - 1, x1: wx + 1, y1: wy + 1 }) : [];
    const hn = hwEdges.length ? world.highways.nearest(wx, wy, hwEdges, world.highways.hw) : null;
    if (hn) {
      c = Math.abs(hn.d) > world.highways.hw - 3 ? [200, 200, 190] : [95, 95, 100];
    } else if (sample.kind !== KIND.NONE) {
      c = MATERIALS[sample.mat].rgb;
    } else {
      const net = world.cellNet(i, j);
      // (a polygon block of the angled world: on its side of every slanted edge)
      const b = net.blocks.find((bl) => wx >= bl.prop.x0 && wx <= bl.prop.x1 && wy >= bl.prop.y0 && wy <= bl.prop.y1 && (!bl.cuts || insideCuts(bl.cuts, wx, wy)));
      if (b) c = blockColor.get(b.district);
      else {
        const h = world.terrain.height(wx, wy);
        c = h < 0 ? [40, 80, 120] : [60, 70 + Math.min(80, h / 8), 50];
      }
    }
    canvas.set(pxi, py, c);
  }
}
canvas.save(out);
console.log(`rendered ${px}x${px} (${sizeM} m) in ${(performance.now() - t0).toFixed(0)} ms -> ${out}`);
