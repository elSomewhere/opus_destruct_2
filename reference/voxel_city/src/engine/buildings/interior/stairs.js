/**
 * U-shaped (switchback) stairs on the voxel grid.
 *
 * Every riser is 1 voxel (12.5 cm) and every tread 2 voxels (25 cm), so a
 * walker with a 1-2 voxel step-up climbs any stair. A stair stacks through
 * floors f0..f1: from each floor f < f1 a first flight climbs half a story
 * along lane A to a mid landing at the far end, a second flight returns
 * along lane B to the NEAR landing of floor f+1, which is part of that
 * floor's slab (the door into the stairwell is always at the near end).
 *
 * Local stair frame: s runs along the flights from the near end (s=0),
 * t runs across (lane A = [0, lane-1], divider = lane, lane B beyond).
 */

export const LANE = 8; // 1.0 m
export const LANDING = 9; // 1.125 m

/** Inner stairwell size for a story height H (voxels). */
export function stairDims(H, lane = LANE, landing = LANDING) {
  const n1 = Math.ceil(H / 2);
  const W = 2 * lane + 1;
  const L = landing + 2 * (n1 - 1) + Math.max(landing, lane + 1);
  return { W, L, lane, landing };
}

/**
 * @param rect   stairwell interior (building canonical)
 * @param axis   'u' | 'v' run axis
 * @param dir    +1 near end at the low coordinate, -1 at the high coordinate
 * @param laneLow lane A at the low across-coordinate
 */
export function makeStair({ rect, axis, dir, laneLow = true, lane = LANE, landing = LANDING, f0, f1, open = false }) {
  const along = axis === "v" ? rect.y1 - rect.y0 + 1 : rect.x1 - rect.x0 + 1;
  const across = axis === "v" ? rect.x1 - rect.x0 + 1 : rect.y1 - rect.y0 + 1;
  return { rect: { ...rect }, axis, dir, laneLow, lane, landing, f0, f1, L: along, W: across, open };
}

/** Canonical (u,v) -> local (s,t); null if outside. */
export function stairLocal(st, u, v) {
  const r = st.rect;
  if (u < r.x0 || u > r.x1 || v < r.y0 || v > r.y1) return null;
  let s;
  let t;
  if (st.axis === "v") {
    s = st.dir > 0 ? v - r.y0 : r.y1 - v;
    t = st.laneLow ? u - r.x0 : r.x1 - u;
  } else {
    s = st.dir > 0 ? u - r.x0 : r.x1 - u;
    t = st.laneLow ? v - r.y0 : r.y1 - v;
  }
  return { s, t };
}

/** Local (s0..s1, t0..t1) -> canonical rect. */
function localRect(st, s0, s1, t0, t1) {
  const r = st.rect;
  let u0;
  let u1;
  let v0;
  let v1;
  if (st.axis === "v") {
    if (st.dir > 0) {
      v0 = r.y0 + s0;
      v1 = r.y0 + s1;
    } else {
      v0 = r.y1 - s1;
      v1 = r.y1 - s0;
    }
    if (st.laneLow) {
      u0 = r.x0 + t0;
      u1 = r.x0 + t1;
    } else {
      u0 = r.x1 - t1;
      u1 = r.x1 - t0;
    }
  } else {
    if (st.dir > 0) {
      u0 = r.x0 + s0;
      u1 = r.x0 + s1;
    } else {
      u0 = r.x1 - s1;
      u1 = r.x1 - s0;
    }
    if (st.laneLow) {
      v0 = r.y0 + t0;
      v1 = r.y0 + t1;
    } else {
      v0 = r.y1 - t1;
      v1 = r.y1 - t0;
    }
  }
  return { x0: u0, y0: v0, x1: u1, y1: v1 };
}

/** The near-landing rect (canonical) — where the stair door must be. */
export function nearLanding(st) {
  return localRect(st, 0, st.landing - 1, 0, st.W - 1);
}

/** Is the slab of floor f open (no floor) at (u,v)? */
export function stairSlabOpen(st, f, u, v) {
  if (f <= st.f0 || f > st.f1) return false;
  const p = stairLocal(st, u, v);
  return !!p && p.s >= st.landing;
}

/**
 * Geometry boxes for the stair, canonical u/v with absolute z, one U-turn
 * per flight record {f, z0, H}. Box: { x0,y0,z0,x1,y1,z1, m }.
 */
export function stairBoxes(st, mats) {
  const out = [];
  const lane = st.lane;
  const W = st.W;
  const add = (s0, s1, t0, t1, z0, z1, m) => {
    if (s1 < s0 || t1 < t0 || z1 < z0) return;
    const r = localRect(st, s0, s1, t0, t1);
    out.push({ x0: r.x0, y0: r.y0, z0, x1: r.x1, y1: r.y1, z1, m });
  };
  const tA0 = 0;
  const tA1 = lane - 1;
  const tB0 = lane + 1;
  const tB1 = W - 1;
  for (const fl of st.flights) {
    const z0 = fl.z0;
    const H = fl.H;
    const base = z0 + 1; // level-0 walking surface voxel
    const n1 = Math.ceil(H / 2);
    const n2 = H - n1;
    const lowest = fl.f === st.f0;
    // flight 1, lane A
    for (let k = 1; k < n1; k += 1) {
      const s = st.landing + 2 * (k - 1);
      const top = base + k;
      add(s, s + 1, tA0, tA1, lowest ? z0 : Math.max(z0, top - 3), top, mats.tread);
    }
    // mid landing
    const sA = st.landing + 2 * (n1 - 1);
    const sB = st.landing + 2 * (n2 - 1);
    const lTop = base + n1;
    add(sA, st.L - 1, tA0, tA1, lTop - 2, lTop, mats.landing);
    add(Math.min(sA, sB), st.L - 1, lane, lane, lTop - 2, lTop, mats.landing);
    add(sB, st.L - 1, tB0, tB1, lTop - 2, lTop, mats.landing);
    // flight 2, lane B (descending towards the near end)
    for (let j = 1; j < n2; j += 1) {
      const s = st.landing + 2 * (n2 - 1 - j);
      const top = base + n1 + j;
      add(s, s + 1, tB0, tB1, Math.max(z0, top - 3), top, mats.tread);
    }
    // divider between the flights
    const dEnd = Math.min(sA, sB) - 1;
    // (a full-height divider keeps both flights safe to walk; open stairs use a lighter material)
    if (dEnd >= st.landing) add(st.landing, dEnd, lane, lane, z0, z0 + H - 1, st.open ? mats.rail : mats.divider);
  }
  return out;
}
