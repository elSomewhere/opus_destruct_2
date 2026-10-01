import { hashFloat, hash32 } from "../core/hash.js";
import { vx } from "../core/units.js";
import { catmullRom, polylineLengths, SpatialGrid } from "../core/geom2d.js";
import { LRU } from "../core/lru.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "./roadSurface.js";
import { segmentLevel } from "./roadLevel.js";
import { wrapOf } from "../world/wrap.js";

/**
 * Elevated highway network.
 *
 * Nodes sit on a jittered lattice (~3.4 km); each lattice edge exists with
 * some probability and is drawn as a Catmull-Rom spline through lateral
 * bends, so highways snake across the city. The deck profile follows the
 * terrain + clearance, max-filtered (to clear bumps) then smoothed (gentle
 * grades), and is pinned to node heights so edges meet cleanly.
 *
 * Everything is a pure function of the lattice => any region can be
 * generated independently. Cells query `corridorsNear` to keep lots under
 * the deck free of buildings; piers skip carriageways.
 */

const SAMPLE = vx(8);
/** Steepest ramp grade, and the share of a ramp's length easing into and out of it at each end. */
const RAMP_GRADE = 0.07;
const RAMP_EASE = 0.15;
/** Longest ramp (voxels). */
const RAMP_MAX = vx(320);
/** Headroom a ramp leaves over a carriageway under it, and over a sidewalk (voxels). */
const RAMP_CLEAR = 36;
const WALK_CLEAR = 20;
/** Pruning passes for chance edges: spurs up to this many edges long vanish. */
const SPUR_PASSES = 2;
/** The four lattice edges at node (a, b): [axis, a, b]. */
const incident = (a, b) => [
  [0, a, b],
  [0, a - 1, b],
  [1, a, b],
  [1, a, b - 1],
];
const NODE_RING = [
  [2400, 0],
  [-2400, 0],
  [0, 2400],
  [0, -2400],
  [1700, 1700],
  [-1700, 1700],
  [1700, -1700],
  [-1700, -1700],
];

export class HighwayNetwork {
  constructor(world) {
    this.world = world;
    const c = world.config.highways;
    this.cfg = c;
    this.spacing = vx(c.nodeSpacing);
    this.hw = Math.round(vx(2 * c.lanesPerSide * c.laneWidth + 2 * c.shoulder + 0.6) / 2) + 3;
    this.clear = vx(c.deckHeight);
    this.edges = new LRU(64);
    /** edge existence by pruning pass (cheap booleans, kept) */
    this.memo = new Map();
    this.rs = makeRoadSample();
    // a wrapping world has n lattice nodes around it (canonical seeds, positions by lap)
    this.wrap = wrapOf(world.config);
    this.n = this.wrap.count(c.nodeSpacing);
    /** half-length of a junction plateau (voxels): where crossing arms overlap, down to 45 degrees */
    this.plateau = Math.round(2.5 * this.hw);
    /** a ramp's band beside the deck: from the deck's edge, 7 m wide; its centre line */
    this.rampIn = this.hw + 1;
    this.rampMid = this.rampIn + vx(3.5);
  }

  /** Canonical lattice index (identity unless the world wraps). */
  canon(a) {
    return this.wrap.canon(a, this.n);
  }

  node(a, b) {
    const s = this.world.seed;
    const j = this.cfg.jitter * this.spacing;
    const W = this.wrap;
    const ca = this.canon(a);
    const cb = this.canon(b);
    const x = Math.round(ca * this.spacing + (hashFloat(s, ca, cb, 501) - 0.5) * 2 * j) + W.lap(a, this.n) * W.sizeV;
    const y = Math.round(cb * this.spacing + (hashFloat(s, ca, cb, 502) - 0.5) * 2 * j) + W.lap(b, this.n) * W.sizeV;
    const ts = this.world.terrain.sample(x, y);
    // in rough country a junction takes the local valley level (it may lie
    // inside a mountain: tunnels meet there)
    let h = ts.h;
    if (ts.u < 0.3) for (const [dx, dy] of NODE_RING) h = Math.min(h, this.world.terrain.sample(x + dx, y + dy).h);
    const z = Math.round(h + this.clearanceAt(ts.u));
    return { a, b, x, y, z };
  }

  /** Deck height above the terrain: elevated in cities, on a low embankment in open country. */
  clearanceAt(u) {
    return u > 0.3 ? this.clear : 4;
  }

