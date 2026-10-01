import { hash32 } from "../core/hash.js";
import { VOXEL_SIZE } from "../core/units.js";
import { roadLevelAt, segmentLevel } from "../network/roadLevel.js";
import { idOf } from "./ids.js";
import { highwayLanes } from "./highwayLanes.js";

/**
 * The city's streets as structvox's road network (opus_destruct_2,
 * game/include/svx/game/roads.hpp RoadNetwork: the lanes its traffic drives,
 * the walkways its people walk, the kerbside places cars park), as plain
 * data a host copies into its own types (docs/MERGE_SVX.md), after the
 * conventions of structvox's own drive city (procgen/src/drive_city.cpp):
 *
 *   - world metres, the export's frame (svx/source.js: a city point q, in
 *     voxels, is at h (q - 1/2)); z the surface: a lane on its road's
 *     carriageway, a walk on its sidewalk (a kerb higher);
 *   - traffic keeps to the right as structvox's frame has it (right-handed,
 *     z up: heading (dx, dy), right is (dy, -dx)); a turn is right when it
 *     turns clockwise (seen from above);
 *   - a lane is a straight run one way: a road is cut at its junctions into
 *     links, a link at its bends into pieces, and a lane stops at the kerb
 *     line of the road it meets (the turn through the junction is the
 *     host's, from one lane's b to the next one's a); lane k counts from the
 *     centre line (or the median) outwards;
 *   - a junction is every road meeting at one place, as each of them sees
 *     it (roads cut at a cell's border meet their continuations through the
 *     roads they cross), and junctions too close for a lane between them (a
 *     jog, a side road just past a crossing) are one;
 *   - `next`: straight on keeps the lane (the nearest there is), a right
 *     turn leaves from the kerb lane to the kerb lane, a left one from the
 *     inner lane to the inner lane (any lane, where there is no straight
 *     on); never a U-turn, but at a dead end;
 *   - `green`: a signalled junction (two roads of collector rank or more
 *     among its roads) gives the roads along its first road (by id; within
 *     45° of it) u < 14 s of a 34 s cycle and the roads across it
 *     17 <= u < 31, u offset per junction: the same for every road there; a
 *     crossing is open the first 4 s of the other roads' green; elsewhere
 *     always green;
 *   - walks: along each side of a link, on the middle of its sidewalk, from
 *     corner to corner (a corner is where two walking lines cross, worked
 *     out alike from either road, so walks meet exactly), between corners a
 *     junction puts on one side, over the road at its crosswalks; down the
 *     middle of alleys and single-track lanes (no sidewalk, no traffic: the
 *     street's sidewalk stops at their mouths), and over a street they cross;
 *     `inset` keeps people towards the buildings, clear of the lamps and
 *     trees at the kerb;
 *   - parking: in a road's parking strips, one place every 6 m from 9 m past
 *     the kerb of the road a link starts at, heading with the traffic on its
 *     side (a unit vector: the host takes its yaw).
 *
 * Streets with a lane each way or more carry traffic (alleys, single-track
 * lanes and pedestrian streets only people: two cars meeting on one track
 * would stop for each other for good, structvox's traffic having no passing
 * places); the highways are the host's to add.
 * Ids are stable 52-bit integers from structural keys (the road's id, the
 * link, the piece, the direction, the lane): the same everywhere, whatever
 * the query, in any worker. Queries return records in id order.
 */

const H = VOXEL_SIZE;
/** A signal's phase (s): green, then all red. */
const PHASE = 17;
const GREEN = 14;
/** A crossing opens this long (s) at the start of the phase after its road's. */
const WALK = 4;
/** Roads of one junction within this of each other's heading (cos 22.5°) move in one phase. */
const ONE_PHASE = 0.924;
/** The first parking place starts this far past the kerb a link starts at, one every PITCH (voxels). */
const PARK_START = 72;
const PARK_PITCH = 48;
const PARK_LEN = 40;
/** Speed limits by class (m/s). */
const SPEED = { arterial: 16.7, collector: 13.9, local: 11.1, village: 11.1, rural: 19.4 };
/** Walks whose ends lie this close meet (voxels): a junction on a bend of a road may give its two roads corners a hair apart. */
const SNAP = 3;
/** Straight on: within this much of the heading (cos 30°). */
const STRAIGHT = Math.cos(Math.PI / 6);
/** The shortest link that has lanes (voxels, kerb to kerb): closer junctions are one. */
const MIN_LINK = 8;
/** How far the roads of one junction may meet from its first road's node (voxels). */
const JUNCTION_REACH = 96;

const infos = new WeakMap();

/**
 * The signal of road `info` at its node `node`, or null where the junction
 * has none: one phase per heading of the junction's roads (roads within
 * 22.5° of each other move together), 17 s each, green the first 14 s (3 s
 * all red after), in the order of their first roads by id, u offset per
 * junction; a crossing over the road is open the first 4 s of the next
 * phase. (Two headings, a crossing: the first road's u < 14 s of a 34 s
 * cycle, the other's 17 <= u < 31.)
 */
