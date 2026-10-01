import { hashFloat, hash32 } from "../core/hash.js";
import { LRU } from "../core/lru.js";
import { MAT, IS_SOLID } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { roadSpecs } from "../network/roadClasses.js";

/**
 * Sewers: a walkable brick network 5 m under the streets of dense districts.
 *
 *   runs      straight vaulted tunnels under street centerlines, with a
 *             central sewage channel and a walkway on each side
 *   chambers  every junction, dead end and at most every 80 m; each has a
 *             ladder shaft up to a manhole (some left open, fenced by cones)
 *   halls     big pillared overflow halls where two collectors cross, with
 *             a stair up to a service opening on the sidewalk
 *
 * Planning is per arterial cell from the roads the cell OWNS (the same
 * ownership rule as street dressing), so the network is a pure function of
 * the cell. Every split point of a run is a node whose invert height is a
 * pure function of its position, which is what makes runs planned by
 * different cells meet exactly. Arterials that carry a subway line have no
 * sewer (the subway tunnel owns that corridor).
 */

const DEPTH = 40; // walkway 5 m below the street surface
const IN_W = 13; // walkway outer edge (inner half width)
const CH_W = 4; // channel half width
const SHELL = 3;
const VAULT = 22; // crown above the walkway at the centerline
const CHAMBER = 22; // junction chamber inner half size
const CHAMBER_H = 26;
const HALL = 64; // hall inner half size
const HALL_H = 30; // hall ceiling above the ledge (1.25 m below the street)
const LEDGE = 10; // walkway ring at run level along the hall walls
const BASIN = 24; // the basin floor lies 3 m below the ledge
const MAX_RUN = 640; // chambers at least every 80 m
const MIN_U = 0.3;
const SHAFT = 4; // ladder shaft clear half size (8 x 8 voxels)
const CLASSES = new Set(["arterial", "collector", "local"]);

const EMPTY = { runs: [], nodes: [], boxes: [], openings: [], bb: null };

export class Sewers {
  constructor(world) {
    this.world = world;
    this.plans = new LRU(48);
    this.specs = roadSpecs(world.config);
  }

  eligible(road, i, j) {
    if (!CLASSES.has(road.cls) || road.pts.length !== 2) return false;
    const [a, b] = road.pts;
    if (a.x !== b.x && a.y !== b.y) return false;
    if (road.arterialEdge) {
      const axis = road.arterialEdge === "W" ? 0 : 1;
      const line = axis === 0 ? i : j;
      if (this.world.subway?.lineExists(axis, line)) return false;
    }
    return this.world.fields.urban((a.x + b.x) / 2, (a.y + b.y) / 2).u >= MIN_U;
  }

  cellPlan(i, j) {
    return this.plans.getOrCreate(`${i},${j}`, () => this.planCell(i, j));
  }

