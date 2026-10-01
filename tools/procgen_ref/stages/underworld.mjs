// Stage "underworld": the subway and the sewers of createWorld's worlds (lib/worlds.mjs
// cityWorlds: UNDERWORLD_KEYS), as the generator runs them - their street level the road levels'
// (createWorld's streetLevel), on cell networks with createWorld's lakes, highways and harbour
// grading - where the stages "subway" and "sewers" check their logic on a World of World.js with
// the terrain's height for a street level: the first cells of the cell network stage's (cellsOf)
// and their plans, the stations round the spawn, blocksSurface (the World's) round openings and
// open manholes, and both feature sources' z ranges and chunks at LOD 0 and 1 round a few nodes
// (lib/underground.mjs: tiles and ground of the stage's own). Each cell network is made once (road
// identity, docs/CITY.md §6); every terrain sample makes what it reads first (pureTerrain).
import { REF, line, samples } from "../lib/rec.mjs";
import { cityWorlds, pureTerrain } from "../lib/worlds.mjs";
import { chunkLine, planLines, stationLines, tileRect } from "../lib/underground.mjs";
import { cellsOf } from "./cellnet.mjs";

const { createWorld } = await import(REF + "world/createWorld.js");
const { LRU } = await import(REF + "core/lru.js");
const { sewerSource } = await import(REF + "underground/sewers.js");
const { subwaySource } = await import(REF + "underground/subway.js");

/** The worlds of lib/worlds.mjs cityWorlds this stage samples (tests/city/test_underworld.cpp has the same). */
export const UNDERWORLD_KEYS = ["cities", "infiniteCity", "island:medium", "oldHarbourTown", "angledCities"];

export default function* underworld() {
  const r = samples(61);
  for (const [key, overrides] of cityWorlds().filter(([k]) => UNDERWORLD_KEYS.includes(k))) {
    const w = pureTerrain(createWorld(overrides));
    w.cellNets = new LRU(Infinity);
    const S = w.sewers;
    const sw = w.subway;
    yield line("world", key, !!sw);
    // the first cells and their plans
    const cells = cellsOf(w, r).slice(0, 12);
    const plans = [];
    for (const [i, j] of cells) {
      yield line("c", i, j);
      const p = S.cellPlan(i, j);
      yield* planLines(p, false);
      plans.push(p);
    }
    // the stations round the spawn
    if (sw) {
      const c0 = w.arterials.cellAt(0, 0);
      for (let axis = 0; axis < 2; axis += 1)
        for (let di = -2; di <= 2; di += 1)
          for (let dj = -2; dj <= 2; dj += 1) {
            const i = (axis === 0 ? c0.i : c0.j) + di;
            const j = (axis === 0 ? c0.j : c0.i) + dj;
            if (!sw.lineExists(axis, i)) continue;
            const s = sw.station(axis, i, j);
            if (s) yield* stationLines(s, false);
          }
    }
    // the World's blocksSurface round openings and the first open manholes
    const nodes = plans.flatMap((p) => p.nodes);
    let b = "";
    for (const p of plans) for (const o of p.openings) for (const [x, y] of [[o.x0 - 1, o.y0], [o.x0, o.y0], [Math.round((o.x0 + o.x1) / 2), o.y1], [o.x1 + 1, o.y1]]) b += w.blocksSurface(x, y) ? "1" : "0";
    for (const n of nodes.filter((q) => q.open).slice(0, 8))
      for (const [dx, dy] of [[0, 0], [19, -19], [20, 0], [-20, 19]]) b += w.blocksSurface(n.x + n.qx * 12 + dx, n.y + n.qy * 12 + dy) ? "1" : "0";
    yield line("bs", b);
    // the feature sources round a few nodes
    const raster = (ch, tile) => {
      sewerSource.rasterize(w, ch, tile);
      if (sw) subwaySource.rasterize(w, ch);
    };
    const picks = [...nodes.filter((n) => n.hall).slice(0, 1), ...nodes.filter((n) => n.open).slice(0, 2), ...nodes.filter((n) => !n.shaft).slice(0, 1)];
    for (const n of picks)
      for (const lod of [0, 1]) {
        const span = 32 << lod;
        const rect = tileRect(lod, Math.floor(n.x / span), Math.floor(n.y / span));
        yield line("tz", n.key, lod, sewerSource.zRange(w, rect, lod) ?? "-", sw ? subwaySource.zRange(w, rect, lod) ?? "-" : "-");
        yield chunkLine(w, lod, n.x + 3, n.y - 5, n.z, raster);
        yield chunkLine(w, lod, n.x + n.qx * 12, n.y + n.qy * 12, n.zr, raster);
      }
    yield line("count", plans.length, nodes.length, picks.length);
  }
}
