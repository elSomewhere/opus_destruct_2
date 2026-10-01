// Stage "garageramps": a parking garage's ramps as pitched parts (buildings/garageRamps.js) -
// rampPart for scripted ramps (rects along either side wall of garage envelopes on scripted lots,
// plain and turned, from every floor but the top, at every table pitch) in the cells of a World of
// World.js holding them: the parts' records, and their content in their own lattices at LOD 0 to 3
// (rasterizeRampPart). tests/city/test_garageramps.cpp makes the same.
import { REF, line, samples } from "../lib/rec.mjs";
import { districtList, envLine } from "../lib/buildings.mjs";
import { shellWorld, shellEnvelope, chunkDigest, chunksAt } from "../lib/shells.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const G = await import(REF + "buildings/garageRamps.js");
const A = await import(REF + "buildings/archetypes.js");
const { ChunkBuffer } = await import(REF + "voxel/chunk.js");

/** A part's fields (world/parts.js makePart's record) with the ramp's: its envelope, f, W, Lu. */
function partFields(p) {
  const pl = p.placement;
  const e = p.extent;
  const a = p.aabb;
  return [p.key, p.id, p.cell, p.kind, [pl.origin.x, pl.origin.y, pl.origin.z], pl.yaw, pl.yaw2, pl.pitch, pl.roll, pl.anchored, pl.priority, [...pl.m], pl.d,
    [e.u0, e.v0, e.w0, e.u1, e.v1, e.w1], [a.x0, a.y0, a.z0, a.x1, a.y1, a.z1], [p.home.cx, p.home.cy, p.home.cz], [p.base.x, p.base.y, p.base.z], p.reach,
    p.anchored, p.priority, p.env, [p.ramp.f, p.ramp.W, p.ramp.Lu]];
}

export default function* garageramps() {
  const r = samples(71);
  const DS = districtList();
  const styles = STYLES.all().map((s) => s.id);
  const garage = ARCHETYPES.get("garage");
  let k = 0;
  for (let n = 0; n < 80; n += 1) {
    const w = shellWorld(Math.floor(r() * 3));
    const style = styles[Math.floor(r() * styles.length)];
    const d = DS[Math.floor(r() * DS.length)];
    const { env } = shellEnvelope(r, garage, style, d, w, (k += 1));
    yield envLine(env);
    if (!env) continue;
    const c = w.cellAt((env.R.x0 + env.R.x1) / 2, (env.R.y0 + env.R.y1) / 2);
    const cell = { i: c.i, j: c.j, ci: w.arterials.canon(c.i), cj: w.arterials.canon(c.j), rect: w.arterials.cellRect(c.i, c.j) };
    const fp = A.tierRects(env, 0)[0];
    for (let q = 0; q < 4; q += 1) {
      const RW = 24 + Math.floor(r() * 16);
      const L = 60 + Math.floor(r() * 160);
      const left = r() < 0.5;
      const f = Math.floor(r() * Math.max(1, env.floors - 1));
      const pitch = 1 + Math.floor(r() * 6);
      const y = r();
      const x0 = left ? fp.x0 + 4 : fp.x1 - 4 - RW + 1;
      const y0 = fp.y0 + 4 + Math.floor(y * Math.max(1, fp.y1 - fp.y0 - L - 8));
      const rect = { x0, y0, x1: x0 + RW - 1, y1: y0 + L - 1 };
      const part = G.rampPart(cell, env, { rect, f, pitch });
      yield line("ramp", ...partFields(part));
      const e = part.extent;
      const pts = [
        [e.u0, e.v0, e.w0],
        [e.u1, e.v1, e.w1],
        [(e.u0 + e.u1) / 2, (e.v0 + e.v1) / 2, 0],
      ];
      for (const [lod, cx, cy, cz] of chunksAt([0, 1, 2, 3], pts)) {
        const ch = new ChunkBuffer(lod, cx, cy, cz);
        G.rasterizeRampPart(w, part, ch);
        yield line("c", lod, cx, cy, cz, ...chunkDigest(ch));
      }
    }
  }
}