  planCell(i, j) {
    const world = this.world;
    const net = world.cellNet(i, j);
    const mine = net.roads.filter((r) => this.eligible(r, i, j));
    if (!mine.length) return EMPTY;
    const others = [];
    for (let dj = -1; dj <= 1; dj += 1)
      for (let di = -1; di <= 1; di += 1) for (const r of world.cellNet(i + di, j + dj).roads) if (this.eligible(r, i + di, j + dj)) others.push(r);

    const nodes = new Map();
    const node = (x, y) => {
      const key = `${x},${y}`;
      let n = nodes.get(key);
      if (!n) {
        const zr = Math.round(world.streetLevel(x, y));
        n = { key, x, y, z: zr - DEPTH, zr, arms: new Set(), hall: false };
        nodes.set(key, n);
      }
      return n;
    };
    const runs = [];
    for (const r of mine) {
      const [a, b] = r.pts;
      const axis = a.x === b.x ? 0 : 1; // 0: runs north-south (along y)
      const fixed = axis === 0 ? a.x : a.y;
      const l0 = axis === 0 ? Math.min(a.y, b.y) : Math.min(a.x, b.x);
      const l1 = axis === 0 ? Math.max(a.y, b.y) : Math.max(a.x, b.x);
      const cuts = new Set([l0, l1]);
      for (const o of others) {
        if (o === r) continue;
        const [oa, ob] = o.pts;
        const oaxis = oa.x === ob.x ? 0 : 1;
        if (oaxis === axis) continue;
        const ofixed = oaxis === 0 ? oa.x : oa.y;
        const ol0 = oaxis === 0 ? Math.min(oa.y, ob.y) : Math.min(oa.x, ob.x);
        const ol1 = oaxis === 0 ? Math.max(oa.y, ob.y) : Math.max(oa.x, ob.x);
        if (ofixed >= l0 && ofixed <= l1 && fixed >= ol0 && fixed <= ol1) cuts.add(ofixed);
      }
      const sorted = [...cuts].sort((p, q) => p - q);
      const ls = [sorted[0]];
      for (let k = 1; k < sorted.length; k += 1) {
        const gap = sorted[k] - sorted[k - 1];
        const n = Math.ceil(gap / MAX_RUN);
        for (let m = 1; m < n; m += 1) ls.push(Math.round(sorted[k - 1] + (gap * m) / n));
        ls.push(sorted[k]);
      }
      const at = (l) => (axis === 0 ? node(fixed, l) : node(l, fixed));
      for (let k = 0; k < ls.length - 1; k += 1) {
        const na = at(ls[k]);
        const nb = at(ls[k + 1]);
        runs.push({ axis, fixed, l0: ls[k], l1: ls[k + 1], z0: na.z, z1: nb.z, na, nb, cls: r.cls });
      }
    }

    // overflow hall where the cell's two collectors cross
    const cols = net.roads.filter((r) => r.cls === "collector" && this.eligible(r, i, j));
    const vx = cols.find((r) => r.pts[0].x === r.pts[1].x);
    const hy = cols.find((r) => r.pts[0].y === r.pts[1].y);
    const hallNode = vx && hy ? nodes.get(`${vx.pts[0].x},${hy.pts[0].y}`) : null;
    if (hallNode) hallNode.hall = true;

    // keep clear of subway stations: blocked chambers and the runs that
    // reach them or cross station volumes are dropped (pure functions of
    // position, so every cell drops the same pieces) ...
    // ... and of river channels (a pure test as well)
    const rivers = world.rivers;
    const wet = (b) => rivers && rivers.hitsRect(b, 4);
    for (const n of nodes.values()) n.blocked = this.hitsSubway(chamberBox(n)) || wet(chamberBox(n));
    const kept = runs.filter((r) => !r.na.blocked && !r.nb.blocked && !this.hitsSubway(runBox(r, r.l0, r.l1)) && !wet(runBox(r, r.l0, r.l1)));
    for (const r of kept) {
      r.na.arms.add(r.axis === 0 ? "S" : "E");
      r.nb.arms.add(r.axis === 0 ? "N" : "W");
      delete r.na;
      delete r.nb;
    }
    runs.length = 0;
    runs.push(...kept);
    // arms of runs owned by neighbours that end on one of our nodes
    const armClear = (n, dir) => {
      const e = CHAMBER + 48;
      const axis = dir === "N" || dir === "S" ? 0 : 1;
      const sgn = dir === "S" || dir === "E" ? 1 : -1;
      const l = axis === 0 ? n.y : n.x;
      const r = { axis, fixed: axis === 0 ? n.x : n.y, z0: n.z, z1: n.z };
      return !this.hitsSubway(runBox(r, Math.min(l, l + sgn * e), Math.max(l, l + sgn * e)));
    };
    for (const o of others) {
      const [oa, ob] = o.pts;
      for (const n of nodes.values()) {
        if (n.blocked) continue;
        const add = (dir) => {
          if (!n.arms.has(dir) && armClear(n, dir)) n.arms.add(dir);
        };
        if (oa.x === ob.x && oa.x === n.x && n.y >= Math.min(oa.y, ob.y) && n.y <= Math.max(oa.y, ob.y)) {
          if (n.y > Math.min(oa.y, ob.y)) add("N");
          if (n.y < Math.max(oa.y, ob.y)) add("S");
        } else if (oa.y === ob.y && oa.y === n.y && n.x >= Math.min(oa.x, ob.x) && n.x <= Math.max(oa.x, ob.x)) {
          if (n.x > Math.min(oa.x, ob.x)) add("W");
          if (n.x < Math.max(oa.x, ob.x)) add("E");
        }
      }
    }
    for (const [key, n] of nodes) if (n.blocked || n.arms.size === 0) nodes.delete(key);
    if (hallNode && !nodes.has(hallNode.key)) hallNode.hall = false;

    const boxes = [];
    const openings = [];
    if (hallNode?.hall) this.hallStair(hallNode, boxes, openings);

    const list = [...nodes.values()];
    for (const n of list) {
      const h = hash32(world.seed, n.x, n.y, 4242);
      n.qx = h & 1 ? 1 : -1;
      n.qy = h & 2 ? 1 : -1;
      // staggered junctions closer than two chambers share one ladder shaft:
      // the junction with the smaller key keeps it. Junctions are
      // intersections of eligible roads, which every cell that knows this
      // node can enumerate identically from its 3x3 neighbourhood.
      n.shaft = !n.hall && !junctionsNear(others, n).some((p) => p.x < n.x || (p.x === n.x && p.y < n.y));
      n.open = n.shaft && hashFloat(world.seed, n.x, n.y, 4243) < 0.35;
      n.R = n.hall ? HALL : CHAMBER;
      n.H = n.hall ? HALL_H : CHAMBER_H;
    }
    let bb = null;
    const grow = (x0, y0, z0, x1, y1, z1) => {
      bb = bb
        ? { x0: Math.min(bb.x0, x0), y0: Math.min(bb.y0, y0), z0: Math.min(bb.z0, z0), x1: Math.max(bb.x1, x1), y1: Math.max(bb.y1, y1), z1: Math.max(bb.z1, z1) }
        : { x0, y0, z0, x1, y1, z1 };
    };
    const W = IN_W + SHELL;
    for (const r of runs) {
      const zl = Math.min(r.z0, r.z1) - 6;
      const zh = Math.max(r.z0, r.z1) + DEPTH + 4;
      if (r.axis === 0) grow(r.fixed - W, r.l0, zl, r.fixed + W, r.l1, zh);
      else grow(r.l0, r.fixed - W, zl, r.l1, r.fixed + W, zh);
    }
    for (const n of list) {
      const e = n.R + SHELL + 12;
      grow(n.x - e, n.y - e, n.z - (n.hall ? BASIN : 0) - 6, n.x + e, n.y + e, n.zr + 6);
    }
    for (const q of boxes) grow(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1);
    return { runs, nodes: list, boxes, openings, bb };
  }

