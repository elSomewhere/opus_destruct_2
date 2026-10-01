// Stage "roadsurface": the road surface of a column (network/roadSurface.js sampleRoadSurface):
// kind, material, height, the dominant segment and the column in its frame, a sidewalk's distance
// from the curb, the signed distances - round the roads of scripted road sets (lib/roads.mjs) and
// of the reference's own views (town centres, old towns of cobbles, the angled world's roads that
// fold in id order, rural roads, a torus whose texture cells wrap). Points: along random segments
// out to their right-of-way and corners, and dense over the approaches of junctions (crosswalks,
// stop lines, lane arrows, fillets, tactile curbs, painted medians); candidates from near() rects
// of several sizes, with and without a reach. One sample object serves every call, as the
// reference's callers keep one (what a call does not set stays). The port reads the views' roads
// from stage "roadview"'s recording (data/roadview.json: every view sampled here is one of its).
import { REF, line, samples } from "../lib/rec.mjs";
import { scriptedRoads, segRef, recordingWorld, warmView } from "../lib/roads.mjs";

const { RoadView } = await import(REF + "network/roadView.js");
const { sampleRoadSurface, makeRoadSample } = await import(REF + "network/roadSurface.js");
const { presetConfig } = await import(REF + "config/presets.js");
const { wrapOf } = await import(REF + "world/wrap.js");

/** [world key, preset, size, cells whose views are sampled] */
export const VIEWS = [
  ["cities", "cities", null, [[0, 0]]],
  ["angledCities", "angledCities", null, [[0, 0]]],
  ["oldHarbourTown", "oldHarbourTown", null, [[0, 0]]],
  ["angledOldHarbourTown", "angledOldHarbourTown", null, [[0, 0]]],
  ["island:medium", "island", "medium", [[2, 1]]],
  ["wrapWorld:small", "wrapWorld", "small", [[77, 0]]],
  ["infiniteCity", "infiniteCity", null, [[3, -2]]],
];

function* surfaceRecords(view, r, out, seed, nAlong, nJunctions) {
  if (!view.segs.length) return;
  const at = (x, y) => {
    const mode = r();
    const R = mode < 0.4 ? 2 : mode < 0.8 ? 40 : view.maxReach + 44;
    const reach = r() < 0.7 ? 0 : r() < 0.5 ? 8 : 44;
    sampleRoadSurface(view.near({ x0: x - R, y0: y - R, x1: x + R, y1: y + R }), x, y, out, seed, reach);
    return line("rs", x, y, R, reach, out.kind, out.mat, out.dz, segRef(out.seg), out.along, out.side, out.q, out.sdfC, out.sdfR);
  };
  for (let k = 0; k < nAlong; k += 1) {
    const s = view.segs[Math.floor(r() * view.segs.length)];
    const t = -30 + r() * (s.len + 60);
    const lat = (r() - 0.5) * 2 * (s.hr + 40);
    let x = s.ax + s.dx * t - s.dy * lat;
    let y = s.ay + s.dy * t + s.dx * lat;
    if (r() < 0.5) {
      x = Math.floor(x) + 0.5;
      y = Math.floor(y) + 0.5;
    }
    yield at(x, y);
  }
  // the approaches of junctions: along the segment from 110 voxels before to 110 after, across its right-of-way
  const withJn = view.segs.filter((s) => s.jn.length);
  if (!withJn.length) return;
  for (let g = 0; g < nJunctions; g += 1) {
    const s = withJn[Math.floor(r() * withJn.length)];
    const j = s.jn[Math.floor(r() * s.jn.length)];
    yield line("junction", segRef(s), j.s, segRef(j.other));
    for (let a = -110; a <= 110; a += 4.5)
      for (let l = -(s.hr + 6); l <= s.hr + 6; l += 3) {
        const x = Math.floor(s.ax + s.dx * (j.s + a) - s.dy * l) + 0.5;
        const y = Math.floor(s.ay + s.dy * (j.s + a) + s.dx * l) + 0.5;
        yield at(x, y);
      }
  }
}

export default function* roadsurface() {
  const r = samples(43);
  const out = makeRoadSample(0);
  for (let k = 0; k < 50; k += 1) {
    const ox = Math.floor((r() - 0.5) * 20000);
    const oy = Math.floor((r() - 0.5) * 20000);
    const view = new RoadView(scriptedRoads(r, k, ox, oy));
    out.period = r() < 0.7 ? 0 : 1000 + Math.floor(r() * 9000);
    const seed = Math.floor((r() - 0.5) * 2e9);
    yield line("set", `S${k}`, out.period, seed);
    yield* surfaceRecords(view, r, out, seed, 300, 2);
  }
  for (const [key, id, size, cells] of VIEWS) {
    const rw = recordingWorld(presetConfig(id, { size }));
    const w = rw.w;
    out.period = wrapOf(w.config).sizeV;
    for (const [i, j] of cells) {
      warmView(w, i, j);
      yield line("set", `${key}:${i},${j}`, out.period, w.seed);
      yield* surfaceRecords(w.roadView(i, j), r, out, w.seed, 2000, 4);
    }
  }
}
