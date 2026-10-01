#!/usr/bin/env node
/**
 * Render interior floor plans of buildings to a PNG sheet.
 *   node scripts/render-plans.js [--cell 0,0] [--arch walkup] [--n 6] [--scale 3] [--out plans.png]
 * Archetypes no district places yet can be staged on real lots:
 *   node scripts/render-plans.js --arch townhouse --stage nordicWood [--from walkup,rowhouse] [--district oldtown] [--plot 16x18]
 */
import { createWorld } from "../src/engine/world/createWorld.js";
import { worldConfigFromArgs } from "./lib/world.js";
import { planBuilding } from "../src/engine/buildings/interior/plan.js";
import { stageArchetype } from "../src/engine/buildings/sample.js";
import { vx } from "../src/engine/core/units.js";
import { Canvas, hexToRgb } from "./lib/png.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const [ci, cj] = (args.cell ?? "0,0").split(",").map(Number);
const arch = args.arch ?? null;
const n = Number(args.n ?? 6);
const scale = Number(args.scale ?? 3);
const out = args.out ?? "plans.png";
const skip = Number(args.skip ?? 0);
// --preset id [--variant size]: a world preset (config/presets.js)
const world = createWorld(worldConfigFromArgs(args));

const COLORS = {
  corridor: "#d8d2c4", lobby: "#f0e4c8", hall: "#ddd5c6", foyer: "#e2d8c4", landing: "#d0c8b8",
  stair: "#8b7f6c", elevator: "#55606c", shaft: "#444444", living: "#e7b98a", kitchen: "#f0d98a",
  dining: "#eecb8a", bedroom: "#a9c7e8", bath: "#8fd8d0", wc: "#8fd8d0", closet: "#c9bba5",
  storage: "#b9ad98", pantry: "#c9bba5", laundry: "#a8d0e8", studio: "#e0c0a0", study: "#c7b8e8",
  office: "#b8c9a4", openOffice: "#d2e2be", meeting: "#9fbf8a", breakroom: "#e8d08a", restroom: "#8fd8d0",
  retail: "#e8a0a0", cafe: "#e8b8a0", restaurant: "#e0a890", backroom: "#c99a9a", warehouse: "#c0c0c8",
  factory: "#b0b0bc", reception: "#e0d0b0", parking: "#9098a0", deck: "#a5a9ae", classroom: "#e8d9a8", gym: "#c9a36b", cafeteria: "#b8e0c8", library: "#b0c8a0", teachers: "#e8c0a0", mechanical: "#888888", bike: "#b0c0a0",
  trash: "#a09888", mailroom: "#c0b090", security: "#a0a8c0", nave: "#d8c8a8",
  // civic buildings, venues, shops (interior/civicPrograms.js)
  orthodoxNave: "#e0c890", void: "#303038", sales: "#f0b0b0", departmentFloor: "#f0c0c0", marketHall: "#e8c090",
  auditorium: "#a04050", venueFloor: "#803060", cinema: "#602838", foyerBar: "#e8c8a0", dressing: "#d0a8c0",
  cloakroom: "#c8b8a0", kiosk: "#f0d0a0", clubroom: "#c0c890", danceHall: "#e0b0d0",
  ward: "#c0e0e8", emergency: "#e89090", exam: "#b0e0e0", operating: "#80c0c0", radiology: "#90a8c0",
  waiting: "#e8e0c0", nurses: "#a8d8c8", pharmacy: "#a0e0a0", policeDesk: "#8098c8", cell: "#707888",
  interview: "#98a0b8", briefing: "#a0b0d0", evidence: "#8890a0", lockerRoom: "#a8b0b0", garageBay: "#b8b0a0",
  dorm: "#a9c7e8", council: "#c8a878", registry: "#d0c0a0", exhibit: "#e0d8c8", gallery: "#f4f0e8",
  museumShop: "#e8b8b0", hotelRoom: "#a9c7e8", distribution: "#c8c0b0", selfStorage: "#b8b8c8",
  coldStore: "#d0e8f0", timberYard: "#c8a878", grocery: "#e8c0a0", bakery: "#f0d0a0", butcher: "#e0a0a0",
  fishmonger: "#a0c0e0", bookshop: "#c0a890", clothing: "#e0b0c8", florist: "#b0e0a0", hardware: "#b0a898",
  barber: "#d0b0b0", pub: "#c09070", bank: "#a0b0c8", laundromat: "#b0d0e0", souvenir: "#e0c0a0", produkty: "#e0c8a0",
};