  /**
   * Stair from a hall's ledge up to the sidewalk of the north-south
   * collector, rising away from the crossing. Boxes use the subway conventions
   * (mode 2 = only replace solid ground).
   */
  hallStair(n, boxes, openings) {
    const world = this.world;
    const h = hash32(world.seed, n.x, n.y, 4244);
    const side = h & 1 ? 1 : -1;
    const dir = h & 2 ? 1 : -1;
    // on the hall's ledge, which lies under the collector's sidewalk
    const c0 = HALL - LEDGE + 1;
    const c1 = HALL - 1;
    const lStart = 36;
    const W = (a0, a1, b0, b1, z0, z1, m, mode = 0) => {
      const ca = side * a0;
      const cb = side * a1;
      const la = dir * b0;
      const lb = dir * b1;
      boxes.push({ x0: n.x + Math.min(ca, cb), x1: n.x + Math.max(ca, cb), y0: n.y + Math.min(la, lb), y1: n.y + Math.max(la, lb), z0, z1, m, mode });
    };
    const zLow = n.z;
    // the stair tops out on the sidewalk surface next to its far end
    const probeL = lStart + 2 * (n.zr + 1 - zLow);
    const zHigh = Math.round(world.streetLevel(n.x + side * (c0 + c1) * 0.5, n.y + dir * probeL)) + 1;
    const R = zHigh - zLow;
    const HEAD = 20;
    for (let k = 1; k <= R; k += 1) {
      const l = lStart + 2 * (k - 1);
      const open = zLow + k + HEAD > zHigh - 1;
      W(c0 - 2, c1 + 2, l, l + 1, zLow + k - 3, Math.min(zHigh - 1, zLow + k + HEAD + 2), MAT.SEWER_BRICK, 2);
      W(c0, c1, l, l + 1, zLow + 1, zLow + k, MAT.STAIR_CONCRETE);
      W(c0, c1, l, l + 1, zLow + k + 1, zLow + k + HEAD, 0);
      W(c0, c0, l, l + 1, zLow + k + 7, zLow + k + 7, MAT.RAILING);
      if (open) {
        W(c0 - 2, c1 + 2, l, l + 1, zHigh + 1, zHigh + HEAD + 4, 0);
        for (const cc of [c0 - 1, c1 + 1]) {
          W(cc, cc, l, l + 1, zHigh, zHigh, MAT.CONCRETE_DARK);
          W(cc, cc, l, l + 1, zHigh + 8, zHigh + 8, MAT.POLE_METAL);
          if ((l & 3) === 0) W(cc, cc, l, l, zHigh + 1, zHigh + 7, MAT.POLE_METAL);
        }
      }
    }
    const lEnd = lStart + 2 * R;
    W(c0, c1, lEnd, lEnd + 3, zHigh - 1, zHigh, MAT.SIDEWALK);
    const lb = lStart + 2 * Math.max(0, R - HEAD - 1);
    W(c0 - 1, c1 + 1, lb - 1, lb - 1, zHigh + 8, zHigh + 8, MAT.POLE_METAL);
    for (let c = c0; c <= c1; c += 3) W(c, c, lb - 1, lb - 1, zHigh + 1, zHigh + 7, MAT.POLE_METAL);
    // service sign and a caged lamp at the top
    W(c1 + 2, c1 + 2, lEnd, lEnd, zHigh + 1, zHigh + 20, MAT.POLE_METAL);
    W(c1 + 1, c1 + 3, lEnd, lEnd, zHigh + 15, zHigh + 19, MAT.HAZARD_YELLOW);
    W(c1 + 2, c1 + 2, lEnd - 1, lEnd + 1, zHigh + 21, zHigh + 22, MAT.LAMP_CAGE);
    const pa = { x: n.x + side * c0, y: n.y + dir * (lStart + 2 * (R - HEAD - 1)) };
    const pb = { x: n.x + side * c1, y: n.y + dir * (lEnd + 3) };
    openings.push({
      x0: Math.min(pa.x, pb.x) - 4,
      y0: Math.min(pa.y, pb.y) - 4,
      x1: Math.max(pa.x, pb.x) + 4,
      y1: Math.max(pa.y, pb.y) + 4,
      top: { x: n.x + side * Math.round((c0 + c1) / 2), y: n.y + dir * (lEnd + 1), z: zHigh + 1 },
    });
    n.stairTop = openings[openings.length - 1].top;
  }

