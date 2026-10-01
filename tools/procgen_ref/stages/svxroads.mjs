// Stage "svxroads": the city's streets, highways, walkways and kerbside parking as structvox's road
// network (svx/roads.js roadNetwork, with svx/highwayLanes.js) on worlds made as the export makes
// them (svx/source.js createSvxSource: makeConfig, the angled world's parts apart), the terrain
// made pure (lib/worlds.mjs pureTerrain) and the World's caches unbounded (one object per road:
// docs/CITY.md §6, road identity). One network per world, asked in a fixed order: in boxes round
// the spawn, a town and a village, highway ramps and a highway junction, every lane (its key), the
// way on from it (next), its signal and green over a cycle, the lanes it leads to as lane(id) has
// them; every walk, the walks at its ends (walkNext), walkOpen over a cycle, the walks they are
// (walk(id)); every parking place. And the answers for ids not (yet) handed out (lane(id) knows
// only the lanes handed out: the roads lanesIn and next have touched), and a region round the
// spawn whole (region). tests/city/test_svxroads.cpp is the twin.
import { REF, line } from "../lib/rec.mjs";
import { pureTerrain } from "../lib/worlds.mjs";

const { createWorld } = await import(REF + "world/createWorld.js");
const { makeConfig } = await import(REF + "config/defaults.js");
const { presetConfig } = await import(REF + "config/presets.js");
const { roadNetwork } = await import(REF + "svx/roads.js");
const { idOf } = await import(REF + "svx/ids.js");
const { LRU } = await import(REF + "core/lru.js");

const H = 0.125;
const m = (q) => H * (q - 0.5);

/**
 * The worlds: the infinite city and its angled twin (highways and ramps near the spawn), towns in
 * the countryside (rural roads, villages, a highway), the old harbour town and its angled twin
 * (graded streets climbing a hillside, cobbled lanes and alleys), an island harbour town.
 * [key, preset, size].
 */
export const ROAD_WORLDS = [
  ["infiniteCity", "infiniteCity", null],
  ["angledInfiniteCity", "angledInfiniteCity", null],
  ["cities", "cities", null],
  ["oldHarbourTown", "oldHarbourTown", null],
  ["angledOldHarbourTown", "angledOldHarbourTown", null],
  ["island", "island", null],
];

/** A world as the export makes it (createSvxSource), its terrain pure and its caches unbounded. */
export function exportWorld(id, size) {
  const cfg = makeConfig(presetConfig(id, { size }));
  cfg.world.angles = { ...cfg.world.angles, partsMode: "separate" };
  const w = pureTerrain(createWorld(cfg));
  w.cellNets = new LRU(Infinity);
  w.roadViews = new LRU(Infinity);
  if (w.highways) w.highways.edges = new LRU(Infinity);
  return w;
}

/**
 * The boxes of a world ([tag, lo, hi], metres): round the spawn (500 m), round the first town and
 * the first village (settlementsIn, villagesIn) whose centre lies over 400 m from the spawn (240
 * m), round the landings of the first two ramps within 1.5 km of the spawn (160 m), round the
 * highway junction or terminus nearest the spawn (200 m).
 */
export function boxesOf(w) {
  const out = [["spawn", [-250, -250], [250, 250]]];
  const F = w.fields;
  const far = (s) => Math.hypot(m(s.x), m(s.y)) > 400;
  const town = F.settlementsIn({ x0: -160000, y0: -160000, x1: 160000, y1: 160000 }).find(far);
  if (town) out.push(["town", [m(town.x) - 120, m(town.y) - 120], [m(town.x) + 120, m(town.y) + 120]]);
  const village = F.villagesIn({ x0: -80000, y0: -80000, x1: 80000, y1: 80000 }).find(far);
  if (village) out.push(["village", [m(village.x) - 120, m(village.y) - 120], [m(village.x) + 120, m(village.y) + 120]]);
  if (w.highways) {
    let n = 0;
    for (const e of w.highways.edgesNear({ x0: -12000, y0: -12000, x1: 12000, y1: 12000 }))
      for (const r of w.highways.ramps(e)) {
        if (n >= 2 || Math.abs(r.x) >= 12000 || Math.abs(r.y) >= 12000) continue;
        out.push(["ramp", [m(r.x) - 80, m(r.y) - 80], [m(r.x) + 80, m(r.y) + 80]]);
        n += 1;
      }
    // (the lattice node nearest the spawn where other than two highways meet: a junction, a terminus)
    let best = null;
    for (let b = -3; b <= 3; b += 1)
      for (let a = -3; a <= 3; a += 1) {
        const deg = w.highways.edgesAt(a, b).length;
        if (deg === 0 || deg === 2) continue;
        const p = w.highways.node(a, b);
        const d = Math.hypot(p.x, p.y);
        if (!best || d < best.d) best = { d, x: p.x, y: p.y };
      }
    if (best) out.push(["node", [m(best.x) - 100, m(best.y) - 100], [m(best.x) + 100, m(best.y) + 100]]);
  }
  return out;
}

