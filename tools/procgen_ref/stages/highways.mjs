// Stage "highways": the elevated highway network (network/highways.js) - the lattice over a window
// of several worlds (nodes and their levels, edge existence after every pruning pass, intercity
// routes, chance edges, grades, degrees; a torus across its seam), and edges built in full: their
// decks (profile over crossing roads and waters, plateaus), ramps at urban arterials (landings,
// blocked ramps, their piers), piers and portals, the junction plateau of four highways in open
// country; queries round them (nearest, rampAt, covers, underside, onRoad, corridors, mapData,
// pointAt past the ends) and the feature source (zRange, rasterize at LOD 0, 2 and 5 over decks,
// tunnels, cuttings, embankments, ramps, piers and the plateau, with and without a ground tile).
// The roads and waters read are recorded in data/highways.json for the port.
import { REF, line, samples } from "../lib/rec.mjs";
import { recordingWorld, recorded, writeInputs, warmAround } from "../lib/roads.mjs";
import { chunkDigest } from "./roadparts.mjs";

const { pointAt, offsetAt, rampZ, highwaySource } = await import(REF + "network/highways.js");
const { ChunkBuffer, P } = await import(REF + "voxel/chunk.js");
const { presetConfig } = await import(REF + "config/presets.js");

/** [world key, preset or JSON overrides, size, lattice window [a0, b0, a1, b1], edges built in full [axis, a, b]] */
export const NETS = [
  ["seed99", '{"seed":99}', null, [-2, -2, 6, 2], [[0, -1, 0], [0, 4, 0]]],
  ["cities", "cities", null, [-4, -4, 4, 4], []],
  ["infiniteCity1337", '{"seed":1337,"world":{"mode":"infiniteCity"}}', null, [-3, -3, 3, 3], []],
  ["wrapWorld:small", "wrapWorld", "small", [11, -2, 16, 2], []],
];

const configOf = (id, size) => (id.startsWith("{") ? JSON.parse(id) : presetConfig(id, { size }));
const opt = (v) => (v === null || v === undefined ? "-" : v);

