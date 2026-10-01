// Stage "roadparts": pitched road pieces (network/roadParts.js) in the angled world's towns - the
// runs of one table grade along every segment of a cell's roads (pitchedRuns: table yaws only,
// on the run's line, never over water), the parts of each road's runs as the cell plan asks for
// them (roadRunParts: one lattice per run, the fewest equal pieces within reach), their content in
// their own lattices at LOD 0, 1 and 2 (rasterizeRoadPart: the road surface sampled on the pitched
// plane, over the slab) and the slab's foot under points (slabFoot). The roads and waters read are
// recorded in data/roadparts.json for the port.
import { REF, line, samples } from "../lib/rec.mjs";
import { segRef, recordingWorld, recorded, writeInputs, warmView } from "../lib/roads.mjs";

const { pitchedRuns, roadRunParts, rasterizeRoadPart, slabFoot } = await import(REF + "network/roadParts.js");
const { ChunkBuffer } = await import(REF + "voxel/chunk.js");
const { hash32 } = await import(REF + "core/hash.js");
const { presetConfig } = await import(REF + "config/presets.js");

/** [world key, preset, size, the cell, waters altered] (no steep street of these crosses water: one world is told some do) */
export const PARTS = [
  ["angledOldHarbourTown", "angledOldHarbourTown", null, [0, 0], null],
  ["angledNordicTown:fjord", "angledNordicTown", "fjord", [0, 0], null],
  ["angledNordicTown:fjord:wet", "angledNordicTown", "fjord", [0, 0], (x, y, m, v) => v || (hash32(Math.floor(x), Math.floor(y), 0x3e7) & 63) === 0],
];

/** Every field of a road piece (makePart and roadRunParts). */
export function partFields(p) {
  const pl = p.placement;
  const e = p.extent;
  const a = p.aabb;
  return [p.key, p.id, p.cell, p.kind, [pl.origin.x, pl.origin.y, pl.origin.z], pl.yaw, pl.yaw2, pl.pitch, pl.roll, pl.anchored, pl.priority, [...pl.m], pl.d,
    [e.u0, e.v0, e.w0, e.u1, e.v1, e.w1], [a.x0, a.y0, a.z0, a.x1, a.y1, a.z1], [p.home.cx, p.home.cy, p.home.cz], [p.base.x, p.base.y, p.base.z], p.reach,
    p.anchored, p.priority, p.road, p.seg, p.s0, p.s1, p.grade, p.hr];
}

/** A chunk's content: a hash of its voxels and their count. */
export function chunkDigest(c) {
  let h = 0;
  let n = 0;
  for (let q = 0; q < c.data.length; q += 1) {
    h = hash32(h, c.data[q], q);
    if (c.data[q]) n += 1;
  }
  return [h, n];
}

export default function* roadparts() {
  const r = samples(53);
  const inputs = {};
  for (const [key, id, size, [i, j], wet] of PARTS) {
    const rw = recordingWorld(presetConfig(id, { size }), { wet });
    const w = rw.w;
    warmView(w, i, j);
    const net = w.cellNet(i, j);
    const view = w.roadView(i, j);
    const cell = { i, j, ci: w.arterials.canon(i), cj: w.arterials.canon(j), rect: net.rect };
    // (the cell plan's own roads: its segments in this view, by road)
    const own = new Map();
    for (const s of view.segs) if (s.road.cell === net.id) own.set(s.road, [...(own.get(s.road) ?? []), s]);
    yield line("world", key, net.id, own.size);
    const parts = [];
    for (const [road, segs] of own) {
      for (const s of segs) yield line("runs", segRef(s), ...pitchedRuns(w, s).map((q) => [q.a0, q.a1, q.pitch, q.yaw]));
      for (const p of roadRunParts(w, road, segs, cell)) {
        parts.push(p);
        yield line("part", ...partFields(p));
      }
    }
    // (their content: the steepest pieces first, then by key, as the cell plan grants them)
    parts.sort((p, q) => Math.abs(q.grade) - Math.abs(p.grade) || (p.key < q.key ? -1 : p.key > q.key ? 1 : 0));
    for (const p of parts.slice(0, 6)) {
      const e = p.extent;
      for (const lod of [0, 1, 2]) {
        const s = 32 << lod;
        for (let cz = Math.floor((e.w0 - 1) / s); cz <= Math.floor((e.w1 + 1) / s); cz += 1)
          for (let cy = Math.floor((e.v0 - 1) / s); cy <= Math.floor((e.v1 + 1) / s); cy += 1)
            for (let cx = Math.floor((e.u0 - 1) / s); cx <= Math.floor((e.u1 + 1) / s); cx += 1) {
              const c = new ChunkBuffer(lod, cx, cy, cz);
              rasterizeRoadPart(w, p, c);
              yield line("chunk", p.key, lod, cx, cy, cz, ...chunkDigest(c));
            }
      }
      const a = p.aabb;
      for (let k = 0; k < 40; k += 1) {
        const x = a.x0 + r() * (a.x1 - a.x0);
        const y = a.y0 + r() * (a.y1 - a.y0);
        yield line("foot", p.key, x, y, slabFoot(p, x, y));
      }
    }
    inputs[key] = recorded(rw);
  }
  writeInputs("roadparts", inputs);
}
