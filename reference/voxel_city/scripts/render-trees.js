#!/usr/bin/env node
/**
 * Tree gallery: every tree kind (or --kinds a,b,c) in a row on a lawn,
 * three sizes each, rendered as an isometric voxel view.
 *   node scripts/render-trees.js [--season summer] [--lod 0] [--kinds oak,birch] [--t 0.4] [--px 1400] [--out trees.png] [--wild 1] [--rows 3]
 * --t is the local temperature the seasonal look is taken at. --wild 1
 * draws the angled world's wild trees (nature/trees.js), within the forest's
 * reach; --rows the number of trees of each kind (sizes small to large).
 */
import { ChunkBuffer, P, P2 } from "../src/engine/voxel/chunk.js";
import { MATERIALS, MAT } from "../src/engine/voxel/materials.js";
import { rasterizeTree, treeBounds, TREE_KINDS } from "../src/engine/nature/trees.js";
import { seasonOf } from "../src/engine/world/season.js";
import { hash32 } from "../src/engine/core/hash.js";
import { Canvas } from "./lib/png.js";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, arr) => (a.startsWith("--") ? [...acc, [a.slice(2), arr[i + 1]]] : acc), []),
);
const lod = Number(args.lod ?? 0);
const season = seasonOf({ world: { season: args.season ?? "summer" } });
const tLocal = Number(args.t ?? 0.4);
const kinds = (args.kinds ?? "oak,maple,street,birch,rowan,blossom,willow,poplar,pine,spruce,dwarfpine,juniper,shrub,jungle,fern,log,stump,snag").split(",");
const px = Number(args.px ?? 1200);
const wild = !!Number(args.wild ?? 0);
const rows = Number(args.rows ?? 3);
const UNDER = new Set(["fern", "berry", "log", "stump", "shrub", "shrubDry", "hazel", "juniper"]);
const out = args.out ?? "trees.png";
const S = 1 << lod;
const GAP = 12 * 8;
const trees = [];
kinds.forEach((kind, a) => {
  const spec = TREE_KINDS[kind];
  for (let b = 0; b < rows; b += 1) {
    const f = rows === 3 ? [0.1, 0.5, 0.95][b] : 0.1 + (0.85 * b) / Math.max(1, rows - 1);
    const seed = hash32(a, b, 7);
    const t = {
      x: a * GAP,
      y: b * GAP,
      z: 1,
      h: Math.round((spec.h[0] + (spec.h[1] - spec.h[0]) * f) * 8),
      r: (spec.r[0] + (spec.r[1] - spec.r[0]) * f) * 8,
      kind,
      seed,
      open: b === 2,
      // (the forest's and the understory's reach, less a wild tree's stray)
      ...(wild ? { wild: true, reach: UNDER.has(kind) ? 27 : 57 } : {}),
    };
    t.look = season.treeLook(kind, seed, tLocal);
    t.bb = treeBounds(t);
    trees.push(t);
  }
});
const chunks = new Map();
function chunkAt(cx, cy, cz) {
  const k = `${cx},${cy},${cz}`;
  let c = chunks.get(k);
  if (!c) {
    c = new ChunkBuffer(lod, cx, cy, cz);
    // a lawn at z <= 0
    for (let kk = 0; kk < P; kk += 1)
      if (c.wz(kk) <= 0) for (let j = 0; j < P; j += 1) for (let i = 0; i < P; i += 1) c.data[i + j * P + kk * P2] = (i + j) & 1 ? MAT.GRASS : MAT.GRASS_LAWN;
    for (const t of trees) rasterizeTree(c, t);
    chunks.set(k, c);
  }
  return c;
}
function voxel(x, y, z) {
  const cx = Math.floor(x / 32);
  const cy = Math.floor(y / 32);
  const cz = Math.floor(z / 32);
  return chunkAt(cx, cy, cz).data[x - cx * 32 + 1 + (y - cy * 32 + 1) * P + (z - cz * 32 + 1) * P2];
}
// camera: orthographic from the south-east, looking at the middle of the rows
const yaw = (Number(args.yaw ?? -100) * Math.PI) / 180;
const pitch = (Number(args.pitch ?? 14) * Math.PI) / 180;
const dir = [-Math.cos(pitch) * Math.cos(yaw), -Math.cos(pitch) * Math.sin(yaw), -Math.sin(pitch)];
const right = [-Math.sin(yaw), Math.cos(yaw), 0];
const up = [right[1] * dir[2] - right[2] * dir[1], right[2] * dir[0] - right[0] * dir[2], right[0] * dir[1] - right[1] * dir[0]];
const cx0 = ((kinds.length - 1) * GAP) / 2 / S;
const cy0 = ((rows - 1) * GAP) / 2 / S;
const cz0 = (9 * 8) / S;
const extent = (Math.max(5, kinds.length) * GAP * 0.95) / S;
const canvas = new Canvas(px, Math.round(px * 0.5), [196, 210, 222]);
const H = Math.round(px * 0.5);
const sun = [0.45, -0.35, 0.82];
const sl = Math.hypot(...sun);
for (let py = 0; py < H; py += 1)
  for (let pxi = 0; pxi < px; pxi += 1) {
    const sx = (pxi / px - 0.5) * extent;
    const sy = (0.5 - py / H) * extent * 0.5;
    const back = extent * 2;
    const ox = cx0 + right[0] * sx + up[0] * sy - dir[0] * back;
    const oy = cy0 + right[1] * sx + up[1] * sy - dir[1] * back;
    const oz = cz0 + right[2] * sx + up[2] * sy - dir[2] * back;
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
    for (let n = 0; n < 8000; n += 1) {
      if (iz < -2) break;
      if (iz < 60 * 8 / S) {
        const m = voxel(ix, iy, iz);
        if (m) {
          const mat = MATERIALS[m];
          const nrm = face === 0 ? [-stepX, 0, 0] : face === 1 ? [0, -stepY, 0] : [0, 0, -stepZ];
          const ndl = Math.max(0, (nrm[0] * sun[0] + nrm[1] * sun[1] + nrm[2] * sun[2]) / sl);
          const hh = (((ix * 73856093) ^ (iy * 19349663) ^ (iz * 83492791)) >>> 0) / 4294967296 - 0.5;
          const shade = (0.55 + 0.45 * ndl) * (1 + hh * mat.noise * 2);
          canvas.set(pxi, py, mat.rgb.map((c) => Math.min(255, c * shade)));
          break;
        }
      }
      if (tmx < tmy && tmx < tmz) {
        ix += stepX;
        tmx += tdx;
        face = 0;
      } else if (tmy < tmz) {
        iy += stepY;
        tmy += tdy;
        face = 1;
      } else {
        iz += stepZ;
        tmz += tdz;
        face = 2;
      }
    }
  }
canvas.save(out);
console.log(`trees ${kinds.length} kinds, lod ${lod}, ${season.id} -> ${out}`);