function signalOf(world, info, node, crossing = false) {
  const j = junctionOf(world, info, node);
  if (!j.signal || j.phases < 2) return null;
  const cycle = PHASE * j.phases;
  const i = j.phase.get(info.road.id);
  if (crossing) {
    const from = PHASE * ((i + 1) % j.phases);
    return { cycle, offset: j.offset, from, to: from + WALK };
  }
  return { cycle, offset: j.offset, from: PHASE * i, to: PHASE * i + GREEN };
}

/** Road `other`'s node where it meets road `id` nearest city point (x, y) (within 8 m), or null. */
function nodeOf(other, id, x, y) {
  let best = null;
  let bestD = Infinity;
  for (const n of other.nodes) {
    if (!n.meets.some((q) => q.road.id === id)) continue;
    const p = along(other, n.at);
    const d = Math.hypot(p.x - x, p.y - y);
    if (d < bestD) [best, bestD] = [n, d];
  }
  return bestD < 64 ? best : null;
}

/**
 * The junction node `node` of road `info` is at, as every road there sees
 * it: the roads meeting there, and the roads they meet there, closed over
 * (a road cut at a cell's border meets its continuation through the roads
 * they both cross), each with its node. Its first road (by id) gives it its
 * place, its axis (the phases) and its signal's offset, so every road there
 * works out the same signal. Cached on the node alone (a pure function of
 * the node, whichever is asked first).
 */
function junctionOf(world, info, node) {
  if (node.junction) return node.junction;
  const p0 = along(info, node.at);
  const members = new Map([[info.road.id, { info, node }]]);
  const queue = [[info, node]];
  while (queue.length) {
    const [I, N] = queue.shift();
    const p = along(I, N.at);
    for (const q of N.meets) {
      if (members.has(q.road.id)) continue;
      const other = roadInfo(world, q.road);
      const on = nodeOf(other, I.road.id, p.x, p.y);
      if (!on) continue;
      const po = along(other, on.at);
      if (Math.hypot(po.x - p0.x, po.y - p0.y) > JUNCTION_REACH) continue;
      members.set(q.road.id, { info: other, node: on });
      queue.push([other, on]);
    }
  }
  const ids = [...members.keys()].sort();
  const first = members.get(ids[0]);
  const pf = along(first.info, first.node.at);
  // (the phases: the roads by heading, each joining the first phase whose first road runs within 22.5° of it)
  const heads = [];
  const phase = new Map();
  for (const id of ids) {
    const g = members.get(id);
    const d = along(g.info, g.node.at);
    let i = heads.findIndex((h) => Math.abs(h.dx * d.dx + h.dy * d.dy) >= ONE_PHASE);
    if (i < 0) i = heads.push(d) - 1;
    phase.set(id, i);
  }
  node.junction = {
    ids,
    members: ids.map((id) => members.get(id)),
    phase,
    phases: heads.length,
    signal: [...members.values()].some((m) => m.node.meets.some((q) => q.signal)),
    offset: (hash32(world.seed, Math.round(pf.x), Math.round(pf.y), 0x516) / 4294967296) * PHASE * heads.length,
  };
  return node.junction;
}

/** Is a signal { cycle, offset, from, to } open at time t (null: always)? */
function open(sig, t) {
  if (!sig) return true;
  const u = (((t + sig.offset) % sig.cycle) + sig.cycle) % sig.cycle;
  return u >= sig.from && u < sig.to;
}


/** City point (voxels) -> export metres. */
const m = (q) => H * (q - 0.5);

function cellOfRoad(road) {
  const [, i, j] = /^C(-?\d+)_(-?\d+)/.exec(road.cell) ?? [0, 0, 0];
  return [Number(i), Number(j)];
}

/**
 * A road's structure (cached per world): its segments in its own cell's
 * view, its nodes along it (junctions, merged where roads meet at one
 * point, and its dead ends) and its links between them.
 */
