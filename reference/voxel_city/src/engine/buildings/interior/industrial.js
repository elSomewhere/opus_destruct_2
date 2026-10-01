import { stairDims, makeStair } from "./stairs.js";
import { addStairRoom, stairDoor, splitLength } from "./common.js";
import { tierRects } from "../archetypes.js";
import { EXT_T, FloorGrid } from "./grid.js";
import { rSubtractAll } from "../../core/rect.js";

/**
 * Warehouses / factories: one tall hall with loading doors, plus a two-level
 * office annex in a front corner. The annex's upper level is a mezzanine
 * floor record stacked inside the hall; the hall floor is capped to the
 * annex ceiling height over the annex footprint ("low region").
 */

const M = 8;

/** The hall of an industrial building by its program: finishes. */
const HALLS = {
  warehouse: { paint: "CONCRETE", floorMat: "FLOOR_EPOXY" },
  factory: { paint: "CONCRETE", floorMat: "FLOOR_EPOXY" },
  barn: { paint: "WOOD_DARK", floorMat: "FLOOR_CONCRETE" },
  distribution: { paint: "CONCRETE", floorMat: "FLOOR_EPOXY" },
  selfStorage: { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" },
  coldStore: { paint: "PAINT_WHITE", floorMat: "FLOOR_EPOXY" },
  timberYard: { paint: "CONCRETE", floorMat: "FLOOR_CONCRETE" },
};

export function planIndustrial({ env, rng, pb }) {
  const rect = tierRects(env, 0)[0];
  const inner = { x0: rect.x0 + EXT_T, y0: rect.y0 + EXT_T, x1: rect.x1 - EXT_T, y1: rect.y1 - EXT_T };
  const H = env.storyH[0];
  const z0 = pb.z(0);
  const ceilA = 28; // annex ground level story (3.5 m)
  const mezzH = Math.min(28, H - ceilA);
  const sd = stairDims(ceilA);
  const aw = Math.min(inner.x1 - inner.x0 - 6 * M, Math.max(sd.L + 2 + 3 * M, Math.round(rng.float(10, 14) * M)));
  const ad = Math.round(rng.float(6.5, 8.5) * M);
  const left = rng.chance(0.5);
  const ax0 = left ? inner.x0 : inner.x1 - aw + 1;
  const annex = { x0: ax0, y0: inner.y0, x1: ax0 + aw - 1, y1: inner.y0 + ad - 1 };

  // stair along u at the back of the annex; near landing towards the annex middle
  const sRect = left
    ? { x0: annex.x1 - sd.L + 1, y0: annex.y1 - sd.W + 1, x1: annex.x1, y1: annex.y1 }
    : { x0: annex.x0, y0: annex.y1 - sd.W + 1, x1: annex.x0 + sd.L - 1, y1: annex.y1 };
  const stair = pb.addStair(
    makeStair({ rect: sRect, axis: "u", dir: left ? 1 : -1, laneLow: true, f0: 0, f1: 1 }),
  );
  stair.flights = [{ f: 0, z0, H: ceilA }];

  // ---- hall floor
  const g0 = pb.newGrid(0);
  const sRoom0 = addStairRoom(g0, stair);
  const frontRow = { x0: annex.x0, y0: annex.y0, x1: annex.x1, y1: sRect.y0 - 2 };
  // reception spans the stair's near landing (so the stair door works); rooms on either side
  const nearU = left ? sRect.x0 + 4 : sRect.x1 - 4;
  const land = left ? [sRect.x0, sRect.x0 + 8] : [sRect.x1 - 8, sRect.x1];
  const rec = { ...frontRow, x0: Math.max(frontRow.x0, land[0] - 10), x1: Math.min(frontRow.x1, land[1] + 10) };
  const annexRooms = [];
  const reception = g0.addRoom("reception", [rec], { paint: "PAINT_WHITE", floorMat: "FLOOR_LINOLEUM", ceiling: ceilA });
  annexRooms.push(reception);
  const sidePieces = [];
  if (rec.x0 - 2 - frontRow.x0 + 1 >= 12) sidePieces.push([frontRow.x0, rec.x0 - 2]);
  if (frontRow.x1 - (rec.x1 + 2) + 1 >= 12) sidePieces.push([rec.x1 + 2, frontRow.x1]);
  for (const [a0, a1] of sidePieces) {
    for (const [a, b] of splitLength(a0, a1, 4 * M, 12, rng)) {
      const type = annexRooms.some((r) => r.type === "wc") ? "office" : "wc";
      annexRooms.push(
        g0.addRoom(type, [{ ...frontRow, x0: a, x1: b }], {
          paint: type === "wc" ? "WALL_TILE_WHITE" : "PAINT_WHITE",
          floorMat: type === "wc" ? "FLOOR_TILE_WHITE" : "FLOOR_LINOLEUM",
          ceiling: ceilA,
        }),
      );
    }
  }
  // the leftover strip beside the stair (if any) joins the reception row as storage
  const beside = left
    ? { x0: annex.x0, y0: sRect.y0, x1: sRect.x0 - 2, y1: annex.y1 }
    : { x0: sRect.x1 + 2, y0: sRect.y0, x1: annex.x1, y1: annex.y1 };
  if (beside.x1 - beside.x0 + 1 >= 10) annexRooms.push(g0.addRoom("storage", [beside], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE", ceiling: ceilA }));
  sRoom0.ceiling = ceilA;

  const hallRects = rSubtractAll([inner], [{ x0: annex.x0 - 1, y0: annex.y0 - 1, x1: annex.x1 + 1, y1: annex.y1 + 1 }]);
  const hallType = HALLS[env.program.ground] ? env.program.ground : "warehouse";
  const hall = g0.addRoom(hallType, hallRects, HALLS[hallType]);
  stairDoor(g0, sRoom0, stair, reception);
  g0.addDoor(reception, null, { kind: "entrance", width: 8, place: "near", near: { u: (reception.rects[0].x0 + reception.rects[0].x1) / 2, v: 0 }, leaf: "glass" });
  g0.addDoor(reception, hall, { width: 8, leaf: "metal" }) ?? g0.addDoor(reception, hall, { width: 7, margin: 1 });
  for (const r of annexRooms) {
    if (r === reception) continue;
    if (g0.addDoor(r, reception, { width: 7 }) || g0.addDoor(r, hall, { width: 7 }) || g0.addDoor(r, reception, { width: 6, margin: 1 })) continue;
    // a room we cannot open becomes solid fit-out
    r.type = "shaft";
  }
  // loading doors on the front facade + a personnel door at the back
  const hallFront = hallRects.find((r) => r.y0 === inner.y0) ?? hallRects[0];
  const nDocks = Math.max(1, Math.min(4, Math.floor((hallFront.x1 - hallFront.x0) / (8 * M))));
  for (let k = 0; k < nDocks; k += 1) {
    const u = hallFront.x0 + ((k + 0.5) * (hallFront.x1 - hallFront.x0)) / nDocks;
    g0.addDoor(hall, null, { kind: "rollup", width: 28, margin: 4, place: "near", near: { u, v: 0 }, leaf: "rollup", height: 32 });
  }
  g0.addDoor(hall, null, { kind: "entrance", width: 8, place: "near", near: { u: (inner.x0 + inner.x1) / 2, v: inner.y1 + 3 }, leaf: "metal" });
  pb.addFloor(0, g0, "industrial", { lowRegions: [{ rect: { x0: annex.x0 - 1, y0: annex.y0 - 1, x1: annex.x1 + 1, y1: annex.y1 + 1 }, height: ceilA }] });

  // ---- mezzanine (annex upper level)
  const annexOuter = { x0: annex.x0 - EXT_T, y0: annex.y0 - EXT_T, x1: annex.x1 + EXT_T, y1: annex.y1 + EXT_T };
  const g1 = new FloorGrid(env.U, env.V, [annexOuter]);
  const sRoom1 = addStairRoom(g1, stair);
  const landingRect = { x0: Math.max(frontRow.x0, nearU - 12), y0: frontRow.y0, x1: Math.min(frontRow.x1, nearU + 12), y1: frontRow.y1 };
  const landing = g1.addRoom("hall", [landingRect], { paint: "PAINT_WHITE", floorMat: "FLOOR_LINOLEUM" });
  stairDoor(g1, sRoom1, stair, landing);
  const rest = rSubtractAll([frontRow], [{ ...landingRect, x0: landingRect.x0 - 1, x1: landingRect.x1 + 1 }]);
  for (const r of rest) {
    if (r.x1 - r.x0 + 1 < 12) continue;
    const room = g1.addRoom(rng.pick(["office", "meeting", "office"]), [r], { paint: "PAINT_WHITE", floorMat: "FLOOR_CARPET_GRAY" });
    if (!g1.addDoor(room, landing, { width: 7 })) g1.addDoor(room, landing, { width: 6, margin: 1 });
  }
  if (beside.x1 - beside.x0 + 1 >= 10) {
    const room = g1.addRoom("storage", [beside], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" });
    if (!g1.addDoor(room, landing, { width: 6, margin: 1 })) {
      // reachable through the stair landing zone only if adjacent; otherwise mark as shaft
      room.type = "shaft";
    }
  }
  pb.addFloor(1, g1, "mezzanine", { z: z0 + ceilA, height: mezzH, mezzanine: true });
}
