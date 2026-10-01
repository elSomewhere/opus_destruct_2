// Stage "subway": the subway (underground/subway.js) of worlds of lib/worlds.mjs (cityWorlds:
// SUBWAY_KEYS - cities, infinite and wrapping, planet faces, islands with a subway, angled worlds,
// and an island whose preset has none): which lines carry one, every node round the spawn, round
// towns, far away and a lap away (its spans, river test, platform level) and every station there
// (its boxes by count and digest, every box of the first station of each kind: on a vertical line,
// on a horizontal one, a transfer under a crossing); the stations and tunnel spans near rects of
// every size and their z range (subwaySource.zRange); mapData; blocksSurface round the entrances
// (each bound of its test) and anywhere; and subwaySource.rasterize into chunks at LOD 0, 1 and 2
// round stations (platform, mezzanine, an entrance's top, a hall's end) and along tunnels (their
// middle, the tracks' spread before a station), each filled with ground first (lib/underground.mjs:
// a column tile of the stage's own). The worlds are lib/underground.mjs undergroundWorld's: the
// street level is the terrain's height (docs/CITY.md §6).
import { REF, line, samples } from "../lib/rec.mjs";
import { cityWorlds } from "../lib/worlds.mjs";
import { chunkLine, stationLines, tileRect, tunnelLine, undergroundWorld } from "../lib/underground.mjs";

const { subwaySource } = await import(REF + "underground/subway.js");

/** The worlds of lib/worlds.mjs cityWorlds this stage samples (tests/city/test_subway.cpp has the same). */
export const SUBWAY_KEYS = ["cities", "infiniteCity", "wrapWorld:small", "island:medium", "planetNorth", "torusInfinite", "cube2", "islandFlat",
  "islandCrowded", "allMountains", "angledCities", "angledInfiniteCity"];

/** The nodes [axis, i, j] (line i of the axis at cross line j) a world's records cover, drawn from r, each once, in the order drawn. */
export function nodesOf(w, r) {
  const A = w.arterials;
  const out = [];
  const seen = new Set();
  const add = (axis, i, j) => {
    const k = `${axis},${i},${j}`;
    if (seen.has(k)) return;
    seen.add(k);
    out.push([axis, i, j]);
  };
  // both axes' nodes round cell (ci, cj): lines ci +- d (axis 0) and cj +- d (axis 1), each at the cross lines +- d
  const around = (ci, cj, d) => {
    for (let axis = 0; axis < 2; axis += 1)
      for (let di = -d; di <= d; di += 1) for (let dj = -d; dj <= d; dj += 1) add(axis, (axis === 0 ? ci : cj) + di, (axis === 0 ? cj : ci) + dj);
  };
  const c0 = A.cellAt(0, 0);
  around(c0.i, c0.j, 4);
  for (const s of w.fields.settlementsIn({ x0: -120000, y0: -120000, x1: 120000, y1: 120000 }).slice(0, 4)) {
    const c = A.cellAt(Math.round(s.x), Math.round(s.y));
    around(c.i, c.j, 2);
  }
  for (let k = 0; k < 6; k += 1) {
    const x = Math.round((r() - 0.5) * 400000);
    const y = Math.round((r() - 0.5) * 400000);
    const c = A.cellAt(x, y);
    around(c.i, c.j, 1);
  }
  // a wrapping world: the spawn's nodes a lap or two away
  if (A.n) for (const [li, lj] of [[1, 0], [-1, 2]]) around(c0.i + li * A.n, c0.j + lj * A.n, 1);
  return out;
}

