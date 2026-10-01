#!/usr/bin/env node
/**
 * Headless isometric voxel render (raycast through engine chunks).
 *   node scripts/render-iso.js --x m --y m [--size m] [--lod 0] [--clip m] [--focus m] [--px 900] [--yaw deg] [--pitch deg] [--face f] [--parts 1]
 * --parts 1 (LOD 0) draws the angled world's parts in their own lattices:
 * turned buildings with straight walls, pitched roads as one plane.
 * --snow c sets config.world.climate.snowCover (roof snow). Archetypes no
 * district places yet can be staged on real lots (see render-plans.js):
 *   node scripts/render-iso.js --arch cabin --stage cabin --from house --plot 16x18 [--pick k]
 */
import { createWorld } from "../src/engine/world/createWorld.js";
import { buildChunk, groundTile } from "../src/engine/voxel/compose.js";
import { MATERIALS } from "../src/engine/voxel/materials.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { Canvas } from "./lib/png.js";
import { worldConfigFromArgs } from "./lib/world.js";
import { stageArchetype } from "../src/engine/buildings/sample.js";
import { vx } from "../src/engine/core/units.js";
import { partsIn, partHit } from "./lib/parts.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
let cxm = Number(args.x ?? 0);
let cym = Number(args.y ?? 0);
let size = Number(args.size ?? 48);
const lod = Number(args.lod ?? 0);
let clip = args.clip !== undefined ? Number(args.clip) * 8 : Infinity;
const px = Number(args.px ?? 900);
const yaw = ((Number(args.yaw ?? 45) * Math.PI) / 180);
const pitch = ((Number(args.pitch ?? 35) * Math.PI) / 180);
const out = args.out ?? "iso.png";
const worldCfg = args.infinite ? { mode: "infiniteCity" } : args.face !== undefined ? { chart: "cube", planet: { radius: 240000, face: Number(args.face) } } : {};
if (args.snow !== undefined) worldCfg.climate = { snowCover: Number(args.snow) };
// --preset id [--variant size]: a world preset (config/presets.js)
const world = createWorld(worldConfigFromArgs(args, { world: worldCfg }));
if (args.arch) {
  const [ci, cj] = (args.cell ?? "0,0").split(",").map(Number);
  const envs = args.stage
    ? stageArchetype(world, args.arch, args.stage, stageOpts(args, Math.max(Number(args.n ?? 0), Number(args.pick ?? 0) + 1)))
    : world.cellPlan(ci, cj).buildings.filter((e) => e.archetype === args.arch);
  const env = envs[Number(args.pick ?? 0)];
  if (!env) throw new Error("no building of that archetype");
  cxm = ((env.R.x0 + env.R.x1) / 2) / 8;
  cym = ((env.R.y0 + env.R.y1) / 2) / 8;
  size = args.size !== undefined ? Number(args.size) : Math.max(env.R.x1 - env.R.x0, env.R.y1 - env.R.y0) / 8 + 6;
  if (args.floor !== undefined) {
    const f = Number(args.floor);
    let z = env.baseZ;
    for (let k = 0; k < f; k += 1) z += env.storyH[k];
    clip = z + env.storyH[f] - 4;
  }
  console.log("building", env.id, env.archetype, env.floors, "floors", env.style);
}

const s = 1 << lod;
const chunks = new Map();
function chunkAt(cx, cy, cz) {
  const k = `${cx},${cy},${cz}`;
  let c = chunks.get(k);
  if (c === undefined) {
    const tile = groundTile(world, lod, cx, cy);
    c = buildChunk(world, lod, cx, cy, cz, tile);
    chunks.set(k, c);
  }
  return c;
}
function voxel(x, y, z) {
  // x,y,z in LOD voxel units
  const cx = Math.floor(x / 32);
  const cy = Math.floor(y / 32);
  const cz = Math.floor(z / 32);
  const c = chunkAt(cx, cy, cz);
  return c.data[x - cx * 32 + 1 + (y - cy * 32 + 1) * P + (z - cz * 32 + 1) * P2];
}

const cx0 = (cxm * 8) / s;
const cy0 = (cym * 8) / s;
const half = (size * 8) / s / 2;
const tile = groundTile(world, lod, Math.floor(cx0 / 32), Math.floor(cy0 / 32));
// --focus m: height of the view center (deep underground levels)
const gz = (args.focus !== undefined ? Number(args.focus) * 8 : args.arch ? Math.min(tile.z[16 + 16 * P], clip - 20) : tile.z[16 + 16 * P]) / s;
const zMax = Math.min(clip / s, gz + (Number(args.height ?? 120) * 8) / s);
const zMin = gz - (Number(args.depth ?? 6) * 8) / s;