  /**
   * Deck profile for resampled points (8 m apart). Hard lower bounds keep
   * the deck elevated where it must clear something (urban land, crossing
   * roads, rivers); elsewhere it follows the terrain softly. A grade limit
   * (5%) then decides where the road ends up above the land (embankments,
   * viaducts) or below it (cuttings, tunnels through hills and mountains).
   */
  profile(pts, zStart, zEnd, flatStart = 0, flatEnd = 0) {
    const w = this.world;
    const n = pts.length;
    // (the 5% limit over the samples' own spacing: the shortest chord, a little under SAMPLE on a bend)
    let step = SAMPLE;
    for (let i = 1; i < n; i += 1) step = Math.min(step, Math.hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y));
    const g = step * 0.05;
    const target = new Float64Array(n);
    const hard = new Float64Array(n).fill(-Infinity);
    for (let i = 0; i < n; i += 1) {
      const p = pts[i];
      const ts = w.terrain.sample(p.x, p.y);
      target[i] = ts.h + this.clearanceAt(ts.u);
      if (ts.u > 0.3) hard[i] = target[i];
      // crossing roads and rivers need an overpass / bridge
      const c = w.cellAt(p.x, p.y);
      const cands = w.roadView(c.i, c.j).near({ x0: p.x - 40, y0: p.y - 40, x1: p.x + 40, y1: p.y + 40 });
      sampleRoadSurface(cands, p.x + 0.5, p.y + 0.5, this.rs, w.seed);
      if (this.rs.kind !== KIND.NONE || this.rs.sdfR < 40) hard[i] = Math.max(hard[i], ts.h + Math.round(this.clear * 0.8));
      if (w.isWet && w.isWet(p.x, p.y, 12)) hard[i] = Math.max(hard[i], ts.h + 40);
    }
    // spread hard constraints over +-3 samples (clear the whole crossing)
    const lo = new Float64Array(n);
    for (let i = 0; i < n; i += 1) {
      let m = -Infinity;
      for (let j = Math.max(0, i - 3); j <= Math.min(n - 1, i + 3); j += 1) m = Math.max(m, hard[j]);
      lo[i] = m;
    }
    // soft target smoothed, then the highest grade-limited profile that
    // stays at or below it: ridges are tunnelled, valleys are never bridged
    // high above the ground (only hard constraints lift the road)
    const z = new Float64Array(n);
    for (let i = 0; i < n; i += 1) {
      let s = 0;
      let c = 0;
      let mn = Infinity;
      for (let j = Math.max(0, i - 6); j <= Math.min(n - 1, i + 6); j += 1) {
        s += target[j];
        c += 1;
        mn = Math.min(mn, target[j]);
      }
      z[i] = Math.min(s / c, mn + 24);
    }
    for (let i = 1; i < n; i += 1) z[i] = Math.min(z[i], z[i - 1] + g);
    for (let i = n - 2; i >= 0; i -= 1) z[i] = Math.min(z[i], z[i + 1] + g);
    // hard bounds win; propagate the raise at the grade limit
    for (let i = 0; i < n; i += 1) z[i] = Math.max(z[i], lo[i]);
    for (let i = 1; i < n; i += 1) z[i] = Math.max(z[i], z[i - 1] - g);
    for (let i = n - 2; i >= 0; i -= 1) z[i] = Math.max(z[i], z[i + 1] - g);
    // round the grade breaks
    const out = new Array(n);
    for (let i = 0; i < n; i += 1) {
      let s = 0;
      let c = 0;
      for (let j = Math.max(0, i - 3); j <= Math.min(n - 1, i + 3); j += 1) {
        s += z[j];
        c += 1;
      }
      out[i] = Math.max(s / c, lo[i]);
    }
    // meet the node heights (shared by every edge at a node) within the
    // grade limit: clamp into the cone reachable from both ends, then sweep
    out[0] = zStart;
    out[n - 1] = zEnd;
    for (let i = 1; i < n - 1; i += 1) {
      const a = Math.max(zStart - g * i, zEnd - g * (n - 1 - i));
      const b = Math.min(zStart + g * i, zEnd + g * (n - 1 - i));
      out[i] = Math.min(Math.max(out[i], a), b);
    }
    for (let pass = 0; pass < 2; pass += 1) {
      for (let i = 1; i < n - 1; i += 1) out[i] = Math.min(Math.max(out[i], out[i - 1] - g), out[i - 1] + g);
      for (let i = n - 2; i > 0; i -= 1) out[i] = Math.min(Math.max(out[i], out[i + 1] - g), out[i + 1] + g);
    }
    // junction plateaus: level at the node's height, then back to the profile within the grade limit
    const fs = Math.min(n - 1, Math.ceil(flatStart / step));
    const fe = Math.min(n - 1, Math.ceil(flatEnd / step));
    if (fs || fe) {
      for (let i = 0; i <= fs; i += 1) out[i] = zStart;
      for (let i = n - 1 - fe; i < n; i += 1) out[i] = zEnd;
      for (let i = fs + 1; i < n - 1 - fe; i += 1) out[i] = Math.min(Math.max(out[i], out[i - 1] - g), out[i - 1] + g);
      for (let i = n - 2 - fe; i > fs; i -= 1) out[i] = Math.min(Math.max(out[i], out[i + 1] - g), out[i + 1] + g);
    }
    return out;
  }

  /**
   * Does lattice edge (a,b)->(a+1,b) [axis 0] or (a,b)->(a,b+1) [axis 1]
   * exist? Along intercity routes always (within the grade limit); inside
   * urban areas by chance (highways snaking through the city), between two
   * urban nodes and never as a spur: a chance edge needs another edge at
   * both of its nodes, tested twice over (spurs of one or two edges are
   * pruned). A pure function of the lattice round the edge.
   */
  edgeExists(axis, a, b) {
    return this.baseEdge(axis, a, b, SPUR_PASSES);
  }

  /** Edge existence after k pruning passes (memoized): routes, and chance edges with company at both ends. */
  baseEdge(axis, a, b, k) {
    const key = `${axis},${a},${b},${k}`;
    let v = this.memo.get(key);
    if (v !== undefined) return v;
    if (!this.gradeOk(axis, a, b)) v = false;
    else if (this.onIntercityRoute(axis, a, b)) v = true;
    else if (!this.chanceEdge(axis, a, b)) v = false;
    else if (k === 0) v = true;
    else {
      const [a1, b1] = axis === 0 ? [a + 1, b] : [a, b + 1];
      const others = (na, nb) => incident(na, nb).filter(([x, p, q]) => !(x === axis && p === a && q === b));
      const company = (na, nb) => others(na, nb).some(([x, p, q]) => this.baseEdge(x, p, q, k - 1));
      v = company(a, b) && company(a1, b1);
    }
    this.memo.set(key, v);
    return v;
  }

  /** A chance edge: drawn by the hash, between two urban nodes. */
  chanceEdge(axis, a, b) {
    if (hashFloat(this.world.seed, this.canon(a) * 2 + axis, this.canon(b), 503) >= this.cfg.edgeChance) return false;
    const n0 = this.latticeXY(a, b);
    const n1 = axis === 0 ? this.latticeXY(a + 1, b) : this.latticeXY(a, b + 1);
    const f = this.world.fields;
    return Math.min(f.urban(n0.x, n0.y).u, f.urban(n1.x, n1.y).u) >= this.cfg.minUrbanization;
  }

  /** Edges at lattice node (a, b): [[axis, a, b], ...] of those that exist. */
  edgesAt(a, b) {
    return incident(a, b).filter(([x, p, q]) => this.edgeExists(x, p, q));
  }

  /**
   * A node's deck height as its edges meet it: the node's own, but at a
   * terminus (a route's last node, where one edge ends) the ground, so the
   * highway comes down to it and ends at grade, open.
   */
  nodeZ(a, b) {
    const n = this.node(a, b);
    if (this.edgesAt(a, b).length !== 1) return n.z;
    const w = this.world;
    return Math.round(w.streetLevel ? w.streetLevel(n.x, n.y) : w.terrain.sample(n.x, n.y).h);
  }

  /** Can the two junction heights be joined within the grade limit (with slack for bends)? */
  gradeOk(axis, a, b) {
    const n0 = this.node(a, b);
    const n1 = axis === 0 ? this.node(a + 1, b) : this.node(a, b + 1);
    return Math.abs(n1.z - n0.z) <= 0.05 * Math.hypot(n1.x - n0.x, n1.y - n0.y) * 0.9;
  }

  /** Unjittered lattice position of node (a,b) (cheap, for tests). */
  latticeXY(a, b) {
    return { x: a * this.spacing, y: b * this.spacing };
  }

  /**
   * Intercity routes: every settlement links to its east and south
   * neighbours (macro cells) by an L-shaped path along the lattice, so the
   * motorway net connects cities without covering the countryside. The
   * test is local: only settlements within two macro cells are examined.
   */
  onIntercityRoute(axis, a, b) {
    const f = this.world.fields;
    const cellV = this.world.config.world.settlementCell * 8;
    const x = a * this.spacing;
    const y = b * this.spacing;
    const ci = Math.round(x / cellV);
    const cj = Math.round(y / cellV);
    const R = Math.ceil((this.spacing * 2) / cellV) + 1;
    for (let j = cj - R; j <= cj + R; j += 1) {
      for (let i = ci - R; i <= ci + R; i += 1) {
        const s = f.settlement(i, j);
        if (!s) continue;
        for (const t of [f.settlement(i + 1, j), f.settlement(i, j + 1)]) {
          if (!t) continue;
          if (this.routeHas(s, t, axis, a, b)) return true;
        }
      }
    }
    return false;
  }

  routeHas(s, t, axis, a, b) {
    const a0 = Math.round(s.x / this.spacing);
    const b0 = Math.round(s.y / this.spacing);
    const a1 = Math.round(t.x / this.spacing);
    const b1 = Math.round(t.y / this.spacing);
    // L-shaped: along a first (at row b0), then along b (at column a1), or the reverse
    const xFirst = hashFloat(this.world.seed, this.canon(a0) * 31 + this.canon(a1), this.canon(b0) * 17 + this.canon(b1), 507) < 0.5;
    const rowB = xFirst ? b0 : b1;
    const colA = xFirst ? a1 : a0;
    if (axis === 0) return b === rowB && a >= Math.min(a0, a1) && a < Math.max(a0, a1);
    return a === colA && b >= Math.min(b0, b1) && b < Math.max(b0, b1);
  }

  edge(axis, a, b) {
    const key = `${axis},${a},${b}`;
    let e = this.edges.get(key);
    if (e !== undefined) return e;
    e = this.edgeExists(axis, a, b) ? this.buildEdge(axis, a, b) : null;
    this.edges.set(key, e);
    return e;
  }

  buildEdge(axis, a, b) {
    const w = this.world;
    const n0 = this.node(a, b);
    const n1 = axis === 0 ? this.node(a + 1, b) : this.node(a, b + 1);
    const dx = n1.x - n0.x;
    const dy = n1.y - n0.y;
    const len = Math.hypot(dx, dy);
    const px = -dy / len;
    const py = dx / len;
    const ctrl = [{ x: n0.x, y: n0.y }];
    const bends = 3;
    for (let k = 1; k <= bends; k += 1) {
      const t = k / (bends + 1);
      const off = (hashFloat(w.seed, this.canon(a) * 2 + axis, this.canon(b), 510 + k) - 0.5) * 2 * 0.2 * len;
      ctrl.push({ x: n0.x + dx * t + px * off, y: n0.y + dy * t + py * off });
    }
    ctrl.push({ x: n1.x, y: n1.y });
    let pts = catmullRom(ctrl, 24);
    // resample uniformly
    const L = polylineLengths(pts);
    const total = L[L.length - 1];
    const n = Math.max(2, Math.round(total / SAMPLE));
    const res = [];
    let k = 0;
    for (let i = 0; i <= n; i += 1) {
      const s = (i / n) * total;
      while (k < L.length - 2 && L[k + 1] < s) k += 1;
      const t = (s - L[k]) / (L[k + 1] - L[k] || 1);
      res.push({ x: pts[k].x + (pts[k + 1].x - pts[k].x) * t, y: pts[k].y + (pts[k + 1].y - pts[k].y) * t });
    }
    pts = res;
    // deck profile (see profile()): elevated through cities and over roads
    // and rivers, near grade in open country, never steeper than 5%; level
    // over a junction plateau at a node where other than two edges meet
    const [a1, b1] = axis === 0 ? [a + 1, b] : [a, b + 1];
    const deg0 = this.edgesAt(a, b).length;
    const deg1 = this.edgesAt(a1, b1).length;
    const z0 = this.nodeZ(a, b);
    const z1 = this.nodeZ(a1, b1);
    const prof = this.profile(pts, z0, z1, deg0 !== 2 ? this.plateau : 0, deg1 !== 2 ? this.plateau : 0);
    // (unrounded: each column rounds its own level, so the grade limit holds between samples)
    for (let i = 0; i < pts.length; i += 1) pts[i].z = prof[i];
    const lengths = polylineLengths(pts);
    // segment index
    const grid = new SpatialGrid(512);
    const segs = [];
    for (let i = 0; i < pts.length - 1; i += 1) {
      const p = pts[i];
      const q = pts[i + 1];
      const sl = Math.hypot(q.x - p.x, q.y - p.y) || 1;
      const seg = { ax: p.x, ay: p.y, az: p.z, bx: q.x, by: q.y, bz: q.z, len: sl, dx: (q.x - p.x) / sl, dy: (q.y - p.y) / sl, s0: lengths[i] };
      const pad = this.hw + 4 + 64; // deck + ramps
      const bb = { x0: Math.min(p.x, q.x) - pad, y0: Math.min(p.y, q.y) - pad, x1: Math.max(p.x, q.x) + pad, y1: Math.max(p.y, q.y) + pad };
      seg.bb = bb;
      segs.push(seg);
      grid.insert(seg, bb);
    }
    let bb = null;
    for (const sgm of segs) bb = bb ? { x0: Math.min(bb.x0, sgm.bb.x0), y0: Math.min(bb.y0, sgm.bb.y0), x1: Math.max(bb.x1, sgm.bb.x1), y1: Math.max(bb.y1, sgm.bb.y1) } : { ...sgm.bb };
    // the junctions at its ends (other than two edges meeting): level, no barriers within reach
    const junctions = [];
    if (deg0 !== 2) junctions.push({ x: n0.x, y: n0.y, z: z0, r: this.plateau, degree: deg0, node: [a, b] });
    if (deg1 !== 2) junctions.push({ x: n1.x, y: n1.y, z: z1, r: this.plateau, degree: deg1, node: [a1, b1] });
    const edge = { id: `H${axis}_${a}_${b}`, axis, a, b, nodes: [[a, b, deg0], [a1, b1, deg1]], pts, lengths, segs, grid, bb, total: lengths[lengths.length - 1], piers: null, junctions };
    return edge;
  }

  /** Highway edges whose bounds overlap rect. */
  edgesNear(rect) {
    const sp = this.spacing;
    const a0 = Math.floor(rect.x0 / sp) - 1;
    const a1 = Math.floor(rect.x1 / sp) + 1;
    const b0 = Math.floor(rect.y0 / sp) - 1;
    const b1 = Math.floor(rect.y1 / sp) + 1;
    const out = [];
    for (let b = b0; b <= b1; b += 1) {
      for (let a = a0; a <= a1; a += 1) {
        for (const axis of [0, 1]) {
          const e = this.edge(axis, a, b);
          if (!e) continue;
          if (e.bb.x1 < rect.x0 || e.bb.x0 > rect.x1 || e.bb.y1 < rect.y0 || e.bb.y0 > rect.y1) continue;
          out.push(e);
        }
      }
    }
    return out;
  }

  /** Corridor objects for cell planning: { hitsRect(r) } */
  corridorsNear(rect) {
    const margin = vx(this.cfg.corridorMargin);
    return this.edgesNear(rect).map((e) => ({
      edge: e,
      hitsRect: (r) => {
        const extra = vx(8);
        const reach = this.hw + margin + extra;
        const q = { x0: r.x0 - reach, y0: r.y0 - reach, x1: r.x1 + reach, y1: r.y1 + reach };
        const ramps = this.ramps(e);
        for (const s of e.grid.query(q)) {
          const d = segRectDist(s, r);
          if (d < this.hw + margin) return true;
          if (d < reach && ramps.length) {
            const cx = (r.x0 + r.x1) / 2;
            const cy = (r.y0 + r.y1) / 2;
            const t = Math.max(0, Math.min(s.len, (cx - s.ax) * s.dx + (cy - s.ay) * s.dy));
            const sp = s.s0 + t;
            const half = Math.max(r.x1 - r.x0, r.y1 - r.y0) / 2;
            if (ramps.some((rp) => sp >= Math.min(rp.sDeck, rp.sGround) - half - vx(20) && sp <= Math.max(rp.sDeck, rp.sGround) + half + vx(20))) return true;
          }
        }
        return false;
      },
    }));
  }

  /** Nearest deck point: { d (lateral, signed), s (arc), z (deck top), edge } or null. */
  nearest(x, y, edges, maxD) {
    let best = null;
    for (const e of edges) {
      for (const s of e.grid.queryPoint(x, y)) {
        const vx0 = x - s.ax;
        const vy0 = y - s.ay;
        let t = vx0 * s.dx + vy0 * s.dy;
        const tc = t < 0 ? 0 : t > s.len ? s.len : t;
        const ex = x - (s.ax + s.dx * tc);
        const ey = y - (s.ay + s.dy * tc);
        const dist = Math.hypot(ex, ey);
        if (dist > maxD || (best && dist >= Math.abs(best.d))) continue;
        const side = s.dx * vy0 - s.dy * vx0;
        best = { d: side >= 0 ? dist : -dist, s: s.s0 + tc, z: s.az + (s.bz - s.az) * (tc / s.len), edge: e, seg: s };
      }
    }
    return best;
  }

  /**
   * Diamond interchanges where the highway crosses an urban arterial: up to
   * four ramps (off before / on after the crossing, both directions), each
   * a lane-wide band beside the deck. A ramp's ground end is where its
   * centre line meets the arterial's kerb (on its near side before the
   * crossing, its far side after), at that street's level; its deck end is
   * as far along the deck as its height needs at RAMP_GRADE (eased in and
   * out over RAMP_EASE of its length at each end). A ramp is left out where
   * a junction plateau is too near, or another street would pass under it
   * with less than RAMP_CLEAR (a carriageway; less than WALK_CLEAR another
   * road's surface): there it would block the street. Interchanges keep
   * clear of each other by twice the longest ramp.
   * ramp: { side (+1 left / -1 right of the deck's direction), sDeck,
   * sGround, zDeck, zGround, cross (arc of the crossing), x, y (landing),
   * arterial (its road id) }
   */
  ramps(e) {
    if (e.ramps) return e.ramps;
    const w = this.world;
    const A = w.arterials;
    const out = [];
    const crossings = [];
    for (let k = 0; k < e.pts.length - 1; k += 1) {
      const a = e.pts[k];
      const b = e.pts[k + 1];
      for (const axis of [0, 1]) {
        const lo = Math.min(axis === 0 ? a.x : a.y, axis === 0 ? b.x : b.y);
        const hi = Math.max(axis === 0 ? a.x : a.y, axis === 0 ? b.x : b.y);
        const i0 = A.indexAt(axis, lo);
        for (let i = i0; i <= i0 + 1; i += 1) {
          const L = A.line(axis, i);
          if (L < lo || L > hi || hi === lo) continue;
          const t = (L - (axis === 0 ? a.x : a.y)) / ((axis === 0 ? b.x : b.y) - (axis === 0 ? a.x : a.y));
          const cx = a.x + (b.x - a.x) * t;
          const cy = a.y + (b.y - a.y) * t;
          if (w.fields.urban(cx, cy).u < 0.3) continue;
          crossings.push({ s: e.lengths[k] + t * (e.lengths[k + 1] - e.lengths[k]), x: cx, y: cy, axis, line: L });
        }
      }
    }
    crossings.sort((p, q) => p.s - q.s);
    const mid = this.rampMid;
    // (no ramp onto a junction plateau, nor past the edge's ends)
    const plateauAt = (k) => e.junctions.some((j) => j.node[0] === e.nodes[k][0] && j.node[1] === e.nodes[k][1]);
    const s0 = plateauAt(0) ? this.plateau + vx(40) : vx(60);
    const s1 = e.total - (plateauAt(1) ? this.plateau + vx(40) : vx(60));
    let last = -Infinity;
    for (const c of crossings) {
      if (c.s - last < 2 * RAMP_MAX + vx(200)) continue;
      if (hashFloat(this.world.seed, this.wrap.vi(c.x), this.wrap.vi(c.y), 777) > 0.7) continue;
      // the arterial there (a lattice line without its street has no interchange)
      const art = arterialAt(w, c.x, c.y, this.rs);
      if (!art) continue;
      const found = [];
      for (const side of [-1, 1]) {
        for (const before of [true, false]) {
          // (landing a little inside the arterial's kerb: on it, not on a side street meeting it there)
          const sGround = kerbArc(e, c, side * mid, art.hc - 6, before);
          if (sGround === null) continue;
          const land = offsetAt(e, sGround, side * mid);
          const zGround = Math.round(w.streetLevel ? w.streetLevel(land.x, land.y) : w.terrain.sample(land.x, land.y).h);
          // the drop under the deck at its landing, closed over its length as the grade allows (the deck's own grade included)
          const drop = pointAt(e, sGround).z - zGround;
          let r = null;
          for (let len = Math.max(vx(40), (Math.abs(drop) / RAMP_GRADE) * (1 / (1 - RAMP_EASE))); len <= RAMP_MAX; len *= 1.1) {
            const sDeck = before ? sGround - len : sGround + len;
            const q = { side, sDeck, sGround, drop, zDeck: Math.round(pointAt(e, Math.max(0, Math.min(e.total, sDeck))).z), zGround, cross: c.s, x: land.x, y: land.y, arterial: art.id };
            if (sDeck >= s0 && sDeck <= s1 && rampSteepest(e, q) <= RAMP_GRADE) {
              r = q;
              break;
            }
          }
          if (!r) continue;
          if (this.rampBlocked(e, r)) continue;
          found.push(r);
        }
      }
      if (!found.length) continue;
      last = c.s;
      out.push(...found);
    }
    e.ramps = out;
    return out;
  }

  /** Would a ramp pass over another street too low? */
  rampBlocked(e, r) {
    const w = this.world;
    const mid = r.side * this.rampMid;
    const s0 = Math.min(r.sDeck, r.sGround);
    const s1 = Math.max(r.sDeck, r.sGround);
    for (let s = s0; s <= s1; s += 8) {
      const p = offsetAt(e, s, mid);
      const z = rampZ(e, r, s);
      const c = w.cellAt(p.x, p.y);
      const view = w.roadView(c.i, c.j);
      sampleRoadSurface(view.near({ x0: p.x - 2, y0: p.y - 2, x1: p.x + 2, y1: p.y + 2 }), p.x + 0.5, p.y + 0.5, this.rs, w.seed);
      if (this.rs.kind === KIND.NONE || !this.rs.seg || this.rs.seg.road.id === r.arterial) continue;
      const level = segmentLevel(w, this.rs.seg, Math.max(0, Math.min(this.rs.seg.len, this.rs.along)));
      const carriage = this.rs.kind === KIND.CARRIAGE || this.rs.kind === KIND.MEDIAN;
      if (z - level < (carriage ? RAMP_CLEAR : WALK_CLEAR)) return true;
    }
    return false;
  }

  /** Ramp surface at (s, lateral d) or null: { z, ramp, t, outer, inner } */
  rampAt(e, sPos, d) {
    const rampIn = this.rampIn;
    const rampOut = rampIn + vx(7);
    const ad = Math.abs(d);
    if (ad < rampIn - 2 || ad > rampOut + 1) return null;
    for (const r of this.ramps(e)) {
      if (Math.sign(d) !== r.side) continue;
      // (from its deck end to its ground end, and an apron on into the arterial)
      const dir = Math.sign(r.sGround - r.sDeck);
      const a = (sPos - r.sDeck) * dir;
      const span = Math.abs(r.sGround - r.sDeck);
      if (a < -vx(2) || a > span + vx(2.25)) continue;
      const tRaw = (sPos - r.sGround) / (r.sDeck - r.sGround);
      return { z: Math.round(rampZ(e, r, sPos)), ramp: r, t: tRaw, outer: ad > rampOut - 3, inner: ad < rampIn + 1 };
    }
    return null;
  }

  /** Is (x, y) under a deck or a ramp, or within `margin` voxels of one (a tree's crown, a prop)? */
  covers(x, y, margin = 0) {
    const outer = this.rampIn + vx(7);
    const n = this.nearest(x, y, this.edgesNear({ x0: x - 1, y0: y - 1, x1: x + 1, y1: y + 1 }), outer + margin);
    if (!n) return false;
    const ad = Math.abs(n.d);
    if (ad <= this.hw + margin) return true;
    // (a ramp's band, widened by the margin across; along it to its ends and aprons)
    return !!this.rampAt(n.edge, n.s, Math.sign(n.d) * Math.max(this.rampIn, Math.min(outer, ad)));
  }

  /**
   * The lowest level (voxels) of a deck or ramp over column (x, y), or
   * null where none is: what stands there must stay under it (a ramp near
   * the ground fills to the ground: -Infinity).
   */
  underside(x, y) {
    const n = this.nearest(x + 0.5, y + 0.5, this.edgesNear({ x0: x - 1, y0: y - 1, x1: x + 1, y1: y + 1 }), this.rampIn + vx(7) + 1);
    if (!n) return null;
    const ad = Math.abs(n.d);
    if (ad <= this.hw) return Math.round(n.z) - DECK_T - 1;
    const rp = this.rampAt(n.edge, n.s, n.d);
    if (!rp) return null;
    const gz = this.world.terrain.sample(x, y).h;
    return rp.z - gz < 10 ? -Infinity : rp.z - 9;
  }

  /** Is (x, y) on a street's carriageway or its kerb (where no pier may stand)? */
  onRoad(x, y) {
    const w = this.world;
    const c = w.cellAt(x, y);
    const cands = w.roadView(c.i, c.j).near({ x0: x - 12, y0: y - 12, x1: x + 12, y1: y + 12 });
    sampleRoadSurface(cands, x + 0.5, y + 0.5, this.rs, w.seed);
    return this.rs.kind === KIND.CARRIAGE || this.rs.kind === KIND.MEDIAN || (this.rs.kind !== KIND.NONE && this.rs.sdfC < 10);
  }

  /**
   * The piers of an edge: one every pierSpacing, never on a street's
   * carriageway and never left out. A station over a street moves along
   * the deck (up to 12 m); a street under it all along gets a portal, a
   * column on either side of it under one cap. A junction plateau stands
   * on a column at its node and a ring between its arms.
   * pier: { s, x, y, tx, ty, z, cols: [lateral offsets of its columns (voxels, left of the deck's direction)], cap }
   */
  piers(e) {
    if (e.piers) return e.piers;
    const sp = vx(this.cfg.pierSpacing);
    const H = this.hw;
    const out = [];
    const plateau = (s) => e.junctions.some((j) => j.degree >= 3 && Math.hypot(pointAt(e, s).x - j.x, pointAt(e, s).y - j.y) < j.r);
    for (let s = sp / 2; s < e.total - sp / 4; s += sp) {
      if (plateau(s)) continue;
      let pier = null;
      for (const off of [0, 4, -4, 8, -8, 12, -12]) {
        const ss = s + vx(off);
        if (ss < 0 || ss > e.total) continue;
        const p = pointAt(e, ss);
        if (this.onRoad(p.x, p.y)) continue;
        pier = { s: ss, x: p.x, y: p.y, tx: p.tx, ty: p.ty, z: p.z, cols: [0], cap: H - 8 };
        break;
      }
      if (!pier) {
        // a street under the deck all along here: a portal astride it
        const p = pointAt(e, s);
        const cols = [-1, 1].map((side) => {
          for (let q = 8; q <= H + 24; q += 4) if (!this.onRoad(p.x - p.ty * q * side, p.y + p.tx * q * side)) return q * side;
          return side * (H - 6);
        });
        pier = { s, x: p.x, y: p.y, tx: p.tx, ty: p.ty, z: p.z, cols, cap: Math.max(H - 8, ...cols.map((d) => Math.abs(d) + 8)) };
      }
      out.push(pier);
    }
    // a junction plateau: a column at its node, and a ring between the arms (under the deck, off the streets)
    for (const j of e.junctions) {
      if (j.degree < 3) continue;
      const edges = this.edgesNear({ x0: j.x - j.r, y0: j.y - j.r, x1: j.x + j.r, y1: j.y + j.r });
      const spots = [[j.x, j.y]];
      for (let k = 0; k < 12; k += 1) for (const f of [0.45, 0.85]) spots.push([j.x + Math.cos((k * Math.PI) / 6) * j.r * f, j.y + Math.sin((k * Math.PI) / 6) * j.r * f]);
      for (const [x, y] of spots) {
        const n = this.nearest(x, y, edges, H);
        if (!n || Math.abs(n.d) > H - 10 || this.onRoad(x, y)) continue;
        out.push({ s: n.s, x, y, tx: 1, ty: 0, z: j.z, cols: [0], cap: 12 });
      }
    }
    e.piers = out;
    return out;
  }

  /** A ramp's pier stations (arcs): every 24 m along it, each moved off any street under it (up to 8 m). */
  rampPiers(e, r) {
    r.piers ??= (() => {
      const out = [];
      const s0 = Math.min(r.sDeck, r.sGround);
      const s1 = Math.max(r.sDeck, r.sGround);
      const off = r.side * this.rampMid;
      for (let s = s0 + vx(8); s < s1; s += vx(24))
        for (const d of [0, 2, -2, 4, -4, 6, -6, 8, -8]) {
          const ss = s + vx(d);
          if (ss < s0 || ss > s1) continue;
          const p = offsetAt(e, ss, off);
          if (this.onRoad(p.x, p.y)) continue;
          out.push(ss);
          break;
        }
      return out;
    })();
    return r.piers;
  }

  mapData(rect) {
    return this.edgesNear(rect).map((e) => ({ id: e.id, pts: e.pts.map((p) => [p.x, p.y]), width: this.hw * 2 }));
  }
}