const sigOf = (s) => (s ? [s.cycle, s.offset, s.from, s.to] : "-");

/** Whether `open(t)` over a signal's cycle (every 0.5 s, from 0.25 s), then at -7.3 s and 123456.7 s. */
function bits(open, cycle) {
  let s = "";
  for (let k = 0; k < 2 * cycle; k += 1) s += open(k * 0.5 + 0.25) ? "1" : "0";
  return `${s}/${open(-7.3) ? 1 : 0}${open(123456.7) ? 1 : 0}`;
}

/** A lane: its id, ends, width, speed and key (a street's "s", a highway deck's "h", a ramp's "r"). */
export function laneFields(l) {
  const k = l.key;
  const head = [l.id, l.a, l.b, l.width, l.speed];
  if (k.road !== undefined) return [...head, "s", k.road, k.link, k.piece, k.dir, k.k, k.last];
  if (k.ramp !== undefined) return [...head, "r", k.hw, k.ramp, k.off, k.piece, k.last, k.sDeck, k.side, k.arterial];
  return [...head, "h", k.hw, k.dir, k.link, k.piece, k.k, k.last, k.sa, k.sb];
}

/** The answers for an id: lane, next, signal, green, walk, walkNext both ends, walkOpen. */
function probe(net, tag, id) {
  return line(tag, id, net.lane(id) ? 1 : 0, net.next(id), sigOf(net.signal(id)), net.green(id, 3), net.walk(id) ? 1 : 0,
    net.walkNext(id, 0), net.walkNext(id, 1), net.walkOpen(id, 3));
}

function* boxRecords(net, tag, lo, hi) {
  yield line("box", tag, lo, hi);
  const lanes = net.lanesIn(lo, hi);
  yield line("lanes", lanes.length);
  for (const l of lanes) yield line("lane", ...laneFields(l));
  for (const l of lanes) {
    const next = net.next(l.id);
    const sig = net.signal(l.id);
    yield line("next", l.id, next, sigOf(sig), sig ? bits((t) => net.green(l.id, t), sig.cycle) : "-");
    for (const [id] of next) {
      const n = net.lane(id);
      yield line("to", id, n ? n.a : "-", n ? n.b : "-");
    }
  }
  const walks = net.walksIn(lo, hi);
  yield line("walks", walks.length);
  for (const q of walks) yield line("walk", q.id, q.a, q.b, q.inset, q.width, q.crossing, q.kind, sigOf(q.signal));
  for (const q of walks) {
    const n0 = net.walkNext(q.id, 0);
    const n1 = net.walkNext(q.id, 1);
    yield line("wnext", q.id, n0, n1, q.signal ? bits((t) => net.walkOpen(q.id, t), q.signal.cycle) : "-");
    for (const [id] of [...n0, ...n1]) {
      const o = net.walk(id);
      yield line("wto", id, o ? o.a : "-", o ? o.b : "-", o ? o.kind : "-");
    }
  }
  const park = net.parkingIn(lo, hi);
  yield line("parks", park.length);
  for (const p of park) yield line("park", p.id, p.pos, p.heading);
}

export default function* svxroads() {
  for (const [key, id, size] of ROAD_WORLDS) {
    const w = exportWorld(id, size);
    const boxes = boxesOf(w);
    const net = roadNetwork(w);
    // (ids of a road at the spawn, before anything is handed out and after)
    const c = w.cellAt(0, 0);
    const road = w.roadView(c.i, c.j).segs[0].road.id;
    const ids = [1, idOf(road, "lane", 0, 0, 0, 0), idOf(road, "walk", 0, 1), idOf(road, "lane", 0, 0, 1, 0)];
    yield line("world", key, boxes.length, road);
    for (const q of ids) yield probe(net, "before", q);
    for (const [tag, lo, hi] of boxes) yield* boxRecords(net, tag, lo, hi);
    for (const q of ids) yield probe(net, "after", q);
    // (a region round the spawn whole, as the worker gives a host)
    const reg = net.region([-60, -60], [60, 60]);
    yield line("region", reg.lanes.length, reg.walks.length, reg.parking.length);
    for (const l of reg.lanes) yield line("rl", l.id, l.a, l.b, l.width, l.speed, l.next, sigOf(l.signal));
    for (const q of reg.walks) yield line("rw", q.id, q.a, q.b, q.inset, q.width, q.crossing, q.kind, sigOf(q.signal), q.nextA, q.nextB);
    for (const p of reg.parking) yield line("rp", p.id, p.pos, p.heading);
  }
}
