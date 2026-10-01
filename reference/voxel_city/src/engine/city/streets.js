import { vx } from "../core/units.js";
import { YAWS } from "../core/placement.js";
import { rectPoly, splitPoly, polyBounds, polyBlock, polyArea } from "./blockPoly.js";

/**
 * Local street patterns. A pattern receives a sub-cell (a rectangle between
 * road CENTERLINES, with the cross-section of the road on each side) and
 * emits local roads plus the resulting blocks. Blocks keep, per side, the
 * half right-of-way of the road they front so the property line can be
 * derived exactly.
 *
 *   emit.road(cls, ax, ay, bx, by) -> road record (with .hr)
 *   emit.block(rect, sides)
 */

const SNAP = 8; // 1 m

function snap(v) {
  return Math.round(v / SNAP) * SNAP;
}

function pickClass(district, rng) {
  const st = district.streets;
  return rng.chance(st.pedestrianChance) ? "pedestrian" : st.localClass;
}

function side(road) {
  return { cls: road.cls, hr: road.hr, id: road.id };
}

/**
 * Grid streets: rows and columns of blocks. Block widths vary a little
 * (±15%), and now and then (`streets.jog`, default 0.3) a north-south
 * street shifts sideways by 8-25 m where it crosses a street, so the grid
 * reads as grown rather than drawn. Neighbouring blocks may merge in pairs
 * (`mergeChance`; only side by side where streets jog).
 */
export function gridStreets(sub, district, rng, emit) {
  const r = sub.rect;
  const W = r.x1 - r.x0;
  const H = r.y1 - r.y0;
  const [longR, shortR] = district.streets.block;
  const longT = vx(rng.float(longR[0], longR[1]));
  const shortT = vx(rng.float(shortR[0], shortR[1]));
  // orient long block side along the longer sub-cell dimension (mostly)
  const longAlongX = W >= H ? rng.chance(0.8) : rng.chance(0.2);
  const tx = longAlongX ? longT : shortT;
  const ty = longAlongX ? shortT : longT;
  const nx = Math.max(1, Math.round(W / tx));
  const ny = Math.max(1, Math.round(H / ty));
  const split = (a0, len, n, minW) => {
    const out = [a0];
    for (let k = 1; k < n; k += 1) out.push(snap(a0 + (len * k) / n + (rng.next() - 0.5) * 0.3 * (len / n)));
    out.push(a0 + len);
    for (let k = 1; k < out.length; k += 1) if (out[k] - out[k - 1] < minW) return split(a0, len, n, 0);
    return out;
  };
  const ys = split(r.y0, H, ny, vx(shortR[0] * 0.6));
  // x positions per row: a line may jog at a row boundary
  const base = split(r.x0, W, nx, vx(shortR[0] * 0.6));
  const jog = district.streets.jog ?? 0.3;
  const xsRow = [base.slice()];
  for (let b = 1; b < ny; b += 1) {
    const row = xsRow[b - 1].slice();
    for (let a = 1; a < nx; a += 1) {
      if (!rng.chance(jog / Math.max(1, ny - 1))) continue;
      const lo = row[a - 1] + vx(shortR[0] * 0.6);
      const hi = row[a + 1] - vx(shortR[0] * 0.6);
      const x = snap(row[a] + vx(rng.float(8, 25)) * (rng.chance(0.5) ? 1 : -1));
      if (x > lo && x < hi) row[a] = x;
    }
    xsRow.push(row);
  }
  const jogged = xsRow.some((row) => row.some((x, a) => x !== base[a]));

  // merges of neighbouring cells (pairs only, keeps rectangles)
  const cellId = (a, b) => b * nx + a;
  const mergedWith = new Array(nx * ny).fill(-1);
  const removedV = new Set(); // vertical edge between (a,b) and (a+1,b): key a,b
  const removedH = new Set(); // horizontal edge between (a,b) and (a,b+1)
  const mc = district.streets.mergeChance ?? 0;
  for (let b = 0; b < ny; b += 1) {
    for (let a = 0; a < nx; a += 1) {
      if (mergedWith[cellId(a, b)] >= 0 || !rng.chance(mc)) continue;
      const horizontal = jogged || rng.chance(0.5);
      const a2 = horizontal ? a + 1 : a;
      const b2 = horizontal ? b : b + 1;
      if (a2 >= nx || b2 >= ny || mergedWith[cellId(a2, b2)] >= 0) continue;
      mergedWith[cellId(a, b)] = cellId(a2, b2);
      mergedWith[cellId(a2, b2)] = cellId(a, b);
      if (horizontal) removedV.add(`${a},${b}`);
      else removedH.add(`${a},${b}`);
    }
  }

  // vertical interior lines: maximal runs of kept edges at one x
  const vRoads = new Map(); // key a,b -> road
  for (let a = 0; a < nx - 1; a += 1) {
    const cls = pickClass(district, rng);
    let runStart = null;
    for (let b = 0; b <= ny; b += 1) {
      const kept = b < ny && !removedV.has(`${a},${b}`);
      const x = b < ny ? xsRow[b][a + 1] : null;
      if (runStart !== null && (!kept || x !== xsRow[runStart][a + 1])) {
        const road = emit.road(cls, xsRow[runStart][a + 1], ys[runStart], xsRow[runStart][a + 1], ys[b]);
        for (let bb = runStart; bb < b; bb += 1) vRoads.set(`${a},${bb}`, road);
        runStart = null;
      }
      if (kept && runStart === null) runStart = b;
    }
  }
  const hRoads = new Map();
  for (let b = 0; b < ny - 1; b += 1) {
    const y = ys[b + 1];
    const cls = pickClass(district, rng);
    let runStart = null;
    for (let a = 0; a <= nx; a += 1) {
      const kept = a < nx && !removedH.has(`${a},${b}`);
      if (kept && runStart === null) runStart = a;
      if (!kept && runStart !== null) {
        // (a jogged grid: the street spans the widest of the two rows it separates)
        const x0 = Math.min(xsRow[b][runStart], xsRow[b + 1][runStart]);
        const x1 = Math.max(xsRow[b][a], xsRow[b + 1][a]);
        const road = emit.road(cls, x0, y, x1, y);
        for (let aa = runStart; aa < a; aa += 1) hRoads.set(`${aa},${b}`, road);
        runStart = null;
      }
    }
  }

  const done = new Set();
  for (let b = 0; b < ny; b += 1) {
    for (let a = 0; a < nx; a += 1) {
      const id = cellId(a, b);
      if (done.has(id)) continue;
      done.add(id);
      let a1 = a;
      let b1 = b;
      if (mergedWith[id] >= 0) {
        const other = mergedWith[id];
        done.add(other);
        a1 = Math.max(a, other % nx);
        b1 = Math.max(b, Math.floor(other / nx));
      }
      const xs = xsRow[b];
      const rect = { x0: xs[a], y0: ys[b], x1: xs[a1 + 1], y1: ys[b1 + 1] };
      const sides = {
        W: a === 0 ? sub.sides.W : side(vRoads.get(`${a - 1},${b}`)),
        E: a1 === nx - 1 ? sub.sides.E : side(vRoads.get(`${a1},${b}`)),
        N: b === 0 ? sub.sides.N : side(hRoads.get(`${a},${b - 1}`)),
        S: b1 === ny - 1 ? sub.sides.S : side(hRoads.get(`${a},${b1}`)),
      };
      emit.block(rect, sides);
    }
  }
}

