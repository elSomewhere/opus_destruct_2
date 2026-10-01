import { stairDims, makeStair } from "./stairs.js";
import { addStairRoom, stairDoor } from "./common.js";
import { tierRects } from "../archetypes.js";
import { EXT_T } from "./grid.js";
import { rSubtractAll } from "../../core/rect.js";
import { PITCHES } from "../../core/placement.js";

/**
 * Multi-storey parking garage.
 *
 * Open decks around a stack of straight ramps along one side wall: the ramp
 * of deck f climbs from the front cross aisle to the back cross aisle of
 * deck f+1, so every deck repeats the same circulation (up the ramp, back
 * along the main aisle, up the next ramp). A stair core in the back corner
 * on the other side serves pedestrians. Stall rows (parked cars, painted
 * lines) and pillar lines fill the deck between the aisles.
 *
 * Plan features beyond rooms:
 *   plan.ramps  [{rect, f, H, pitch?}]   rising towards +v from floor f to f+1
 *                                (pitch: the table pitch of a pitched ramp)
 *   plan.links  [[fa, ra], [fb, rb]] room connections that are not doors
 */

const RW = 30; // ramp width (3.75 m)
const STALL_W = 20; // 2.5 m
const STALL_D = 40; // 5 m
const AISLE = 48; // 6 m
const CROSS = 56; // cross aisles at the front and back (7 m)
/** Table pitches a pitched ramp may take, gentlest first: 12.5%, 14.4%, 16.8% (1:8 to 1:6). */
const RAMP_PITCHES = [3, 4, 5];

export function planGarage({ env, rng, pb }) {
  const nF = env.floors;
  const H = env.storyH[0];
  const fp = tierRects(env, 0)[0];
  const inner = { x0: fp.x0 + EXT_T, y0: fp.y0 + EXT_T, x1: fp.x1 - EXT_T, y1: fp.y1 - EXT_T };
  const Vi = inner.y1 - inner.y0 + 1;
  const rampLeft = env.mirror;
  // ramp: slope 1:8 when there is room, never steeper than 1:6; in the
  // angled world (`env.pitchedRamps`) the gentlest table grade that fits, its
  // run H c / s, so a pitched slab part rises exactly a storey (cellPlan)
  let L = Math.max(6 * H, Math.min(8 * H, Vi - 2 * CROSS));
  let pitch = 0;
  if (env.pitchedRamps) {
    for (const p of RAMP_PITCHES) {
      const run = Math.round((H * PITCHES[p].c) / PITCHES[p].s);
      if (run <= Vi - 2 * CROSS) {
        [L, pitch] = [run, p];
        break;
      }
    }
  }
  const ra = inner.y0 + Math.floor((Vi - L) / 2);
  const ramp = rampLeft
    ? { x0: inner.x0, x1: inner.x0 + RW - 1, y0: ra, y1: ra + L - 1 }
    : { x0: inner.x1 - RW + 1, x1: inner.x1, y0: ra, y1: ra + L - 1 };
  // stair core in the back corner on the other side, landing towards the deck
  const sd = stairDims(H);
  const sr = rampLeft
    ? { x0: inner.x1 - sd.W + 1, x1: inner.x1, y0: inner.y1 - sd.L + 1, y1: inner.y1 }
    : { x0: inner.x0, x1: inner.x0 + sd.W - 1, y0: inner.y1 - sd.L + 1, y1: inner.y1 };
  const stair = pb.addStair(makeStair({ rect: sr, axis: "v", dir: 1, laneLow: rng.chance(0.5), f0: 0, f1: nF }));
  const deckRects = rSubtractAll([inner], [{ x0: sr.x0 - 1, y0: sr.y0 - 1, x1: sr.x1 + 1, y1: sr.y1 + 1 }]).filter((r) => r.x1 >= r.x0 && r.y1 >= r.y0);
  const layout = stallLayout(inner, ramp, sr, rampLeft, rng);

  let typical = null;
  const decks = [];
  for (let f = 0; f < nF; f += 1) {
    let grid = f > 0 ? typical : null;
    if (!grid) {
      grid = pb.newGrid(f);
      const stairRoom = addStairRoom(grid, stair);
      const deck = grid.addRoom("deck", deckRects, { paint: "CONCRETE", floorMat: "FLOOR_CONCRETE", ...layout, deckH: H });
      stairDoor(grid, stairRoom, stair, deck, { width: 8 });
      if (f === 0) {
        // vehicle entrance in line with the ramp aisle, pedestrian door beside it
        const cu = rampLeft ? ramp.x1 + 1 + AISLE / 2 : ramp.x0 - 1 - AISLE / 2;
        grid.addDoor(deck, null, { kind: "entrance", width: 40, place: "near", near: { u: cu, v: 0 }, leaf: "none", height: 20 });
        grid.addDoor(deck, null, { kind: "entrance", width: 8, place: "near", near: { u: rampLeft ? inner.x1 - 24 : inner.x0 + 24, v: 0 }, leaf: "metal" });
      } else typical = grid;
    }
    decks.push(pb.addFloor(f, grid, "garage"));
  }
  for (let f = 0; f + 1 < nF; f += 1) {
    const r = pb.addRamp({ rect: { ...ramp }, f, H: pb.h(f), ...(pitch ? { pitch } : {}) });
    const a = decks[f].grid.roomAt(ramp.x0, ramp.y0);
    const b = decks[f + 1].grid.roomAt(ramp.x0, ramp.y1);
    pb.links.push([[f, a.id], [f + 1, b.id], { ramp: r.id }]);
  }
}

