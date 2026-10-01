/**
 * Oriented rects (ANGLED_WORLD_PLAN.md S3): a local rect of a placement
 * with only a yaw (core/placement.js), seen from the world. The local rect
 * r = {x0, y0, x1, y1} (inclusive cells, x = u, y = v) covers the
 * continuous box [x0, x1 + 1) x [y0, y1 + 1) of local space, which the
 * placement's exact rotation puts in the world at origin + M (u, v) / D.
 * Distances and fits are plain floating point (+ - * / and sqrt, the same
 * everywhere); membership of a voxel stays the placement's integer map.
 */

/** Local continuous point (u, v) -> world point [x, y]. */
export function localPointToWorld(p, u, v) {
  const m = p.m;
  return [p.origin.x + (m[0] * u + m[1] * v) / p.d, p.origin.y + (m[3] * u + m[4] * v) / p.d];
}

/** World continuous point (x, y) -> local point [u, v] (Mᵀ: a rotation's inverse). */
export function worldPointToLocal(p, x, y) {
  const m = p.m;
  const dx = x - p.origin.x;
  const dy = y - p.origin.y;
  return [(m[0] * dx + m[3] * dy) / p.d, (m[1] * dx + m[4] * dy) / p.d];
}

/** World corners of a local rect, in order round it: [[x, y] x 4]. */
export function obbCorners(p, r) {
  return [
    localPointToWorld(p, r.x0, r.y0),
    localPointToWorld(p, r.x1 + 1, r.y0),
    localPointToWorld(p, r.x1 + 1, r.y1 + 1),
    localPointToWorld(p, r.x0, r.y1 + 1),
  ];
}

/** Integer world bounds {x0, y0, x1, y1} (inclusive voxels) of a local rect: every voxel it touches. */
export function obbBounds(p, r) {
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (const [x, y] of obbCorners(p, r)) {
    if (x < x0) x0 = x;
    if (y < y0) y0 = y;
    if (x > x1) x1 = x;
    if (y > y1) y1 = y;
  }
  return { x0: Math.floor(x0), y0: Math.floor(y0), x1: Math.ceil(x1) - 1, y1: Math.ceil(y1) - 1 };
}

/**
 * Distance (voxels) from the centre of world voxel (x, y) to a local rect,
 * 0 inside: its local offset from the rect (a rotation keeps distances).
 */
export function obbDistance(p, r, x, y) {
  const [u, v] = worldPointToLocal(p, x + 0.5, y + 0.5);
  const du = Math.max(r.x0 - u, 0, u - (r.x1 + 1));
  const dv = Math.max(r.y0 - v, 0, v - (r.y1 + 1));
  return Math.sqrt(du * du + dv * dv);
}

/** Is the centre of world voxel (x, y) inside a local rect grown by `pad` voxels? */
export function obbContains(p, r, x, y, pad = 0) {
  const [u, v] = worldPointToLocal(p, x + 0.5, y + 0.5);
  return u >= r.x0 - pad && u <= r.x1 + 1 + pad && v >= r.y0 - pad && v <= r.y1 + 1 + pad;
}

/**
 * The u range [lo, hi] a convex polygon (local points, in order) spans at
 * depth v, or null where it does not reach.
 */
function spanAt(poly, v) {
  let lo = Infinity;
  let hi = -Infinity;
  for (let k = 0; k < poly.length; k += 1) {
    const [au, av] = poly[k];
    const [bu, bv] = poly[(k + 1) % poly.length];
    if ((av - v) * (bv - v) > 0) continue;
    if (av === bv) {
      lo = Math.min(lo, au, bu);
      hi = Math.max(hi, au, bu);
      continue;
    }
    const u = au + ((bu - au) * (v - av)) / (bv - av);
    lo = Math.min(lo, u);
    hi = Math.max(hi, u);
  }
  return lo <= hi ? [lo, hi] : null;
}

/**
 * The largest local rect (integer cells) of a placement inside a convex
 * world polygon `poly` ([[x, y], ...] in order), its front (v = its y0)
 * as near the polygon's front (its smallest v) as it can be (up to
 * `frontSlack` cells behind it): depths `depth` [min, max] cells, widths
 * at least `minWidth` cells; the largest wins. For a convex polygon the u
 * range common to a band of depths is the one common to its two ends; a
 * span wider than `maxWidth` gives its middle. Null if nothing fits.
 */
export function fitLocalRect(p, poly, { depth = [1, Infinity], minWidth = 1, maxWidth = Infinity, frontSlack = 0 } = {}) {
  const loc = poly.map(([x, y]) => worldPointToLocal(p, x, y));
  let vMin = Infinity;
  let vMax = -Infinity;
  for (const [, v] of loc) {
    vMin = Math.min(vMin, v);
    vMax = Math.max(vMax, v);
  }
  let best = null;
  // the front: the first whole cell behind the polygon's front, give or take the slack
  for (let f = Math.ceil(vMin); f <= Math.ceil(vMin) + frontSlack; f += 1) {
    const a = spanAt(loc, f);
    if (!a) continue;
    for (let b = f + Math.max(1, depth[0]); b <= Math.min(Math.floor(vMax), f + depth[1]); b += 1) {
      const s = spanAt(loc, b);
      if (!s) break;
      let u0 = Math.ceil(Math.max(a[0], s[0]));
      let u1 = Math.floor(Math.min(a[1], s[1])) - 1;
      if (u1 - u0 + 1 < minWidth) continue;
      if (u1 - u0 + 1 > maxWidth) {
        // (wider than wanted: the middle of the span)
        u0 += Math.floor((u1 - u0 + 1 - maxWidth) / 2);
        u1 = u0 + maxWidth - 1;
      }
      const area = (u1 - u0 + 1) * (b - f);
      if (!best || area > best.area) best = { rect: { x0: u0, y0: f, x1: u1, y1: b - 1 }, area };
    }
  }
  return best ? best.rect : null;
}

/**
 * A convex polygon ([[x, y], ...]) clipped to the half-plane
 * nx x + ny y >= c (Sutherland-Hodgman), in order; fewer than 3 points
 * when nothing is left.
 */
export function clipHalfPlane(poly, nx, ny, c) {
  const out = [];
  for (let k = 0; k < poly.length; k += 1) {
    const a = poly[k];
    const b = poly[(k + 1) % poly.length];
    const da = nx * a[0] + ny * a[1] - c;
    const db = nx * b[0] + ny * b[1] - c;
    if (da >= 0) out.push(a);
    if ((da >= 0) !== (db >= 0)) {
      const t = da / (da - db);
      out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]);
    }
  }
  return out;
}
