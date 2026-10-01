/**
 * Integer axis-aligned rectangles with INCLUSIVE bounds {x0, y0, x1, y1}.
 * All building/interior planning works on these on the voxel grid.
 */

export const rect = (x0, y0, x1, y1) => ({ x0, y0, x1, y1 });
export const rw = (r) => r.x1 - r.x0 + 1;
export const rh = (r) => r.y1 - r.y0 + 1;
export const rArea = (r) => (r.x1 < r.x0 || r.y1 < r.y0 ? 0 : rw(r) * rh(r));
export const rValid = (r) => r && r.x1 >= r.x0 && r.y1 >= r.y0;
export const rCopy = (r) => ({ x0: r.x0, y0: r.y0, x1: r.x1, y1: r.y1 });
export const rCenter = (r) => ({ x: (r.x0 + r.x1) / 2, y: (r.y0 + r.y1) / 2 });
export const rMinSide = (r) => Math.min(rw(r), rh(r));
export const rMaxSide = (r) => Math.max(rw(r), rh(r));
export const rKey = (r) => `${r.x0},${r.y0},${r.x1},${r.y1}`;

export function rContains(r, x, y) {
  return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

export function rContainsRect(outer, inner) {
  return inner.x0 >= outer.x0 && inner.x1 <= outer.x1 && inner.y0 >= outer.y0 && inner.y1 <= outer.y1;
}

export function rOverlaps(a, b) {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

export function rIntersect(a, b) {
  const r = {
    x0: Math.max(a.x0, b.x0),
    y0: Math.max(a.y0, b.y0),
    x1: Math.min(a.x1, b.x1),
    y1: Math.min(a.y1, b.y1),
  };
  return rValid(r) ? r : null;
}

export function rUnionBounds(a, b) {
  return {
    x0: Math.min(a.x0, b.x0),
    y0: Math.min(a.y0, b.y0),
    x1: Math.max(a.x1, b.x1),
    y1: Math.max(a.y1, b.y1),
  };
}

export function rBoundsOf(rects) {
  let out = null;
  for (const r of rects) out = out ? rUnionBounds(out, r) : rCopy(r);
  return out;
}

export function rInset(r, d) {
  return { x0: r.x0 + d, y0: r.y0 + d, x1: r.x1 - d, y1: r.y1 - d };
}

export function rInsetSides(r, { n = 0, s = 0, e = 0, w = 0 }) {
  // N = -y, S = +y, W = -x, E = +x
  return { x0: r.x0 + w, y0: r.y0 + n, x1: r.x1 - e, y1: r.y1 - s };
}

export function rTranslate(r, dx, dy) {
  return { x0: r.x0 + dx, y0: r.y0 + dy, x1: r.x1 + dx, y1: r.y1 + dy };
}

/** A minus B → up to 4 rects (non-overlapping). */
export function rSubtract(a, b) {
  const i = rIntersect(a, b);
  if (!i) return [rCopy(a)];
  const out = [];
  if (a.y0 < i.y0) out.push({ x0: a.x0, y0: a.y0, x1: a.x1, y1: i.y0 - 1 });
  if (i.y1 < a.y1) out.push({ x0: a.x0, y0: i.y1 + 1, x1: a.x1, y1: a.y1 });
  if (a.x0 < i.x0) out.push({ x0: a.x0, y0: i.y0, x1: i.x0 - 1, y1: i.y1 });
  if (i.x1 < a.x1) out.push({ x0: i.x1 + 1, y0: i.y0, x1: a.x1, y1: i.y1 });
  return out;
}

export function rSubtractAll(rects, cutters) {
  let current = rects.map(rCopy);
  for (const c of cutters) {
    const next = [];
    for (const r of current) next.push(...rSubtract(r, c));
    current = next;
  }
  return current;
}

/**
 * Split along an axis at coordinate `at` leaving a wall line of `gap` cells
 * between the two halves. axis 'x' means a vertical cut (splits x range).
 */
export function rSplit(r, axis, at, gap = 0) {
  if (axis === "x") {
    return [
      { x0: r.x0, y0: r.y0, x1: at - 1, y1: r.y1 },
      { x0: at + gap, y0: r.y0, x1: r.x1, y1: r.y1 },
    ];
  }
  return [
    { x0: r.x0, y0: r.y0, x1: r.x1, y1: at - 1 },
    { x0: r.x0, y0: at + gap, x1: r.x1, y1: r.y1 },
  ];
}

/** Side names: N (-y), S (+y), W (-x), E (+x). */
export const SIDES = ["N", "E", "S", "W"];
export const SIDE_DIR = { N: [0, -1], S: [0, 1], E: [1, 0], W: [-1, 0] };
export const OPPOSITE = { N: "S", S: "N", E: "W", W: "E" };
export const sideAxis = (side) => (side === "N" || side === "S" ? "y" : "x");

/** The 1-cell-thick strip of r lying along `side` (inside r). */
export function rEdgeStrip(r, side, depth = 1) {
  switch (side) {
    case "N":
      return { x0: r.x0, y0: r.y0, x1: r.x1, y1: r.y0 + depth - 1 };
    case "S":
      return { x0: r.x0, y0: r.y1 - depth + 1, x1: r.x1, y1: r.y1 };
    case "W":
      return { x0: r.x0, y0: r.y0, x1: r.x0 + depth - 1, y1: r.y1 };
    default:
      return { x0: r.x1 - depth + 1, y0: r.y0, x1: r.x1, y1: r.y1 };
  }
}

/** Strip just outside r along side. */
export function rOuterStrip(r, side, depth = 1) {
  switch (side) {
    case "N":
      return { x0: r.x0, y0: r.y0 - depth, x1: r.x1, y1: r.y0 - 1 };
    case "S":
      return { x0: r.x0, y0: r.y1 + 1, x1: r.x1, y1: r.y1 + depth };
    case "W":
      return { x0: r.x0 - depth, y0: r.y0, x1: r.x0 - 1, y1: r.y1 };
    default:
      return { x0: r.x1 + 1, y0: r.y0, x1: r.x1 + depth, y1: r.y1 };
  }
}

/** Length of the side of r. */
export function rSideLength(r, side) {
  return side === "N" || side === "S" ? rw(r) : rh(r);
}

/**
 * Shared boundary between two rects separated by a wall line of thickness
 * `gap` (0 = touching). Returns {axis, fixed, a0, a1, sideOfA} or null.
 * For axis 'x' the wall runs along y at column `fixed`... we express the
 * wall as the cells strictly between them.
 */
export function rSharedWall(a, b, gap = 1) {
  // b to the east of a
  if (b.x0 - a.x1 - 1 === gap) {
    const y0 = Math.max(a.y0, b.y0);
    const y1 = Math.min(a.y1, b.y1);
    if (y0 <= y1) return { orient: "v", x0: a.x1 + 1, x1: b.x0 - 1, t0: y0, t1: y1, sideOfA: "E" };
  }
  if (a.x0 - b.x1 - 1 === gap) {
    const y0 = Math.max(a.y0, b.y0);
    const y1 = Math.min(a.y1, b.y1);
    if (y0 <= y1) return { orient: "v", x0: b.x1 + 1, x1: a.x0 - 1, t0: y0, t1: y1, sideOfA: "W" };
  }
  if (b.y0 - a.y1 - 1 === gap) {
    const x0 = Math.max(a.x0, b.x0);
    const x1 = Math.min(a.x1, b.x1);
    if (x0 <= x1) return { orient: "h", y0: a.y1 + 1, y1: b.y0 - 1, t0: x0, t1: x1, sideOfA: "S" };
  }
  if (a.y0 - b.y1 - 1 === gap) {
    const x0 = Math.max(a.x0, b.x0);
    const x1 = Math.min(a.x1, b.x1);
    if (x0 <= x1) return { orient: "h", y0: b.y1 + 1, y1: a.y0 - 1, t0: x0, t1: x1, sideOfA: "N" };
  }
  return null;
}
