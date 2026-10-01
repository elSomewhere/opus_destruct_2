/**
 * A building's chamfer (ANGLED_WORLD_PLAN.md S5, buildings/wings.js): the
 * front corner at a street corner cut off along a line of the 20-21-29
 * triple, `c` { side: "L" | "R" (the front's left or right end), a (the
 * leg along the front), b (the leg along the side street) }, the legs 20
 * and 21 times k in either order: the cut faces the corner at 43.6° or
 * 46.4° to both streets (no table yaw is 45°), the length of its facade
 * 29 k cells. Every floor from the ground floor up loses the cells whose
 * centres lie in front of the line through (a, 0) and (0, b) (mirrored at
 * the right end, through (U - a, 0) and (U, b)); a slab part on the line
 * carries the facade (wings.js). Pure integers: (2u + 1) b + (2v + 1) a
 * against 2 a b, never a tie (divided by k, the left side is odd, the
 * right 840 k even).
 */

/** Is canonical cell (u, v) of a building U cells wide in front of its chamfer `c` (cut off)? */
export function chamferCut(c, U, u, v) {
  const x = c.side === "L" ? u : U - 1 - u;
  return c.b * (2 * x + 1) + c.a * (2 * v + 1) < 2 * c.a * c.b;
}

/**
 * Does a canonical rect (cells, any extent, outside the footprint too)
 * reach in front of the chamfer within its corner's quadrant (u within the
 * front leg, v within the side leg: the cut, and the sidewalk before it)?
 */
export function chamferHits(c, U, r) {
  // (the rect's cell nearest the corner decides: in front of the line is monotone towards it)
  const x = c.side === "L" ? r.x0 : U - 1 - r.x1;
  if (x >= c.a || r.y0 >= c.b) return false;
  return c.b * (2 * x + 1) + c.a * (2 * r.y0 + 1) < 2 * c.a * c.b;
}