function roadInfo(world, road) {
  let byRoad = infos.get(world);
  if (!byRoad) infos.set(world, (byRoad = new Map()));
  let info = byRoad.get(road.id);
  if (info) return info;
  const view = world.roadView(...cellOfRoad(road));
  const segs = view.segs.filter((s) => s.road.id === road.id).sort((p, q) => p.idx - q.idx);
  if (!segs.length) {
    // (no segment of its own: a degenerate road, nothing to drive or walk)
    info = { road, view, segs, L: 0, nodes: [], links: [], lanes: 0, laneList: [], walkList: [] };
    byRoad.set(road.id, info);
    return info;
  }
  const L = segs[segs.length - 1].s0 + segs[segs.length - 1].len;
  const nodes = [];
  const meet = (at, q) => {
    let n = nodes.find((r) => Math.abs(r.at - at) < 4);
    if (!n) nodes.push((n = { at, meets: [] }));
    n.meets.push(q);
  };
  for (const s of segs)
    for (const j of s.jn)
      // (own: this road's segment the junction was found on, seg: the other road's; both roads see the same pair)
      meet(s.s0 + j.s, { road: j.other.road, own: s, seg: j.other, t: j.tOther, hc: j.hc, hr: j.hr, ends: j.cEnds, signal: j.signal, cw: j.cw });
  for (const { at, q } of looseEnds(world, road, view, segs)) meet(at, q);
  // (where a highway's ramp lands on it: a node of its own, so lanes end and leave there)
  for (const { at, ramp } of rampLandings(world, road, segs)) {
    let n = nodes.find((r) => Math.abs(r.at - at) < 4);
    if (!n) nodes.push((n = { at, meets: [] }));
    (n.ramps ??= []).push(ramp);
  }
  nodes.sort((p, q) => p.at - q.at);
  // (a road end with no junction there: a dead end)
  if (!nodes.length || nodes[0].at > 4) nodes.unshift({ at: 0, meets: [] });
  if (nodes[nodes.length - 1].at < L - 4) nodes.push({ at: L, meets: [] });
  // (the kerb line of the widest road met, along this one)
  for (const n of nodes) n.trim = n.meets.reduce((t, q) => Math.max(t, q.hc), 0);
  // (junctions too close for a lane between their kerbs are one: a jog, a side road just past a crossing, a dead end at a junction)
  for (let k = 0; k + 1 < nodes.length; ) {
    const a = nodes[k];
    const b = nodes[k + 1];
    if (b.at - b.trim - (a.at + a.trim) >= MIN_LINK) {
      k += 1;
      continue;
    }
    const lo = Math.min(a.at - a.trim, b.at - b.trim);
    const hi = Math.max(a.at + a.trim, b.at + b.trim);
    nodes.splice(k, 2, { at: (lo + hi) / 2, trim: (hi - lo) / 2, meets: [...a.meets, ...b.meets], ...(a.ramps || b.ramps ? { ramps: [...(a.ramps ?? []), ...(b.ramps ?? [])] } : {}) });
  }
  const links = [];
  for (let k = 0; k + 1 < nodes.length; k += 1) links.push({ k, from: nodes[k], to: nodes[k + 1] });
  info = { road, view, segs, L, nodes, links, lanes: road.lanes >= 2 ? Math.floor(road.lanes / 2) : 0 };
  byRoad.set(road.id, info);
  return info;
}

/**
 * The highway ramps landing on an arterial (network/highways.js ramps):
 * [{ at (arc along the road), ramp: { edge, index, off } }].
 */
function rampLandings(world, road, segs) {
  const hw = world.highways;
  if (!hw || road.cls !== "arterial") return [];
  let box = null;
  for (const s of segs) box = box ? { x0: Math.min(box.x0, s.bbox.x0), y0: Math.min(box.y0, s.bbox.y0), x1: Math.max(box.x1, s.bbox.x1), y1: Math.max(box.y1, s.bbox.y1) } : { ...s.bbox };
  const out = [];
  for (const e of hw.edgesNear(box))
    hw.ramps(e).forEach((r, index) => {
      if (r.arterial !== road.id) return;
      // (its arc: the nearest point of the road's centre line)
      let best = null;
      for (const s of segs) {
        const t = Math.max(0, Math.min(s.len, (r.x - s.ax) * s.dx + (r.y - s.ay) * s.dy));
        const d = Math.hypot(r.x - (s.ax + s.dx * t), r.y - (s.ay + s.dy * t));
        if (!best || d < best.d) best = { d, at: s.s0 + t };
      }
      if (best) out.push({ at: best.at, ramp: { edge: e.id, index, off: r.side < 0 ? r.sDeck < r.sGround : r.sDeck > r.sGround } });
    });
  return out;
}

/** A street by its id (C<i>_<j>/…), from its cell's road view, or null. */
function roadById(world, id) {
  const [, i, j] = /^C(-?\d+)_(-?\d+)/.exec(id) ?? [];
  if (i === undefined) return null;
  return world.roadView(Number(i), Number(j)).segs.find((s) => s.road.id === id)?.road ?? null;
}

/** Most two roads' levels may differ (voxels) where one ends in the other's carriageway for them to meet. */
const LOOSE_STEP = 2;

/**
 * Where a road's end lies in another road's carriageway at its level, but
 * the road network has no junction of the two (network/roadView.js makes
 * one only where that end meets no other road: an end at a corner keeps
 * the corner's level in the voxels): the junctions it makes for traffic
 * and walks, on road `road` (its segments
 * `segs` of its cell's view), as [{ at, q }] (q a meet as roadInfo keeps
 * them). Both roads work out the same pair: this road's ends in the others'
 * carriageways, and the others' ends in this one's.
 */
