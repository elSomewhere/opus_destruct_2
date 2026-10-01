// Dump a district of the city through the svx export (src/engine/svx), for the native structvox
// harness (harness.cpp): materials, the world grid's chunks, the parts as grids, a sample of lanes.
//   node scripts/svx-harness/dump.js <preset> <out.bin> [radius chunks] [kind of part to centre on | @x,y metres]
import { writeFileSync } from "node:fs";
import { createSvxSource } from "../../src/engine/svx/source.js";
import { roadNetwork } from "../../src/engine/svx/roads.js";
import { presetConfig } from "../../src/engine/config/presets.js";
import { groundTile, tileContentRange } from "../../src/engine/voxel/compose.js";
import { pointAt } from "../../src/engine/network/highways.js";

const [preset = "angledOldHarbourTown", out = "dump.bin", radiusArg = "7", kind = null] = process.argv.slice(2);
const R = Number(radiusArg);
const src = createSvxSource(presetConfig(preset));
const w = src.world;
const H = 0.125;

// the centre: "@x,y" (metres), else the home chunk of a part (of `kind`) near the origin: the part with the
// most others at home within the region's reach, over the 3 x 3 cells round the origin
const parts = [];
for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) parts.push(...w.cellPlan(i, j).parts);
let b = parts[0] ?? null;
let cx0;
let cy0;
if (kind?.startsWith("@")) {
  const [mx, my] = kind.slice(1).split(",").map(Number);
  cx0 = Math.floor(mx / H / 32);
  cy0 = Math.floor(my / H / 32);
  b = { kind: "point", key: kind };
} else {
  let most = -1;
  for (const p of parts) {
    if (kind && p.kind !== kind) continue;
    const n = parts.filter((q) => Math.abs(q.home.cx - p.home.cx) <= R - 2 && Math.abs(q.home.cy - p.home.cy) <= R - 2).length;
    if (n > most) [b, most] = [p, n];
  }
  cx0 = b.home.cx;
  cy0 = b.home.cy;
}

const buf = [];
const u8 = (v) => buf.push(Buffer.from([v & 255]));
const u32 = (v) => { const x = Buffer.alloc(4); x.writeUInt32LE(v >>> 0); buf.push(x); };
const i32 = (v) => { const x = Buffer.alloc(4); x.writeInt32LE(v | 0); buf.push(x); };
const f64 = (v) => { const x = Buffer.alloc(8); x.writeDoubleLE(v); buf.push(x); };
const str = (s) => { const x = Buffer.from(s, "utf8"); u32(x.length); buf.push(x); };

buf.push(Buffer.from("SVXD"));
u32(1);
f64(H);
// materials the host registers, in order, and the ids they must get
const mats = src.materials().register;
u32(mats.length);
for (const m of mats) {
  i32(m.id);
  str(m.name);
  for (const f of ["E", "G", "rho", "ft", "fb", "fc", "cohesion", "friction", "Gf"]) f64(m[f]);
  for (const f of m.frag) f64(f);
  f64(m.frag_noise);
}
// the region, and a focus on the ground at its centre
let zlo = Infinity;
let zhi = -Infinity;
const cols = [];
for (let cy = cy0 - R; cy <= cy0 + R; cy += 1)
  for (let cx = cx0 - R; cx <= cx0 + R; cx += 1) {
    const [lo, hi] = tileContentRange(w, groundTile(w, 0, cx, cy));
    cols.push([cx, cy, Math.floor(lo / 32), Math.floor(hi / 32)]);
    zlo = Math.min(zlo, Math.floor(lo / 32));
    zhi = Math.max(zhi, Math.floor(hi / 32));
  }