export function subdivideStreets(sub, district, rng, emit) {
  const [longR, shortR] = district.streets.block;
  const longT = vx(rng.float(longR[0], longR[1]));
  const shortT = vx(rng.float(shortR[0], shortR[1]));

  const recurse = (rect, sides, depth) => {
    const w = rect.x1 - rect.x0;
    const h = rect.y1 - rect.y0;
    const longIsX = w >= h;
    const longDim = longIsX ? w : h;
    const shortDim = longIsX ? h : w;
    let axis = null;
    if (shortDim > shortT * 1.45) axis = longIsX ? "y" : "x";
    else if (longDim > longT * 1.3) axis = longIsX ? "x" : "y";
    if (!axis || depth > 12) {
      emit.block(rect, sides);
      return;
    }
    const dim = axis === "x" ? w : h;
    const target = axis === (longIsX ? "x" : "y") ? longT : shortT;
    const parts = Math.max(2, Math.round(dim / target));
    // cut near a multiple of the target so blocks stay near target size
    const k = rng.int(1, parts - 1);
    let at = (dim * k) / parts + (rng.next() - 0.5) * vx(10);
    at = snap((axis === "x" ? rect.x0 : rect.y0) + at);
    const cls = pickClass(district, rng);
    if (axis === "x") {
      const road = emit.road(cls, at, rect.y0, at, rect.y1);
      recurse({ ...rect, x1: at }, { ...sides, E: side(road) }, depth + 1);
      recurse({ ...rect, x0: at }, { ...sides, W: side(road) }, depth + 1);
    } else {
      const road = emit.road(cls, rect.x0, at, rect.x1, at);
      recurse({ ...rect, y1: at }, { ...sides, S: side(road) }, depth + 1);
      recurse({ ...rect, y0: at }, { ...sides, N: side(road) }, depth + 1);
    }
  };
  recurse(sub.rect, sub.sides, 0);
}