function looseEnds(world, road, view, segs) {
  const out = [];
  const ends = (s) => [...(s.first ? [0] : []), ...(s.last ? [s.len] : [])];
  // (the end of segment e at t lies in segment c's carriageway, at its level, crossing it, with no junction of the two)
  const lies = (e, t, c) => {
    if (e.road === c.road || Math.abs(e.dx * c.dy - e.dy * c.dx) < 0.5) return null;
    if (e.jn.some((j) => j.other.road === c.road) || c.jn.some((j) => j.other.road === e.road)) return null;
    const px = e.ax + e.dx * t;
    const py = e.ay + e.dy * t;
    const tc = (px - c.ax) * c.dx + (py - c.ay) * c.dy;
    if (tc < -3 || tc > c.len + 3 || Math.abs((px - c.ax) * c.dy - (py - c.ay) * c.dx) > c.hc) return null;
    const at = Math.max(0, Math.min(c.len, tc));
    if (Math.abs(segmentLevel(world, e, t) - segmentLevel(world, c, at)) > LOOSE_STEP) return null;
    return at;
  };
  const box = (x, y) => ({ x0: x - 1, y0: y - 1, x1: x + 1, y1: y + 1 });
  for (const e of segs)
    for (const t of ends(e))
      for (const c of view.near(box(e.ax + e.dx * t, e.ay + e.dy * t))) {
        const tc = lies(e, t, c);
        if (tc === null) continue;
        const ends = (tc < 3 && c.first) || (tc > c.len - 3 && c.last);
        out.push({ at: e.s0 + t, q: { road: c.road, own: e, seg: c, t: tc, hc: c.hc, hr: c.hr, ends, signal: e.rank >= 4 && c.rank >= 4, cw: false } });
      }
  for (const c of segs)
    for (const e of view.near(c.bbox)) {
      if (e.road === road) continue;
      for (const t of ends(e)) {
        const tc = lies(e, t, c);
        if (tc !== null) out.push({ at: c.s0 + tc, q: { road: e.road, own: c, seg: e, t, hc: e.hc, hr: e.hr, ends: true, signal: e.rank >= 4 && c.rank >= 4, cw: false } });
      }
    }
  return out;
}

/** Point and unit direction of a road at arc position s: { x, y, dx, dy, seg }. */
function along(info, s) {
  let seg = info.segs[0];
  for (const q of info.segs) if (q.s0 <= s) seg = q;
  const t = Math.max(0, Math.min(seg.len, s - seg.s0));
  return { x: seg.ax + seg.dx * t, y: seg.ay + seg.dy * t, dx: seg.dx, dy: seg.dy, seg };
}

/** Top voxel of the carriageway at a city point (the nearest road's: for a point off the lanes, a corner). */
function roadTop(world, info, x, y) {
  const r = roadLevelAt(world, info.view, x, y, 8);
  return r ? Math.round(r.z) : 0;
}

/** The level (voxels, continuous) of a road's own profile at arc position s. */
function levelAlong(world, info, s) {
  const q = along(info, s);
  return segmentLevel(world, q.seg, Math.max(0, Math.min(q.seg.len, s - q.seg.s0)));
}

/** Most a lane's straight line may stray from its road's profile (voxels) before its piece is cut in two. */
const PROFILE_SLACK = 1;

/**
 * The pieces of a link one way (dir 0 along the road, 1 against it): the
 * arc intervals between its trimmed ends and the road's bends.
 */
function pieces(world, info, link) {
  if (link.pieces) return link.pieces;
  const s0 = link.from.at + link.from.trim;
  const s1 = link.to.at - link.to.trim;
  const out = [];
  if (s1 - s0 >= MIN_LINK) {
    const cuts = [s0];
    for (const q of info.segs) if (q.s0 > s0 + 4 && q.s0 < s1 - 4) cuts.push(q.s0);
    cuts.push(s1);
    // (and where the road's profile bends away from the straight line: a landing, a change of grade)
    const refine = (a, b, depth) => {
      const za = levelAlong(world, info, a);
      const zb = levelAlong(world, info, b);
      let worst = 0;
      for (let k = 1; k < 8; k += 1) {
        const t = k / 8;
        worst = Math.max(worst, Math.abs(levelAlong(world, info, a + (b - a) * t) - (za + (zb - za) * t)));
      }
      if (worst > PROFILE_SLACK && depth < 5 && b - a > 32) {
        refine(a, (a + b) / 2, depth + 1);
        refine((a + b) / 2, b, depth + 1);
      } else out.push([a, b]);
    };
    for (let k = 0; k + 1 < cuts.length; k += 1) refine(cuts[k], cuts[k + 1], 0);
  }
  link.pieces = out;
  return out;
}

/** The lane of `info`'s link `li`, piece `pi` (in its direction's order), direction dir, lane k: a record. */
function laneRecord(world, info, li, pi, dir, k) {
  const link = info.links[li];
  const ps = pieces(world, info, link);
  const order = dir === 0 ? ps : ps.slice().reverse();
  const [p0, p1] = order[pi];
  const [sa, sb] = dir === 0 ? [p0, p1] : [p1, p0];
  const road = info.road;
  const off = road.median / 2 + (k + 0.5) * road.lane;
  const end = (s) => {
    const q = along(info, s);
    // (heading with the traffic, right of it in structvox's frame; on its own road's surface)
    const hx = dir === 0 ? q.dx : -q.dx;
    const hy = dir === 0 ? q.dy : -q.dy;
    const x = q.x + hy * off;
    const y = q.y - hx * off;
    return [m(x), m(y), H * (Math.round(levelAlong(world, info, s)) + 0.5)];
  };
  return {
    id: idOf(road.id, "lane", li, pi, dir, k),
    a: end(sa),
    b: end(sb),
    width: road.lane * H,
    speed: SPEED[road.cls] ?? 11.1,
    key: { road: road.id, link: li, piece: pi, dir, k, last: pi === order.length - 1 },
  };
}