/** The deck's centre at arc s: { x, y, z, tx, ty } (unit tangent). */
export function pointAt(e, s) {
  const L = e.lengths;
  let lo = 0;
  let hi = L.length - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (L[mid] <= s) lo = mid;
    else hi = mid;
  }
  const a = e.pts[lo];
  const b = e.pts[hi];
  const t = (s - L[lo]) / (L[hi] - L[lo] || 1);
  const sl = L[hi] - L[lo] || 1;
  return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t, z: a.z + (b.z - a.z) * t, tx: (b.x - a.x) / sl, ty: (b.y - a.y) / sl };
}

/** The point at arc s offset `off` voxels to the left of the deck's direction. */
export function offsetAt(e, s, off) {
  const p = pointAt(e, s);
  return { x: p.x - p.ty * off, y: p.y + p.tx * off, z: p.z };
}

/**
 * The arc where a ramp's centre line (offset `off` from the deck) meets the
 * arterial crossing at c (an axis-aligned lattice line, half-width hc): as
 * it enters the arterial (before the crossing) or leaves it (after), or null.
 */
function kerbArc(e, c, off, hc, before) {
  const inside = (s) => {
    const p = offsetAt(e, s, off);
    return Math.abs((c.axis === 0 ? p.x : p.y) - c.line) <= hc;
  };
  const reach = vx(120);
  let first = null;
  let last = null;
  for (let s = Math.max(0, c.s - reach); s <= Math.min(e.total, c.s + reach); s += 2)
    if (inside(s)) {
      if (first === null) first = s;
      last = s;
    }
  if (first === null) return null;
  return before ? first : last;
}

