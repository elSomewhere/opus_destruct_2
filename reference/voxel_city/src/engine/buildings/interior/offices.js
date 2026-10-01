import { stairDims, makeStair } from "./stairs.js";
import { addStairRoom, stairDoor, splitLength } from "./common.js";
import { tierRects } from "../archetypes.js";
import { EXT_T } from "./grid.js";
import { rSubtractAll, rOverlaps } from "../../core/rect.js";
import { shopWithBackroom } from "./apartments.js";
import { frameOf } from "../frame.js";

/**
 * Office buildings and towers.
 *
 * A central core (two stairs, an elevator bank with a machine shaft behind
 * it, two restrooms) stacks from the lowest basement to the roof. Typical
 * floors are an open plan ring around the core with enclosed meeting rooms
 * and offices along the end facades. The ground floor is a lobby with shops
 * in the front corners; basements are parking levels.
 */

const M = 8;

export function officeCore(inner, sd, nF) {
  const Ui = inner.x1 - inner.x0 + 1;
  const Vi = inner.y1 - inner.y0 + 1;
  const nElev = nF <= 5 ? 1 : nF <= 14 ? 2 : nF <= 28 ? 3 : 4;
  const compact = Ui < 26 * M || Vi < sd.L + 2 * 5 * M;
  const parts = compact
    ? [["stair", sd.W], ["restroom", 20], ["elevators", nElev * 17 - 1]]
    : [["stair", sd.W], ["restroom", 20], ["elevators", nElev * 17 - 1], ["restroom", 20], ["stair", sd.W]];
  const Wc = parts.reduce((s, p) => s + p[1], 0) + parts.length - 1;
  const D = sd.L;
  const x0 = inner.x0 + Math.floor((Ui - Wc) / 2);
  const y0 = inner.y0 + Math.floor((Vi - D) / 2);
  const comps = [];
  let x = x0;
  for (const [kind, w] of parts) {
    comps.push({ kind, rect: { x0: x, y0, x1: x + w - 1, y1: y0 + D - 1 } });
    x += w + 1;
  }
  return { rect: { x0, y0, x1: x - 2, y1: y0 + D - 1 }, comps, nElev };
}

export function planOfficeBuilding({ env, rng, pb }) {
  const nF = env.floors;
  const top = tierRects(env, nF - 1)[0];
  const inner = { x0: top.x0 + EXT_T, y0: top.y0 + EXT_T, x1: top.x1 - EXT_T, y1: top.y1 - EXT_T };
  let maxH = env.basements ? env.basementH : 0;
  for (const h of env.storyH) maxH = Math.max(maxH, h);
  const sd = stairDims(maxH);
  const core = officeCore(inner, sd, nF);
  const fBottom = -env.basements;
  const layout = { stairs: [], elevators: [], restrooms: [], shafts: [] };
  for (const c of core.comps) {
    if (c.kind === "stair") {
      layout.stairs.push({ rect: c.rect, stair: pb.addStair(makeStair({ rect: c.rect, axis: "v", dir: 1, laneLow: rng.chance(0.5), f0: fBottom, f1: nF })) });
    } else if (c.kind === "restroom") {
      layout.restrooms.push(c.rect);
    } else {
      // elevator bank: shafts at the front, machine/shaft room behind
      for (let k = 0; k < core.nElev; k += 1) {
        const ex0 = c.rect.x0 + k * 17;
        const rect = { x0: ex0, y0: c.rect.y0, x1: ex0 + 15, y1: c.rect.y0 + 15 };
        layout.elevators.push({ rect, elev: pb.addElevator({ rect, f0: fBottom, f1: nF - 1, doorSide: "N" }) });
      }
      layout.shafts.push({ x0: c.rect.x0, y0: c.rect.y0 + 17, x1: c.rect.x1, y1: c.rect.y1 });
    }
  }
  const ctx = { env, rng, pb, inner, layout };
  for (let f = fBottom; f < 0; f += 1) {
    const grid = pb.newGrid(f);
    const cores = paintOfficeCore(grid, layout, f < 0);
    planOpenFloor(grid, ctx, cores, "parking");
    pb.addFloor(f, grid, "parking");
  }
  const g0 = pb.newGrid(0);
  planOpenFloor(g0, ctx, paintOfficeCore(g0, layout, false), "lobby");
  pb.addFloor(0, g0, "ground");
  let typical = null;
  let tierKey = null;
  const sky = env.skyDoors ?? [];
  for (let f = 1; f < nF; f += 1) {
    const doors = sky.filter((d) => d.floor === f);
    if (doors.length) {
      // skybridge floor: its own grid with a glass door where the bridge lands
      const g = pb.newGrid(f);
      const open = planOpenFloor(g, { ...ctx, rng: rng.fork(`office${f}`) }, paintOfficeCore(g, layout, false), "office");
      for (const d of doors) skyDoor(g, env, open, d);
      pb.addFloor(f, g, "office");
      continue;
    }
    const key = JSON.stringify(tierRects(env, f));
    if (!typical || key !== tierKey) {
      typical = pb.newGrid(f);
      tierKey = key;
      planOpenFloor(typical, { ...ctx, rng: rng.fork(`office${f}`) }, paintOfficeCore(typical, layout, false), "office");
    }
    pb.addFloor(f, typical, "office");
  }
}