/**
 * Stall rows as bands across u, from the wall opposite the ramp: pairs of
 * back-to-back rows share an aisle (row | aisle | row), whatever is left
 * next to the ramp becomes the main aisle. Rows run along v between the
 * cross aisles; pillars stand on the line between back-to-back rows.
 */
function stallLayout(inner, ramp, sr, rampLeft, rng) {
  const stalls = [];
  const pillars = [];
  const v0 = inner.y0 + CROSS;
  const v1 = inner.y1 - CROSS;
  // bands in "distance from the far wall" coordinates
  const avail = inner.x1 - inner.x0 + 1 - RW - AISLE;
  const bands = [];
  let d = 0;
  let facing = 1; // +1: nose towards increasing d
  while (d + STALL_D <= avail) {
    bands.push({ d0: d, d1: d + STALL_D - 1, nose: facing });
    d += STALL_D;
    if (facing === 1) {
      if (d + AISLE + STALL_D > avail) break;
      d += AISLE;
      facing = -1;
    } else {
      pillars.push(d);
      facing = 1;
    }
  }
  const toU = (dd) => (rampLeft ? inner.x1 - dd : inner.x0 + dd);
  const stairBox = { x0: sr.x0 - 2, y0: sr.y0 - 14, x1: sr.x1 + 2, y1: sr.y1 };
  for (const b of bands) {
    const ua = toU(b.d0);
    const ub = toU(b.d1);
    const u0 = Math.min(ua, ub);
    const u1 = Math.max(ua, ub);
    // nose direction in canonical u
    const noseU = rampLeft ? -b.nose : b.nose;
    for (let v = v0; v + STALL_W - 1 <= v1; v += STALL_W) {
      const s = { x0: u0, x1: u1, y0: v, y1: v + STALL_W - 1, noseU, car: rng.chance(0.65) };
      if (s.x1 >= stairBox.x0 && s.x0 <= stairBox.x1 && s.y1 >= stairBox.y0 && s.y0 <= stairBox.y1) continue;
      stalls.push(s);
    }
  }
  const cols = [];
  for (const pd of pillars) {
    const u = toU(pd);
    for (let v = v0 + STALL_W * 2; v < v1 - 8; v += STALL_W * 3) cols.push({ x0: u - 2, x1: u + 1, y0: v - 2, y1: v + 1 });
  }
  return { stalls, pillars: cols, rampRect: { ...ramp } };
}
