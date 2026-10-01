#!/usr/bin/env node
// A district of the reference city through its structvox export (src/engine/svx), for the engine's
// district testbed (procgen/district_source.hpp, tools/district_check): the materials to register,
// the world grid's chunks with their look, flora and water layers and their isolated props (the
// export's furniture and street props), every column's content range and region, the parts as
// grids, the lanes and the highway decks' touch points. An extension of the reference's
// scripts/svx-harness/dump.js (format "SVXD" 2; chunks run-length encoded).
//
//   node tools/procgen_ref/district.mjs <preset> <out.bin> [radius in chunks] [part kind | @x,y metres]
import { writeFileSync } from "node:fs";
import { REF } from "./lib/rec.mjs";

const { createSvxSource } = await import(REF + "svx/source.js");
const { roadNetwork } = await import(REF + "svx/roads.js");
const { presetConfig } = await import(REF + "config/presets.js");
const { groundTile, tileContentRange } = await import(REF + "voxel/compose.js");
const { pointAt } = await import(REF + "network/highways.js");

const [preset = "angledOldHarbourTown", out = "district.bin", radiusArg = "7", kind = null] = process.argv.slice(2);
const R = Number(radiusArg);
const src = createSvxSource(presetConfig(preset));
const w = src.world;
const H = 0.125;

const parts = [];
for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) parts.push(...w.cellPlan(i, j).parts);
let b = parts[0] ?? null;
let cx0 = 0;
let cy0 = 0;
if (kind?.startsWith("@")) {
  const [mx, my] = kind.slice(1).split(",").map(Number);
  cx0 = Math.floor(mx / H / 32);
  cy0 = Math.floor(my / H / 32);
  b = { kind: "point", key: kind };
} else if (b) {
  let most = -1;
  for (const p of parts) {
    if (kind && p.kind !== kind) continue;
    const n = parts.filter((q) => Math.abs(q.home.cx - p.home.cx) <= R - 2 && Math.abs(q.home.cy - p.home.cy) <= R - 2).length;
    if (n > most) [b, most] = [p, n];
  }
  cx0 = b.home.cx;
  cy0 = b.home.cy;
} else b = { kind: "origin", key: "0,0" };

const buf = [];
const u8 = (v) => buf.push(Buffer.from([v & 255]));
const u32 = (v) => { const x = Buffer.alloc(4); x.writeUInt32LE(v >>> 0); buf.push(x); };
const i32 = (v) => { const x = Buffer.alloc(4); x.writeInt32LE(v | 0); buf.push(x); };
const u64 = (v) => { const x = Buffer.alloc(8); x.writeBigUInt64LE(BigInt(v)); buf.push(x); };
const f64 = (v) => { const x = Buffer.alloc(8); x.writeDoubleLE(v); buf.push(x); };
const str = (s) => { const x = Buffer.from(s, "utf8"); u32(x.length); buf.push(x); };
// a 32^3 byte array run-length encoded: (value, run as a LEB128 varint) pairs
function rle(a) {
  const o = [];
  for (let i = 0; i < a.length; ) {
    const v = a[i];
    let n = 1;
    while (i + n < a.length && a[i + n] === v) n += 1;
    o.push(v);
    for (let r = n; ; ) {
      if (r < 128) { o.push(r); break; }
      o.push((r & 127) | 128);
      r = Math.floor(r / 128);
    }
    i += n;
  }
  u32(o.length);
  buf.push(Buffer.from(o));
}
const layer = (a) => { u8(a ? 1 : 0); if (a) rle(a); };

buf.push(Buffer.from("SVXD"));
u32(2);
f64(H);
const mats = src.materials().register;
u32(mats.length);
for (const m of mats) {
  i32(m.id);
  str(m.name);
  for (const f of ["E", "G", "rho", "ft", "fb", "fc", "cohesion", "friction", "Gf"]) f64(m[f]);
  for (const f of m.frag) f64(f);
  f64(m.frag_noise);
}
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
// columns: their content range (chunks, inclusive) and region
u32(cols.length);
for (const [cx, cy, a, z1] of cols) {
  i32(cx);
  i32(cy);
  i32(a);
  i32(z1);
  u64(src.region(cx, cy));
}
// the world grid's chunks with content (solid voxels, layers or props)
const chunks = [];
let props = 0;
for (const [cx, cy, a, z1] of cols)
  for (let cz = a; cz <= z1; cz += 1) {
    const g = src.generate(cx, cy, cz);
    const look = src.generateLayer(cx, cy, cz, "look");
    const flora = src.generateLayer(cx, cy, cz, "flora");
    const water = src.generateLayer(cx, cy, cz, "water");
    const iso = src.isolated(cx, cy, cz);
    if (g.any || flora || water || iso) chunks.push({ cx, cy, cz, vox: g.vox.slice(), look, flora, water, iso });
  }
u32(chunks.length);
for (const c of chunks) {
  i32(c.cx);
  i32(c.cy);
  i32(c.cz);
  rle(c.vox);
  layer(c.look);
  layer(c.flora);
  layer(c.water);
  const iso = c.iso ?? [];
  u32(iso.length / 3);
  for (let k = 0; k < iso.length; k += 3) {
    u32(iso[k]);
    u8(iso[k + 1]);
    u8(iso[k + 2]);
  }
  props += iso.length / 3;
}
// the grids at home in the region
const grids = [];
for (const p of reaching) for (const g of src.grids(p.home.cx, p.home.cy, p.home.cz)) if (g.id === p.id) grids.push([g, [p.home.cx, p.home.cy, p.home.cz]]);
u32(grids.length);
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
    rle(c.vox);
    layer(c.look);
    layer(c.flora);
  }
}
const net = roadNetwork(w);
const lo = [H * (cx0 - R) * 32, H * (cy0 - R) * 32];
const hi = [H * (cx0 + R + 1) * 32, H * (cy0 + R + 1) * 32];
const inside = (l) => { const x = (l.a[0] + l.b[0]) / 2; const y = (l.a[1] + l.b[1]) / 2; return x > lo[0] + 4 && x < hi[0] - 4 && y > lo[1] + 4 && y < hi[1] - 4; };
const lanes = net.lanesIn(lo, hi).filter(inside);
u32(lanes.length);
for (const l of lanes) for (const p of [l.a, l.b]) for (const v of p) f64(v);
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
// the export's class table (for decoding looks: class id -> its looks' names) and flora kinds
const M = src.materials();
u32(M.classes.length);
for (const c of M.classes) {
  i32(c.id);
  str(c.name);
  const looks = M.looks[c.id] ?? [];
  u32(looks.length);
  for (const n of looks) str(n);
}
u32(M.flora?.length ?? 0);
for (const f of M.flora ?? []) str(typeof f === "string" ? f : f.name ?? String(f));

writeFileSync(out, Buffer.concat(buf));
console.log(`${preset}: centre chunk ${cx0},${cy0} (${b.kind} ${b.key}), ${cols.length} columns, ${chunks.length} chunks, ${props} prop voxels, ${grids.length} grids, ${lanes.length} lanes, ${touches.length} touch points -> ${out}`);