// camera: orthographic, looking along dir
const dir = [-Math.cos(pitch) * Math.cos(yaw), -Math.cos(pitch) * Math.sin(yaw), -Math.sin(pitch)];
const right = [-Math.sin(yaw), Math.cos(yaw), 0];
const up = [right[1] * dir[2] - right[2] * dir[1], right[2] * dir[0] - right[0] * dir[2], right[0] * dir[1] - right[1] * dir[0]];
const canvas = new Canvas(px, px, [184, 201, 217]);
// parts mode: the parts in view, each drawn in its own lattice
const parts = world.config.world.angles?.partsMode === "separate" && lod === 0 ? partsIn(world, { x0: cx0 - half, y0: cy0 - half, x1: cx0 + half, y1: cy0 + half }) : [];
const inView = (x, y, z) => Math.abs(x - cx0) <= half + 1 && Math.abs(y - cy0) <= half + 1 && z <= zMax + 1 && z >= zMin;
const extent = half * 2.2;
const sun = [0.45, -0.35, 0.82];
const sl = Math.hypot(...sun);
const t0 = performance.now();
for (let py = 0; py < px; py += 1) {
  for (let pxi = 0; pxi < px; pxi += 1) {
    const sx = (pxi / px - 0.5) * extent;
    const sy = (0.5 - py / px) * extent;
    // start far back along -dir from the focus plane
    const fx = cx0 + right[0] * sx + up[0] * sy;
    const fy = cy0 + right[1] * sx + up[1] * sy;
    const fz = gz + right[2] * sx + up[2] * sy;
    const back = 4 * half + (zMax - gz);
    let ox = fx - dir[0] * back;
    let oy = fy - dir[1] * back;
    let oz = fz - dir[2] * back;
    // DDA
    let ix = Math.floor(ox);
    let iy = Math.floor(oy);
    let iz = Math.floor(oz);
    const stepX = dir[0] > 0 ? 1 : -1;
    const stepY = dir[1] > 0 ? 1 : -1;
    const stepZ = dir[2] > 0 ? 1 : -1;
    const tdx = Math.abs(1 / dir[0]);
    const tdy = Math.abs(1 / dir[1]);
    const tdz = Math.abs(1 / dir[2]);
    let tmx = (dir[0] > 0 ? ix + 1 - ox : ox - ix) * tdx;
    let tmy = (dir[1] > 0 ? iy + 1 - oy : oy - iy) * tdy;
    let tmz = (dir[2] > 0 ? iz + 1 - oz : oz - iz) * tdz;
    let face = 2;
    let color = null;
    let tint = [1, 1, 1];
    let tEnter = 0;
    let hitT = Infinity;
    for (let n = 0; n < 6000; n += 1) {
      if (iz < zMin) break;
      if (iz <= zMax && Math.abs(ix - cx0) <= half && Math.abs(iy - cy0) <= half) {
        const m = voxel(ix, iy, iz);
        if (m) {
          const mat = MATERIALS[m];
          if (mat.transparent) {
            tint = tint.map((t, k) => t * (1 - mat.opacity * 0.6) + (mat.rgb[k] / 255) * mat.opacity * 0.6 * t);
          } else {
            const nrm = face === 0 ? [-stepX, 0, 0] : face === 1 ? [0, -stepY, 0] : [0, 0, -stepZ];
            const ndl = Math.max(0, (nrm[0] * sun[0] + nrm[1] * sun[1] + nrm[2] * sun[2]) / sl);
            const shade = 0.55 + 0.45 * ndl + (nrm[2] > 0 ? 0.05 : 0);
            const h = (((ix * 73856093) ^ (iy * 19349663) ^ (iz * 83492791)) >>> 0) / 4294967296 - 0.5;
            const nz = 1 + h * mat.noise * 2;
            color = mat.rgb.map((c, k) => Math.min(255, c * shade * nz * tint[k] + mat.emissive * 30));
            hitT = tEnter;
            break;
          }
        }
      }
      if (tmx < tmy && tmx < tmz) {
        ix += stepX;
        tEnter = tmx;
        tmx += tdx;
        face = 0;
      } else if (tmy < tmz) {
        iy += stepY;
        tEnter = tmy;
        tmy += tdy;
        face = 1;
      } else {
        iz += stepZ;
        tEnter = tmz;
        tmz += tdz;
        face = 2;
      }
    }
    // a part nearer than the world grid's hit (its own lattice: straight walls, one plane of road)
    for (const part of parts) {
      const h = partHit(world, part, [ox, oy, oz], dir, hitT);
      if (!h || !inView(ox + dir[0] * h.t, oy + dir[1] * h.t, oz + dir[2] * h.t)) continue;
      hitT = h.t;
      const mat = MATERIALS[h.m];
      const ndl = Math.max(0, (h.n[0] * sun[0] + h.n[1] * sun[1] + h.n[2] * sun[2]) / sl);
      const shade = 0.55 + 0.45 * ndl + (h.n[2] > 0.5 ? 0.05 : 0);
      const p = [ox + dir[0] * h.t, oy + dir[1] * h.t, oz + dir[2] * h.t].map(Math.floor);
      const hh = (((p[0] * 73856093) ^ (p[1] * 19349663) ^ (p[2] * 83492791)) >>> 0) / 4294967296 - 0.5;
      color = mat.rgb.map((c) => Math.min(255, c * shade * (1 + hh * mat.noise * 2) + mat.emissive * 30));
    }
    if (color) canvas.set(pxi, py, color);
  }
}
canvas.save(out);
console.log(`iso ${px}px lod ${lod} ${size}m around (${cxm},${cym}) chunks ${chunks.size} in ${(performance.now() - t0).toFixed(0)}ms -> ${out}`);

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