/** Every lane of a road: records. */
function roadLanes(world, info) {
  if (info.laneList) return info.laneList;
  const out = [];
  if (info.lanes)
    for (let li = 0; li < info.links.length; li += 1) {
      const n = pieces(world, info, info.links[li]).length;
      for (let pi = 0; pi < n; pi += 1) for (const dir of [0, 1]) for (let k = 0; k < info.lanes; k += 1) out.push(laneRecord(world, info, li, pi, dir, k));
    }
  info.laneList = out;
  info.laneById = new Map(out.map((l) => [l.id, l]));
  return out;
}

/** The roads whose right-of-way reaches into a box of city voxels (every cell's view round it). */
function roadsIn(world, box) {
  const seen = new Map();
  const c0 = world.cellAt(box.x0, box.y0);
  const c1 = world.cellAt(box.x1, box.y1);
  for (let j = c0.j; j <= c1.j; j += 1)
    for (let i = c0.i; i <= c1.i; i += 1)
      for (const s of world.roadView(i, j).near(box)) if (!seen.has(s.road.id)) seen.set(s.road.id, s.road);
  return [...seen.values()].sort((p, q) => (p.id < q.id ? -1 : p.id > q.id ? 1 : 0));
}

/** The box (city voxels) of a query in metres ({ lo: [x, y], hi: [x, y] }). */
const voxBox = (lo, hi) => ({ x0: lo[0] / H + 0.5, y0: lo[1] / H + 0.5, x1: hi[0] / H + 0.5, y1: hi[1] / H + 0.5 });

const inBox = (p, q, lo, hi) => Math.max(p[0], q[0]) >= lo[0] && Math.min(p[0], q[0]) <= hi[0] && Math.max(p[1], q[1]) >= lo[1] && Math.min(p[1], q[1]) <= hi[1];

const byId = (p, q) => p.id - q.id;

/**
 * The road network of a world (world/createWorld.js) as structvox's
 * RoadNetwork reads it (see above). Every method a pure function of the
 * world's config and its arguments.
 */
