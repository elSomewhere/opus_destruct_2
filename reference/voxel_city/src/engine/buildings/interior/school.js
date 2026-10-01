import { stairDims, makeStair } from "./stairs.js";
import { addStairRoom, stairDoor, splitLength } from "./common.js";
import { tierRects } from "../archetypes.js";
import { EXT_T } from "./grid.js";
import { rSubtractAll } from "../../core/rect.js";

/**
 * School: a double-loaded corridor along the building with a stair core at
 * each end. Ground floor: entrance lobby on the street side with the
 * office and restrooms beside it, a gym (two bays wide, with a door to the
 * schoolyard), cafeteria + kitchen and classrooms. Upper floors: classrooms,
 * a library, a teachers' room and restrooms.
 */

const CORR = 24; // corridor width (3 m)
const BAY = 72; // classroom width target (9 m)

const STYLE = {
  classroom: { paint: "PAINT_CREAM", floorMat: "FLOOR_LINOLEUM" },
  gym: { paint: "PAINT_BLUE", floorMat: "FLOOR_PARQUET" },
  cafeteria: { paint: "PAINT_MINT", floorMat: "FLOOR_TILE_GRAY" },
  kitchen: { paint: "WALL_TILE_WHITE", floorMat: "FLOOR_TILE_WHITE" },
  library: { paint: "PAINT_SAGE", floorMat: "FLOOR_CARPET_BLUE" },
  office: { paint: "PAINT_WHITE", floorMat: "FLOOR_CARPET_GRAY" },
  teachers: { paint: "PAINT_PEACH", floorMat: "FLOOR_LINOLEUM" },
  restroom: { paint: "WALL_TILE_WHITE", floorMat: "FLOOR_TILE_GRAY" },
  lobby: { paint: "PAINT_WHITE", floorMat: "FLOOR_TERRAZZO" },
  corridor: { paint: "PAINT_CREAM", floorMat: "FLOOR_TERRAZZO" },
};

