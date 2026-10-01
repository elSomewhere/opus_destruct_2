/** Continuous 2D geometry helpers (points are {x, y} in voxel units). */

/**
 * Project p onto segment ab. Returns { t (0..1 clamped), dist, side, along }
 * where `side` is the signed lateral offset (positive = left of a→b) and
 * `along` the distance from a along the segment.
 */
export function projectToSegment(px, py, ax, ay, bx, by) {
  const dx = bx - ax;
  const dy = by - ay;
  const len2 = dx * dx + dy * dy;
  let t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
  const tc = t < 0 ? 0 : t > 1 ? 1 : t;
  const cx = ax + dx * tc;
  const cy = ay + dy * tc;
  const len = Math.sqrt(len2);
  const side = len > 0 ? ((px - ax) * -dy + (py - ay) * dx) / len : 0;
  return { t: tc, rawT: t, dist: Math.hypot(px - cx, py - cy), side, along: t * len, len, cx, cy };
}

export function distToSegment(px, py, ax, ay, bx, by) {
  return projectToSegment(px, py, ax, ay, bx, by).dist;
}

/** Centripetal-ish uniform Catmull-Rom through control points, sampled. */
export function catmullRom(points, samplesPerSpan = 8, closed = false) {
  const out = [];
  const n = points.length;
  if (n < 2) return points.slice();
  const get = (i) => {
    if (closed) return points[((i % n) + n) % n];
    return points[Math.max(0, Math.min(n - 1, i))];
  };
  const spans = closed ? n : n - 1;
  for (let i = 0; i < spans; i += 1) {
    const p0 = get(i - 1);
    const p1 = get(i);
    const p2 = get(i + 1);
    const p3 = get(i + 2);
    for (let s = 0; s < samplesPerSpan; s += 1) {
      const t = s / samplesPerSpan;
      const t2 = t * t;
      const t3 = t2 * t;
      const f = (a, b, c, d) =>
        0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
      const pt = { x: f(p0.x, p1.x, p2.x, p3.x), y: f(p0.y, p1.y, p2.y, p3.y) };
      if (p1.z !== undefined) pt.z = f(p0.z ?? p1.z, p1.z, p2.z ?? p1.z, p3.z ?? p2.z ?? p1.z);
      out.push(pt);
    }
  }
  if (!closed) out.push({ ...points[n - 1] });
  return out;
}

/** Cumulative arc lengths for a polyline. */
export function polylineLengths(points) {
  const acc = [0];
  for (let i = 1; i < points.length; i += 1) {
    acc.push(acc[i - 1] + Math.hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y));
  }
  return acc;
}

/** Point + tangent at arc distance s along polyline. */
export function polylineAt(points, lengths, s) {
  const total = lengths[lengths.length - 1];
  const sc = s < 0 ? 0 : s > total ? total : s;
  let lo = 0;
  let hi = lengths.length - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (lengths[mid] <= sc) lo = mid;
    else hi = mid;
  }
  const a = points[lo];
  const b = points[hi];
  const segLen = lengths[hi] - lengths[lo] || 1;
  const t = (sc - lengths[lo]) / segLen;
  const tx = (b.x - a.x) / segLen;
  const ty = (b.y - a.y) / segLen;
  const out = { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t, tx, ty };
  if (a.z !== undefined && b.z !== undefined) out.z = a.z + (b.z - a.z) * t;
  return out;
}

/** Axis-aligned bounds of points padded by `pad`. */
export function pointsBounds(points, pad = 0) {
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (const p of points) {
    if (p.x < x0) x0 = p.x;
    if (p.y < y0) y0 = p.y;
    if (p.x > x1) x1 = p.x;
    if (p.y > y1) y1 = p.y;
  }
  return { x0: Math.floor(x0 - pad), y0: Math.floor(y0 - pad), x1: Math.ceil(x1 + pad), y1: Math.ceil(y1 + pad) };
}

/**
 * Uniform-grid spatial hash over axis-aligned bounded items. Items are any
 * objects; `boundsOf(item)` returns an inclusive {x0,y0,x1,y1}.
 */
export class SpatialGrid {
  constructor(cellSize = 256) {
    this.cellSize = cellSize;
    this.cells = new Map();
    this.items = [];
  }

  key(cx, cy) {
    return cx * 73856093 + cy * 19349663;
  }

  insert(item, b) {
    const cs = this.cellSize;
    const cx0 = Math.floor(b.x0 / cs);
    const cy0 = Math.floor(b.y0 / cs);
    const cx1 = Math.floor(b.x1 / cs);
    const cy1 = Math.floor(b.y1 / cs);
    const entry = { item, b, stamp: 0 };
    this.items.push(entry);
    for (let cy = cy0; cy <= cy1; cy += 1) {
      for (let cx = cx0; cx <= cx1; cx += 1) {
        const k = this.key(cx, cy);
        let list = this.cells.get(k);
        if (!list) {
          list = [];
          this.cells.set(k, list);
        }
        list.push(entry);
      }
    }
  }

  /** Items whose bounds overlap the query rect. */
  query(q, out = []) {
    const cs = this.cellSize;
    const cx0 = Math.floor(q.x0 / cs);
    const cy0 = Math.floor(q.y0 / cs);
    const cx1 = Math.floor(q.x1 / cs);
    const cy1 = Math.floor(q.y1 / cs);
    const stamp = (this._stamp = (this._stamp ?? 0) + 1);
    for (let cy = cy0; cy <= cy1; cy += 1) {
      for (let cx = cx0; cx <= cx1; cx += 1) {
        const list = this.cells.get(this.key(cx, cy));
        if (!list) continue;
        for (const e of list) {
          if (e.stamp === stamp) continue;
          e.stamp = stamp;
          const b = e.b;
          if (b.x0 <= q.x1 && q.x0 <= b.x1 && b.y0 <= q.y1 && q.y0 <= b.y1) out.push(e.item);
        }
      }
    }
    return out;
  }

  queryPoint(x, y, out = []) {
    const list = this.cells.get(this.key(Math.floor(x / this.cellSize), Math.floor(y / this.cellSize)));
    if (!list) return out;
    for (const e of list) {
      const b = e.b;
      if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) out.push(e.item);
    }
    return out;
  }
}