  /** Does a box (world voxels, with z) touch any subway station volume? */
  hitsSubway(b) {
    const sw = this.world.subway;
    if (!sw) return false;
    for (const s of sw.stationsNear(b)) {
      if (!overlap3(s.bb, b)) continue;
      for (const q of s.boxes) if (overlap3(q, b)) return true;
    }
    return false;
  }

  /** Sewer plans of every cell that may reach into `rect`. */
  near(rect) {
    const pad = HALL + 160;
    const out = [];
    for (const { i, j } of this.world.cellsOverlapping({ x0: rect.x0 - pad, y0: rect.y0 - pad, x1: rect.x1 + pad, y1: rect.y1 + pad })) {
      const p = this.cellPlan(i, j);
      if (p.bb && p.bb.x0 <= rect.x1 && p.bb.x1 >= rect.x0 && p.bb.y0 <= rect.y1 && p.bb.y1 >= rect.y0) out.push(p);
    }
    return out;
  }

  /** Street props should keep clear of stair openings and open manholes. */
  blocksSurface(x, y) {
    for (const p of this.near({ x0: x, y0: y, x1: x, y1: y })) {
      for (const o of p.openings) if (x >= o.x0 && x <= o.x1 && y >= o.y0 && y <= o.y1) return true;
      for (const n of p.nodes) {
        if (!n.open) continue;
        const sx = n.x + n.qx * 12;
        const sy = n.y + n.qy * 12;
        if (Math.abs(x - sx) < 20 && Math.abs(y - sy) < 20) return true;
      }
    }
    return false;
  }

  mapData(rect) {
    const lines = [];
    const halls = [];
    const seen = new Set();
    for (const p of this.near(rect)) {
      for (const r of p.runs) {
        const key = `${r.axis},${r.fixed},${r.l0}`;
        if (seen.has(key)) continue;
        seen.add(key);
        lines.push(r.axis === 0 ? [[r.fixed, r.l0], [r.fixed, r.l1]] : [[r.l0, r.fixed], [r.l1, r.fixed]]);
      }
      for (const n of p.nodes) if (n.hall) halls.push({ x: n.x, y: n.y, top: n.stairTop });
    }
    return { lines, halls };
  }

  /** Nearest hall (with its stair top) to a point, for points of interest. */
  nearestHall(x, y, reach = 8000) {
    let best = null;
    let bd = Infinity;
    for (const p of this.near({ x0: x - reach, y0: y - reach, x1: x + reach, y1: y + reach })) {
      for (const n of p.nodes) {
        if (!n.hall) continue;
        const d = Math.hypot(n.x - x, n.y - y);
        if (d < bd) {
          bd = d;
          best = n;
        }
      }
    }
    return best;
  }
}

function overlap3(a, b) {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1 && a.z0 <= b.z1 && b.z0 <= a.z1;
}

/** Volume of a node's chamber (hall size unknown here: use the larger one when flagged). */
function chamberBox(n) {
  const e = (n.hall ? HALL : CHAMBER) + SHELL + 2;
  return { x0: n.x - e, y0: n.y - e, x1: n.x + e, y1: n.y + e, z0: n.z - (n.hall ? BASIN : 0) - 6, z1: n.zr };
}

