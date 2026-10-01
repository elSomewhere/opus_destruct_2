/**
 * Blocks as convex polygons (ANGLED_WORLD_PLAN.md S1): an angled street
 * (a diagonal boulevard, a tilted old-town cut) splits a block along its
 * centre line, so blocks of the angled world are convex polygons between
 * road centre lines. Each edge carries the side of the road it follows
 * ({ cls, hr, id }, or no road). The rest of the engine keeps its rect
 * blocks: a polygon block is recorded as its bounding rect with the
 * sides that lie on it, plus one property half-plane per slanted edge
 * (`cuts`), and lots planned in the rect are trimmed to those.
 *
 * Polygons are arrays of { x, y, side } in order; the edge from vertex i
 * to vertex i + 1 follows `side` of vertex i.
 */

const NO_SIDE = Object.freeze({ cls: null, hr: 0, id: null });

/** A rect block {x0, y0, x1, y1} with sides {N, E, S, W} as a polygon. */
export function rectPoly(r, sides) {
  return [
    { x: r.x0, y: r.y0, side: sides.N ?? NO_SIDE },
    { x: r.x1, y: r.y0, side: sides.E ?? NO_SIDE },
    { x: r.x1, y: r.y1, side: sides.S ?? NO_SIDE },
    { x: r.x0, y: r.y1, side: sides.W ?? NO_SIDE },
  ];
}

export function polyArea(poly) {
  let a = 0;
  for (let k = 0; k < poly.length; k += 1) {
    const p = poly[k];
    const q = poly[(k + 1) % poly.length];
    a += p.x * q.y - q.x * p.y;
  }
  return Math.abs(a) / 2;
}

export function polyCentroid(poly) {
  let x = 0;
  let y = 0;
  for (const p of poly) {
    x += p.x;
    y += p.y;
  }
  return { x: x / poly.length, y: y / poly.length };
}

export function polyBounds(poly) {
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (const p of poly) {
    x0 = Math.min(x0, p.x);
    y0 = Math.min(y0, p.y);
    x1 = Math.max(x1, p.x);
    y1 = Math.max(y1, p.y);
  }
  return { x0, y0, x1, y1 };
}

/**
 * Split a convex polygon by the line through a with direction (dx, dy):
 * [left, right] (either may be null), the new edge on both following
 * `side`. Left: dx (y - ay) - dy (x - ax) < 0.
 */
export function splitPoly(poly, a, dx, dy, side) {
  const f = (p) => dx * (p.y - a.y) - dy * (p.x - a.x);
  const left = [];
  const right = [];
  const n = poly.length;
  for (let k = 0; k < n; k += 1) {
    const p = poly[k];
    const q = poly[(k + 1) % n];
    const fp = f(p);
    const fq = f(q);
    if (fp <= 0) left.push({ ...p });
    if (fp >= 0) right.push({ ...p });
    if ((fp < 0 && fq > 0) || (fp > 0 && fq < 0)) {
      const t = fp / (fp - fq);
      // (an axis-aligned cut crosses exactly on its own line: no stray 231.99999999999994)
      const x = dx === 0 ? a.x : p.x + (q.x - p.x) * t;
      const y = dy === 0 ? a.y : p.y + (q.y - p.y) * t;
      // the crossing continues p's edge on p's side, then runs along the cut
      if (fp < 0) {
        left.push({ x, y, side });
        right.push({ x, y, side: p.side });
      } else {
        right.push({ x, y, side });
        left.push({ x, y, side: p.side });
      }
    }
  }
  // two vertices on the line in a row: the edge between them is the cut
  const eps = 1e-7 * (Math.abs(dx) + Math.abs(dy));
  const close = (poly2) => {
    if (poly2.length < 3) return null;
    for (let k = 0; k < poly2.length; k += 1) {
      const p = poly2[k];
      const q = poly2[(k + 1) % poly2.length];
      if (Math.abs(f(p)) <= eps && Math.abs(f(q)) <= eps) p.side = side;
    }
    return polyArea(poly2) > 1 ? poly2 : null;
  };
  return [close(left), close(right)];
}

/** Does the line through a with direction (dx, dy) cross the polygon's interior? */
export function lineCrosses(poly, a, dx, dy) {
  let neg = false;
  let pos = false;
  for (const p of poly) {
    const v = dx * (p.y - a.y) - dy * (p.x - a.x);
    if (v < -1e-9) neg = true;
    if (v > 1e-9) pos = true;
  }
  return neg && pos;
}

