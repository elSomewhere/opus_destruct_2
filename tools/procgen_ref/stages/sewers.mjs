// Stage "sewers": the sewers (underground/sewers.js) of worlds of lib/worlds.mjs (cityWorlds:
// SEWER_KEYS - cities, infinite and wrapping, islands with and without a subway, northern towns,
// planet faces, angled worlds): every cell of the cell network stage (stages/cellnet.mjs cellsOf:
// round the spawn, towns, villages, open country, a lap away, an island) - which of its roads are
// eligible, and its plan (runs, nodes with their arms, shafts, open manholes and halls, the boxes of
// a hall's stair by count and digest - every box of the first - and the openings); the plans near
// rects of every size and their z range (sewerSource.zRange at LOD 0, 1 and 2); blocksSurface,
// the sewers' and the World's (createWorld.js's: with the subway's), round every opening and open
// manhole (each bound of its test) and anywhere; mapData; nearestHall; hitsSubway round stations;
// and sewerSource.rasterize into chunks at LOD 0, 1 and 2 round halls, chambers, shafts (their
// manholes and cones on the street) and runs, each filled with ground first (lib/underground.mjs:
// a column tile of the stage's own, or none). The worlds are lib/underground.mjs undergroundWorld's:
// the street level is the terrain's height (docs/CITY.md §6).
import { REF, line, samples } from "../lib/rec.mjs";
import { cityWorlds } from "../lib/worlds.mjs";
import { chunkLine, fbb, planLines, tileRect, undergroundWorld } from "../lib/underground.mjs";
import { cellsOf } from "./cellnet.mjs";

const { sewerSource } = await import(REF + "underground/sewers.js");

/** The worlds of lib/worlds.mjs cityWorlds this stage samples (tests/city/test_sewers.cpp has the same). */
export const SEWER_KEYS = ["cities", "infiniteCity", "wrapWorld:small", "island:medium", "island:large", "nordicIsland:small", "oldHarbourTown", "planetNorth",
  "torusInfinite", "islandFlat", "allMountains", "cube5Wet", "angledCities", "angledInfiniteCity", "angledNordicTown:forest"];