/** Volume of a run between l0 and l1. */
function runBox(r, l0, l1) {
  const w = IN_W + SHELL + 2;
  const z0 = Math.min(r.z0, r.z1) - 6;
  const z1 = Math.max(r.z0, r.z1) + VAULT + SHELL + 2;
  return r.axis === 0 ? { x0: r.fixed - w, x1: r.fixed + w, y0: l0, y1: l1, z0, z1 } : { x0: l0, x1: l1, y0: r.fixed - w, y1: r.fixed + w, z0, z1 };
}

/** Intersections of perpendicular roads within 64 voxels of node n (excluding n). */
function junctionsNear(roads, n) {
  const R = 64;
  const near = roads.filter((r) => {
    const [a, b] = r.pts;
    const x0 = Math.min(a.x, b.x);
    const x1 = Math.max(a.x, b.x);
    const y0 = Math.min(a.y, b.y);
    const y1 = Math.max(a.y, b.y);
    return n.x >= x0 - R && n.x <= x1 + R && n.y >= y0 - R && n.y <= y1 + R;
  });
  const out = [];
  for (const v of near) {
    if (v.pts[0].x !== v.pts[1].x) continue;
    const x = v.pts[0].x;
    for (const hz of near) {
      if (hz.pts[0].y !== hz.pts[1].y) continue;
      const y = hz.pts[0].y;
      if (x === n.x && y === n.y) continue;
      if (Math.abs(x - n.x) >= R || Math.abs(y - n.y) >= R) continue;
      const vy0 = Math.min(v.pts[0].y, v.pts[1].y);
      const vy1 = Math.max(v.pts[0].y, v.pts[1].y);
      const hx0 = Math.min(hz.pts[0].x, hz.pts[1].x);
      const hx1 = Math.max(hz.pts[0].x, hz.pts[1].x);
      if (y >= vy0 && y <= vy1 && x >= hx0 && x <= hx1) out.push({ x, y });
    }
  }
  return out;
}

/**
 * Per-column profile writer. For each column the dominant piece is chosen:
 * chamber interior > run interior > chamber wall > run wall, so tunnels open
 * cleanly into chambers.
 */
export const sewerSource = {
  id: "sewers",
  order: 2,
  maxLod: 1,
  zRange(world, rect, lod = 0) {
    if (lod > 1 || !world.sewers) return null;
    let lo = Infinity;
    let hi = -Infinity;
    const hit = (x0, y0, x1, y1) => x0 <= rect.x1 && x1 >= rect.x0 && y0 <= rect.y1 && y1 >= rect.y0;
    const W = IN_W + SHELL;
    for (const p of world.sewers.near(rect)) {
      for (const r of p.runs) {
        const ok = r.axis === 0 ? hit(r.fixed - W, r.l0, r.fixed + W, r.l1) : hit(r.l0, r.fixed - W, r.l1, r.fixed + W);
        if (!ok) continue;
        lo = Math.min(lo, Math.min(r.z0, r.z1) - 6);
        hi = Math.max(hi, Math.max(r.z0, r.z1) + VAULT + SHELL + 1);
      }
      for (const n of p.nodes) {
        const e = n.R + SHELL + (n.open ? 16 : 8);
        if (!hit(n.x - e, n.y - e, n.x + e, n.y + e)) continue;
        lo = Math.min(lo, n.z - (n.hall ? BASIN : 0) - 6);
        hi = Math.max(hi, n.zr + 4);
      }
      for (const q of p.boxes) {
        if (!hit(q.x0, q.y0, q.x1, q.y1)) continue;
        lo = Math.min(lo, q.z0);
        hi = Math.max(hi, q.z1);
      }
    }
    return lo === Infinity ? null : [lo, hi];
  },
  rasterize(world, chunk, tile) {
    if (chunk.lod > 1 || !world.sewers) return;
    const box = chunk.worldBox;
    const plans = world.sewers.near(box);
    if (!plans.length) return;
    const runs = [];
    const nodes = [];
    const seen = new Set();
    for (const p of plans) {
      if (p.bb.z0 > box.z1 || p.bb.z1 < box.z0) continue;
      for (const r of p.runs) {
        const lo = r.axis === 0 ? box.y0 : box.x0;
        const hi = r.axis === 0 ? box.y1 : box.x1;
        const c0 = r.axis === 0 ? box.x0 : box.y0;
        const c1 = r.axis === 0 ? box.x1 : box.y1;
        if (r.l1 < lo || r.l0 > hi || r.fixed + IN_W + SHELL < c0 || r.fixed - IN_W - SHELL > c1) continue;
        runs.push(r);
      }
      for (const n of p.nodes) {
        if (seen.has(n.key)) continue;
        const e = n.R + SHELL + 12;
        if (n.x + e < box.x0 || n.x - e > box.x1 || n.y + e < box.y0 || n.y - e > box.y1) continue;
        seen.add(n.key);
        nodes.push(n);
      }
    }
    if (runs.length || nodes.length) rasterizeColumns(chunk, tile, runs, nodes);
    for (const p of plans) {
      for (const q of p.boxes) {
        if (q.x1 < box.x0 || q.x0 > box.x1 || q.y1 < box.y0 || q.y0 > box.y1 || q.z1 < box.z0 || q.z0 > box.z1) continue;
        chunk.fillBox(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode ?? 0);
      }
    }
  },
};

