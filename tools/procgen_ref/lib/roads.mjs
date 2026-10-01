// Road inputs of the road network's stages (network/roadView.js, roadSurface.js, roadLevel.js,
// roadParts.js, highways.js), the same in tests/city/road_inputs.hpp:
//
//   - scripted road sets drawn from samples(): every class (and one the class table lacks), every
//     cross-section field left out now and then (the road view's defaults), cobbles, strips, the
//     angled world's owner cells, degenerate pieces, roads ending in another's carriageway;
//   - the reference's own roads and waters, recorded while a stage reads them (data/<stage>.json):
//     the port's cell networks and waters (createWorld's isWet: rivers, lakes, the sea) are later
//     stages, so its tests read the reference's (World::cell_roads, World::wet_source).
import { writeFileSync, mkdirSync } from "node:fs";
import { REF, f } from "./rec.mjs";
import { warmBasesIn } from "./worlds.mjs";

const { createWorld } = await import(REF + "world/createWorld.js");
const { LRU } = await import(REF + "core/lru.js");
const { roadSpecs } = await import(REF + "network/roadClasses.js");

export const DATA = new URL("../data/", import.meta.url);

/** The classes scripted roads take: the class table's, and one it lacks (rank undefined). */
export const SCRIPT_CLASSES = ["highway", "arterial", "collector", "local", "village", "alley", "lane", "pedestrian", "rural", "path", "bogus"];
const STRIPS = ["grass", "pits", "none", null];

/**
 * A scripted road set (index k) of roads in a 1200 x 1200 area round (ox, oy), drawn from r: [road].
 * Every draw is sequenced (tests/city/road_inputs.hpp draws in the same order).
 */
export function scriptedRoads(r, k, ox = 0, oy = 0) {
  const n = 4 + Math.floor(r() * 28);
  const angled = r() < 0.35;
  const roads = [];
  const undef = (p = 0.08) => r() < p;
  for (let q = 0; q < n; q += 1) {
    const cls = SCRIPT_CLASSES[Math.floor(r() * SCRIPT_CLASSES.length)];
    const hc0 = r() < 0.15 ? 0 : 8 + Math.floor(r() * 52);
    const walk = Math.floor(r() * 44) - 4;
    const road = { id: `S${k}/r${q}`, cell: `S${k}`, cls, pts: [] };
    road.hc = hc0;
    road.hr = hc0 + Math.max(0, walk);
    const corner = Math.floor(r() * 70);
    if (!undef(0.02)) road.corner = corner;
    const median = r() < 0.6 ? 0 : 6 + Math.floor(r() * 20);
    if (!undef()) road.median = median;
    const parking = r() < 0.6 ? 0 : 14 + Math.floor(r() * 8);
    if (!undef()) road.parking = parking;
    const lanes = 1 + Math.floor(r() * 6);
    if (!undef()) road.lanes = lanes;
    const lane = 20 + Math.floor(r() * 10);
    if (!undef()) road.lane = lane;
    if (!undef()) road.sidewalk = walk;
    const shoulder = r() < 0.6 ? 0 : 4 + Math.floor(r() * 12);
    if (!undef()) road.shoulder = shoulder;
    if (r() < 0.2) road.paving = "cobble";
    const strip = STRIPS[Math.floor(r() * STRIPS.length)];
    if (strip !== null) road.strip = strip;
    if (angled && r() < 0.85) road.home = [Math.floor(r() * 5) - 2, Math.floor(r() * 5) - 2];
    // the centre line: from a free point, or from a point in an earlier road's carriageway
    let x;
    let y;
    if (q > 0 && r() < 0.35) {
      const o = roads[Math.floor(r() * q)];
      const p = o.pts[0];
      const e = o.pts[o.pts.length - 1];
      const t = r();
      const lat = (r() - 0.5) * 2 * (o.hc + 4);
      const L = Math.hypot(e.x - p.x, e.y - p.y) || 1;
      x = p.x + (e.x - p.x) * t - ((e.y - p.y) / L) * lat;
      y = p.y + (e.y - p.y) * t + ((e.x - p.x) / L) * lat;
    } else {
      x = ox + r() * 1200;
      y = oy + r() * 1200;
    }
    if (r() < 0.5) {
      x = Math.round(x);
      y = Math.round(y);
    }
    road.pts.push({ x, y });
    const m = 1 + Math.floor(r() * 4);
    let a = r() * 2 * Math.PI;
    for (let s = 0; s < m; s += 1) {
      const kind = r();
      if (kind < 0.12) {
        // a degenerate piece: the same point again
        road.pts.push({ x, y });
        continue;
      }
      if (kind < 0.5) a = Math.floor(r() * 4) * (Math.PI / 2);
      else a += (r() - 0.5) * 1.2;
      const len = 40 + r() * 500;
      x += Math.cos(a) * len;
      y += Math.sin(a) * len;
      if (kind < 0.5) {
        x = Math.round(x);
        y = Math.round(y);
      }
      road.pts.push({ x, y });
    }
    roads.push(road);
  }
  return roads;
}

/** A segment's reference in records: its road's id and its index. */
export const segRef = (s) => (s ? `${s.road.id}#${s.idx}` : "-");

/** Every field of a segment (roadView.js buildSegments). */
export function segFields(s) {
  return [segRef(s), s.ax, s.ay, s.bx, s.by, s.az, s.bz, s.len, s.dx, s.dy, s.s0, s.hc, s.hr, s.sidewalk, s.parking, s.median, s.lanes, s.lane, s.shoulder, s.cls, s.rank, s.first, s.last, [s.bbox.x0, s.bbox.y0, s.bbox.x1, s.bbox.y1]];
}

