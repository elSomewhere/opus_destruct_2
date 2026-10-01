import { SpatialGrid } from "../core/geom2d.js";
import { CLASS_RANK } from "./roadClasses.js";

/**
 * A road view flattens the polyline roads of a neighbourhood of cells into
 * straight segments with junction annotations. Junctions are computed on the
 * merged set so roads owned by different cells still know about each other
 * (crosswalks, stop lines and marking suppression need that).
 */

export function buildSegments(roads) {
  const segs = [];
  for (const road of roads) {
    let acc = 0;
    for (let k = 0; k < road.pts.length - 1; k += 1) {
      const a = road.pts[k];
      const b = road.pts[k + 1];
      const len = Math.hypot(b.x - a.x, b.y - a.y);
      if (len < 1e-6) continue;
      const pad = road.hr + road.corner + 2;
      segs.push({
        road,
        idx: k,
        ax: a.x,
        ay: a.y,
        bx: b.x,
        by: b.y,
        az: a.z,
        bz: b.z,
        len,
        dx: (b.x - a.x) / len,
        dy: (b.y - a.y) / len,
        s0: acc,
        hc: road.hc,
        hr: road.hr,
        sidewalk: road.sidewalk ?? road.hr - road.hc,
        parking: road.parking ?? 0,
        median: road.median ?? 0,
        lanes: road.lanes ?? 2,
        lane: road.lane ?? 26,
        shoulder: road.shoulder ?? 0,
        cls: road.cls,
        rank: CLASS_RANK[road.cls] ?? 0,
        first: k === 0,
        last: k === road.pts.length - 2,
        jn: [],
        bbox: {
          x0: Math.min(a.x, b.x) - pad,
          y0: Math.min(a.y, b.y) - pad,
          x1: Math.max(a.x, b.x) + pad,
          y1: Math.max(a.y, b.y) + pad,
        },
      });
      acc += len;
    }
  }
  return segs;
}

export function annotateJunctions(segs, grid) {
  const tmp = [];
  const eps = 3;
  // (a road's end lying in the other's carriageway, the centre lines crossing short of it or past it,
  // as a side road winding into a winding street does: decided once every plain junction is known)
  const loose = [];
  for (const s of segs) {
    tmp.length = 0;
    grid.query(s.bbox, tmp);
    for (const c of tmp) {
      if (c === s || c.road === s.road) continue;
      const cross = s.dx * c.dy - s.dy * c.dx;
      if (Math.abs(cross) < 0.5) continue; // near-parallel
      // intersect infinite lines
      const qx = c.ax - s.ax;
      const qy = c.ay - s.ay;
      const ts = (qx * c.dy - qy * c.dx) / cross;
      const tc = (qx * s.dy - qy * s.dx) / cross;
      if (ts >= -eps && ts <= s.len + eps && tc >= -eps && tc <= c.len + eps) {
        s.jn.push(junction(s, c, ts, tc, eps));
        continue;
      }
      // (else a road's end in the other's carriageway: the junction where that end lies, on it and across)
      const se = endIn(s, c, eps);
      const ce = endIn(c, s, eps);
      if (se) loose.push({ s, c, ts: se.t, tc: se.to, ends: ce ? [[s, se.t], [c, ce.t]] : [[s, se.t]] });
      else if (ce) loose.push({ s, c, ts: ce.to, tc: ce.t, ends: [[c, ce.t]] });
    }
  }
  // (such an end is a junction where it meets no other road: a dead end standing in the street, else
  // a ledge or a wall; an end at a junction already has that junction's level)
  const byRoad = new Map();
  for (const s of segs) byRoad.set(s.road, [...(byRoad.get(s.road) ?? []), s]);
  const free = (e, t) => !byRoad.get(e.road).some((x) => x.jn.some((j) => Math.abs(x.s0 + j.s - (e.s0 + t)) <= e.hr + 8));
  // (two roads that cross or meet elsewhere already have their junction)
  const met = (a, b) => byRoad.get(a).some((x) => x.jn.some((j) => j.other.road === b));
  const joined = loose.filter((q) => !met(q.s.road, q.c.road) && q.ends.every(([e, t]) => free(e, t)));
  for (const q of joined) q.s.jn.push(junction(q.s, q.c, q.ts, q.tc, eps));
  for (const s of segs) s.jn.sort((a, b) => a.s - b.s);
}

/** Segment s's junction with c, at s's arc ts and c's tc (where their centre lines cross, or a road's end lies). */
function junction(s, c, ts, tc, eps) {
  // does c continue on both sides of s (crossing) or end at it (T)?
  const cEndsHere = (tc < eps && c.first) || (tc > c.len - eps && c.last);
  const sEndsHere = (ts < eps && s.first) || (ts > s.len - eps && s.last);
  return {
    s: Math.max(0, Math.min(s.len, ts)),
    hc: c.hc,
    hr: c.hr,
    cls: c.cls,
    rank: c.rank,
    cEnds: cEndsHere,
    sEnds: sEndsHere,
    cw: crosswalkRule(s, c),
    signal: s.rank >= 4 && c.rank >= 4,
    // the crossing segment and where it is crossed (road levels meet there)
    other: c,
    tOther: Math.max(0, Math.min(c.len, tc)),
  };
}

/**
 * Does a road end on segment e (its first or last) lie in segment o's
 * carriageway? { t: that end's arc on e, to: where it lies along o }, or null.
 */
function endIn(e, o, eps) {
  for (const t of [...(e.first ? [0] : []), ...(e.last ? [e.len] : [])]) {
    const px = e.ax + e.dx * t;
    const py = e.ay + e.dy * t;
    const to = (px - o.ax) * o.dx + (py - o.ay) * o.dy;
    if (to >= -eps && to <= o.len + eps && Math.abs((px - o.ax) * o.dy - (py - o.ay) * o.dx) <= o.hc) return { t, to: Math.max(0, Math.min(o.len, to)) };
  }
  return null;
}

function crosswalkRule(s, c) {
  if (s.rank < 3 || c.rank < 2) return false;
  if (s.cls === "rural" || c.cls === "rural" || s.cls === "alley") return false;
  if (c.cls === "pedestrian") return true;
  return c.rank >= Math.min(s.rank, 4);
}

export class RoadView {
  constructor(roads) {
    this.segs = buildSegments(roads);
    this.grid = new SpatialGrid(128);
    for (const s of this.segs) this.grid.insert(s, s.bbox);
    annotateJunctions(this.segs, this.grid);
    // every junction along each road (road arc position `at`), shared by its segments (roadLevel.js)
    const byRoad = new Map();
    for (const s of this.segs) {
      let list = byRoad.get(s.road);
      if (!list) byRoad.set(s.road, (list = []));
      for (const j of s.jn) list.push({ j, seg: s, at: s.s0 + j.s });
    }
    for (const s of this.segs) s.rj = byRoad.get(s.road);
    this.maxReach = 0;
    for (const s of this.segs) this.maxReach = Math.max(this.maxReach, s.hr + s.road.corner + 2);
  }

  near(rect, out = []) {
    return this.grid.query(rect, out);
  }
}