/**
 * A polygon block's record: `r` its bounding rect (whole voxels), `s` the
 * sides of the polygon's edges that lie on that rect's edges (the others
 * have no road), `cuts` the property half-planes of the slanted edges
 * ({ nx, ny, c, cls, hr, id }: a point is on the block's side when
 * nx x + ny y >= c, the road's right-of-way off), and `poly`.
 */
export function polyBlock(poly) {
  const b = polyBounds(poly);
  const r = { x0: Math.floor(b.x0), y0: Math.floor(b.y0), x1: Math.ceil(b.x1), y1: Math.ceil(b.y1) };
  const s = { N: NO_SIDE, E: NO_SIDE, S: NO_SIDE, W: NO_SIDE };
  const cuts = [];
  const c = polyCentroid(poly);
  for (let k = 0; k < poly.length; k += 1) {
    const p = poly[k];
    const q = poly[(k + 1) % poly.length];
    if (p.y === q.y && p.y === b.y0) s.N = p.side;
    else if (p.y === q.y && p.y === b.y1) s.S = p.side;
    else if (p.x === q.x && p.x === b.x0) s.W = p.side;
    else if (p.x === q.x && p.x === b.x1) s.E = p.side;
    else if (p.x !== q.x && p.y !== q.y) {
      // slanted: the inward unit normal, offset by the road's half right-of-way
      const len = Math.hypot(q.x - p.x, q.y - p.y);
      let nx = -(q.y - p.y) / len;
      let ny = (q.x - p.x) / len;
      if (nx * (c.x - p.x) + ny * (c.y - p.y) < 0) {
        nx = -nx;
        ny = -ny;
      }
      cuts.push({ nx, ny, c: nx * p.x + ny * p.y + (p.side.hr ?? 0), cls: p.side.cls, hr: p.side.hr ?? 0, id: p.side.id });
    }
  }
  return { r, s, cuts, poly };
}

/** Is (x, y) on the block's side of all its cuts? */
export function insideCuts(cuts, x, y) {
  for (const k of cuts) if (k.nx * x + k.ny * y < k.c) return false;
  return true;
}

/**
 * A rect (inclusive voxels) trimmed to lie on the block's side of every
 * cut (voxel centres tested): the side facing each cut moves in, the way
 * that keeps the most area; null when nothing is left. `trimmed` lists the
 * rect sides that moved, with the cut's road.
 */
export function trimToCuts(rect, cuts) {
  let r = { ...rect };
  const trimmed = [];
  for (const k of cuts) {
    const ok = (x, y) => k.nx * (x + 0.5) + k.ny * (y + 0.5) >= k.c;
    if (ok(r.x0, r.y0) && ok(r.x1, r.y0) && ok(r.x0, r.y1) && ok(r.x1, r.y1)) continue;
    if (!ok(r.x0, r.y0) && !ok(r.x1, r.y0) && !ok(r.x0, r.y1) && !ok(r.x1, r.y1)) return null;
    const opts = [];
    // (a voxel column x is in when nx (x + 1/2) + ny (y + 1/2) >= c at the rect's worst y)
    if (k.nx !== 0) {
      const worstY = k.ny > 0 ? r.y0 : r.y1;
      const bound = (k.c - k.ny * (worstY + 0.5)) / k.nx - 0.5;
      if (k.nx > 0) opts.push({ ...r, x0: Math.max(r.x0, Math.ceil(bound)), side: "W" });
      else opts.push({ ...r, x1: Math.min(r.x1, Math.floor(bound)), side: "E" });
    }
    if (k.ny !== 0) {
      const worstX = k.nx > 0 ? r.x0 : r.x1;
      const bound = (k.c - k.nx * (worstX + 0.5)) / k.ny - 0.5;
      if (k.ny > 0) opts.push({ ...r, y0: Math.max(r.y0, Math.ceil(bound)), side: "N" });
      else opts.push({ ...r, y1: Math.min(r.y1, Math.floor(bound)), side: "S" });
    }
    const area = (q) => (q.x1 >= q.x0 && q.y1 >= q.y0 ? (q.x1 - q.x0 + 1) * (q.y1 - q.y0 + 1) : 0);
    const best = opts.reduce((p, q) => (area(q) > area(p) ? q : p));
    if (area(best) === 0) return null;
    trimmed.push({ side: best.side, cls: k.cls, id: k.id });
    r = { x0: best.x0, y0: best.y0, x1: best.x1, y1: best.y1 };
  }
  return { rect: r, trimmed };
}