function rasterizeColumns(chunk, tile, runs, nodes) {
  for (let j = 0; j < P; j += 1) {
    const y = chunk.wy(j);
    for (let i = 0; i < P; i += 1) {
      const x = chunk.wx(i);
      const col = i + j * P;
      let nIn = null;
      let nWall = null;
      let dIn = Infinity;
      for (const n of nodes) {
        const m = Math.max(Math.abs(x - n.x), Math.abs(y - n.y));
        if (m <= n.R && m < dIn) {
          nIn = n;
          dIn = m;
        } else if (m > n.R && m <= n.R + SHELL) nWall = n;
      }
      if (nIn) chamberColumn(chunk, col, x, y, nIn);
      else {
        let best = null;
        let bc = Infinity;
        for (const r of runs) {
          const l = r.axis === 0 ? y : x;
          if (l < r.l0 || l > r.l1) continue;
          const c = Math.abs((r.axis === 0 ? x : y) - r.fixed);
          if (c < bc) {
            bc = c;
            best = r;
          }
        }
        if (best && bc <= IN_W) runColumn(chunk, col, bc, best, best.axis === 0 ? y : x);
        else if (nWall) wallColumn(chunk, col, nWall.z - (nWall.hall ? BASIN : 0) - 3, nWall.z + nWall.H + SHELL - 1);
        else if (best && bc <= IN_W + SHELL) {
          const zb = runZ(best, best.axis === 0 ? y : x);
          wallColumn(chunk, col, zb - 3, zb + VAULT - 5 + SHELL);
        }
      }
      // ladder shafts (and the cones around open manholes) go on top
      for (const n of nodes) {
        if (!n.shaft) continue;
        const sx = n.x + n.qx * 12;
        const sy = n.y + n.qy * 12;
        const reach = n.open ? 15 : SHAFT + 2;
        if (Math.abs(x - sx) <= reach && Math.abs(y - sy) <= reach) shaftColumn(chunk, tile, col, x, y, n, sx, sy);
      }
    }
  }
}

function runZ(r, l) {
  const len = r.l1 - r.l0;
  return Math.round(r.z0 + ((r.z1 - r.z0) * (l - r.l0)) / (len || 1));
}

function put(chunk, col, z, m, onlySolid = false) {
  const k = chunk.rangeZ(z, z);
  if (k[0] > k[1]) return;
  const idx = col + k[0] * P2;
  if (onlySolid && !IS_SOLID[chunk.data[idx]]) return;
  chunk.data[idx] = m;
}

function span(chunk, col, z0, z1, m, onlySolid = false) {
  const [k0, k1] = chunk.rangeZ(z0, z1);
  const d = chunk.data;
  for (let k = k0; k <= k1; k += 1) {
    const idx = col + k * P2;
    if (onlySolid && !IS_SOLID[d[idx]]) continue;
    d[idx] = m;
  }
}

function wallColumn(chunk, col, z0, z1) {
  const [k0, k1] = chunk.rangeZ(z0, z1);
  const d = chunk.data;
  for (let k = k0; k <= k1; k += 1) {
    const idx = col + k * P2;
    if (!IS_SOLID[d[idx]]) continue;
    const z = chunk.wz(k);
    d[idx] = z - z0 < 6 ? MAT.SEWER_BRICK_DARK : MAT.SEWER_BRICK;
  }
}

function runColumn(chunk, col, ac, r, l) {
  const zb = runZ(r, l);
  const vault = zb + VAULT - Math.round(5 * (ac / IN_W) ** 2);
  span(chunk, col, zb - 5, zb - 3, MAT.SEWER_BRICK, true);
  if (ac <= CH_W) {
    put(chunk, col, zb - 2, MAT.SLUDGE);
    put(chunk, col, zb - 1, MAT.SEWER_WATER);
    span(chunk, col, zb, vault, 0);
  } else {
    span(chunk, col, zb - 2, zb - 1, MAT.SEWER_BRICK_DARK);
    put(chunk, col, zb, ac === CH_W + 1 ? MAT.SEWER_CURB : MAT.SEWER_FLOOR);
    span(chunk, col, zb + 1, vault, 0);
  }
  span(chunk, col, vault + 1, vault + SHELL, MAT.SEWER_BRICK, true);
  const la = ((l % 96) + 96) % 96;
  if (ac <= 1 && la < 3) put(chunk, col, vault, MAT.LAMP_CAGE);
  if (ac === IN_W) put(chunk, col, vault, MAT.PIPE);
  if (ac === IN_W && la >= 40 && la < 42) span(chunk, col, zb + 9, vault - 1, MAT.PIPE);
}