/**
 * Old-town streets: a sub-cell split again and again at irregular places
 * (anywhere between a third and two thirds of the block, never leaving a
 * sliver), so blocks come in every size and proportion, streets end at
 * T-junctions and jog where they cross. Now and then the cut is a narrow
 * lane (`streets.laneClass`, a cobbled passage between the houses) instead
 * of a street, and small blocks stay whole.
 */
export function organicStreets(sub, district, rng, emit, opts = {}) {
  if (opts.angled) {
    organicAngled(sub, district, rng, emit, opts);
    return;
  }
  const recurse = (rect, sides, depth) => {
    // adaptive: each part asks for the district at its own centre (emit.local),
    // so blocks grow small in the old core and large by the works
    const d = (emit.local && emit.local(rect)) || district;
    const st = d.streets;
    if (PATTERN_FAMILY[st.pattern] === "none") {
      emit.block(rect, sides);
      return;
    }
    const [longR, shortR] = st.block;
    const laneChance = st.laneChance ?? 0;
    const laneCls = st.laneClass ?? "alley";
    const minPart = vx(shortR[0] * 0.7);
    const w = rect.x1 - rect.x0;
    const h = rect.y1 - rect.y0;
    const longIsX = w >= h;
    const longDim = longIsX ? w : h;
    const shortDim = longIsX ? h : w;
    const longT = vx(rng.float(longR[0], longR[1]));
    const shortT = vx(rng.float(shortR[0], shortR[1]));
    let axis = null;
    if (longDim > longT * 1.2) axis = longIsX ? "x" : "y";
    else if (shortDim > shortT * 1.55) axis = longIsX ? "y" : "x";
    const dim = axis === "x" ? w : h;
    if (!axis || depth > 14 || dim < 2 * minPart) {
      emit.block(rect, sides);
      return;
    }
    const at0 = Math.max(minPart, Math.min(dim - minPart, dim * rng.float(0.3, 0.7)));
    const at = snap((axis === "x" ? rect.x0 : rect.y0) + at0);
    // a lane only between blocks that are small enough to be walked round
    const cls = depth > 0 && dim < vx(longR[1] * 2.2) && rng.chance(laneChance) ? laneCls : pickClass(d, rng);
    const paving = st.paving ?? null;
    if (axis === "x") {
      const road = emit.road(cls, at, rect.y0, at, rect.y1, paving);
      recurse({ ...rect, x1: at }, { ...sides, E: side(road) }, depth + 1);
      recurse({ ...rect, x0: at }, { ...sides, W: side(road) }, depth + 1);
    } else {
      const road = emit.road(cls, rect.x0, at, rect.x1, at, paving);
      recurse({ ...rect, y1: at }, { ...sides, S: side(road) }, depth + 1);
      recurse({ ...rect, y0: at }, { ...sides, N: side(road) }, depth + 1);
    }
  };
  recurse(sub.rect, sub.sides, 0);
}

/** Tilts of old-town cuts: the exact yaws of 8.8° to 14.3° (core/placement.js). */
const TILTS = [1, 2, 3, 4];

/** A block of the angled old town must fill at least this share of its bounding rect (no slivers, no fans). */
const FAT = 0.6;

/** The piece of the line through a with direction (dx, dy) inside a convex polygon: [p, q] or null. */
function clipLine(poly, a, dx, dy) {
  let t0 = -Infinity;
  let t1 = Infinity;
  const n = poly.length;
  // the polygon's winding decides which side of an edge is inside
  const ccw = poly.reduce((s, p, k) => s + p.x * poly[(k + 1) % n].y - poly[(k + 1) % n].x * p.y, 0) > 0;
  for (let k = 0; k < n; k += 1) {
    const p = poly[k];
    const q = poly[(k + 1) % n];
    // inside: cross(q - p, x - p) has the winding's sign
    const ex = q.x - p.x;
    const ey = q.y - p.y;
    const s0 = (ex * (a.y - p.y) - ey * (a.x - p.x)) * (ccw ? 1 : -1);
    const ds = (ex * dy - ey * dx) * (ccw ? 1 : -1);
    if (ds === 0) {
      if (s0 < 0) return null;
      continue;
    }
    const t = -s0 / ds;
    if (ds > 0) t0 = Math.max(t0, t);
    else t1 = Math.min(t1, t);
  }
  if (!(t1 > t0)) return null;
  return [
    { x: a.x + dx * t0, y: a.y + dy * t0 },
    { x: a.x + dx * t1, y: a.y + dy * t1 },
  ];
}

/**
 * Old-town streets of the angled world (ANGLED_WORLD_PLAN.md S1): the
 * organic pattern on convex polygons, and now and then (`opts.tilt`) a cut
 * tilted off the axes by a small exact angle, so lanes meet at natural
 * angles and blocks come out as irregular quadrilaterals. The same split
 * rules as `organicStreets` (a third to two thirds of the block, never a
 * sliver, lanes between small blocks); a block is its polygon (blockPoly.js).
 */
