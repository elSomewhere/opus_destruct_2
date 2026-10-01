// Stage "roadview": road views (network/roadView.js) - buildSegments, annotateJunctions and the
// RoadView (every segment, its junction annotations, the junctions along each road, maxReach,
// near()) - of scripted road sets (lib/roads.mjs: every class, defaults, crossings, T's, ends in a
// carriageway, near-parallel and degenerate pieces, the angled world's owner cells) and of the
// reference's own roads: the views of the 3 x 3 cells round cells of several worlds (town
// centres, old towns of cobbles and lanes, the angled world's slanted streets and diagonal
// boulevards, rural roads and villages of an island, a torus across its seam). The views' roads
// are recorded in data/roadview.json for the port (its cell networks are a later stage).
import { REF, line, samples } from "../lib/rec.mjs";
import { scriptedRoads, segFields, junctionFields, segRef, recordingWorld, recorded, writeInputs, warmView } from "../lib/roads.mjs";

const { RoadView } = await import(REF + "network/roadView.js");
const { presetConfig } = await import(REF + "config/presets.js");

/** [world key, preset, size, cells whose views are recorded] */
export const VIEWS = [
  ["cities", "cities", null, [[0, 0], [1, -1]]],
  ["angledCities", "angledCities", null, [[0, 0], [-1, 1]]],
  ["oldHarbourTown", "oldHarbourTown", null, [[0, 0]]],
  ["angledOldHarbourTown", "angledOldHarbourTown", null, [[0, 0]]],
  ["island:medium", "island", "medium", [[0, 0], [2, 1]]],
  ["nordicTown:fjord", "nordicTown", "fjord", [[0, 0]]],
  ["wrapWorld:small", "wrapWorld", "small", [[77, 0]]],
  ["infiniteCity", "infiniteCity", null, [[3, -2]]],
];

/** A view's records: its segments and their junctions, each road's junction list (at its first segment), and near() over random rects. */
export function* viewRecords(view, r, label) {
  yield line("view", label, view.segs.length, view.maxReach);
  const seen = new Set();
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (const s of view.segs) {
    yield line("seg", ...segFields(s));
    for (const j of s.jn) yield line("jn", ...junctionFields(j));
    if (!seen.has(s.road)) {
      seen.add(s.road);
      yield line("rj", segRef(s), ...s.rj.map((e) => `${segRef(e.seg)}:${e.seg.jn.indexOf(e.j)}:${e.at}`));
    }
    x0 = Math.min(x0, s.bbox.x0);
    y0 = Math.min(y0, s.bbox.y0);
    x1 = Math.max(x1, s.bbox.x1);
    y1 = Math.max(y1, s.bbox.y1);
  }
  if (!view.segs.length) return;
  for (let k = 0; k < 60; k += 1) {
    const qx = x0 + r() * (x1 - x0);
    const qy = y0 + r() * (y1 - y0);
    const w = r() * 300;
    const h = r() * 300;
    yield line("near", qx, qy, w, h, ...view.near({ x0: qx, y0: qy, x1: qx + w, y1: qy + h }).map(segRef));
  }
}

export default function* roadview() {
  const r = samples(41);
  for (let k = 0; k < 160; k += 1) {
    const ox = Math.floor((r() - 0.5) * 20000);
    const oy = Math.floor((r() - 0.5) * 20000);
    yield* viewRecords(new RoadView(scriptedRoads(r, k, ox, oy)), r, `S${k}`);
  }
  const inputs = {};
  for (const [key, id, size, cells] of VIEWS) {
    const rw = recordingWorld(presetConfig(id, { size }));
    for (const [i, j] of cells) {
      warmView(rw.w, i, j);
      yield* viewRecords(rw.w.roadView(i, j), r, `${key}:${i},${j}`);
    }
    inputs[key] = recorded(rw);
  }
  writeInputs("roadview", inputs);
}