/** The arterial street at a crossing point: { id, hc } or null. */
function arterialAt(w, x, y, rs) {
  const c = w.cellAt(x, y);
  sampleRoadSurface(w.roadView(c.i, c.j).near({ x0: x - 2, y0: y - 2, x1: x + 2, y1: y + 2 }), x + 0.5, y + 0.5, rs, w.seed);
  const seg = rs.seg;
  if (!seg || rs.kind === KIND.NONE || seg.road.cls !== "arterial") return null;
  return { id: seg.road.id, hc: seg.hc };
}

/**
 * A ramp's level at arc s: the deck's level less a drop, the drop at its
 * landing (to the street's level) closing to none at its deck end, eased
 * over RAMP_EASE of its length at each end: it lands level with the street
 * and meets the deck tangent to it.
 */
export function rampZ(e, r, s) {
  const t = Math.max(0, Math.min(1, (s - r.sGround) / (r.sDeck - r.sGround)));
  const k = RAMP_EASE;
  const m = 1 / (1 - k);
  const f = t < k ? (m * t * t) / (2 * k) : t <= 1 - k ? m * (t - k / 2) : 1 - (m * (1 - t) * (1 - t)) / (2 * k);
  return pointAt(e, Math.max(0, Math.min(e.total, s))).z - r.drop * (1 - f);
}