export default function* sewers() {
  const r = samples(89);
  for (const [key, overrides] of cityWorlds().filter(([k]) => SEWER_KEYS.includes(k))) {
    const w = undergroundWorld(overrides);
    const S = w.sewers;
    const A = w.arterials;
    yield line("sewers", key, !!w.subway);
    // the cells and their plans
    const cells = cellsOf(w, r);
    yield line("cells", cells.length);
    const plans = [];
    let listed = false;
    for (const [i, j] of cells) {
      const net = w.cellNet(i, j);
      yield line("c", i, j, net.roads.map((rd) => (S.eligible(rd, i, j) ? "1" : "0")).join(""));
      const p = S.cellPlan(i, j);
      // every box of the world's first hall stair
      const full = !listed && p.boxes.length > 0;
      if (full) listed = true;
      yield* planLines(p, full);
      plans.push(p);
    }
    const nodes = plans.flatMap((p) => p.nodes);
    const runs = plans.flatMap((p) => p.runs);
    // plans near rects (a point, a chunk, a tile, a cell) round the cells, and their z range
    for (let k = 0; k < 24; k += 1) {
      const [i, j] = cells[Math.floor(r() * cells.length)];
      const R = A.cellRect(i, j);
      const x0 = Math.round(R.x0 + r() * (R.x1 - R.x0));
      const y0 = Math.round(R.y0 + r() * (R.y1 - R.y0));
      const size = [0, 33, 270, 4000][k % 4];
      const rect = { x0, y0, x1: x0 + size, y1: y0 + size };
      yield line("near", x0, y0, size, ...S.near(rect).map((p) => `${fbb(p.bb)}/${p.runs.length}/${p.nodes.length}`));
      yield line("zr", sewerSource.zRange(w, rect, k % 3) ?? "-");
    }
    // blocksSurface, the sewers' and the World's: round every opening and the first open manholes
    // (each bound of its test), and anywhere in the cells
    const probe = (x, y) => (S.blocksSurface(x, y) ? "1" : "0") + (w.blocksSurface(x, y) ? "1" : "0");
    for (const p of plans)
      for (const o of p.openings) {
        let b = "";
        for (const x of [o.x0 - 1, o.x0, Math.round((o.x0 + o.x1) / 2), o.x1, o.x1 + 1]) for (const y of [o.y0 - 1, o.y0, Math.round((o.y0 + o.y1) / 2), o.y1, o.y1 + 1]) b += probe(x, y);
        yield line("bo", o.x0, o.y0, b);
      }
    let manholes = 0;
    for (const n of nodes) {
      if (!n.open || manholes >= 12) continue;
      manholes += 1;
      const sx = n.x + n.qx * 12;
      const sy = n.y + n.qy * 12;
      let b = "";
      for (const dx of [-20, -19, 0, 19, 20]) for (const dy of [-20, -19, 0, 19, 20]) b += probe(sx + dx, sy + dy);
      yield line("bm", n.key, b);
    }
    let b = "";
    for (let k = 0; k < 120; k += 1) {
      const [i, j] = cells[Math.floor(r() * cells.length)];
      const R = A.cellRect(i, j);
      const x = Math.round(R.x0 + r() * (R.x1 - R.x0));
      const y = Math.round(R.y0 + r() * (R.y1 - R.y0));
      b += probe(x, y);
    }
    yield line("br", b);
    // mapData, nearestHall
    for (let k = 0; k < 2; k += 1) {
      const [i, j] = cells[Math.floor(r() * cells.length)];
      const R = A.cellRect(i, j);
      const md = S.mapData({ x0: R.x0 - 400, y0: R.y0 - 300, x1: R.x1 + 200, y1: R.y1 + 500 });
      yield line("map", md.lines.length, md.halls.length);
      for (const l of md.lines) yield line("ml", l.flat());
      for (const h of md.halls) yield line("mh", h.x, h.y, h.top.x, h.top.y, h.top.z);
    }
    for (let k = 0; k < 6; k += 1) {
      const [i, j] = cells[Math.floor(r() * cells.length)];
      const R = A.cellRect(i, j);
      const x = Math.round(R.x0 + r() * (R.x1 - R.x0));
      const y = Math.round(R.y0 + r() * (R.y1 - R.y0));
      const h = k === 0 ? S.nearestHall(x, y) : S.nearestHall(x, y, 3000);
      yield line("hall", x, y, h ? [h.key, h.stairTop.x, h.stairTop.y, h.stairTop.z] : "-");
    }
    // hitsSubway round the stations near the spawn's cells
    if (w.subway) {
      const c0 = A.cellAt(0, 0);
      const R = A.cellRect(c0.i, c0.j);
      const st = w.subway.stationsNear({ x0: R.x0 - 6000, y0: R.y0 - 6000, x1: R.x1 + 6000, y1: R.y1 + 6000 });
      let h = "";
      for (const s of st.slice(0, 6))
        for (let k = 0; k < 20; k += 1) {
          const x = Math.round(s.x + (r() - 0.5) * 900);
          const y = Math.round(s.y + (r() - 0.5) * 900);
          const z = Math.round(s.zp + (r() - 0.6) * 200);
          const e = Math.floor(r() * 40);
          h += S.hitsSubway({ x0: x - e, y0: y - e, z0: z - e, x1: x + e, y1: y + e, z1: z + e }) ? "1" : "0";
        }
      yield line("hs", st.length, h);
    }
    // column tiles' z range and chunks round halls, chambers, shafts and runs
    const raster = (ch, tile) => sewerSource.rasterize(w, ch, tile);
    const picks = [...nodes.filter((n) => n.hall).slice(0, 2), ...nodes.filter((n) => n.open).slice(0, 3), ...nodes.filter((n) => n.shaft && !n.open).slice(0, 3),
      ...nodes.filter((n) => !n.shaft && !n.hall).slice(0, 2)];
    let q = 0;
    for (const n of picks) {
      for (const lod of [0, 1, 2]) {
        const span = 32 << lod;
        yield line("tz", n.key, lod, sewerSource.zRange(w, tileRect(lod, Math.floor(n.x / span), Math.floor(n.y / span)), lod) ?? "-");
        const sx = n.x + n.qx * 12;
        const sy = n.y + n.qy * 12;
        yield chunkLine(w, lod, n.x + 3, n.y - 5, n.z, raster, q++ % 5 === 4);
        yield chunkLine(w, lod, sx, sy, n.zr, raster, q++ % 5 === 4);
        if (n.hall) yield chunkLine(w, lod, n.x - 40, n.y + 30, n.z - 20, raster, q++ % 5 === 4);
      }
    }
    for (const rn of runs.slice(0, 4)) {
      const l = Math.round((rn.l0 + rn.l1) / 2);
      const [x, y] = rn.axis === 0 ? [rn.fixed + 6, l] : [l, rn.fixed - 8];
      for (const lod of [0, 1, 2]) yield chunkLine(w, lod, x, y, Math.round((rn.z0 + rn.z1) / 2) + 4, raster, q++ % 5 === 4);
    }
    // ... above and below the first pick (the plans kept out by their bounds), and in the middle of
    // the first cell without sewers
    if (picks.length) {
      const n = picks[0];
      yield chunkLine(w, 0, n.x, n.y, n.zr + 120, raster);
      yield chunkLine(w, 0, n.x, n.y, n.z - 200, raster);
    }
    const bare = cells.find(([i, j]) => !S.cellPlan(i, j).bb);
    if (bare) {
      const R = A.cellRect(bare[0], bare[1]);
      const x = Math.round((R.x0 + R.x1) / 2);
      const y = Math.round((R.y0 + R.y1) / 2);
      yield chunkLine(w, 0, x, y, Math.round(w.streetLevel(x, y)), raster);
    }
    yield line("count", plans.length, nodes.length, runs.length, picks.length, bare ?? "-");
  }
}