export function roadNetwork(world) {
  const walks = walkMethods(world);
  // (the lanes met so far, and the road each is of)
  const laneIndex = new Map();
  const laneInfo = new Map();
  const lookup = (id) => laneIndex.get(id) ?? null;
  const index = (list, info) => {
    for (const l of list) {
      laneIndex.set(l.id, l);
      laneInfo.set(l.id, info);
    }
    return list;
  };

  /** The node a lane runs into (its link's end node for its direction), with its road's info. */
  const endNode = (l) => {
    const info = laneInfo.get(l.id);
    const link = info.links[l.key.link];
    return { info, node: l.key.dir === 0 ? link.to : link.from, link };
  };

  /** The lanes leaving a node of road `info` away from it (the first piece of the links on either side). */
  const leaving = (info, node) => {
    const out = [];
    const li = info.links.findIndex((q) => q.from === node);
    const lb = info.links.findIndex((q) => q.to === node);
    const all = index(roadLanes(world, info), info);
    if (li >= 0) for (const l of all) if (l.key.link === li && l.key.dir === 0 && l.key.piece === 0) out.push(l);
    if (lb >= 0) for (const l of all) if (l.key.link === lb && l.key.dir === 1 && l.key.piece === 0) out.push(l);
    return out;
  };

  const heading = (l) => {
    const dx = l.b[0] - l.a[0];
    const dy = l.b[1] - l.a[1];
    const n = Math.hypot(dx, dy) || 1;
    return [dx / n, dy / n];
  };

  // the highways' lanes, linked to the streets where their ramps land
  const hl = world.highways
    ? highwayLanes(world, {
        out(edgeId, ri) {
          const [, axis, a, b] = /^H(\d)_(-?\d+)_(-?\d+)$/.exec(edgeId).map(Number);
          const r = world.highways.ramps(world.highways.edge(axis, a, b))[ri];
          const road = r && roadById(world, r.arterial);
          if (!road) return [];
          const info = roadInfo(world, road);
          const node = info.nodes.find((q) => q.ramps?.some((x) => x.edge === edgeId && x.index === ri));
          return node ? leaving(info, node) : [];
        },
      })
    : null;

  return {
    /** The lanes running through a box (metres, x and y), in id order. */
    lanesIn(lo, hi) {
      const out = [];
      for (const road of roadsIn(world, voxBox(lo, hi))) {
        const info = roadInfo(world, road);
        for (const l of index(roadLanes(world, info), info)) if (inBox(l.a, l.b, lo, hi)) out.push(l);
      }
      if (hl) out.push(...hl.lanesIn(voxBox(lo, hi), (l) => inBox(l.a, l.b, lo, hi)));
      return out.sort(byId);
    },

    /** One lane by id (as lanesIn gave it), or null. */
    lane(id) {
      return lookup(id) ?? hl?.lane(id) ?? null;
    },

    /** The lanes a car at the end of lane `id` may go on along: [[id, turn]] (-1 left, 0 straight on, 1 right). */
    next(id) {
      if (hl?.has(id)) return hl.next(id);
      const l = lookup(id);
      if (!l) return [];
      const { info, node, link } = endNode(l);
      const all = index(roadLanes(world, info), info);
      // (along its link: the next piece, the same lane)
      if (!l.key.last) {
        const q = all.find((r) => r.key.link === l.key.link && r.key.dir === l.key.dir && r.key.k === l.key.k && r.key.piece === l.key.piece + 1);
        return q ? [[q.id, 0]] : [];
      }
      const n = info.lanes;
      // the ways on: every road of the junction (this one on beyond it among them), each way it leaves
      const [hx, hy] = heading(l);
      const ways = [];
      for (const g of junctionOf(world, info, node).members) {
        const outs = leaving(g.info, g.node).filter((r) => !(g.info === info && r.key.link === l.key.link));
        const byDir = new Map();
        for (const r of outs) {
          const key = `${r.key.link}/${r.key.dir}`;
          if (!byDir.has(key)) byDir.set(key, []);
          byDir.get(key).push(r);
        }
        for (const list of byDir.values()) {
          list.sort((a, b) => a.key.k - b.key.k);
          const [ox, oy] = heading(list[0]);
          const dot = hx * ox + hy * oy;
          if (dot < -0.95) continue;
          const cross = hx * oy - hy * ox;
          ways.push({ list, turn: dot > STRAIGHT ? 0 : cross < 0 ? 1 : -1 });
        }
      }
      // (an on-ramp of a highway starting here)
      for (const q of node.ramps ?? []) {
        if (q.off || !hl) continue;
        const first = hl.rampLanes(q.edge, q.index).first;
        if (!first) continue;
        const [ox, oy] = heading(first);
        const dot = hx * ox + hy * oy;
        if (dot < -0.95) continue;
        ways.push({ list: [first], turn: dot > STRAIGHT ? 0 : hx * oy - hy * ox < 0 ? 1 : -1 });
      }
      // (straight on keeps its lane; right from the kerb lane, left from the inner one; with no straight on, any lane turns)
      const straight = ways.some((q) => q.turn === 0);
      const cands = [];
      for (const { list, turn } of ways) {
        if (turn === 0) cands.push([list[Math.min(l.key.k, list.length - 1)].id, 0]);
        else if (turn === 1 && (l.key.k === n - 1 || n === 1 || !straight)) cands.push([list[list.length - 1].id, 1]);
        else if (turn === -1 && (l.key.k === 0 || n === 1 || !straight)) cands.push([list[0].id, -1]);
      }
      if (!cands.length) {
        // (a dead end, for traffic too where it meets only alleys or footways: back the way it came, from its inner lane)
        const back = all.find((r) => r.key.link === link.k && r.key.dir !== l.key.dir && r.key.k === 0 && r.key.piece === 0);
        if (back) cands.push([back.id, -1]);
      }
      return cands.sort((a, b) => a[0] - b[0]);
    },

    /**
     * Lane `id`'s signal: { cycle, offset, from, to } (s: green while
     * (t + offset) mod cycle lies in [from, to)), or null (never held).
     */
    signal(id) {
      if (hl?.has(id)) return hl.signal(id);
      const l = lookup(id);
      if (!l || !l.key.last) return null;
      const { info, node } = endNode(l);
      return signalOf(world, info, node);
    },

    /** May lane `id`'s traffic enter its junction at time t (s)? */
    green(id, t) {
      return open(this.signal(id), t);
    },

    /** Kerbside parking places in a box (metres): [{ id, pos [x, y, z], heading [dx, dy] }], in id order. */
    parkingIn(lo, hi) {
      const out = [];
      for (const road of roadsIn(world, voxBox(lo, hi))) {
        if (!road.parking || road.lanes < 2) continue;
        const info = roadInfo(world, road);
        const off = road.hc - (road.shoulder ?? 0) - road.parking / 2;
        for (let li = 0; li < info.links.length; li += 1) {
          const link = info.links[li];
          const s0 = link.from.at + link.from.trim;
          const s1 = link.to.at - link.to.trim;
          for (const dir of [0, 1]) {
            let n = 0;
            for (let s = PARK_START; s + PARK_LEN <= s1 - s0 - PARK_START; s += PARK_PITCH, n += 1) {
              const at = dir === 0 ? s0 + s + PARK_LEN / 2 : s1 - s - PARK_LEN / 2;
              const q = along(info, at);
              const hx = dir === 0 ? q.dx : -q.dx;
              const hy = dir === 0 ? q.dy : -q.dy;
              const x = q.x + hy * off;
              const y = q.y - hx * off;
              const pos = [m(x), m(y), H * (Math.round(levelAlong(world, info, at)) + 0.5)];
              if (pos[0] < lo[0] || pos[0] > hi[0] || pos[1] < lo[1] || pos[1] > hi[1]) continue;
              out.push({ id: idOf(road.id, "park", li, dir, n), pos, heading: [hx, hy] });
            }
          }
        }
      }
      return out.sort(byId);
    },

    ...walks,

    /**
     * Everything of a box a host's own RoadNetwork holds (a host that cannot
     * call back, a browser's engine beside this worker): its lanes with
     * their next lanes and signals, its walkways with the walkways at their
     * ends and their signals, its parking places. Plain arrays, ids as
     * numbers (52 bits).
     */
    region(lo, hi) {
      const lanes = this.lanesIn(lo, hi).map((l) => ({ id: l.id, a: l.a, b: l.b, width: l.width, speed: l.speed, next: this.next(l.id), signal: this.signal(l.id) }));
      const walkList = this.walksIn(lo, hi).map((w) => ({ id: w.id, a: w.a, b: w.b, inset: w.inset, width: w.width, crossing: w.crossing, kind: w.kind, signal: w.signal ?? null, nextA: this.walkNext(w.id, 0), nextB: this.walkNext(w.id, 1) }));
      return { lanes, walks: walkList, parking: this.parkingIn(lo, hi) };
    },
  };
}

