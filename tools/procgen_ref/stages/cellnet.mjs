// Stage "cellnet": the cell networks (city/cellNetwork.js planCellNetwork: roads, sub-cells and
// blocks of an arterial cell, with city/streets.js and city/diagonals.js) of every world of
// lib/worlds.mjs (cityWorlds) - the cells round the spawn, at the centre, middle, edge and
// outskirts of towns, at villages, out in the country, a lap away in a wrapping world and all
// over an island - every field of every edge, road, block and sub-cell; edgeInfo of edges
// anywhere; the island's sea tests (createWorld.js). Every base height a cell's planning can
// read is made first (lib/worlds.mjs warmBasesIn, warmIsland; docs/CITY.md §6), so the records
// are what they are in any order.
import { REF, line, samples } from "../lib/rec.mjs";
import { cityWorlds, warmBasesIn, warmIsland, withSeaTests } from "../lib/worlds.mjs";
import { edgeLine, netLines } from "../lib/city.mjs";

const { World } = await import(REF + "world/World.js");
const { planCellNetwork, edgeInfo } = await import(REF + "city/cellNetwork.js");

/** The cells a world's records cover, drawn from r: [i, j], each once, in the order drawn. */
export function cellsOf(w, r) {
  const A = w.arterials;
  const F = w.fields;
  const out = [];
  const seen = new Set();
  const add = (i, j) => {
    const k = `${i},${j}`;
    if (seen.has(k)) return;
    seen.add(k);
    out.push([i, j]);
  };
  const at = (x, y) => {
    const c = A.cellAt(Math.round(x), Math.round(y));
    add(c.i, c.j);
  };
  // the spawn town's centre and round it
  const c0 = A.cellAt(0, 0);
  for (let dj = -1; dj <= 1; dj += 1) for (let di = -1; di <= 1; di += 1) add(c0.i + di, c0.j + dj);
  // towns: their centres, middles, edges and outskirts, each in a direction of its own
  for (const s of F.settlementsIn({ x0: -120000, y0: -120000, x1: 120000, y1: 120000 }).slice(0, 6))
    for (const d of [0, 0.45, 0.85, 1.25, 1.8]) {
      const a = r() * 2 * Math.PI;
      at(s.x + Math.cos(a) * d * s.radius, s.y + Math.sin(a) * d * s.radius);
    }
  // villages and hamlets: their centres and edges
  for (const v of F.villagesIn({ x0: -60000, y0: -60000, x1: 60000, y1: 60000 }).slice(0, 6))
    for (const d of [0, 1.1]) {
      const a = r() * 2 * Math.PI;
      at(v.x + Math.cos(a) * d * v.radius, v.y + Math.sin(a) * d * v.radius);
    }
  // open country, near and far
  for (let k = 0; k < 6; k += 1) at((r() - 0.5) * 400000, (r() - 0.5) * 400000);
  // a wrapping world: the spawn's cells a lap or two away
  if (A.n) for (const [li, lj] of [[1, 0], [-1, 1], [2, -1]]) add(c0.i + li * A.n, c0.j + lj * A.n + li);
  // an island: all over it (its coasts)
  if (F.island) {
    const b = F.island.bounds();
    for (let k = 0; k < 10; k += 1) at((b.x0 + r() * (b.x1 - b.x0)) * 8, (b.y0 + r() * (b.y1 - b.y0)) * 8);
  }
  return out;
}

export default function* cellnet() {
  const r = samples(53);
  for (const [key, overrides] of cityWorlds()) {
    const w = withSeaTests(new World(overrides));
    const A = w.arterials;
    warmIsland(w);
    const cells = cellsOf(w, r);
    yield line("world", key, cells.length);
    for (const [i, j] of cells) {
      warmBasesIn(w, A.cellRect(i, j));
      yield* netLines(planCellNetwork(w, i, j));
    }
    // edges anywhere (country roads and their slopes, villages, towns)
    for (let k = 0; k < 24; k += 1) {
      const axis = r() < 0.5 ? 0 : 1;
      const ln = Math.floor((r() - 0.5) * 300);
      const span = Math.floor((r() - 0.5) * 300);
      const fixed = A.line(axis, ln);
      const s0 = A.line(1 - axis, span);
      const s1 = A.line(1 - axis, span + 1);
      warmBasesIn(w, axis === 0 ? { x0: fixed, y0: s0, x1: fixed, y1: s1 } : { x0: s0, y0: fixed, x1: s1, y1: fixed });
      yield edgeLine("e", edgeInfo(w, axis, ln, span));
    }
    // the island's sea tests
    if (w.fields.island) {
      const b = w.fields.island.bounds();
      for (let k = 0; k < 60; k += 1) {
        const x = Math.round((b.x0 + r() * (b.x1 - b.x0)) * 8);
        const y = Math.round((b.y0 + r() * (b.y1 - b.y0)) * 8);
        const rect = { x0: x, y0: y, x1: x + Math.floor(r() * 3000), y1: y + Math.floor(r() * 3000) };
        const m = Math.floor(r() * 12) - 2;
        const bx = x + (r() - 0.5) * 6000;
        const by = y + (r() - 0.5) * 6000;
        yield line("sea", x, y, w.seaAt(x, y), w.seaAt(x, y, m), w.seaHitsRect(rect), w.seaHitsRect(rect, m), w.seaShare(rect), w.seaHitsSeg(x, y, bx, by), w.seaHitsSeg(x, y, bx, by, m));
      }
    }
  }
}