/** A ramp's steepest grade (every 4 voxels along it). */
function rampSteepest(e, r) {
  const n = Math.max(2, Math.ceil(Math.abs(r.sDeck - r.sGround) / 4));
  let worst = 0;
  let prev = rampZ(e, r, r.sGround);
  for (let k = 1; k <= n; k += 1) {
    const s = r.sGround + ((r.sDeck - r.sGround) * k) / n;
    const z = rampZ(e, r, s);
    worst = Math.max(worst, Math.abs(z - prev) / (Math.abs(r.sDeck - r.sGround) / n));
    prev = z;
  }
  return worst;
}

function segRectDist(s, r) {
  // distance from segment to rect (0 if intersecting)
  const inside = (x, y) => x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
  if (inside(s.ax, s.ay) || inside(s.bx, s.by)) return 0;
  let best = Infinity;
  const pts = [
    [r.x0, r.y0],
    [r.x1, r.y0],
    [r.x0, r.y1],
    [r.x1, r.y1],
  ];
  for (const [x, y] of pts) {
    const t = Math.max(0, Math.min(s.len, (x - s.ax) * s.dx + (y - s.ay) * s.dy));
    best = Math.min(best, Math.hypot(x - (s.ax + s.dx * t), y - (s.ay + s.dy * t)));
  }
  for (const [x, y] of [
    [s.ax, s.ay],
    [s.bx, s.by],
  ]) {
    const cx = Math.max(r.x0, Math.min(r.x1, x));
    const cy = Math.max(r.y0, Math.min(r.y1, y));
    best = Math.min(best, Math.hypot(x - cx, y - cy));
  }
  // crossing test: segment intersects rect edges
  const steps = Math.ceil(s.len / 16);
  for (let k = 0; k <= steps; k += 1) {
    const t = (k / steps) * s.len;
    if (inside(s.ax + s.dx * t, s.ay + s.dy * t)) return 0;
  }
  return best;
}