/** Every field of a junction annotation (roadView.js junction). */
export function junctionFields(j) {
  return [j.s, j.hc, j.hr, j.cls, j.rank, j.cEnds, j.sEnds, j.cw, j.signal, segRef(j.other), j.tOther];
}

/**
 * A world (createWorld) whose cell networks and waters are recorded as a stage reads them: its
 * terrain without the port lakes' grading (the port's World has none until lakes are ported), and
 * every cache unbounded (one object per road, as the port's road views share them; nothing a
 * reference cache evicts is made again in another order). A stage makes the base heights a region
 * reads first (warmAround; docs/CITY.md §6). `wet(x, y, m, v)` may alter the waters' answers (a
 * branch no water reaches; the port replays what was recorded).
 */
export function recordingWorld(overrides, { wet: alter = null } = {}) {
  const w = createWorld(overrides);
  w.terrain.portGrade = null;
  w.cellNets = new LRU(Infinity);
  w.roadViews = new LRU(Infinity);
  if (w.highways) w.highways.edges = new LRU(Infinity);
  const cells = new Map();
  const cellNet = w.cellNet.bind(w);
  w.cellNet = (i, j) => {
    const k = `${i},${j}`;
    let e = cells.get(k);
    if (!e) cells.set(k, (e = { i, j, net: cellNet(i, j) }));
    return e.net;
  };
  const wet = new Map();
  const isWet = w.isWet;
  w.isWet = (x, y, m = 2) => {
    let v = isWet(x, y, m);
    if (alter) v = alter(x, y, m, v);
    wet.set(`${x},${y},${m}`, [x, y, m, v ? 1 : 0]);
    return v;
  };
  return { w, cells, wet };
}

/**
 * Makes the base height of every town and village within 25 km of (x, y) (voxels) first - all an
 * island's, whose trunk roads cross it - before anything there samples the terrain (warmBasesIn):
 * the reference's samples are then the same in any order, and the port's (docs/CITY.md §6).
 */
export function warmAround(w, x, y) {
  const R = 200000;
  warmBasesIn(w, { x0: x - R, y0: y - R, x1: x + R, y1: y + R });
  const isl = w.fields.island;
  if (isl) {
    const b = isl.bounds();
    warmBasesIn(w, { x0: b.x0 * 8, y0: b.y0 * 8, x1: b.x1 * 8, y1: b.y1 * 8 });
  }
}

/** warmAround the centre of cell (i, j). */
export function warmView(w, i, j) {
  const rc = w.arterials.cellRect(i, j);
  warmAround(w, (rc.x0 + rc.x1) / 2, (rc.y0 + rc.y1) / 2);
}

const SPEC_KEYS = ["hc", "hr", "corner", "median", "parking", "lanes", "lane", "sidewalk", "shoulder"];
const ROAD_KEYS = new Set(["id", "cell", "cls", "pts", ...SPEC_KEYS, "home", "arterialEdge", "paving", "diagonal", "sub", "strip", "prof", "prof0"]);

/** A recorded road: [n (its id's counter), cls, [x, y, ...], extras?] (its cross-section is its class's). */
function roadJson(road, net, specs) {
  const prefix = `${net.id}/r`;
  if (!road.id.startsWith(prefix) || road.cell !== net.id) throw new Error(`roads: ${road.id} is no road of ${net.id}`);
  for (const k of Object.keys(road)) if (!ROAD_KEYS.has(k)) throw new Error(`roads: ${road.id} has a field the record lacks: ${k}`);
  const spec = specs[road.cls];
  for (const k of SPEC_KEYS) if (road[k] !== spec[k]) throw new Error(`roads: ${road.id}: ${k} is not its class's`);
  const pts = [];
  for (const p of road.pts) {
    if (Object.keys(p).some((k) => k !== "x" && k !== "y")) throw new Error(`roads: ${road.id} has a point with more than x, y`);
    pts.push(p.x, p.y);
  }
  const extra = {};
  if (road.arterialEdge !== undefined) extra.e = road.arterialEdge;
  if (road.paving !== undefined && road.paving !== null) extra.p = road.paving;
  if (road.diagonal !== undefined) extra.d = [road.diagonal.f, road.diagonal.k];
  if (road.sub !== undefined) extra.u = road.sub;
  if (road.strip !== undefined) extra.s = road.strip;
  if (road.home !== undefined) extra.h = road.home;
  const out = [Number(road.id.slice(prefix.length)), road.cls, pts];
  if (Object.keys(extra).length) out.push(extra);
  return out;
}

/** The recorded inputs of a recording world: { specs, cells: [[i, j, cell id, [road]]], wet: [[x, y, m, 0 | 1]] }. */
export function recorded({ w, cells, wet }) {
  const specs = roadSpecs(w.config);
  const sp = {};
  for (const [cls, s] of Object.entries(specs)) sp[cls] = SPEC_KEYS.map((k) => s[k]);
  return {
    specs: sp,
    cells: [...cells.values()].map(({ i, j, net }) => [i, j, net.id, net.roads.map((r) => roadJson(r, net, specs))]),
    wet: [...wet.values()],
  };
}

/** Writes a stage's recorded inputs (data/<stage>.json): { world key: recorded(...) }. */
export function writeInputs(stage, worlds) {
  mkdirSync(DATA, { recursive: true });
  writeFileSync(new URL(`${stage}.json`, DATA), JSON.stringify(worlds) + "\n");
}

export { f };
