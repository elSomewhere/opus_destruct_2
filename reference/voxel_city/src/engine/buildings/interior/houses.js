import { stairDims, makeStair } from "./stairs.js";
import { addStairRoom, stairDoor, splitLength } from "./common.js";
import { tierRects } from "../archetypes.js";
import { EXT_T } from "./grid.js";
import { pickUnitStyle, floorFor } from "./units.js";

/**
 * Houses and row houses: three columns across the width
 *
 *   [ stair column | hall | rooms column ]   (mirrored for half the houses)
 *
 * The hall runs the full depth from the front door, so every room on every
 * floor opens onto it; the open U-stair sits mid-depth in its column with
 * small rooms (closet, WC, laundry, bath) in front of and behind it.
 */

const M = 8;
export const HOUSE_LANE = 7;
export const HOUSE_LANDING = 8;
const HALL_W = 10;

/** Column layout of a house; `mirror` swaps left/right. Canonical u ranges. */
export function houseColumns(U, mirror) {
  const i0 = EXT_T;
  const i1 = U - 1 - EXT_T;
  const sw = 2 * HOUSE_LANE + 1;
  let stairCol = [i0, i0 + sw - 1];
  let hall = [stairCol[1] + 2, stairCol[1] + 1 + HALL_W];
  let rooms = [hall[1] + 2, i1];
  if (mirror) {
    const flip = ([a, b]) => [i0 + i1 - b, i0 + i1 - a];
    stairCol = flip(stairCol);
    hall = flip(hall);
    rooms = flip(rooms);
  }
  return { stairCol, hall, rooms, entranceU: Math.round((hall[0] + hall[1]) / 2) };
}

/**
 * @param frontDoorMargin corner margin of the front door in the 10-cell hall
 *   (an 8-cell door fits with a margin of 1; 2 would leave no room for it)
 */
export function planHouse({ env, rng, pb, frontDoorMargin = 1 }) {
  const nF = env.floors;
  const rect = tierRects(env, 0)[0];
  const inner = { x0: rect.x0 + EXT_T, y0: rect.y0 + EXT_T, x1: rect.x1 - EXT_T, y1: rect.y1 - EXT_T };
  const cols = houseColumns(env.U, !!env.mirror);
  if (cols.rooms[1] - cols.rooms[0] + 1 < 3 * M) return fallbackHouse({ env, rng, pb, inner });
  const fBottom = -env.basements;
  const multi = nF + env.basements > 1;
  let stair = null;
  if (multi) {
    let maxH = env.basements ? env.basementH : 0;
    for (const h of env.storyH) maxH = Math.max(maxH, h);
    const sd = stairDims(maxH, HOUSE_LANE, HOUSE_LANDING);
    const Vi = inner.y1 - inner.y0 + 1;
    const sv0 = inner.y0 + Math.max(0, Math.round((Vi - sd.L) / 2));
    const sRect = { x0: cols.stairCol[0], y0: sv0, x1: cols.stairCol[1], y1: Math.min(inner.y1, sv0 + sd.L - 1) };
    stair = pb.addStair(
      makeStair({ rect: sRect, axis: "v", dir: 1, laneLow: !env.mirror, lane: HOUSE_LANE, landing: HOUSE_LANDING, f0: fBottom, f1: nF - 1, open: true }),
    );
  }
  const style = pickUnitStyle(rng);
  for (let f = fBottom; f < nF; f += 1) {
    const grid = pb.newGrid(f);
    const kind = f < 0 ? "basement" : f === 0 ? "ground" : "upper";
    planHouseFloor(grid, { env, rng: rng.fork(`hf${f}`), inner, cols, stair, f, kind, style, nF, frontDoorMargin });
    pb.addFloor(f, grid, kind);
  }
}