// ------------------------------------------------------------ rasterizer

const DECK_T = 10; // slab thickness (voxels)

export const highwaySource = {
  id: "highways",
  order: 5,
  maxLod: 7,
  zRange(world, rect) {
    const hw = world.highways;
    const edges = hw.edgesNear(rect);
    let lo = Infinity;
    let hi = -Infinity;
    for (const e of edges) {
      for (const s of e.grid.query(rect)) {
        lo = Math.min(lo, s.az - 200, s.bz - 200);
        hi = Math.max(hi, s.az + TUNNEL_H + 6, s.bz + TUNNEL_H + 6);
      }
    }
    if (lo === Infinity) return null;
    return [Math.max(lo, -200), hi];
  },
  rasterize(world, chunk, tile) {
    const hw = world.highways;
    const box = chunk.worldBox;
    const edges = hw.edgesNear(box);
    if (!edges.length) return;
    const H = hw.hw;
    const d = chunk.data;
    const coarse = chunk.lod >= 2;
    const lanesPerSide = world.config.highways.lanesPerSide;
    const lane = vx(world.config.highways.laneWidth);
    const shoulder = vx(world.config.highways.shoulder);
    for (let j = 0; j < P; j += 1) {
      const y = chunk.wy(j);
      for (let i = 0; i < P; i += 1) {
        const x = chunk.wx(i);
        const n = hw.nearest(x + 0.5, y + 0.5, edges, H + 4 + 60);
        if (!n) continue;
        const ad = Math.abs(n.d);
        const col = i + j * P;
        const gz = tile ? tile.z[col] : Math.round(world.terrain.sample(x, y).h);
        if (ad > H) {
          const rp = hw.rampAt(n.edge, n.s, n.d);
          if (rp) drawRamp(chunk, d, i, j, rp, gz);
          else earthworks(chunk, d, i, j, x, y, ad - H, Math.round(n.z), gz, world);
          continue;
        }
        const zTop = Math.round(n.z);
        // through a hill: open cutting, or a tunnel under a mountain (bores
        // are invisible from afar: coarse LODs leave the mountain whole)
        if (gz > zTop + TUNNEL_COVER) {
          if (chunk.lod <= 2) tunnelColumn(chunk, d, i, j, ad, H, zTop, n.s);
          continue;
        }
        if (gz > zTop) {
          const [c0, c1] = chunk.rangeZ(zTop + 1, gz);
          for (let k = c0; k <= c1; k += 1) d[i + j * P + k * P2] = 0;
        } else if (zTop - DECK_T - gz < EMBANK && zTop - DECK_T > gz && !wet(world, x, y)) {
          // low deck: an earth embankment instead of air under the slab
          const [c0, c1] = chunk.rangeZ(gz + 1, zTop - DECK_T - 1);
          for (let k = c0; k <= c1; k += 1) d[i + j * P + k * P2] = MAT.GRAVEL;
        }
        // a junction plateau (a crossing of highways, a terminus): one level deck, no barriers across it,
        // a railing round it (where the edge of this arm is no other arm's carriageway; at grade, none)
        const jn = edges.flatMap((e) => e.junctions).find((q) => Math.hypot(x + 0.5 - q.x, y + 0.5 - q.y) < q.r);
        const junction = !!jn;
        let rim = false;
        if (jn && jn.degree >= 3 && ad > H - 3) {
          const other = hw.nearest(x + 0.5, y + 0.5, edges.filter((e) => e !== n.edge), H);
          rim = !other || Math.abs(other.d) > H - 3;
        }
        // barrier opening where a ramp merges at deck level
        let merge = junction;
        if (ad > H - 3) {
          const rp = hw.rampAt(n.edge, n.s, Math.sign(n.d) * (H + 5));
          if (rp && rp.t > 1 - RAMP_EASE && Math.abs(rp.z - zTop) <= 1) merge = true;
        }
        const [k0, k1] = chunk.rangeZ(zTop - DECK_T, zTop + 8);
        for (let k = k0; k <= k1; k += 1) {
          const z = chunk.wz(k);
          let m = 0;
          // surface = the top representative voxel at this LOD (thin layers must survive point sampling)
          const surf = z > zTop - chunk.s && z <= zTop;
          if (z < zTop && !surf) m = z === zTop - DECK_T ? MAT.CONCRETE_DARK : MAT.HW_CONCRETE;
          else if (surf) {
            if (ad > H - 3 && !junction) m = MAT.HW_CONCRETE;
            else m = coarse || junction ? MAT.ASPHALT : deckMarking(ad, n.s, lanesPerSide, lane, shoulder);
          } else if (ad > H - 3 && ad <= H) m = z <= zTop + 7 && (!merge || rim) ? MAT.HW_BARRIER : 0;
          else if (ad < 2 && z <= zTop + 7 && !junction) m = MAT.HW_BARRIER;
          if (m || (merge && !rim && z > zTop)) d[i + j * P + k * P2] = m;
        }
      }
    }
    // piers
    for (const e of edges) {
      for (const p of hw.piers(e)) {
        const reach = p.cap + 16;
        if (p.x < box.x0 - reach || p.x > box.x1 + reach || p.y < box.y0 - reach || p.y > box.y1 + reach) continue;
        const zTop = Math.round(p.z);
        const nx = -p.ty;
        const ny = p.tx;
        // columns: 2.0 m across the deck, 1.25 m along, down into the ground
        for (const d of p.cols) {
          const cx = p.x + nx * d;
          const cy = p.y + ny * d;
          const gz = Math.round(world.terrain.sample(cx, cy).h) - 4;
          stampOriented(chunk, cx, cy, p.tx, p.ty, nx, ny, -5, 5, -8, 8, gz, zTop - DECK_T - 1, MAT.HW_CONCRETE);
        }
        // the cap: a hammerhead under the deck (a portal's beam astride the street)
        const lo = Math.min(-p.cap, ...p.cols.map((d) => d - 8));
        const hi = Math.max(p.cap, ...p.cols.map((d) => d + 8));
        stampOriented(chunk, p.x, p.y, p.tx, p.ty, nx, ny, -6, 6, lo, hi, zTop - DECK_T - 7, zTop - DECK_T - 1, MAT.HW_CONCRETE);
      }
      // ramp piers where the ramp is well above ground
      for (const r of hw.ramps(e)) {
        for (const sp of hw.rampPiers(e, r)) {
          const p = pointAt(e, sp);
          const off = r.side * hw.rampMid;
          const px = p.x - p.ty * off;
          const py = p.y + p.tx * off;
          if (px < box.x0 - 40 || px > box.x1 + 40 || py < box.y0 - 40 || py > box.y1 + 40) continue;
          const rp = hw.rampAt(e, sp, off);
          if (!rp) continue;
          const gz = Math.round(world.terrain.sample(px, py).h);
          if (rp.z - gz < 20) continue;
          stampOriented(chunk, px, py, p.tx, p.ty, -p.ty, p.tx, -4, 4, -5, 5, gz - 3, rp.z - 9, MAT.HW_CONCRETE);
        }
      }
    }
    void tile;
  },
};