/** Door in the front facade exactly where a skybridge meets the building. */
function skyDoor(grid, env, open, d) {
  const frame = frameOf(env);
  const R = env.R;
  const span = d.span.x0 !== undefined ? { x0: d.span.x0, x1: d.span.x1, y0: R.y0, y1: R.y1 } : { x0: R.x0, x1: R.x1, y0: d.span.y0, y1: d.span.y1 };
  const c = frame.rectFromWorld(span);
  const width = c.x1 - c.x0 + 1;
  const door = grid.addDoor(open, null, { kind: "sky", width, margin: 1, place: "near", near: { u: (c.x0 + c.x1 + 1) / 2, v: -12 }, leaf: "glass", height: 20 });
  if (door && Math.abs(door.u0 - c.x0) > 1) door.misplaced = true;
}

function paintOfficeCore(grid, layout, basement) {
  const stairRooms = layout.stairs.map((s) => addStairRoom(grid, s.stair));
  const elevRooms = layout.elevators.map((e) => grid.addRoom("elevator", [e.rect], { elevator: e.elev.id }));
  for (const s of layout.shafts) grid.addRoom("shaft", [s]);
  const restrooms = layout.restrooms.map((r) =>
    grid.addRoom(basement ? "storage" : "restroom", [r], { paint: "WALL_TILE_WHITE", floorMat: "FLOOR_TILE_GRAY" }),
  );
  return { stairRooms, elevRooms, restrooms, layout };
}

const OPEN_STYLE = {
  office: { type: "openOffice", paint: "PAINT_WHITE", floorMat: "FLOOR_CARPET_GRAY" },
  lobby: { type: "lobby", paint: "PAINT_WHITE", floorMat: "FLOOR_MARBLE" },
  parking: { type: "parking", paint: "CONCRETE", floorMat: "FLOOR_CONCRETE" },
  store: { type: "departmentFloor", paint: "PAINT_WHITE", floorMat: "FLOOR_TERRAZZO" },
};

/**
 * Fill the rest of a floor with one open-plan room around already painted
 * rooms (cores), carving enclosed rooms / shops first, then connect every
 * core component. Works for any core layout (office cores, apartment cores
 * on tower podium floors).
 */
