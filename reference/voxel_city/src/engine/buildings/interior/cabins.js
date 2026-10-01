import { EXT_T } from "./grid.js";
import { pickUnitStyle, floorFor } from "./units.js";
import { tierRects } from "../archetypes.js";
import { clamp } from "../../core/math.js";

/**
 * Forest cabins (hytte): one floor, no stair, two strips across the width
 *
 *   [ bath  | entry  | kitchen ]   front door into the entry
 *   [ bed 1 |                  ]
 *   [-------|      living      ]
 *   [ bed 2 |                  ]
 *
 * The side strip holds the bathroom at the front and one or two bedrooms
 * behind it, all opening onto the entry / living room beside them; the main
 * strip has the entry at the front door, the kitchen beside it when the strip
 * is wide enough (else behind the living room) and the living room. Mirrored
 * for half the cabins. The layout is a pure function of the footprint, so the
 * envelope knows where the door is before the plan exists.
 */

const rectOf = ([x0, x1], [y0, y1]) => ({ x0, y0, x1, y1 });

/** Room rects (canonical) + front door position of a U x V cabin. */
export function cabinLayout(U, V, mirror) {
  const i0 = EXT_T;
  const i1 = U - 1 - EXT_T;
  const j0 = EXT_T;
  const j1 = V - 1 - EXT_T;
  const Ui = i1 - i0 + 1;
  const Vi = j1 - j0 + 1;
  const side = clamp(Math.round(Ui * 0.42), 20, 26);
  const mainX = [i0 + side + 1, i1];
  const mainW = mainX[1] - mainX[0] + 1;
  const sideX = [i0, i0 + side - 1];
  const front = clamp(Math.round(Vi * 0.24), 14, 17);
  const frontY = [j0, j0 + front - 1];
  const restY = [j0 + front + 1, j1];
  const restLen = restY[1] - restY[0] + 1;
  const rooms = [{ key: "bath", type: "bath", rect: rectOf(sideX, frontY) }];
  // bedrooms down the side strip
  if (restLen >= 2 * 20 + 1) {
    const mid = restY[0] + Math.floor((restLen - 1) / 2);
    rooms.push({ key: "bed1", type: "bedroom", rect: rectOf(sideX, [restY[0], mid - 1]) });
    rooms.push({ key: "bed2", type: "bedroom", rect: rectOf(sideX, [mid + 1, restY[1]]) });
  } else {
    rooms.push({ key: "bed1", type: "bedroom", rect: rectOf(sideX, restY) });
  }
  // main strip: entry (+ kitchen) at the front, living room behind
  if (mainW >= 12 + 1 + 20) {
    const ew = clamp(Math.round(mainW * 0.4), 12, 16);
    rooms.push({ key: "entry", type: "foyer", rect: rectOf([mainX[0], mainX[0] + ew - 1], frontY) });
    rooms.push({ key: "kitchen", type: "kitchen", rect: rectOf([mainX[0] + ew + 1, mainX[1]], frontY) });
    rooms.push({ key: "living", type: "living", rect: rectOf(mainX, restY) });
  } else {
    const kd = clamp(Math.round(restLen * 0.4), 14, 22);
    rooms.push({ key: "entry", type: "foyer", rect: rectOf(mainX, frontY) });
    rooms.push({ key: "living", type: "living", rect: rectOf(mainX, [restY[0], restY[1] - kd - 1]) });
    rooms.push({ key: "kitchen", type: "kitchen", rect: rectOf(mainX, [restY[1] - kd + 1, restY[1]]) });
  }
  if (mirror) for (const r of rooms) [r.rect.x0, r.rect.x1] = [i0 + i1 - r.rect.x1, i0 + i1 - r.rect.x0];
  const entry = rooms.find((r) => r.key === "entry").rect;
  return { rooms, entranceU: Math.round((entry.x0 + entry.x1) / 2) };
}

export function planCabin({ env, rng, pb }) {
  const layout = cabinLayout(env.U, env.V, !!env.mirror);
  const style = pickUnitStyle(rng);
  const panelled = rng.chance(0.75);
  const mat = (type) => ({
    paint: type === "bath" ? style.tile : panelled ? "PINE_PANEL" : style.paint,
    floorMat: type === "bath" ? style.wet : floorFor(type, style),
  });
  const grid = pb.newGrid(0);
  const R = {};
  for (const r of layout.rooms) R[r.key] = grid.addRoom(r.type, [r.rect], mat(r.type));
  grid.addDoor(R.entry, null, { kind: "entrance", width: 8, place: "near", near: { u: layout.entranceU, v: 0 }, leaf: "wood" });
  grid.addDoor(R.bath, R.entry, { width: 6, margin: 1 }) || grid.addDoor(R.bath, R.living, { width: 6, margin: 1 });
  grid.addDoor(R.entry, R.living, { width: 7, place: "center" }) || grid.addDoor(R.entry, R.living, { width: 6, margin: 1 });
  grid.addDoor(R.kitchen, R.living, { kind: "opening", width: 12, place: "center" }) || grid.addDoor(R.kitchen, R.living, { width: 7, margin: 1 }) || grid.addDoor(R.kitchen, R.entry, { width: 7, margin: 1 });
  for (const key of ["bed1", "bed2"]) {
    const bed = R[key];
    if (!bed) continue;
    grid.addDoor(bed, R.living, { width: 7 }) || grid.addDoor(bed, R.kitchen, { width: 7 }) || grid.addDoor(bed, R.bed1, { width: 7, margin: 1 });
  }
  pb.addFloor(0, grid, "ground");
}

/**
 * Wooden church: the tower base is the vestibule (front door), one wide door
 * leads into the open nave (pews and altar: furnish.js `nave`). The tower
 * rect overlaps the nave's front wall, so the two share a 1-cell partition.
 */
export function planChurch({ env, pb }) {
  const [nave, tower] = tierRects(env, 0);
  const grid = pb.newGrid(0);
  const vest = grid.addRoom("foyer", [{ x0: tower.x0 + EXT_T, y0: tower.y0 + EXT_T, x1: tower.x1 - EXT_T, y1: nave.y0 }], { paint: "PAINT_WHITE", floorMat: "FLOOR_OAK" });
  // (an Orthodox church, under onion domes: an icon screen, candle stands, no pews)
  const hall = grid.addRoom(env.steeple?.dome ? "orthodoxNave" : "nave", [{ x0: nave.x0 + EXT_T, y0: nave.y0 + EXT_T, x1: nave.x1 - EXT_T, y1: nave.y1 - EXT_T }], { paint: "PAINT_WHITE", floorMat: "FLOOR_OAK" });
  const cu = (tower.x0 + tower.x1) / 2;
  grid.addDoor(vest, null, { kind: "entrance", width: 10, place: "near", near: { u: cu, v: 0 }, leaf: "wood" });
  grid.addDoor(vest, hall, { width: 10, place: "center", leaf: "wood" }) || grid.addDoor(vest, hall, { width: 7, margin: 1 });
  pb.addFloor(0, grid, "ground");
}
