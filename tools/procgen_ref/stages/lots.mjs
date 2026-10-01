// Stage "lots": the subdivision of blocks into lots (city/lots.js). The blocks of the cell
// networks of every world of lib/worlds.mjs (cityWorlds: the cells of stages/cellnet.mjs cellsOf,
// each network made once), one in six by its own district's lot mode, one in six by another
// district's (each of districts.js's in turn), one in six by microLots, rowLotsOf, wholeBlockLot or
// freeLot (in turn); synthetic blocks - slivers to farmland, every class of road side, alleys,
// sides with and without a road id - by a lot mode each (every district's in turn) and by every
// other function; mainFrontage of synthetic frontages. Every field of every lot.
// tests/city/test_lots.cpp writes the same records. (lib/worlds.mjs pureTerrain: the networks are
// what they are in any order; docs/CITY.md §6.)
import { REF, f, line, samples } from "../lib/rec.mjs";
import { cityWorlds, pureTerrain, withSeaTests } from "../lib/worlds.mjs";
import { frect } from "../lib/city.mjs";
import { cellsOf } from "./cellnet.mjs";

const { World } = await import(REF + "world/World.js");
const { planCellNetwork } = await import(REF + "city/cellNetwork.js");
const L = await import(REF + "city/lots.js");
const { DISTRICTS } = await import(REF + "world/registry.js");
const { Rng } = await import(REF + "core/hash.js");

/** districts.js's districts, in registration order (the port's register_all may hold more). */
export const DISTRICT_IDS = ["downtown", "midtown", "mixed", "residential", "suburban", "industrial", "heavyIndustry", "microdistrict", "projects", "port", "harbour",
  "oldtown", "oldcore", "village", "sea", "park", "rural"];
const SIDES = ["N", "S", "W", "E"];

// The fields of the port's lot record (city/lots.hpp) lots.js sets: a field the reference gives a
// lot that is not listed here would be one the port lacks.
const LOT_KEYS = new Set(["id", "rect", "block", "cell", "district", "frontages", "front", "alley", "corner", "whole", "farmstead", "farmRole", "farm", "village", "micro", "arch",
  "cabin", "chapel", "civic"]);
const ffront = (fr) => fr.map((q) => `${q.side}/${q.cls}`).join(";");

/** A lot as a line: every field ("-" where JS leaves one undefined or null). */
export function lotLine(lot) {
  for (const k of Object.keys(lot)) if (!LOT_KEYS.has(k)) throw new Error(`lot ${lot.id}: a field the port's record lacks: ${k}`);
  for (const q of lot.frontages) for (const k of Object.keys(q)) if (k !== "side" && k !== "cls") throw new Error(`lot ${lot.id}: a frontage field the port lacks: ${k}`);
  return line("l", lot.id, frect(lot.rect), lot.block, lot.cell, lot.district, ffront(lot.frontages), lot.front, lot.alley, lot.corner, lot.whole, lot.farmstead, lot.farmRole,
    lot.farm, lot.village, lot.micro, lot.arch, lot.cabin, lot.chapel, lot.civic);
}

/** A block's lots: a header (the block, what planned them, how many) and a line each. */
function* lotsOf(tag, block, lots) {
  yield line(tag, block.id, lots.length);
  for (const lot of lots) yield lotLine(lot);
}

/**
 * The other lot functions on a block, the k-th in turn (k % 4): microLots, rowLotsOf (one of
 * three width ranges), wholeBlockLot, freeLot (a rect drawn inside the block from r, a front and
 * an extra).
 */
function* otherLots(block, k, rng, r) {
  const p = block.prop;
  switch (k % 4) {
    case 0:
      yield* lotsOf("micro", block, L.microLots(block, rng));
      return;
    case 1: {
      const width = [[7, 11], [12, 26], [30, 60]][Math.floor(r() * 3)];
      yield* lotsOf(`row${width[0]}`, block, L.rowLotsOf(block, rng, width));
      return;
    }
    case 2:
      yield* lotsOf("whole", block, [L.wholeBlockLot(block, rng)]);
      return;
    default: {
      const x0 = p.x0 + Math.floor(r() * Math.max(1, p.x1 - p.x0));
      const y0 = p.y0 + Math.floor(r() * Math.max(1, p.y1 - p.y0));
      const rect = { x0, y0, x1: Math.min(p.x1, x0 + 8 + Math.floor(r() * 300)), y1: Math.min(p.y1, y0 + 8 + Math.floor(r() * 300)) };
      const front = SIDES[Math.floor(r() * 4)];
      const e = Math.floor(r() * 4);
      const extra = e === 0 ? { cabin: true } : e === 1 ? { chapel: true } : e === 2 ? { civic: "museum" } : {};
      yield* lotsOf("free", block, [L.freeLot(block, rect, front, rng, extra)]);
    }
  }
}