const TUNNEL_COVER = 56; // 7 m of ground over the deck: bore a tunnel
const EMBANK = 44; // decks lower than 5.5 m over the ground sit on earth
const TUNNEL_H = 52;

function wet(world, x, y) {
  return world.isWet ? world.isWet(x, y, 4) : false;
}

/** Tunnel bore: carve the clearance, line the walls and vault, light the ceiling. */
function tunnelColumn(chunk, d, i, j, ad, H, zTop, s) {
  const [k0, k1] = chunk.rangeZ(zTop - DECK_T, zTop + TUNNEL_H + 3);
  const lightRow = ad < 2 && ((Math.floor(s) % 64) + 64) % 64 < 10;
  for (let k = k0; k <= k1; k += 1) {
    const z = chunk.wz(k);
    let m;
    if (z <= zTop - 1) m = MAT.HW_CONCRETE;
    else if (z === zTop) m = ad > H - 3 ? MAT.HW_CONCRETE : ad < 2 ? MAT.LINE_YELLOW : MAT.ASPHALT;
    else if (z > zTop + TUNNEL_H) m = MAT.HW_CONCRETE; // vault
    else if (ad > H - 2) m = z <= zTop + 7 ? MAT.HW_BARRIER : MAT.TUNNEL_TILE; // lined walls
    else if (z === zTop + TUNNEL_H && lightRow) m = MAT.LIGHT_STRIP;
    else m = 0;
    d[i + j * P + k * P2] = m;
  }
}

