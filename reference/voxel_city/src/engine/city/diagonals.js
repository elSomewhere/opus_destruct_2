import { hashFloat } from "../core/hash.js";
import { vx } from "../core/units.js";
import { YAWS, nearestYaw } from "../core/placement.js";

/**
 * Diagonal boulevards (ANGLED_WORLD_PLAN.md S1): global lines at a fixed
 * exact yaw laid over the arterial grid, which itself stays as it is
 * (network/arterials.js, §4.4). Like the arterial lines, line k of a family
 * depends only on its index, so every cell clips the lines crossing it on
 * its own and the pieces of a line in two cells meet exactly on their
 * shared edge.
 *
 * Two families cross each other at 73.7° and the arterial grid at 36.9° and
 * 53.1°, the 3-4-5 triple and its mirror: steep enough that every crossing
 * is a proper junction (network/roadView.js). With direction (c, s) / r a
 * line is { p : -s x + c y = D } in integers; D = r x (signed distance of
 * the line from the origin, voxels), about `angles.diagonalSpacing` apart.
 * The piece of a line in a cell is built where it runs through a town.
 */

export const DIAGONAL_FAMILIES = [nearestYaw(4, 3), nearestYaw(4, -3)].map((yaw, f) => ({ f, yaw, ...YAWS[yaw] }));

const JITTER = 0.25;

/** Offset D (integer, r x voxels) of line k of family `fam`. */
export function diagonalOffset(seed, fam, k, spacing) {
  const j = (hashFloat(seed, k, fam.f, 0xd1a6) - 0.5) * 2 * JITTER * spacing;
  return Math.round(fam.r * (k * spacing + j));
}

/** Do diagonals exist in this world (angled streets on a flat chart)? */
export function diagonalsOn(config) {
  const a = config.world.angles;
  return !!(a?.enabled && a.features?.roads !== false && (config.world.chart ?? "flat") === "flat");
}

/**
 * The pieces of diagonal lines crossing cell rect `rect` ([x0, x1) x
 * [y0, y1), arterial line positions): [{ fam, k, D, a: {x, y}, b: {x, y} }]
 * with a -> b along the family's direction. Pure: no neighbour asked.
 */
export function diagonalPieces(config, rect) {
  const spacing = vx(config.world.angles.diagonalSpacing ?? 2000);
  const out = [];
  for (const fam of DIAGONAL_FAMILIES) {
    const L = (x, y) => -fam.s * x + fam.c * y;
    const vals = [L(rect.x0, rect.y0), L(rect.x1, rect.y0), L(rect.x0, rect.y1), L(rect.x1, rect.y1)];
    const lo = Math.min(...vals);
    const hi = Math.max(...vals);
    const k0 = Math.floor(lo / fam.r / spacing - JITTER) - 1;
    const k1 = Math.ceil(hi / fam.r / spacing + JITTER) + 1;
    for (let k = k0; k <= k1; k += 1) {
      const D = diagonalOffset(config.seed, fam, k, spacing);
      if (D <= lo || D >= hi) continue;
      // crossings with the cell's four edge lines (each edge line computed
      // the same way by both cells that share it)
      const pts = [];
      for (const x of [rect.x0, rect.x1]) {
        const y = (D + fam.s * x) / fam.c;
        if (y >= rect.y0 && y <= rect.y1) pts.push({ x, y });
      }
      for (const y of [rect.y0, rect.y1]) {
        const x = (fam.c * y - D) / fam.s;
        if (x >= rect.x0 && x <= rect.x1) pts.push({ x, y });
      }
      if (pts.length < 2) continue;
      // the two ends along the family's direction
      pts.sort((p, q) => fam.c * p.x + fam.s * p.y - (fam.c * q.x + fam.s * q.y));
      const a = pts[0];
      const b = pts[pts.length - 1];
      if (Math.hypot(b.x - a.x, b.y - a.y) < 1) continue;
      out.push({ fam, k, D, a, b });
    }
  }
  return out;
}