function planHouseFloor(grid, ctx) {
  const { inner, cols, stair, kind, style, rng, f, nF, frontDoorMargin } = ctx;
  const mat = (type) => ({ paint: type === "bath" || type === "wc" ? style.tile : style.paint, floorMat: floorFor(type, style) });
  const hallRect = { x0: cols.hall[0], y0: inner.y0, x1: cols.hall[1], y1: inner.y1 };
  const hall = grid.addRoom(kind === "basement" ? "hall" : "hall", [hallRect], { ...mat("hall"), floorMat: kind === "basement" ? "FLOOR_CONCRETE" : style.wood });
  const hasStair = stair && f >= stair.f0 && f <= stair.f1;
  // stair column
  const sc = { x0: cols.stairCol[0], x1: cols.stairCol[1] };
  const smallRooms = [];
  if (hasStair) {
    const sRoom = addStairRoom(grid, stair);
    sRoom.paint = style.paint;
    sRoom.floorMat = style.wood;
    stairDoor(grid, sRoom, stair, hall, { kind: "opening", width: 8, leaf: "none" });
    const front = { ...sc, y0: inner.y0, y1: stair.rect.y0 - 2 };
    const back = { ...sc, y0: stair.rect.y1 + 2, y1: inner.y1 };
    const frontType = kind === "ground" ? "wc" : kind === "basement" ? "storage" : "closet";
    const backType = kind === "ground" ? rng.pick(["laundry", "pantry"]) : kind === "basement" ? "mechanical" : "bath";
    if (front.y1 - front.y0 + 1 >= 10) smallRooms.push(grid.addRoom(frontType, [front], mat(frontType)));
    if (back.y1 - back.y0 + 1 >= 10) smallRooms.push(grid.addRoom(backType, [back], mat(backType)));
  } else {
    const types = kind === "ground" ? ["closet", "wc", "laundry"] : ["closet", "bath"];
    for (const [a, b] of splitLength(inner.y0, inner.y1, 14 * 1, 12, rng).slice(0, 3)) {
      smallRooms.push(grid.addRoom(types[smallRooms.length % types.length], [{ ...sc, y0: a, y1: b }], mat(types[smallRooms.length % types.length])));
    }
  }
  for (const r of smallRooms) if (!grid.addDoor(r, hall, { width: 6, margin: 1 })) r.type = "shaft";

  // rooms column
  const rc = { x0: cols.rooms[0], x1: cols.rooms[1] };
  const Vi = inner.y1 - inner.y0 + 1;
  let program;
  if (kind === "basement") program = [["storage", 1], ["mechanical", 0.6]];
  else if (kind === "ground") program = Vi > 13 * M ? [["living", 1.3], ["dining", 0.9], ["kitchen", 1]] : [["living", 1.2], ["kitchen", 1]];
  else if (f === nF - 1 && nF >= 3 && rng.chance(0.5)) program = [["bedroom", 1.2], ["study", 0.8], ["bath", 0.55]];
  else program = Vi > 12 * M ? [["bedroom", 1.1], ["bath", 0.55], ["bedroom", 1]] : [["bedroom", 1], ["bedroom", 1]];
  const total = program.reduce((s, p) => s + p[1], 0);
  const avail = Vi - (program.length - 1);
  let v = inner.y0;
  const rooms = [];
  program.forEach(([type, w], k) => {
    const last = k === program.length - 1;
    const d = last ? inner.y1 - v + 1 : Math.max(14, Math.round((avail * w) / total));
    const r = grid.addRoom(type, [{ ...rc, y0: v, y1: v + d - 1 }], mat(type));
    rooms.push(r);
    v += d + 1;
  });
  for (const r of rooms) {
    const small = r.type === "bath" || r.type === "wc";
    if (!grid.addDoor(r, hall, { width: small ? 6 : 7 })) grid.addDoor(r, hall, { width: 6, margin: 1 });
  }
  // open kitchen / dining connections
  const liv = rooms.find((r) => r.type === "living");
  const din = rooms.find((r) => r.type === "dining");
  const kit = rooms.find((r) => r.type === "kitchen");
  if (liv && din) grid.addDoor(liv, din, { kind: "opening", width: 14, place: "center" });
  if (din && kit) grid.addDoor(din, kit, { kind: "opening", width: 12, place: "center" });
  if (liv && kit && !din) grid.addDoor(liv, kit, { kind: "opening", width: 12, place: "center" });

  if (kind === "ground") {
    grid.addDoor(hall, null, { kind: "entrance", width: 8, margin: frontDoorMargin, place: "near", near: { u: cols.entranceU, v: 0 }, leaf: "wood" });
    // back door to the garden from the kitchen side
    const backRoom = rooms[rooms.length - 1];
    grid.addDoor(backRoom, null, { kind: "entrance", width: 7, place: "near", near: { u: (rc.x0 + rc.x1) / 2, v: inner.y1 + 3 }, leaf: "glass" });
  }
}

function fallbackHouse({ env, rng, pb, inner }) {
  // very narrow: single-room-per-floor cottage without stair
  const grid = pb.newGrid(0);
  const r = grid.addRoom("studio", [inner], { paint: "PAINT_WHITE", floorMat: "FLOOR_OAK" });
  grid.addDoor(r, null, { kind: "entrance", width: 8, place: "near", near: { u: env.U / 2, v: 0 } });
  pb.addFloor(0, grid, "ground");
  void rng;
}