let envs;
if (args.stage) envs = stageArchetype(world, arch, args.stage, stageOpts(args, skip + n));
else {
  envs = world.cellPlan(ci, cj).buildings;
  if (arch) envs = envs.filter((e) => e.archetype === arch);
}
envs = envs.slice(skip, skip + n);
const sheets = [];
let totalIssues = 0;
for (const env of envs) {
  const t0 = performance.now();
  const plan = planBuilding(world, env);
  const ms = performance.now() - t0;
  if (!plan) continue;
  const unique = [];
  const seen = new Set();
  for (const f of plan.floors) {
    if (seen.has(f.grid)) continue;
    seen.add(f.grid);
    unique.push(f);
  }
  totalIssues += plan.issues.length;
  console.log(
    `${env.id} ${env.archetype} ${env.floors}F+${env.basements}B ${env.U}x${env.V} program=${JSON.stringify(env.program)} unique=${unique.length} stairs=${plan.stairs.length} issues=${plan.issues.length} ${ms.toFixed(1)}ms`,
  );
  for (const is of plan.issues.slice(0, 6)) console.log("   issue", JSON.stringify(is));
  sheets.push({ env, plan, unique });
}

const pad = 12;
let W = 0;
let H = 0;
for (const s of sheets) {
  const w = s.unique.reduce((a, f) => a + f.grid.U * scale + pad, pad);
  W = Math.max(W, w);
  H += s.env.V * scale + pad * 2;
}
const c = new Canvas(Math.max(64, W), Math.max(64, H), [24, 26, 30]);
let y = pad;
for (const s of sheets) {
  let x = pad;
  for (const f of s.unique) {
    const g = f.grid;
    for (let v = 0; v < g.V; v += 1) {
      for (let u = 0; u < g.U; u += 1) {
        const l = g.cells[u + v * g.U];
        let col = null;
        if (l === 1) col = [30, 30, 34];
        else if (l === 2) col = [70, 70, 76];
        else if (l === 3) col = [255, 255, 255];
        else if (l >= 16) col = hexToRgb(COLORS[g.rooms[l - 16].type] ?? "#ff00ff");
        if (col) c.fillRect(x + u * scale, y + v * scale, x + u * scale + scale - 1, y + v * scale + scale - 1, col);
      }
    }
    // doors by kind
    for (const d of g.doors) {
      const col = d.kind === "entrance" || d.kind === "shopfront" || d.kind === "rollup" ? [255, 60, 60] : d.kind === "entry" ? [255, 200, 40] : d.kind === "stair" ? [80, 200, 255] : d.kind === "elevator" ? [160, 80, 255] : [255, 255, 255];
      c.fillRect(x + d.u0 * scale, y + d.v0 * scale, x + d.u1 * scale + scale - 1, y + d.v1 * scale + scale - 1, col);
    }
    // stairs: draw the lane divider + near landing marker
    for (const st of s.plan.stairs) {
      if (f.index < st.f0 || f.index > st.f1) continue;
      const r = st.rect;
      c.strokeRect(x + r.x0 * scale, y + r.y0 * scale, x + r.x1 * scale + scale - 1, y + r.y1 * scale + scale - 1, [255, 240, 120]);
    }
    // front marker
    c.fillRect(x, y - 4, x + g.U * scale - 1, y - 3, [255, 216, 74]);
    x += g.U * scale + pad;
  }
  y += s.env.V * scale + pad * 2;
}
c.save(out);
console.log(`saved ${out} (${c.w}x${c.h}), total issues ${totalIssues}`);

/** Options for stageArchetype from --from, --district and --plot (meters, a plot at the front of each lot). */
function stageOpts(a, count) {
  const plot = a.plot ? a.plot.split("x").map(Number) : null;
  return {
    n: count,
    from: a.from ? a.from.split(",") : null,
    district: a.district ? { id: a.district, floors: [1, 3], archetypes: [], styles: [] } : null,
    lotRect: plot ? (lot, f) => ({ x0: 0, y0: 0, x1: Math.min(f.U, vx(plot[0])) - 1, y1: Math.min(f.V, vx(plot[1])) - 1 }) : null,
  };
}