/**
 * Walkways: sidewalks along each side of a link from corner to corner, the
 * stretches between corners a junction puts on one side, crossings over a
 * road at its crosswalks; walks down the middle of alleys and single-track
 * lanes, and over a street they run across. A walk's end meets every walk
 * of the junction's roads ending within SNAP of it (a pure function of the
 * junction: the same whatever was asked before).
 */
function walkMethods(world) {
  const walkIndex = new Map();
  // (each walk's ends, city voxels, and the roads of the junctions there: where walkNext looks)
  const walkEnds = new Map();

  /**
   * Where people walk along a road: on the middle of its sidewalks (w off
   * its centre line, each side); down the middle of an alley or a
   * single-track lane, which has none (and no traffic); nowhere on a road
   * with neither (a rural road: w is its kerb, where a walk meeting it stops).
   */
  const side = (road) => {
    if (road.hr > road.hc) return { w: (road.hc + road.hr) / 2, width: road.hr - road.hc, sides: [1, -1] };
    if (road.lanes <= 1) return { w: 0, width: 2 * road.hc, sides: [0] };
    return { w: road.hc, width: 0, sides: [] };
  };

  /**
   * The corner where road A's walking line on side sa meets road B's on side
   * sb (0: its middle): the crossing of the two lines, from the two
   * segments' own base points and directions, with the roads in id order (so
   * either road finds the very same point).
   */
  const corner = (A, sgA, sa, B, sgB, sb) => {
    if (A.road.id > B.road.id) return corner(B, sgB, sb, A, sgA, sa);
    const oa = side(A.road).w * sa;
    const ob = side(B.road).w * sb;
    const ax = sgA.ax + sgA.dy * oa;
    const ay = sgA.ay - sgA.dx * oa;
    const bx = sgB.ax + sgB.dy * ob;
    const by = sgB.ay - sgB.dx * ob;
    const den = sgA.dx * sgB.dy - sgA.dy * sgB.dx;
    const t = ((bx - ax) * sgB.dy - (by - ay) * sgB.dx) / den;
    return { x: ax + sgA.dx * t, y: ay + sgA.dy * t };
  };

  /** Road `other`'s own segment where road `info` meets it (meet q): its info's, by index. */
  const segOf = (other, q) => other.segs.find((sg) => sg.idx === q.seg.idx) ?? other.segs[0];

  /** Which side of road `info` (at point p) the road of meet q lies on, when it ends there: +1 right, -1 left, 0 both (it crosses). */
  const sideOfEnding = (info, p, q) => {
    if (!q.ends) return 0;
    // (its body runs away from the junction: along it if it starts here, against it if it ends here)
    const away = q.t < q.seg.len / 2 ? 1 : -1;
    const vx = q.seg.dx * away;
    const vy = q.seg.dy * away;
    return Math.sign(vx * p.dy - vy * p.dx) || 1;
  };

  /**
   * The corners on road `info`'s walking line on side sgn at `node`, in
   * order along the road: where the walking lines of the roads met there on
   * that side cross it (a road with sidewalks: both of them; an alley: its
   * middle; a rural road: its kerbs), else (a dead end, or a road ending on
   * the other side) the line at the node itself. Each with its meet (s: arc
   * offset from the node).
   */
  const cornersAt = (info, node, sgn) => {
    const key = `c${sgn}`;
    if (node[key]) return node[key];
    const p = along(info, node.at);
    const out = [];
    for (const q of node.meets) {
      const on = sideOfEnding(info, p, q);
      if (sgn !== 0 && on !== 0 && on !== sgn) continue;
      const other = roadInfo(world, q.road);
      const so = segOf(other, q);
      for (const f of side(other.road).w === 0 ? [0] : [1, -1]) {
        const c = corner(info, q.own, sgn, other, so, f);
        out.push({ x: c.x, y: c.y, s: (c.x - p.x) * p.dx + (c.y - p.y) * p.dy, q, other });
      }
    }
    if (!out.length) {
      const o = side(info.road).w * sgn;
      out.push({ x: p.x + p.dy * o, y: p.y - p.dx * o, s: 0, q: null });
    }
    node[key] = out.sort((a, b) => a.s - b.s || a.x - b.x || a.y - b.y);
    return node[key];
  };

  /** Where road `info`'s walk on side sgn ends at `node` on its arm `arm` (+1: the link runs on from the node, -1: back): the corner nearest the link. */
  const end = (info, node, arm, sgn) => {
    const cs = cornersAt(info, node, sgn);
    return arm > 0 ? cs[cs.length - 1] : cs[0];
  };

  const walksOf = (info) => {
    if (info.walkList) return info.walkList;
    const road = info.road;
    const out = [];
    const { width, sides } = side(road);
    // (on the sidewalk, a kerb above the carriageway; on the carriageway of an alley)
    const lift = sides.length === 2 ? 1.5 : 0.5;
    const z = (c) => H * (roadTop(world, info, c.x, c.y) + lift);
    const add = (w, pa, pb, nodes) => {
      const roads = new Set();
      for (const n of nodes) for (const g of junctionOf(world, info, n).members) roads.add(g.info);
      walkIndex.set(w.id, w);
      walkEnds.set(w.id, { pa, pb, roads: [...roads] });
      out.push(w);
    };
    const walk = (id, kind, pa, pb, extra) => ({ id, a: [m(pa.x), m(pa.y), z(pa)], b: [m(pb.x), m(pb.y), z(pb)], inset: [0, 0, 0], width: width * H, crossing: kind === "crossing", kind, ...extra });
    for (let li = 0; li < info.links.length; li += 1) {
      const link = info.links[li];
      const mid = along(info, (link.from.at + link.to.at) / 2);
      for (const sgn of sides) {
        const pa = end(info, link.from, 1, sgn);
        const pb = end(info, link.to, -1, sgn);
        if ((pb.x - pa.x) * mid.dx + (pb.y - pa.y) * mid.dy < 8) continue;
        // (towards the buildings: a fifth of the sidewalk)
        const ins = 0.2 * width * H * sgn;
        add(walk(idOf(road.id, "walk", li, sgn), sgn ? "sidewalk" : "middle", pa, pb, { inset: [mid.dy * ins, -mid.dx * ins, 0] }), pa, pb, [link.from, link.to]);
      }
      // a crosswalk over this road on each of the link's arms at a junction that marks one (between its two corners there)
      if (sides.length === 2)
        for (const [node, arm, tag] of [[link.from, 1, "a"], [link.to, -1, "b"]]) {
          if (!node.meets.some((q) => q.cw)) continue;
          const ca = end(info, node, arm, 1);
          const cb = end(info, node, arm, -1);
          if (!ca.q || !cb.q) continue;
          // (over a road at a signalled junction: open while its traffic is held)
          const signal = signalOf(world, info, node, true);
          add(walk(idOf(road.id, "cross", li, tag), "crossing", ca, cb, { width: 3, ...(signal ? { signal } : {}) }), ca, cb, [node]);
        }
    }
    // at each junction: on along a side between the corners of two roads met there (a side road's mouth
    // just past a crossing); an alley over a street it runs across (between that street's two sidewalks)
    for (let ni = 0; ni < info.nodes.length; ni += 1) {
      const node = info.nodes[ni];
      const through = ni > 0 && ni < info.nodes.length - 1;
      for (const sgn of sides) {
        const cs = cornersAt(info, node, sgn);
        for (let k = 0; k + 1 < cs.length; k += 1) {
          const [c0, c1] = [cs[k], cs[k + 1]];
          if (!c0.q || !c1.q || c1.s - c0.s <= SNAP) continue;
          if (c0.q !== c1.q) add(walk(idOf(road.id, "jw", ni, sgn, k), sgn ? "corner" : "middle", c0, c1), c0, c1, [node]);
          else if (sgn === 0 && through && c0.other.lanes) {
            const at = junctionOf(world, info, node).members.find((g) => g.info === c0.other);
            const signal = at ? signalOf(world, at.info, at.node, true) : null;
            add(walk(idOf(road.id, "cross", ni, k), "crossing", c0, c1, signal ? { signal } : {}), c0, c1, [node]);
          }
        }
      }
    }
    info.walkList = out;
    return out;
  };

  return {
    /**
     * The walkways in a box (metres), in id order: [{ id, a, b, inset, width,
     * crossing, kind, signal? }], kind "sidewalk", "corner" (between two
     * roads' corners on one side of a junction), "crossing" or "middle" (down
     * an alley or a single-track lane).
     */
    walksIn(lo, hi) {
      const out = [];
      for (const road of roadsIn(world, voxBox(lo, hi))) for (const w of walksOf(roadInfo(world, road))) if (inBox(w.a, w.b, lo, hi)) out.push(w);
      return out.sort(byId);
    },
    /** May people step onto walkway `id` at time t (s)? (a sidewalk: always) */
    walkOpen(id, t) {
      return open(walkIndex.get(id)?.signal ?? null, t);
    },
    /** One walkway by id (as walksIn gave it), or null. */
    walk(id) {
      return walkIndex.get(id) ?? null;
    },
    /** The walkways meeting walk `id`'s end (0 a, 1 b) at its corner: [[id, their end there]], in id order. */
    walkNext(id, end) {
      const e = walkEnds.get(id);
      if (!e) return [];
      const p = end === 0 ? e.pa : e.pb;
      const out = [];
      for (const info of e.roads)
        for (const w of walksOf(info)) {
          if (w.id === id) continue;
          const q = walkEnds.get(w.id);
          if (Math.hypot(q.pa.x - p.x, q.pa.y - p.y) <= SNAP) out.push([w.id, 0]);
          if (Math.hypot(q.pb.x - p.x, q.pb.y - p.y) <= SNAP) out.push([w.id, 1]);
        }
      return out.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    },
  };
}