function* edgeRecords(w, e, r) {
  const hw = w.highways;
  yield line("edge", e.id, e.axis, e.a, e.b, e.nodes, e.total, [e.bb.x0, e.bb.y0, e.bb.x1, e.bb.y1], e.junctions.map((j) => [j.x, j.y, j.z, j.r, j.degree, ...j.node]), e.pts.length);
  yield line("pts", ...e.pts.flatMap((p) => [p.x, p.y, p.z]));
  yield line("lengths", ...e.lengths);
  for (const s of e.segs) yield line("hseg", s.ax, s.ay, s.az, s.bx, s.by, s.bz, s.len, s.dx, s.dy, s.s0, [s.bb.x0, s.bb.y0, s.bb.x1, s.bb.y1]);
  const ramps = hw.ramps(e);
  for (const rp of ramps) yield line("ramp", rp.side, rp.sDeck, rp.sGround, rp.drop, rp.zDeck, rp.zGround, rp.cross, rp.x, rp.y, rp.arterial, hw.rampPiers(e, rp), hw.rampBlocked(e, rp));
  for (const p of hw.piers(e)) yield line("pier", p.s, p.x, p.y, p.tx, p.ty, p.z, p.cols, p.cap);
  // queries beside the deck and its ramps
  const R = hw.rampIn + 56 + 40;
  for (let k = 0; k < 300; k += 1) {
    const s = r() * e.total;
    const d = (r() - 0.5) * 2 * R;
    const margin = r() < 0.5 ? 0 : 8;
    const p = offsetAt(e, s, d);
    const nr = hw.nearest(p.x, p.y, hw.edgesNear({ x0: p.x - 1, y0: p.y - 1, x1: p.x + 1, y1: p.y + 1 }), R);
    const ra = hw.rampAt(e, s, d);
    yield line("q", s, d, p.x, p.y, nr ? [nr.d, nr.s, nr.z, nr.edge.id, nr.seg.s0] : "-", hw.covers(p.x, p.y, margin), opt(hw.underside(Math.floor(p.x), Math.floor(p.y))),
      hw.onRoad(p.x, p.y), ra ? [ra.z, ra.t, ra.outer, ra.inner, ra.ramp.sDeck] : "-");
  }
  for (const rp of ramps) {
    const s0 = Math.min(rp.sDeck, rp.sGround) - 30;
    const s1 = Math.max(rp.sDeck, rp.sGround) + 30;
    for (let k = 0; k < 40; k += 1) {
      const s = s0 + r() * (s1 - s0);
      const d = rp.side * (hw.rampMid + (r() - 0.5) * 80);
      const ra = hw.rampAt(e, s, d);
      yield line("rq", s, d, rampZ(e, rp, s), ra ? [ra.z, ra.t, ra.outer, ra.inner, ra.ramp.sDeck] : "-");
    }
  }
  for (let k = 0; k < 40; k += 1) {
    const s = -100 + r() * (e.total + 200);
    const p = pointAt(e, s);
    const q = offsetAt(e, s, (r() - 0.5) * 200);
    yield line("at", s, p.x, p.y, p.z, p.tx, p.ty, q.x, q.y, q.z);
  }
  for (let k = 0; k < 60; k += 1) {
    const s = r() * e.total;
    const d = (r() - 0.5) * 2 * (R + 60);
    const w2 = 8 + r() * 200;
    const h2 = 8 + r() * 200;
    const p = offsetAt(e, s, d);
    const rect = { x0: Math.floor(p.x), y0: Math.floor(p.y), x1: Math.floor(p.x + w2), y1: Math.floor(p.y + h2) };
    yield line("corr", rect.x0, rect.y0, rect.x1, rect.y1, ...hw.corridorsNear(rect).map((c) => `${c.edge.id}:${c.hitsRect(rect) ? 1 : 0}`));
  }
  const mid = pointAt(e, e.total / 2);
  for (const m of hw.mapData({ x0: mid.x - 500, y0: mid.y - 500, x1: mid.x + 500, y1: mid.y + 500 })) yield line("map", m.id, m.width, m.pts.length, m.pts[0], m.pts[m.pts.length - 1]);
  // the feature source: tiles' z ranges, and chunks over the deck, below it, on ramps, at piers and plateaus
  const spots = [];
  for (let k = 1; k <= 5; k += 1) {
    const p = pointAt(e, (e.total * k) / 6);
    spots.push([p.x, p.y, p.z], [p.x, p.y, p.z - 32]);
  }
  for (const rp of ramps) {
    const sm = (rp.sDeck + rp.sGround) / 2;
    const p = offsetAt(e, sm, rp.side * hw.rampMid);
    spots.push([p.x, p.y, rampZ(e, rp, sm)], [rp.x, rp.y, rp.zGround]);
  }
  for (const pr of hw.piers(e).slice(0, 4)) spots.push([pr.x, pr.y, pr.z - 24]);
  for (const j of e.junctions) spots.push([j.x, j.y, j.z], [j.x + j.r * 0.7, j.y - j.r * 0.7, j.z]);
  let n = 0;
  for (const [x, y, z] of spots) {
    for (const lod of [0, 2, 5]) {
      if (lod > 0 && n % 3 !== 0) continue;
      const sz = 32 << lod;
      const c = new ChunkBuffer(lod, Math.floor(x / sz), Math.floor(y / sz), Math.floor(z / sz));
      let tile = null;
      if (n % 2 === 1) {
        tile = { z: new Int32Array(P * P) };
        for (let j = 0; j < P; j += 1) for (let i = 0; i < P; i += 1) tile.z[i + j * P] = Math.round(w.terrain.sample(c.wx(i), c.wy(j)).h) + ((i * 7 + j * 3) % 9) - 4;
      }
      highwaySource.rasterize(w, c, tile);
      const box = c.worldBox;
      yield line("chunk", lod, c.cx, c.cy, c.cz, tile ? 1 : 0, opt(highwaySource.zRange(w, { x0: box.x0, y0: box.y0, x1: box.x1, y1: box.y1 })), ...chunkDigest(c));
    }
    n += 1;
  }
}

export default function* highways() {
  const r = samples(59);
  const inputs = {};
  for (const [key, id, size, [a0, b0, a1, b1], built] of NETS) {
    const rw = recordingWorld(configOf(id, size));
    const w = rw.w;
    const hw = w.highways;
    const c = hw.cfg;
    yield line("net", key, hw.spacing, hw.hw, hw.clear, hw.n, hw.plateau, hw.rampIn, hw.rampMid, c.jitter, c.edgeChance, c.minUrbanization, c.lanesPerSide, c.laneWidth, c.pierSpacing);
    warmAround(w, ((a0 + a1) / 2) * hw.spacing, ((b0 + b1) / 2) * hw.spacing);
    for (let b = b0; b <= b1; b += 1)
      for (let a = a0; a <= a1; a += 1) {
        const nd = hw.node(a, b);
        yield line("node", a, b, nd.x, nd.y, nd.z, hw.edgesAt(a, b).map((q) => q.join(":")), hw.nodeZ(a, b));
        for (const axis of [0, 1])
          yield line("lat", axis, a, b, hw.gradeOk(axis, a, b), hw.onIntercityRoute(axis, a, b), hw.chanceEdge(axis, a, b), hw.baseEdge(axis, a, b, 0), hw.baseEdge(axis, a, b, 1), hw.edgeExists(axis, a, b));
      }
    for (const [axis, a, b] of built) {
      const nd = hw.node(a, b);
      warmAround(w, nd.x, nd.y);
      yield* edgeRecords(w, hw.edge(axis, a, b), r);
    }
    inputs[key] = recorded(rw);
  }
  writeInputs("highways", inputs);
}