// (every part reaching into the region, and its home chunk: the extent holds them all)
const reaching = parts.filter((p) => p.aabb.x1 >= (cx0 - R) * 32 && p.aabb.x0 < (cx0 + R + 1) * 32 && p.aabb.y1 >= (cy0 - R) * 32 && p.aabb.y0 < (cy0 + R + 1) * 32);
const ext = { x0: cx0 - R, y0: cy0 - R, z0: zlo, x1: cx0 + R + 1, y1: cy0 + R + 1, z1: zhi + 1 };
for (const p of reaching) {
  ext.x0 = Math.min(ext.x0, p.home.cx);
  ext.y0 = Math.min(ext.y0, p.home.cy);
  ext.z0 = Math.min(ext.z0, p.home.cz);
  ext.x1 = Math.max(ext.x1, p.home.cx + 1);
  ext.y1 = Math.max(ext.y1, p.home.cy + 1);
  ext.z1 = Math.max(ext.z1, p.home.cz + 1);
}
for (const v of [ext.x0, ext.y0, ext.z0]) i32(v);
for (const v of [ext.x1, ext.y1, ext.z1]) i32(v);
const g0 = groundTile(w, 0, cx0, cy0);
const gz = g0.z[16 + 16 * 34];
for (const v of [H * (cx0 * 32 + 16 - 0.5), H * (cy0 * 32 + 16 - 0.5), H * (gz + 1 - 0.5)]) f64(v);
// the world grid's chunks (not air)
const chunks = [];
for (const [cx, cy, a, z1] of cols)
  for (let cz = a; cz <= z1; cz += 1) {
    const g = src.generate(cx, cy, cz);
    if (g.any) chunks.push([cx, cy, cz, g.vox.slice()]);
  }
u32(chunks.length);
for (const [cx, cy, cz, vox] of chunks) {
  i32(cx);
  i32(cy);
  i32(cz);
  buf.push(Buffer.from(vox.buffer, vox.byteOffset, vox.byteLength));
}
// the grids at home in the region
const grids = [];
for (const p of reaching) for (const g of src.grids(p.home.cx, p.home.cy, p.home.cz)) if (g.id === p.id) grids.push([g, [p.home.cx, p.home.cy, p.home.cz]]);
u32(grids.length);
let gridVox = 0;
for (const [g, home] of grids) {
  u32(g.id);
  for (const v of g.origin) f64(v);
  for (const v of [g.rot.x, g.rot.y, g.rot.z, g.rot.w]) f64(v);
  f64(g.voxelSize);
  i32(g.priority);
  u8(g.anchored ? 1 : 0);
  for (const v of home) i32(v);
  str(g.kind);
  const vox = src.generateGrid(g.id);
  u32(vox.chunks.length);
  for (const c of vox.chunks) {
    i32(c.cx);
    i32(c.cy);
    i32(c.cz);
    buf.push(Buffer.from(c.vox.buffer, c.vox.byteOffset, c.vox.byteLength));
    for (const v of c.vox) if (v) gridVox += 1;
  }
}
// lanes of the region (their ends, metres)
const net = roadNetwork(w);
const lo = [H * (cx0 - R) * 32, H * (cy0 - R) * 32];
const hi = [H * (cx0 + R + 1) * 32, H * (cy0 + R + 1) * 32];
const inside = (l) => { const x = (l.a[0] + l.b[0]) / 2; const y = (l.a[1] + l.b[1]) / 2; return x > lo[0] + 4 && x < hi[0] - 4 && y > lo[1] + 4 && y < hi[1] - 4; };
const lanes = net.lanesIn(lo, hi).filter(inside);
u32(lanes.length);
for (const l of lanes) for (const p of [l.a, l.b]) for (const v of p) f64(v);
// touch points: on every highway deck in the region, every 24 m (its surface voxel, loaded by the harness)
const touches = [];
if (w.highways) {
  const box = { x0: (cx0 - R + 1) * 32, y0: (cy0 - R + 1) * 32, x1: (cx0 + R) * 32, y1: (cy0 + R) * 32 };
  for (const e of w.highways.edgesNear(box))
    for (let s = 0; s < e.total; s += 192) {
      const p = pointAt(e, s);
      if (p.x < box.x0 || p.x > box.x1 || p.y < box.y0 || p.y > box.y1) continue;
      touches.push([H * (p.x - 0.5) + 2 * p.ty, H * (p.y - 0.5) - 2 * p.tx, H * (Math.round(p.z) + 0.5) - 0.06]);
    }
}
u32(touches.length);
for (const t of touches) for (const v of t) f64(v);

writeFileSync(out, Buffer.concat(buf));
const kinds = {};
for (const [g] of grids) kinds[g.kind] = (kinds[g.kind] ?? 0) + 1;
console.log(`${preset}: centre chunk ${cx0},${cy0} (${b.kind} ${b.key}), ${chunks.length} chunks, ${grids.length} grids ${JSON.stringify(kinds)} (${gridVox} voxels), ${lanes.length} lanes, ${touches.length} touch points -> ${out}`);