function chamberColumn(chunk, col, x, y, n) {
  if (n.hall) {
    hallColumn(chunk, col, x - n.x, y - n.y, n);
    return;
  }
  const dx = x - n.x;
  const dy = y - n.y;
  const zb = n.z;
  const top = zb + n.H;
  span(chunk, col, zb - 5, zb - 3, MAT.SEWER_BRICK, true);
  if (inArm(n, dx, dy, CH_W)) {
    put(chunk, col, zb - 2, MAT.SLUDGE);
    put(chunk, col, zb - 1, MAT.SEWER_WATER);
    span(chunk, col, zb, top - 1, 0);
  } else {
    span(chunk, col, zb - 2, zb - 1, MAT.SEWER_BRICK_DARK);
    const edge = Math.abs(dx) === CH_W + 1 || Math.abs(dy) === CH_W + 1;
    put(chunk, col, zb, edge ? MAT.SEWER_CURB : MAT.SEWER_FLOOR);
    span(chunk, col, zb + 1, top - 1, 0);
  }
  span(chunk, col, top, top + SHELL - 1, MAT.SEWER_BRICK, true);
  const ax = Math.abs(dx);
  const ay = Math.abs(dy);
  // a caged lamp in the middle of the ceiling, pipes with valves on the walls
  if (ax <= 1 && ay <= 1) put(chunk, col, top - 1, MAT.LAMP_CAGE);
  if (ax === n.R && ay % 12 === 6) {
    span(chunk, col, zb + 1, top - 1, MAT.PIPE);
    put(chunk, col, zb + 10, MAT.VALVE_RED);
  }
}

/** Channels run from the centre along every incident arm. */
function inArm(n, dx, dy, cw) {
  return (
    (Math.abs(dx) <= cw && ((dy < 0 && n.arms.has("N")) || (dy > 0 && n.arms.has("S")) || Math.abs(dy) <= cw)) ||
    (Math.abs(dy) <= cw && ((dx < 0 && n.arms.has("W")) || (dx > 0 && n.arms.has("E")) || Math.abs(dx) <= cw))
  );
}

/**
 * Overflow hall: the runs arrive on a ledge ring along the walls; sewage
 * pours over the ledge into a pillared basin 3 m lower, reached by four
 * stairs that descend along the ledge.
 */
function hallColumn(chunk, col, dx, dy, n) {
  const zb = n.z;
  const top = zb + n.H;
  const ax = Math.abs(dx);
  const ay = Math.abs(dy);
  const m = Math.max(ax, ay);
  const inner = n.R - LEDGE; // last basin ring
  const zf = zb - BASIN;
  span(chunk, col, top, top + SHELL - 1, MAT.SEWER_BRICK, true);
  if (m > inner) {
    // ledge
    span(chunk, col, zb - 5, zb - 3, MAT.SEWER_BRICK, true);
    if (inArm(n, dx, dy, CH_W + 1)) {
      put(chunk, col, zb - 2, MAT.SLUDGE);
      put(chunk, col, zb - 1, MAT.SEWER_WATER);
      span(chunk, col, zb, top - 1, 0);
    } else {
      span(chunk, col, zb - 2, zb - 1, MAT.SEWER_BRICK_DARK);
      put(chunk, col, zb, m === inner + 1 ? MAT.SEWER_CURB : MAT.SEWER_FLOOR);
      span(chunk, col, zb + 1, top - 1, 0);
      // railing along the drop, except where a basin stair starts
      const stairTop = ax === inner + 1 && ay >= 7 && ay < 18;
      if (m === inner + 1 && !stairTop) {
        put(chunk, col, zb + 7, MAT.RAILING);
        if (((dx + dy) & 3) === 0) span(chunk, col, zb + 1, zb + 6, MAT.RAILING);
      }
    }
    if (m === n.R && (ax + ay) % 24 === 0) span(chunk, col, zb + 1, top - 1, MAT.PIPE);
    return;
  }
  // basin
  span(chunk, col, zf - 5, zf - 3, MAT.SEWER_BRICK, true);
  let floor = zf;
  if (ax > inner - 8 && ay >= 7) floor = Math.max(zf, zb - ((ay - 6) >> 1)); // stairs along the x walls
  const wet = inArm(n, dx, dy, CH_W + 2);
  if (wet && floor === zf) {
    put(chunk, col, zf - 2, MAT.SLUDGE);
    put(chunk, col, zf - 1, MAT.SEWER_WATER);
    span(chunk, col, zf, top - 1, 0);
  } else {
    span(chunk, col, zf - 2, floor - 1, floor > zf ? MAT.STAIR_CONCRETE : MAT.SEWER_BRICK_DARK);
    put(chunk, col, floor, floor > zf ? MAT.STAIR_CONCRETE : ((dx >> 3) + (dy >> 3)) & 1 ? MAT.CONCRETE_DARK : MAT.SEWER_FLOOR);
    span(chunk, col, floor + 1, top - 1, 0);
  }
  // sewage pours over the ledge where an arm arrives
  if (m === inner && inArm(n, dx, dy, CH_W + 1)) span(chunk, col, zf, zb - 1, MAT.SEWER_WATER);
  const px = (ax >= 20 && ax <= 23) || (ax >= 40 && ax <= 43);
  const py = (ay >= 20 && ay <= 23) || (ay >= 40 && ay <= 43);
  if (px && py) {
    span(chunk, col, zf + 1, top - 1, MAT.CONCRETE);
    span(chunk, col, zf + 1, zf + 4, MAT.SEWER_BRICK_DARK);
    if ((ax === 20 || ax === 40) && (ay === 21 || ay === 41)) put(chunk, col, zb + 4, MAT.LAMP_CAGE);
    return;
  }
  if (ax % 20 === 10 && ay % 20 === 10) put(chunk, col, top - 1, MAT.LAMP_CAGE);
}

