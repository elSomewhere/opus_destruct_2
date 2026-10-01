// Stage "roadlevel": road levels (network/roadLevel.js) in several worlds - the profile of every
// road of a cell (roadProfile: its samples, length and, where roads are pitched, its knots; the
// ends pinned to node levels or to a through road's level), the plain profiles the T's read, the
// level along every segment of those roads and round each of its junctions (segmentLevel: junction
// boxes, blends shared in a gap, overlapping boxes; the angled world junction by junction from the
// owner cell's view), the nearest road's level at points (roadLevelAt) and the street level
// (createWorld's streetLevel) - in towns, hill towns, old towns, rural roads of an island, the
// angled world with and without pitched roads, a torus across its seam. The roads read are
// recorded in data/roadlevel.json for the port.
import { REF, line, samples } from "../lib/rec.mjs";
import { segRef, recordingWorld, recorded, writeInputs, warmView } from "../lib/roads.mjs";

const { roadProfile, segmentLevel, roadLevelAt } = await import(REF + "network/roadLevel.js");
const { presetConfig } = await import(REF + "config/presets.js");

/** [world key, preset or JSON overrides, size, the cell] */
export const LEVELS = [
  ["cities", "cities", null, [0, 0]],
  ["nordicTown:fjord", "nordicTown", "fjord", [0, 0]],
  ["oldHarbourTown", "oldHarbourTown", null, [0, 0]],
  ["island:medium", "island", "medium", [1, 1]],
  ["angledCities", "angledCities", null, [0, 0]],
  ["angledOldHarbourTown", "angledOldHarbourTown", null, [0, 0]],
  ["angledNoRamps", '{"seed":77,"world":{"mode":"island","island":{"radius":2600,"population":5000,"towns":0},"angles":{"enabled":true,"features":{"ramps":false}}}}', null, [0, 0]],
  ["wrapWorld:small", "wrapWorld", "small", [77, 0]],
];

export const configOf = (id, size) => (id.startsWith("{") ? JSON.parse(id) : presetConfig(id, { size }));

const profFields = (p) => [p.L, p.n, [...p.z], p.ks ? [...p.ks] : "-", p.kz ? [...p.kz] : "-"];

/** A point near a random segment of segs (out to 30 voxels past its right-of-way), kept within rect, drawn from r. */
export function nearSeg(segs, rect, r) {
  const s = segs[Math.floor(r() * segs.length)];
  const t = r() * s.len;
  const lat = (r() - 0.5) * 2 * (s.hr + 30);
  const x = s.ax + s.dx * t - s.dy * lat;
  const y = s.ay + s.dy * t + s.dx * lat;
  return [Math.max(rect.x0, Math.min(rect.x1, x)), Math.max(rect.y0, Math.min(rect.y1, y))];
}

export default function* roadlevel() {
  const r = samples(47);
  const inputs = {};
  for (const [key, id, size, [i, j]] of LEVELS) {
    const rw = recordingWorld(configOf(id, size));
    const w = rw.w;
    warmView(w, i, j);
    const net = w.cellNet(i, j);
    const view = w.roadView(i, j);
    yield line("world", key, net.id, net.roads.length, view.segs.length);
    for (const road of net.roads) yield line("prof", road.id, ...profFields(roadProfile(w, road)));
    const own = view.segs.filter((s) => s.road.cell === net.id);
    for (const s of own) {
      const zs = [];
      for (let a = 0; a < s.len; a += 12) zs.push(segmentLevel(w, s, a));
      zs.push(segmentLevel(w, s, s.len));
      yield line("seg", segRef(s), ...zs);
      for (const e of s.rj) {
        const at = e.at - s.s0;
        const out = [];
        for (const o of [-60, -30, -14, -7, -2, 0, 2, 7, 14, 30, 60]) {
          const a = at + o;
          if (a >= -20 && a <= s.len + 20) out.push(a, segmentLevel(w, s, a));
        }
        yield line("jlv", segRef(s), segRef(e.seg), e.at, ...out);
      }
    }
    // (the angled world: segments of the east neighbour's roads, levelled from its own view)
    const east = view.segs.filter((s) => s.road.home && s.road.home[0] === i + 1 && s.road.home[1] === j).slice(0, 12);
    for (const s of east) yield line("east", segRef(s), ...[0, 0.25, 0.5, 0.75, 1].map((f) => segmentLevel(w, s, s.len * f)));
    // (points in the cell, 200 voxels in from its edges: the street level asks the cell's own view)
    const rc = w.arterials.cellRect(i, j);
    const inner = { x0: rc.x0 + 200, y0: rc.y0 + 200, x1: rc.x1 - 200, y1: rc.y1 - 200 };
    if (own.length) {
      for (let k = 0; k < 1200; k += 1) {
        const [x, y] = r() < 0.3 ? [inner.x0 + r() * (inner.x1 - inner.x0), inner.y0 + r() * (inner.y1 - inner.y0)] : nearSeg(own, inner, r);
        const reach = r() < 0.5 ? 8 : 24;
        const q = roadLevelAt(w, view, x, y, reach);
        yield line("at", x, y, reach, ...(q ? [q.z, segRef(q.seg), q.along, q.dist, q.sidewalk] : ["-"]));
      }
      for (let k = 0; k < 300; k += 1) {
        const [x, y] = nearSeg(own, inner, r);
        yield line("street", x, y, w.streetLevel(x, y));
      }
    }
    for (const road of net.roads) if (road.prof0) yield line("prof0", road.id, ...profFields(road.prof0));
    inputs[key] = recorded(rw);
  }
  writeInputs("roadlevel", inputs);
}
