import { nearLanding } from "./stairs.js";
import { rOverlaps } from "../../core/rect.js";

/**
 * Helpers shared by the floor planners.
 */

/** Paint the stairwell room of `stair` on a floor grid. */
export function addStairRoom(grid, stair) {
  return grid.addRoom("stair", [stair.rect], { stair: stair.id, paint: "PAINT_GRAY", floorMat: "STAIR_CONCRETE" });
}

/**
 * Door from a circulation room into a stairwell: must land on the near
 * landing. Tries the landing's walls that touch `circ`.
 */
export function stairDoor(grid, stairRoom, stair, circ, opts = {}) {
  const land = nearLanding(stair);
  const runs = grid.wallRuns(stairRoom, circ);
  const want = opts.width ?? 8;
  for (let width = want; width >= 6; width -= 1) {
    const d = tryStairDoor(grid, stairRoom, stair, circ, opts, land, runs, width);
    if (d) return d;
  }
  return null;
}

function tryStairDoor(grid, stairRoom, stair, circ, opts, land, runs, width) {
  let best = null;
  for (const run of runs) {
    // the run's cells along the wall that are adjacent to the landing
    let lo;
    let hi;
    if (run.orient === "v") {
      lo = Math.max(run.t0, land.y0);
      hi = Math.min(run.t1, land.y1);
    } else {
      lo = Math.max(run.t0, land.x0);
      hi = Math.min(run.t1, land.x1);
    }
    // wall must actually border the landing (fixed coordinate adjacent to landing rect)
    const adj =
      run.orient === "v"
        ? run.fixed === land.x1 + 1 || run.fixed + run.thick - 1 === land.x0 - 1
        : run.fixed === land.y1 + 1 || run.fixed + run.thick - 1 === land.y0 - 1;
    if (!adj) continue;
    if (hi - lo + 1 < width) continue;
    const t = Math.round((lo + hi - width + 1) / 2);
    best = { run, t };
    break;
  }
  if (!best) return null;
  const { run, t } = best;
  const d =
    run.orient === "v"
      ? { u0: run.fixed, u1: run.fixed + run.thick - 1, v0: t, v1: t + width - 1, orient: "v" }
      : { u0: t, u1: t + width - 1, v0: run.fixed, v1: run.fixed + run.thick - 1, orient: "h" };
  Object.assign(d, {
    a: stairRoom.id,
    b: circ ? circ.id : -1,
    kind: opts.kind ?? "stair",
    sideA: run.sideA,
    width,
    leaf: opts.leaf ?? (stair.open ? "none" : "metal"),
    id: grid.doors.length,
  });
  for (let v = d.v0; v <= d.v1; v += 1) for (let u = d.u0; u <= d.u1; u += 1) grid.set(u, v, 3);
  grid.doors.push(d);
  return d;
}

/** Split a length into pieces of ~target (min minW) separated by 1-cell walls. */
export function splitLength(a0, a1, target, minW, rng, jitter = 0.25) {
  const len = a1 - a0 + 1;
  let n = Math.max(1, Math.round((len + 1) / (target + 1)));
  while (n > 1 && (len - (n - 1)) / n < minW) n -= 1;
  const pieces = [];
  let a = a0;
  const avail = len - (n - 1);
  let used = 0;
  for (let k = 0; k < n; k += 1) {
    const remaining = n - k;
    let w;
    if (remaining === 1) w = avail - used;
    else {
      const ideal = (avail - used) / remaining;
      w = Math.round(ideal * (1 + (rng.next() - 0.5) * jitter));
      w = Math.max(minW, Math.min(w, avail - used - minW * (remaining - 1)));
      w = Math.round(w / 4) * 4 || minW;
    }
    pieces.push([a, a + w - 1]);
    a += w + 1;
    used += w;
  }
  pieces[pieces.length - 1][1] = a1;
  return pieces;
}

/** Sides of a rect that lie on the exterior wall of the grid. */
export function facadeSidesOf(grid, r) {
  const out = new Set();
  const probe = (u, v) => grid.get(u, v) === 1;
  if (probe(Math.round((r.x0 + r.x1) / 2), r.y0 - 1)) out.add("N");
  if (probe(Math.round((r.x0 + r.x1) / 2), r.y1 + 1)) out.add("S");
  if (probe(r.x0 - 1, Math.round((r.y0 + r.y1) / 2))) out.add("W");
  if (probe(r.x1 + 1, Math.round((r.y0 + r.y1) / 2))) out.add("E");
  return out;
}

export function rectsOverlapAny(r, list) {
  return list.some((x) => rOverlaps(x, r));
}

/** Cut a set of intervals [a0,a1] by blocked intervals, keeping gaps of 1 wall cell. */
export function freeIntervals(a0, a1, blocked) {
  const sorted = blocked.slice().sort((x, y) => x[0] - y[0]);
  const out = [];
  let a = a0;
  for (const [b0, b1] of sorted) {
    if (b1 < a) continue;
    if (b0 - 2 >= a) out.push([a, b0 - 2]);
    a = Math.max(a, b1 + 2);
  }
  if (a <= a1) out.push([a, a1]);
  return out;
}