export function planSchool({ env, rng, pb }) {
  const nF = env.floors;
  const H = env.storyH[0];
  const fp = tierRects(env, 0)[0];
  const inner = { x0: fp.x0 + EXT_T, y0: fp.y0 + EXT_T, x1: fp.x1 - EXT_T, y1: fp.y1 - EXT_T };
  const Vi = inner.y1 - inner.y0 + 1;
  const depth = Math.floor((Vi - CORR - 2) / 2);
  const band = { x0: inner.x0, x1: inner.x1, y0: inner.y0 + depth + 1, y1: inner.y0 + depth + CORR };
  const front = { x0: inner.x0, x1: inner.x1, y0: inner.y0, y1: band.y0 - 2 };
  const back = { x0: inner.x0, x1: inner.x1, y0: band.y1 + 2, y1: inner.y1 };
  const sd = stairDims(H);
  const cc = Math.round((band.y0 + band.y1) / 2);
  const sy0 = cc - Math.floor(sd.W / 2);
  const left = { x0: inner.x0, x1: inner.x0 + sd.L - 1, y0: sy0, y1: sy0 + sd.W - 1 };
  const right = { x0: inner.x1 - sd.L + 1, x1: inner.x1, y0: sy0, y1: sy0 + sd.W - 1 };
  const stairs = [
    pb.addStair(makeStair({ rect: left, axis: "u", dir: -1, laneLow: true, f0: 0, f1: nF })),
    pb.addStair(makeStair({ rect: right, axis: "u", dir: 1, laneLow: false, f0: 0, f1: nF })),
  ];
  // the stair cores sit at the ends of the corridor band; the room bands
  // run the full length (the end rooms reach past the stairs to the corridor)
  // (the band beside a stair is too narrow to walk: it stays solid wall)
  const corrRects = rSubtractAll([band], [left, right].map((q) => ({ x0: q.x0 - 1, y0: band.y0, x1: q.x1 + 1, y1: band.y1 }))).filter((q) => q.x1 >= q.x0 && q.y1 >= q.y0);
  const minEnd = sd.L + 24;
  const fPieces = endSafe(splitLength(front.x0, front.x1, BAY, 52, rng), minEnd);
  const bPieces = endSafe(splitLength(back.x0, back.x1, BAY, 52, rng), minEnd);

  const floorPlan = (f) => {
    const grid = pb.newGrid(f);
    const stairRooms = stairs.map((st) => addStairRoom(grid, st));
    const corridor = grid.addRoom("corridor", corrRects, STYLE.corridor);
    stairRooms.forEach((sr, k) => stairDoor(grid, sr, stairs[k], corridor));
    const rooms = [];
    const add = (type, rect) => {
      const room = grid.addRoom(type, [rect], STYLE[type] ?? STYLE.classroom);
      rooms.push(room);
      return room;
    };
    const span = (pieces, a, b) => ({ x0: pieces[a][0], x1: pieces[b][1] });
    if (f === 0) {
      // street side: lobby in the middle (open to the corridor), office and restrooms beside it
      const mid = Math.floor(fPieces.length / 2);
      fPieces.forEach(([a0, a1], k) => {
        const r = { x0: a0, x1: a1, y0: front.y0, y1: front.y1 };
        const type = k === mid ? "lobby" : k === mid - 1 ? "office" : k === mid + 1 ? "restroom" : "classroom";
        const room = add(type, r);
        if (type === "lobby") {
          grid.addDoor(room, null, { kind: "entrance", width: 16, place: "near", near: { u: (a0 + a1) / 2, v: -12 }, leaf: "glass" });
          grid.addDoor(room, corridor, { width: 16, place: "center", kind: "opening", leaf: "none" });
        } else grid.addDoor(room, corridor, { width: 8 });
      });
      // yard side: gym (two bays), cafeteria + kitchen, classrooms
      let k = 0;
      if (bPieces.length >= 4) {
        const gym = add("gym", { ...span(bPieces, 0, 1), y0: back.y0, y1: back.y1 });
        grid.addDoor(gym, corridor, { width: 12 });
        grid.addDoor(gym, null, { kind: "entrance", width: 12, place: "near", near: { u: (bPieces[0][0] + bPieces[1][1]) / 2, v: env.V + 12 }, leaf: "metal" });
        k = 2;
      }
      const caf = k < bPieces.length ? add("cafeteria", { x0: bPieces[k][0], x1: bPieces[k][1], y0: back.y0, y1: back.y1 }) : null;
      if (caf) grid.addDoor(caf, corridor, { width: 12 });
      if (caf && k + 1 < bPieces.length) {
        const kit = add("kitchen", { x0: bPieces[k + 1][0], x1: bPieces[k + 1][1], y0: back.y0, y1: back.y1 });
        if (!grid.addDoor(kit, caf, { width: 8 })) grid.addDoor(kit, corridor, { width: 8 });
      }
      for (let m = k + 2; m < bPieces.length; m += 1) {
        const room = add("classroom", { x0: bPieces[m][0], x1: bPieces[m][1], y0: back.y0, y1: back.y1 });
        grid.addDoor(room, corridor, { width: 8 });
      }
    } else {
      const lib = rng.int(0, Math.max(0, bPieces.length - 2));
      fPieces.forEach(([a0, a1], k) => {
        const type = k === 0 ? "teachers" : k === fPieces.length - 1 ? "restroom" : "classroom";
        const room = add(type, { x0: a0, x1: a1, y0: front.y0, y1: front.y1 });
        grid.addDoor(room, corridor, { width: 8 });
      });
      for (let m = 0; m < bPieces.length; m += 1) {
        let room;
        if (m === lib && bPieces.length >= 3) {
          room = add("library", { ...span(bPieces, m, m + 1), y0: back.y0, y1: back.y1 });
          m += 1;
        } else room = add("classroom", { x0: bPieces[m][0], x1: bPieces[m][1], y0: back.y0, y1: back.y1 });
        grid.addDoor(room, corridor, { width: 8 });
      }
    }
    return grid;
  };

  pb.addFloor(0, floorPlan(0), "school");
  const upper = nF > 1 ? floorPlan(1) : null;
  for (let f = 1; f < nF; f += 1) pb.addFloor(f, upper, "school");
}

/** Merge end pieces that would not reach past the stair core to the corridor. */
function endSafe(pieces, minEnd) {
  const out = pieces.map((p) => p.slice());
  while (out.length > 2 && out[0][1] - out[0][0] + 1 < minEnd) {
    out[1][0] = out[0][0];
    out.shift();
  }
  while (out.length > 2 && out[out.length - 1][1] - out[out.length - 1][0] + 1 < minEnd) {
    out[out.length - 2][1] = out[out.length - 1][1];
    out.pop();
  }
  return out;
}