/**
 * Ladder shaft from a chamber up to a manhole in the street. Runs after the
 * chamber profile of the same column only when the column is outside the
 * chamber interior; inside, chamberColumn has carved the room and this adds
 * the ladder and the shaft above the ceiling.
 */
function shaftColumn(chunk, tile, col, x, y, n, sx, sy) {
  const dx = x - sx;
  const dy = y - sy;
  const clear = dx >= -SHAFT && dx < SHAFT && dy >= -SHAFT && dy < SHAFT;
  const zs = tile ? tile.z[col] : n.zr;
  const ceil = n.z + n.H;
  if (!clear) {
    const ring = dx >= -SHAFT - 2 && dx <= SHAFT + 1 && dy >= -SHAFT - 2 && dy <= SHAFT + 1;
    if (ring) {
      // concrete ring and an iron rim at the street surface
      span(chunk, col, ceil, zs - 1, MAT.CONCRETE, true);
      if (dx >= -SHAFT - 1 && dx <= SHAFT && dy >= -SHAFT - 1 && dy <= SHAFT) put(chunk, col, zs, MAT.MANHOLE_RIM);
    } else if (n.open) coneColumn(chunk, col, x, y, sx, sy, zs);
    return;
  }
  span(chunk, col, ceil - 1, zs - 1, 0);
  if (n.open) put(chunk, col, zs, 0);
  else put(chunk, col, zs, (dx + dy) & 1 ? MAT.MANHOLE : MAT.MANHOLE_RIM);
  // ladder on the outer wall of the shaft, from the chamber floor to the street
  const lx = n.qx > 0 ? SHAFT - 1 : -SHAFT;
  if (dx === lx && dy >= -SHAFT + 1 && dy <= SHAFT - 2) {
    const rail = dy === -SHAFT + 1 || dy === SHAFT - 2;
    for (let z = n.z + 1; z < zs; z += 1) if (rail || ((z - n.z) & 1) === 0) put(chunk, col, z, MAT.LADDER);
  }
}

function coneColumn(chunk, col, x, y, sx, sy, zs) {
  for (const [ox, oy] of [
    [-8, -8],
    [7, -8],
    [-8, 7],
    [7, 7],
  ]) {
    const u = x - (sx + ox);
    const v = y - (sy + oy);
    if (u < 0 || u > 1 || v < 0 || v > 1) continue;
    put(chunk, col, zs + 1, MAT.CONE_ORANGE);
    put(chunk, col, zs + 2, MAT.CONE_WHITE);
    if (u === 0 && v === 0) put(chunk, col, zs + 3, MAT.CONE_ORANGE);
  }
  // the lifted cover lies next to the hole
  const u = x - (sx + 8);
  const v = y - sy;
  if (u >= 1 && u <= 6 && v >= -3 && v <= 2) put(chunk, col, zs + 1, MAT.MANHOLE);
}