export function planOpenFloor(grid, ctx, cores, kind) {
  const { rng } = ctx;
  const inner = grid.innerRects()[0];
  const taken = grid.rooms.flatMap((r) => r.rects).map((r) => ({ x0: r.x0 - 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1 }));
  const coreBox = taken.length
    ? taken.reduce((a, r) => ({ x0: Math.min(a.x0, r.x0), y0: Math.min(a.y0, r.y0), x1: Math.max(a.x1, r.x1), y1: Math.max(a.y1, r.y1) }))
    : null;
  const enclosed = [];
  const st = OPEN_STYLE[kind];

  if (kind === "office" || kind === "lobby") {
    // end zones along the side facades
    const ew = Math.round(rng.float(3.6, 4.6) * M);
    for (const side of ["W", "E"]) {
      const zone =
        side === "W"
          ? { x0: inner.x0, y0: inner.y0, x1: inner.x0 + ew - 1, y1: inner.y1 }
          : { x0: inner.x1 - ew + 1, y0: inner.y0, x1: inner.x1, y1: inner.y1 };
      if (coreBox && rOverlaps(coreBox, { ...zone, x0: zone.x0 - 6 * M, x1: zone.x1 + 6 * M })) continue;
      if (zone.y1 - zone.y0 + 1 < 6 * M) continue;
      if (kind === "lobby") {
        // shops at the front corner, back-of-house behind
        const shopDepth = Math.min(Math.round((zone.y1 - zone.y0) * 0.55), 9 * M);
        const shopRect = { ...zone, y1: zone.y0 + shopDepth - 1 };
        const boh = { ...zone, y0: shopRect.y1 + 2 };
        const shop = shopWithBackroom(grid, ctx, shopRect, null);
        enclosed.push({ room: shop, own: true });
        const b = grid.addRoom(rng.pick(["mailroom", "security", "storage"]), [boh], { paint: "PAINT_GRAY", floorMat: "FLOOR_LINOLEUM" });
        enclosed.push({ room: b });
      } else {
        for (const [p0, p1] of splitLength(zone.y0, zone.y1, Math.round(rng.float(3.2, 4.8) * M), 3 * M, rng)) {
          const type = rng.weighted([["meeting", 3], ["office", 4], ["breakroom", 1], ["storage", 0.5]]);
          const r = grid.addRoom(type, [{ ...zone, y0: p0, y1: p1 }], {
            paint: type === "breakroom" ? "PAINT_MINT" : "PAINT_WHITE",
            floorMat: type === "breakroom" ? "FLOOR_LINOLEUM" : "FLOOR_CARPET_BLUE",
          });
          enclosed.push({ room: r });
        }
      }
      taken.push({ x0: zone.x0 - 1, y0: zone.y0 - 1, x1: zone.x1 + 1, y1: zone.y1 + 1 });
    }
  }

  const free = rSubtractAll([inner], taken).filter((r) => r.x1 >= r.x0 && r.y1 >= r.y0);
  const open = grid.addRoom(st.type, free, { paint: st.paint, floorMat: st.floorMat });
  for (const e of enclosed) {
    if (e.own) continue;
    if (!grid.addDoor(e.room, open, { width: 8 })) grid.addDoor(e.room, open, { width: 6, margin: 1 });
  }
  for (const e of enclosed) {
    if (!e.own) continue;
    // shops may also open to the lobby
    grid.addDoor(e.room, open, { width: 8, leaf: "glass" });
  }
  cores.stairRooms.forEach((sr, k) => {
    const st2 = cores.layout ? cores.layout.stairs[k].stair : null;
    const stair = st2 ?? ctx.layout.stairs[k].stair;
    stairDoor(grid, sr, stair, open);
  });
  cores.elevRooms.forEach((er) => grid.addDoor(er, open, { width: 8, kind: "elevator", place: "center", leaf: "elevator" }));
  for (const rr of cores.restrooms ?? []) {
    if (!grid.addDoor(rr, open, { width: 7 })) grid.addDoor(rr, open, { width: 6, margin: 1 });
  }
  if (kind === "lobby") {
    grid.addDoor(open, null, { kind: "entrance", width: 16, place: "near", near: { u: (inner.x0 + inner.x1) / 2, v: 0 }, leaf: "glass" });
  }
  return open;
}