export default function* subway() {
  const r = samples(97);
  for (const [key, overrides] of cityWorlds().filter(([k]) => SUBWAY_KEYS.includes(k))) {
    const w = undergroundWorld(overrides);
    const sw = w.subway;
    yield line("subway", key, sw ? [sw.cfg.lineEvery, sw.cfg.minUrbanization, sw.art.hc] : "-");
    if (!sw) continue;
    const bits = (axis) => {
      let s = "";
      for (let i = -12; i <= 12; i += 1) s += sw.lineExists(axis, i) ? "1" : "0";
      return s;
    };
    yield line("lines", bits(0), bits(1));
    // the nodes and their stations
    const nodes = nodesOf(w, r);
    const stations = [];
    const listed = new Set();
    for (const [axis, i, j] of nodes) {
      const n = sw.nodeXY(axis, i, j);
      yield line("n", axis, i, j, n.x, n.y, sw.lineExists(axis, i), sw.spanActive(axis, i, j - 1), sw.spanActive(axis, i, j), sw.riverBlocked(axis, i, j), sw.hasStation(axis, i, j), sw.platformZ(axis, i, j));
      const s = sw.station(axis, i, j);
      if (!s) continue;
      stations.push(s);
      // the first station of each kind in full: on a vertical line, a horizontal one, a transfer
      const kind = axis === 0 ? "v" : sw.hasStation(0, j, i) ? "x" : "h";
      const full = !listed.has(kind);
      listed.add(kind);
      yield* stationLines(s, full);
    }
    // stations and tunnel spans near rects (a point, a chunk, a tile, a district, a town) round
    // stations (four in five) and nodes, and their z range
    for (let k = 0; k < 40; k += 1) {
      const n = k % 5 !== 4 && stations.length ? stations[Math.floor(r() * stations.length)] : sw.nodeXY(...nodes[Math.floor(r() * nodes.length)]);
      const size = [0, 33, 270, 1500, 6000][k % 5];
      const x0 = Math.round(n.x + (r() - 0.5) * 1000 - size / 2);
      const y0 = Math.round(n.y + (r() - 0.5) * 1000 - size / 2);
      const rect = { x0, y0, x1: x0 + size, y1: y0 + Math.round(size * r()) };
      yield line("near", rect.x0, rect.y0, rect.x1, rect.y1, ...sw.stationsNear(rect).map((s) => `${s.axis}/${s.i}/${s.j}`));
      for (const t of sw.tunnelsNear(rect)) yield tunnelLine(t);
      yield line("zr", subwaySource.zRange(w, rect, k % 3) ?? "-");
    }
    // mapData
    for (let k = 0; k < 3; k += 1) {
      const [axis, i, j] = nodes[Math.floor(r() * nodes.length)];
      const n = sw.nodeXY(axis, i, j);
      const rect = { x0: n.x - 3000, y0: n.y - 2000, x1: n.x + 2000, y1: n.y + 3000 };
      const md = sw.mapData(rect);
      yield line("map", md.lines.length, md.stations.length);
      for (const l of md.lines) yield line("ml", l.pts.flat());
      for (const s of md.stations) yield line("ms", s.x, s.y, s.axis);
    }
    // blocksSurface round the entrances of the first stations (each bound of its test) ...
    const hc = sw.art.hc;
    const cs = [0, hc + 1, hc + 2, hc + 3, hc + 20, hc + 35, hc + 36, hc + 37];
    const ls = [0, 49, 50, 51, 130, 219, 220, 221, 260];
    for (const s of stations.slice(0, 12)) {
      let b = "";
      for (const sc of [-1, 1])
        for (const c of cs)
          for (const sl of [-1, 1])
            for (const l of ls) {
              const x = s.x + (s.axis === 0 ? sc * c : sl * l);
              const y = s.y + (s.axis === 0 ? sl * l : sc * c);
              b += sw.blocksSurface(x, y) ? "1" : "0";
            }
      yield line("bs", s.axis, s.i, s.j, b);
    }
    // ... and anywhere near the nodes
    let b = "";
    for (let k = 0; k < 200; k += 1) {
      const [axis, i, j] = nodes[Math.floor(r() * nodes.length)];
      const n = sw.nodeXY(axis, i, j);
      const x = Math.round(n.x + (r() - 0.5) * 600);
      const y = Math.round(n.y + (r() - 0.5) * 600);
      b += sw.blocksSurface(x, y) ? "1" : "0";
    }
    yield line("bsr", b);
    // column tiles' z range and chunks round stations: the platform, the mezzanine, an entrance's top, a hall's end
    const raster = (ch) => subwaySource.rasterize(w, ch);
    for (const s of stations.slice(0, 4)) {
      const along = (c, l) => (s.axis === 0 ? [s.x + c, s.y + l] : [s.x + l, s.y + c]);
      for (const lod of [0, 1, 2]) {
        const span = 32 << lod;
        const rect = tileRect(lod, Math.floor(s.x / span), Math.floor(s.y / span));
        yield line("tz", lod, subwaySource.zRange(w, rect, lod) ?? "-");
        const [ex, ey] = along(hc + 20, 200);
        const [hx, hy] = along(-30, 384);
        yield chunkLine(w, lod, s.x + 5, s.y - 7, s.zp, raster);
        yield chunkLine(w, lod, s.x, s.y, s.zs - 56, raster);
        yield chunkLine(w, lod, ex, ey, s.zs, raster);
        yield chunkLine(w, lod, hx, hy, s.zp + 20, raster);
      }
    }
    // ... above and below the first (its boxes kept out by their bounds)
    if (stations.length) {
      const s = stations[0];
      yield chunkLine(w, 0, s.x, s.y, s.zs + 200, raster);
      yield chunkLine(w, 0, s.x, s.y, s.zp - 300, raster);
    }
    // ... and along tunnels: the middle of a span, the tracks' spread before a station, its first voxel
    let tunnels = 0;
    for (let k = 0; k < stations.length && tunnels < 4; k += 1) {
      const s = stations[k];
      for (const t of sw.tunnelsNear({ x0: s.x - 600, y0: s.y - 600, x1: s.x + 600, y1: s.y + 600 })) {
        if (tunnels >= 4) break;
        tunnels += 1;
        const at = (l) => (t.axis === 0 ? [t.fixed + 9, l] : [l, t.fixed - 11]);
        const [mx, my] = at(Math.round((t.l0 + t.l1) / 2));
        const [sx, sy] = at(t.l0 + 100);
        for (const lod of [0, 1, 2]) {
          yield chunkLine(w, lod, mx, my, Math.round((t.z0 + t.z1) / 2) + 6, raster);
          yield chunkLine(w, lod, sx, sy, t.z0 + 6, raster);
        }
        const [fx, fy] = at(t.l0);
        yield chunkLine(w, 0, fx, fy, t.z0 + 6, raster);
      }
    }
    // ... through the first node a river keeps a station from (the tracks do not spread there)
    const wet = nodes.find(([axis, i, j]) => sw.riverBlocked(axis, i, j) && sw.spanActive(axis, i, j));
    if (wet) {
      const n = sw.nodeXY(...wet);
      for (const t of sw.tunnelsNear({ x0: n.x - 50, y0: n.y - 50, x1: n.x + 50, y1: n.y + 50 })) {
        if (t.s0) continue;
        const l = (t.axis === 0 ? n.y : n.x) + 30;
        const [x, y] = t.axis === 0 ? [t.fixed - 20, l] : [l, t.fixed + 20];
        yield tunnelLine(t);
        for (const lod of [0, 1]) yield chunkLine(w, lod, x, y, t.z0 + 6, raster);
      }
    }
    yield line("count", stations.length, tunnels, wet ?? "-");
  }
}