/**
 * Beside the carriageway: retaining walls along cuttings (and at tunnel
 * portals), grassy slopes down from embankments.
 */
function earthworks(chunk, d, i, j, x, y, out, zTop, gz, world) {
  if (gz > zTop + 1) {
    // cutting: a retaining wall two voxels thick, the ground beyond untouched
    if (out > 2) return;
    const top = Math.min(gz, zTop + TUNNEL_COVER + 8);
    const [k0, k1] = chunk.rangeZ(zTop - 2, top);
    for (let k = k0; k <= k1; k += 1) d[i + j * P + k * P2] = chunk.wz(k) === top ? MAT.PARAPET_CAP : MAT.CONCRETE_LIGHT;
    return;
  }
  const h = zTop - DECK_T - gz;
  if (h <= 0 || zTop - gz >= EMBANK + DECK_T || wet(world, x, y)) return;
  // embankment slope 1:1.5 from the deck edge down to the ground
  const zs = Math.round(zTop - 2 - out / 1.5);
  if (zs <= gz) return;
  const [k0, k1] = chunk.rangeZ(gz + 1, zs);
  for (let k = k0; k <= k1; k += 1) d[i + j * P + k * P2] = chunk.wz(k) > zs - chunk.s ? MAT.GRASS : MAT.DIRT;
}

function deckMarking(ad, s, lanesPerSide, lane, shoulder) {
  const inner = 3; // median barrier half-width + gap
  if (ad < inner) return MAT.ASPHALT;
  const x = ad - inner;
  const edgeOut = lanesPerSide * lane;
  if (x >= edgeOut && x < edgeOut + 1) return MAT.LINE_WHITE; // outer edge line
  if (x < 1) return MAT.LINE_YELLOW; // inner edge line
  for (let k = 1; k < lanesPerSide; k += 1) {
    const lx = k * lane;
    if (x >= lx && x < lx + 1 && ((Math.floor(s) % 96) + 96) % 96 < 32) return MAT.LINE_WHITE;
  }
  void shoulder;
  return MAT.ASPHALT;
}

/** Fill an oriented box (along tangent t, across n) by point sampling. */
function stampOriented(chunk, cx, cy, tx, ty, nx, ny, a0, a1, b0, b1, z0, z1, m) {
  const r = Math.max(Math.abs(a0), Math.abs(a1), Math.abs(b0), Math.abs(b1)) + 2;
  const [i0, i1] = chunk.rangeX(cx - r, cx + r);
  const [j0, j1] = chunk.rangeY(cy - r, cy + r);
  const [k0, k1] = chunk.rangeZ(z0, z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const d = chunk.data;
  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j) + 0.5 - cy;
    for (let i = i0; i <= i1; i += 1) {
      const x = chunk.wx(i) + 0.5 - cx;
      const a = x * tx + y * ty;
      const b = x * nx + y * ny;
      if (a < a0 || a > a1 || b < b0 || b > b1) continue;
      for (let k = k0; k <= k1; k += 1) d[i + j * P + k * P2] = m;
    }
  }
}

export { hash32 };

/**
 * One column of a ramp at its own level: a slab (an embankment near the
 * ground), barriers and edge lines; through ground above it a cutting, a
 * retaining wall along its outer edge. `gz` the ground's top there.
 */
function drawRamp(chunk, d, i, j, rp, gz) {
  const zTop = rp.z;
  const cut = gz > zTop;
  const low = zTop - gz < 10;
  const z0 = low ? Math.min(gz, zTop) - 2 : zTop - 8;
  const z1 = cut ? Math.max(gz, zTop + 8) : zTop + 8;
  const [k0, k1] = chunk.rangeZ(z0, z1);
  for (let k = k0; k <= k1; k += 1) {
    const z = chunk.wz(k);
    let m = 0;
    const surf = z > zTop - chunk.s && z <= zTop;
    if (z < zTop && !surf) m = low ? MAT.GRAVEL : MAT.HW_CONCRETE;
    else if (surf) m = rp.outer || rp.inner ? MAT.LINE_WHITE : MAT.ASPHALT;
    else if (cut) m = rp.outer && z <= gz ? MAT.CONCRETE_LIGHT : 0;
    else if (rp.outer && rp.t > 0.05 && z <= zTop + 7 && zTop - gz > 3) m = MAT.HW_BARRIER;
    else if (rp.inner && rp.t > 0.05 && rp.t < 1 - RAMP_EASE && z <= zTop + 7 && zTop - gz > 3) m = MAT.HW_BARRIER;
    if (m || z > zTop) d[i + j * P + k * P2] = m;
  }
}