const CLS = ["arterial", "collector", "local", "village", "pedestrian", "rural", "lane", "alley", null, null];
/** A road side drawn from r: a class (or none), a half width and an id (or null). */
function side(r) {
  const cls = CLS[Math.floor(r() * CLS.length)];
  if (cls === null) return { cls: null, hr: 0, id: null };
  const hr = 8 + Math.floor(r() * 40);
  const id = r() < 0.5 ? `S/r${Math.floor(r() * 100)}` : null;
  return { cls, hr, id };
}

const FCLS = ["arterial", "collector", "local", "village", "pedestrian", "rural", "lane", "alley", "highway", "street"];

export default function* lots() {
  const r = samples(83);
  // the blocks of the cell networks of every world
  let alt = 0;
  let other = 0;
  for (const [key, overrides] of cityWorlds()) {
    const w = pureTerrain(withSeaTests(new World(overrides)));
    const cells = cellsOf(w, r);
    yield line("world", key, cells.length);
    for (const [i, j] of cells) {
      const net = planCellNetwork(w, i, j);
      yield line("cell", net.id, net.blocks.length);
      for (let k = 0; k < net.blocks.length; k += 1) {
        const b = net.blocks[k];
        if (k % 6 === 0) yield* lotsOf("own", b, L.planBlockLots(b, DISTRICTS.get(b.district), Rng.from(w.seed, b.id, "use")));
        else if (k % 6 === 2) {
          const id = DISTRICT_IDS[alt % DISTRICT_IDS.length];
          alt += 1;
          yield* lotsOf(id, b, L.planBlockLots(b, DISTRICTS.get(id), Rng.from(w.seed, b.id, "alt")));
        } else if (k % 6 === 4) {
          yield* otherLots(b, other, Rng.from(w.seed, b.id, "other"), r);
          other += 1;
        }
      }
    }
  }
  // synthetic blocks: a lot mode each (every district's in turn), then every other function
  for (let k = 0; k < 400; k += 1) {
    const id = DISTRICT_IDS[k % DISTRICT_IDS.length];
    const d = DISTRICTS.get(id);
    const big = d.lots.mode === "rural" || d.lots.mode === "village" ? 4000 : d.lots.mode === "industrial" ? 2400 : 1200;
    const x0 = Math.floor((r() - 0.5) * 200000);
    const y0 = Math.floor((r() - 0.5) * 200000);
    const a = r();
    const b = r();
    const c = r();
    const e = r();
    const width = Math.floor(a * b * big);
    const height = Math.floor(c * e * big);
    const sides = {};
    for (const s of ["N", "E", "S", "W"]) sides[s] = side(r);
    const block = { id: `S${k}/b${k % 7}`, cell: `S${k}`, district: id, prop: { x0, y0, x1: x0 + width, y1: y0 + height }, sides };
    yield line("syn", block.id, id, frect(block.prop), ...["N", "E", "S", "W"].map((s) => `${f(sides[s].cls)}/${f(sides[s].hr)}/${f(sides[s].id)}`));
    yield* lotsOf("plan", block, L.planBlockLots(block, d, Rng.from(77, block.id, "plan")));
    for (let q = 0; q < 4; q += 1) yield* otherLots(block, q, Rng.from(77, block.id, q), r);
  }
  // mainFrontage
  for (let k = 0; k < 300; k += 1) {
    const x0 = Math.floor((r() - 0.5) * 20000);
    const y0 = Math.floor((r() - 0.5) * 20000);
    const rect = { x0, y0, x1: x0 + Math.floor(r() * 600), y1: y0 + Math.floor(r() * 600) };
    const n = Math.floor(r() * 5);
    const fr = [];
    for (let q = 0; q < n; q += 1) {
      const s = SIDES[Math.floor(r() * 4)];
      fr.push({ side: s, cls: FCLS[Math.floor(r() * FCLS.length)] });
    }
    yield line("mf", frect(rect), ffront(fr), L.mainFrontage(rect, fr));
  }
}