function organicAngled(sub, district, rng, emit, opts) {
  const tilt = opts.tilt ?? 0.35;
  const emitPoly = (poly) => {
    const b = polyBlock(poly);
    emit.block(b.r, b.s, b.cuts.length ? poly : null);
  };
  const recurse = (poly, depth) => {
    const bb = polyBounds(poly);
    const rect = { x0: Math.floor(bb.x0), y0: Math.floor(bb.y0), x1: Math.ceil(bb.x1), y1: Math.ceil(bb.y1) };
    const d = (emit.local && emit.local(rect)) || district;
    const st = d.streets;
    if (PATTERN_FAMILY[st.pattern] === "none") {
      emitPoly(poly);
      return;
    }
    const [longR, shortR] = st.block;
    const laneChance = st.laneChance ?? 0;
    const laneCls = st.laneClass ?? "alley";
    const minPart = vx(shortR[0] * 0.7);
    const w = rect.x1 - rect.x0;
    const h = rect.y1 - rect.y0;
    const longIsX = w >= h;
    const longDim = longIsX ? w : h;
    const shortDim = longIsX ? h : w;
    const longT = vx(rng.float(longR[0], longR[1]));
    const shortT = vx(rng.float(shortR[0], shortR[1]));
    let axis = null;
    if (longDim > longT * 1.2) axis = longIsX ? "x" : "y";
    else if (shortDim > shortT * 1.55) axis = longIsX ? "y" : "x";
    const dim = axis === "x" ? w : h;
    if (!axis || depth > 14 || dim < 2 * minPart) {
      emitPoly(poly);
      return;
    }
    const at0 = Math.max(minPart, Math.min(dim - minPart, dim * rng.float(0.3, 0.7)));
    const at = snap((axis === "x" ? rect.x0 : rect.y0) + at0);
    const a = axis === "x" ? { x: at, y: (bb.y0 + bb.y1) / 2 } : { x: (bb.x0 + bb.x1) / 2, y: at };
    // the cut: across the axis, now and then tilted by an exact small angle
    // (straight after all where the tilt would leave a sliver or a fan)
    const cutSide = { cls: null, hr: 0, id: null };
    const poor = (q) => {
      if (!q) return true;
      const b = polyBounds(q);
      return Math.min(b.x1 - b.x0, b.y1 - b.y0) < minPart || polyArea(q) < Math.max(minPart * minPart, FAT * (b.x1 - b.x0) * (b.y1 - b.y0));
    };
    const tryCut = (dx, dy) => {
      const seg = clipLine(poly, a, dx, dy);
      if (!seg) return null;
      const [L, R] = splitPoly(poly, a, dx, dy, cutSide);
      return poor(L) || poor(R) ? null : { seg, L, R };
    };
    let cut = null;
    // (a region with a slanted edge running the same way is cut parallel to
    // it: tilted lanes run side by side instead of closing into a fan)
    let par = null;
    for (let k = 0; k < poly.length && !par; k += 1) {
      const p = poly[k];
      const q = poly[(k + 1) % poly.length];
      const ex = q.x - p.x;
      const ey = q.y - p.y;
      if (ex !== 0 && ey !== 0 && (axis === "x" ? Math.abs(ey) > Math.abs(ex) : Math.abs(ex) > Math.abs(ey))) par = [ex, ey];
    }
    if (rng.chance(tilt)) {
      const t = YAWS[rng.pick(TILTS)];
      const sg = rng.sign();
      cut = par ? tryCut(par[0], par[1]) : axis === "x" ? tryCut(-sg * t.s, t.c) : tryCut(t.c, sg * t.s);
    }
    cut ??= axis === "x" ? tryCut(0, 1) : tryCut(1, 0);
    if (!cut) {
      emitPoly(poly);
      return;
    }
    const { seg, L, R } = cut;
    // a lane only between blocks that are small enough to be walked round
    const cls = depth > 0 && dim < vx(longR[1] * 2.2) && rng.chance(laneChance) ? laneCls : pickClass(d, rng);
    const road = emit.road(cls, seg[0].x, seg[0].y, seg[1].x, seg[1].y, st.paving ?? null);
    Object.assign(cutSide, side(road));
    recurse(L, depth + 1);
    recurse(R, depth + 1);
  };
  recurse(rectPoly(sub.rect, sub.sides), 0);
}

export function noStreets(sub, district, rng, emit) {
  emit.block(sub.rect, sub.sides);
}

export const STREET_PATTERNS = {
  grid: gridStreets,
  subdivide: subdivideStreets,
  organic: organicStreets,
  none: noStreets,
};

/** Coarse families of street patterns: a block may change district within its family only. */
export const PATTERN_FAMILY = { grid: "fine", organic: "fine", subdivide: "coarse", none: "none" };
