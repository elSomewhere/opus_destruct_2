import { stairDims, makeStair } from "./stairs.js";
import { addStairRoom, stairDoor, splitLength, facadeSidesOf, freeIntervals } from "./common.js";
import { planUnit, pickUnitStyle } from "./units.js";
import { tierRects } from "../archetypes.js";
import { EXT_T } from "./grid.js";
import { planOpenFloor } from "./offices.js";
import { STYLES } from "../../world/registry.js";
import { Rng } from "../../core/hash.js";
import { nearWing } from "../wings.js";

/**
 * Apartment buildings (walk-ups, mid-rises, residential towers).
 *
 * Two circulation systems, chosen from the typical floor depth:
 *   corridor  double-loaded corridor along the building, cores (stair +
 *             elevator) in the back zone opening onto the corridor
 *   sections  point access: each section has a stair hall at the front and
 *             a stair behind it; two through-units per floor per section
 *
 * The core layout is derived from the top (smallest) tier so it stacks
 * through every floor, basements and podiums included.
 */

const M = 8;

export function planApartmentBuilding({ env, rng, pb }) {
  const nF = env.floors;
  const top = tierRects(env, nF - 1)[0];
  const inner = { x0: top.x0 + EXT_T, y0: top.y0 + EXT_T, x1: top.x1 - EXT_T, y1: top.y1 - EXT_T };
  const Vi = inner.y1 - inner.y0 + 1;
  let maxH = env.basements ? env.basementH : 0;
  for (const h of env.storyH) maxH = Math.max(maxH, h);
  const sd = stairDims(maxH);
  const lift = nF > 5;
  const mode = Vi >= sd.L + 13 + 2 + 5 * M && Vi >= 118 ? "corridor" : "sections";
  const layout = mode === "corridor" ? corridorCores(inner, sd, lift, rng) : sectionCores(inner, sd, lift, rng);
  if (!layout) return;

  // stairs / elevators span basements .. roof (flat roofs: up to a bulkhead;
  // under a pitched roof the stair ends on the top floor)
  const fBottom = -env.basements;
  const fStairTop = env.roof.type === "flat" ? nF : nF - 1;
  for (const s of layout.stairs) {
    s.stair = pb.addStair(makeStair({ rect: s.rect, axis: "v", dir: 1, laneLow: rng.chance(0.5), f0: fBottom, f1: fStairTop }));
  }
  for (const e of layout.elevators) e.elev = pb.addElevator({ rect: e.rect, f0: fBottom, f1: nF - 1, doorSide: "N" });

  const firstApt = env.archetype === "tower" ? env.podiumFloors : 1;
  const ctxBase = { env, rng, pb, inner, layout, mode, unitStyle: pickUnitStyle(rng) };

  // basements
  for (let f = fBottom; f < 0; f += 1) pb.addFloor(f, basementFloor({ ...ctxBase, f }), "basement");
  // ground
  pb.addFloor(0, groundFloor({ ...ctxBase, f: 0 }), "ground");
  // podium (towers): open office floors around the same cores
  let podiumGrid = null;
  for (let f = 1; f < firstApt; f += 1) {
    if (!podiumGrid) podiumGrid = podiumFloor({ ...ctxBase, f });
    pb.addFloor(f, podiumGrid, "office");
  }
  // typical residential floors share one grid
  let typical = null;
  for (let f = firstApt; f < nF; f += 1) {
    if (!typical) {
      typical = residentialFloor({ ...ctxBase, f, rng: rng.fork("typical") });
      addBalconies(typical, env, rng.fork("balconies"));
    }
    pb.addFloor(f, typical, "residential");
  }
}

// ---------------------------------------------------------------- cores

function corridorCores(inner, sd, lift, rng) {
  const cw = 13;
  const Vi = inner.y1 - inner.y0 + 1;
  let cv0 = inner.y0 + Math.floor((Vi - cw) / 2);
  // keep the back zone deep enough for the stair run
  const maxCv1 = inner.y1 - sd.L - 1;
  if (cv0 + cw - 1 > maxCv1) cv0 = maxCv1 - cw + 1;
  const cv1 = cv0 + cw - 1;
  const Ui = inner.x1 - inner.x0 + 1;
  const coreW = lift ? sd.W + 1 + 16 : sd.W;
  const nCores = Ui > 44 * M ? 2 : 1;
  const stairs = [];
  const elevators = [];
  const coreIntervals = [];
  for (let k = 0; k < nCores; k += 1) {
    const center = nCores === 1 ? inner.x0 + Ui * (0.5 + rng.float(-0.12, 0.12)) : inner.x0 + Ui * (k === 0 ? 0.24 : 0.76);
    let c0 = Math.round(center - coreW / 2);
    c0 = Math.max(inner.x0 + 5 * M, Math.min(inner.x1 - 5 * M - coreW, c0));
    const stairRect = { x0: c0, y0: cv1 + 2, x1: c0 + sd.W - 1, y1: inner.y1 };
    stairs.push({ rect: stairRect });
    if (lift) {
      const ex0 = c0 + sd.W + 1;
      elevators.push({ rect: { x0: ex0, y0: cv1 + 2, x1: ex0 + 15, y1: cv1 + 2 + 15 }, shaft: { x0: ex0, y0: cv1 + 2 + 17, x1: ex0 + 15, y1: inner.y1 } });
    }
    coreIntervals.push([c0, c0 + coreW - 1]);
  }
  return { mode: "corridor", corridor: { x0: inner.x0, y0: cv0, x1: inner.x1, y1: cv1 }, stairs, elevators, coreIntervals };
}

function sectionCores(inner, sd, lift, rng) {
  const Ui = inner.x1 - inner.x0 + 1;
  const Vi = inner.y1 - inner.y0 + 1;
  if (Vi < sd.L + 1 + 12) return null;
  const coreW = lift ? sd.W + 1 + 16 : sd.W;
  const nSec = Math.max(1, Math.round(Ui / (19 * M)));
  const secs = splitLength(inner.x0, inner.x1, Math.floor((Ui - (nSec - 1)) / nSec), 8 * M, rng, 0.1);
  const stairs = [];
  const elevators = [];
  const sections = [];
  for (const [a0, a1] of secs) {
    const w = a1 - a0 + 1;
    let c0;
    let single = false;
    if (w >= coreW + 2 + 2 * 5 * M) c0 = Math.round((a0 + a1 - coreW) / 2 + rng.float(-0.08, 0.08) * w);
    else {
      single = true;
      c0 = rng.chance(0.5) ? a0 : a1 - coreW + 1;
    }
    c0 = Math.max(a0, Math.min(a1 - coreW + 1, c0));
    const stairRect = { x0: c0, y0: inner.y1 - sd.L + 1, x1: c0 + sd.W - 1, y1: inner.y1 };
    stairs.push({ rect: stairRect });
    let shaft = null;
    if (lift) {
      const ex0 = c0 + sd.W + 1;
      const el = { rect: { x0: ex0, y0: stairRect.y0, x1: ex0 + 15, y1: stairRect.y0 + 15 } };
      shaft = { x0: ex0, y0: stairRect.y0 + 17, x1: ex0 + 15, y1: inner.y1 };
      el.shaft = shaft;
      elevators.push(el);
    }
    const hall = { x0: c0, y0: inner.y0, x1: c0 + coreW - 1, y1: stairRect.y0 - 2 };
    sections.push({ a0, a1, c0, c1: c0 + coreW - 1, hall, single, stairIndex: stairs.length - 1 });
  }
  return { mode: "sections", sections, stairs, elevators };
}

function paintCores(grid, layout, withElevators = true) {
  const stairRooms = [];
  for (const s of layout.stairs) stairRooms.push(addStairRoom(grid, s.stair));
  const elevRooms = [];
  if (withElevators) {
    for (const e of layout.elevators) {
      elevRooms.push(grid.addRoom("elevator", [e.rect], { elevator: e.elev.id }));
      if (e.shaft) grid.addRoom("shaft", [e.shaft]);
    }
  }
  return { stairRooms, elevRooms };
}

function connectCores(grid, layout, cores, circFor) {
  layout.stairs.forEach((s, k) => {
    const circ = circFor(k);
    if (circ) stairDoor(grid, cores.stairRooms[k], s.stair, circ);
  });
  layout.elevators.forEach((e, k) => {
    const circ = circFor(k);
    if (circ) grid.addDoor(cores.elevRooms[k], circ, { width: 8, kind: "elevator", place: "center", leaf: "elevator" });
  });
}

// ---------------------------------------------------------------- floors

function residentialFloor(ctx) {
  const { pb, f, layout } = ctx;
  const grid = pb.newGrid(f);
  const cores = paintCores(grid, layout);
  if (layout.mode === "corridor") {
    const corr = grid.addRoom("corridor", [layout.corridor], { paint: "PAINT_CREAM", floorMat: "FLOOR_CARPET_RED" });
    connectCores(grid, layout, cores, () => corr);
    corridorUnits(grid, ctx, corr, { front: true, back: true });
  } else {
    for (const sec of layout.sections) {
      const hall = grid.addRoom("landing", [sec.hall], { paint: "PAINT_WHITE", floorMat: "FLOOR_TERRAZZO" });
      sec.hallRoom = hall;
    }
    // stairs[k] and elevators[k] both belong to sections[k]
    connectCores(grid, layout, cores, (k) => layout.sections[k].hallRoom);
    sectionUnits(grid, ctx);
  }
  return grid;
}

function groundFloor(ctx) {
  const { pb, env, layout, inner } = ctx;
  const grid = pb.newGrid(0);
  const cores = paintCores(grid, layout);
  const program = env.archetype === "tower" ? "officeLobby" : env.program.ground;
  if (env.archetype === "tower") {
    return lobbyOpenFloor(grid, ctx, cores);
  }
  if (layout.mode === "corridor") {
    const corr = grid.addRoom("corridor", [layout.corridor], { paint: "PAINT_CREAM", floorMat: "FLOOR_TERRAZZO" });
    connectCores(grid, layout, cores, () => corr);
    // lobby in front of the first core
    const [c0, c1] = layout.coreIntervals[0];
    const lob = { x0: Math.max(inner.x0, c0 - 12), y0: inner.y0, x1: Math.min(inner.x1, c1 + 12), y1: layout.corridor.y0 - 2 };
    const lobby = grid.addRoom("lobby", [lob], { paint: "PAINT_WHITE", floorMat: "FLOOR_MARBLE" });
    grid.addDoor(lobby, corr, { kind: "opening", width: Math.min(24, lob.x1 - lob.x0 - 4), place: "center" });
    grid.addDoor(lobby, null, { kind: "entrance", width: 12, place: "near", near: { u: (lob.x0 + lob.x1) / 2, v: 0 }, leaf: "glass" });
    if (program === "retail") {
      shopsInZone(grid, ctx, { x0: inner.x0, y0: inner.y0, x1: inner.x1, y1: layout.corridor.y0 - 2 }, [lob], lobby);
      serviceRooms(grid, ctx, corr, { x0: inner.x0, y0: layout.corridor.y1 + 2, x1: inner.x1, y1: inner.y1 }, layout.coreIntervals);
    } else {
      corridorUnits(grid, ctx, corr, { front: true, back: true, skip: [[lob.x0, lob.x1]] });
    }
    return grid;
  }
  // sections: the stair hall is the entrance lobby of each section
  for (const sec of layout.sections) {
    const hall = grid.addRoom("lobby", [sec.hall], { paint: "PAINT_WHITE", floorMat: "FLOOR_TERRAZZO" });
    sec.hallRoom = hall;
    grid.addDoor(hall, null, { kind: "entrance", width: 10, place: "near", near: { u: (sec.hall.x0 + sec.hall.x1) / 2, v: 0 }, leaf: "wood" });
  }
  connectCores(grid, layout, cores, (k) => layout.sections[Math.min(k, layout.sections.length - 1)].hallRoom);
  if (program === "retail") {
    for (const sec of layout.sections) {
      const zones = sectionZones(sec, inner);
      for (const z of zones) shopWithBackroom(grid, ctx, z.rect, sec.hallRoom);
    }
  } else {
    sectionUnits(grid, ctx);
  }
  return grid;
}

function basementFloor(ctx) {
  const { pb, f, layout, inner } = ctx;
  const grid = pb.newGrid(f);
  const cores = paintCores(grid, layout);
  if (layout.mode === "corridor") {
    const corr = grid.addRoom("corridor", [layout.corridor], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" });
    connectCores(grid, layout, cores, () => corr);
    const zones = [
      { rect: { x0: inner.x0, y0: inner.y0, x1: inner.x1, y1: layout.corridor.y0 - 2 }, entry: "S", blocked: [] },
      { rect: { x0: inner.x0, y0: layout.corridor.y1 + 2, x1: inner.x1, y1: inner.y1 }, entry: "N", blocked: layout.coreIntervals },
    ];
    let k = 0;
    for (const z of zones) {
      for (const [a0, a1] of freeIntervals(z.rect.x0, z.rect.x1, z.blocked)) {
        for (const [p0, p1] of splitLength(a0, a1, 4 * M, 20, ctx.rng)) {
          const type = k % 7 === 3 ? "mechanical" : k % 7 === 5 ? "laundry" : "storage";
          const r = grid.addRoom(type, [{ x0: p0, y0: z.rect.y0, x1: p1, y1: z.rect.y1 }], { paint: "CONCRETE", floorMat: "FLOOR_CONCRETE" });
          // (a sliver beside a core too narrow for a door is a service shaft)
          if (!grid.addDoor(r, corr, { width: 7, kind: "interior", leaf: "metal" }) && !grid.addDoor(r, corr, { width: 6, margin: 1 })) r.type = "shaft";
          k += 1;
        }
      }
    }
    return grid;
  }
  for (const sec of layout.sections) {
    sec.hallRoom = grid.addRoom("hall", [sec.hall], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" });
  }
  connectCores(grid, layout, cores, (k) => layout.sections[Math.min(k, layout.sections.length - 1)].hallRoom);
  for (const sec of layout.sections) {
    for (const z of sectionZones(sec, inner)) {
      const r = grid.addRoom("storage", [z.rect], { paint: "CONCRETE", floorMat: "FLOOR_CONCRETE" });
      grid.addDoor(r, sec.hallRoom, { width: 7, leaf: "metal" });
    }
  }
  return grid;
}

function podiumFloor(ctx) {
  const { pb, f, layout } = ctx;
  const grid = pb.newGrid(f);
  const cores = paintCores(grid, layout);
  planOpenFloor(grid, ctx, cores, "office");
  return grid;
}

function lobbyOpenFloor(grid, ctx, cores) {
  planOpenFloor(grid, ctx, cores, "lobby");
  return grid;
}

// ---------------------------------------------------------------- units

function corridorUnits(grid, ctx, corr, { front, back, skip = [] }) {
  const { inner, layout, rng } = ctx;
  const zones = [];
  if (front) zones.push({ rect: { x0: inner.x0, y0: inner.y0, x1: inner.x1, y1: layout.corridor.y0 - 2 }, entry: "S", blocked: skip });
  if (back) zones.push({ rect: { x0: inner.x0, y0: layout.corridor.y1 + 2, x1: inner.x1, y1: inner.y1 }, entry: "N", blocked: layout.coreIntervals });
  let unitNo = 0;
  for (const z of zones) {
    for (const [a0, a1] of freeIntervals(z.rect.x0, z.rect.x1, z.blocked)) {
      if (a1 - a0 + 1 < 3 * M) {
        const r = grid.addRoom("storage", [{ x0: a0, y0: z.rect.y0, x1: a1, y1: z.rect.y1 }], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" });
        if (!grid.addDoor(r, corr, { width: 6, margin: 1 })) r.type = "shaft";
        continue;
      }
      for (const [p0, p1] of splitLength(a0, a1, Math.round(rng.float(7.5, 10.5) * M), 5 * M, rng)) {
        const rect = { x0: p0, y0: z.rect.y0, x1: p1, y1: z.rect.y1 };
        placeUnit(grid, ctx, rect, z.entry, corr, `${z.entry}${unitNo++}`);
      }
    }
  }
}

function sectionZones(sec, inner) {
  const out = [];
  if (sec.c0 - 2 >= sec.a0 + 3 * M) out.push({ rect: { x0: sec.a0, y0: inner.y0, x1: sec.c0 - 2, y1: inner.y1 }, entry: "E" });
  if (sec.a1 >= sec.c1 + 2 + 3 * M) out.push({ rect: { x0: sec.c1 + 2, y0: inner.y0, x1: sec.a1, y1: inner.y1 }, entry: "W" });
  return out;
}

function sectionUnits(grid, ctx) {
  const { layout, inner } = ctx;
  let unitNo = 0;
  for (const sec of layout.sections) {
    for (const z of sectionZones(sec, inner)) {
      placeUnit(grid, ctx, z.rect, z.entry, sec.hallRoom, `s${unitNo++}`, { u: (sec.hall.x0 + sec.hall.x1) / 2, v: (sec.hall.y0 + sec.hall.y1) / 2 });
    }
  }
}

function placeUnit(grid, ctx, rect, entry, circ, unitId, near = null) {
  const facades = facadeSidesOf(grid, rect);
  const res = planUnit(grid, rect, entry, circ, facades, { rng: ctx.rng, unit: unitId, entryNear: near });
  if (res) return res;
  // fallback: a single room that is guaranteed reachable
  const r = grid.addRoom("studio", [rect], { unit: unitId, paint: "PAINT_WHITE", floorMat: "FLOOR_OAK" });
  if (!grid.addDoor(r, circ, { width: 8, kind: "entry" })) grid.addDoor(r, circ, { width: 6, margin: 1, kind: "entry" });
  return { rooms: [r] };
}

// ---------------------------------------------------------------- retail / service

function shopsInZone(grid, ctx, zone, blockedRects, lobby) {
  const blocked = blockedRects.map((r) => [r.x0, r.x1]);
  for (const [a0, a1] of freeIntervals(zone.x0, zone.x1, blocked)) {
    if (a1 - a0 + 1 < 3 * M) {
      const st = grid.addRoom("storage", [{ x0: a0, y0: zone.y0, x1: a1, y1: zone.y1 }], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" });
      if (!(lobby && grid.addDoor(st, lobby, { width: 6, margin: 1 })) && !grid.addDoor(st, null, { width: 7, margin: 1, kind: "entrance", leaf: "metal" })) st.type = "shaft";
      continue;
    }
    for (const [p0, p1] of splitLength(a0, a1, Math.round(ctx.rng.float(6, 11) * M), 4 * M, ctx.rng)) {
      shopWithBackroom(grid, ctx, { x0: p0, y0: zone.y0, x1: p1, y1: zone.y1 }, null);
    }
  }
}

/**
 * Ground-floor tenants by town flavor: shops, cafés, bars, services. Each
 * kind is a room type with its own furnishing (furnish.js, civicRules.js).
 */
const SHOP_KINDS = {
  default: [["retail", 3], ["cafe", 2], ["restaurant", 1.2], ["bakery", 1], ["pharmacy", 0.7], ["bookshop", 0.6], ["clothing", 1], ["florist", 0.5], ["hardware", 0.4], ["barber", 0.6], ["pub", 1], ["bank", 0.5], ["laundromat", 0.3], ["grocery", 1], ["gallery", 0.35], ["venueFloor", 0.25], ["kiosk", 0.3]],
  nordic: [["retail", 2], ["cafe", 2], ["bakery", 1.5], ["fishmonger", 0.8], ["pharmacy", 0.7], ["bookshop", 0.6], ["clothing", 0.8], ["florist", 0.5], ["hardware", 0.5], ["barber", 0.5], ["pub", 0.8], ["bank", 0.4], ["grocery", 1.2], ["souvenir", 0.5], ["gallery", 0.3], ["venueFloor", 0.2], ["restaurant", 0.8]],
  soviet: [["produkty", 3], ["pharmacy", 1.2], ["kiosk", 1], ["bakery", 1], ["hardware", 0.8], ["barber", 0.8], ["clothing", 0.6], ["cafe", 0.8], ["bookshop", 0.4], ["bank", 0.4], ["butcher", 0.6], ["laundromat", 0.3]],
};
const FLAVOR_SHOPS = { nordic: "nordic", nordicHarbour: "nordic", harbourTown: "nordic", nordicBleak: "soviet", soviet: "soviet" };

/** A shop: sales floor with a street door, optional backroom and WC. */
export function shopWithBackroom(grid, ctx, rect, backCirc, kindOverride = null) {
  const { rng } = ctx;
  const depth = rect.y1 - rect.y0 + 1;
  const width = rect.x1 - rect.x0 + 1;
  const flavor = String(ctx.env?.flavor ?? "").replace(/Village$/, "");
  const kind = kindOverride ?? rng.weighted(SHOP_KINDS[FLAVOR_SHOPS[flavor] ?? "default"]);
  const withBack = depth >= 7 * M && width >= 4 * M;
  const salesEnd = withBack ? rect.y0 + Math.round(depth * 0.66) : rect.y1;
  const shop = grid.addRoom(kind, [{ ...rect, y1: salesEnd }], { paint: rng.pick(["PAINT_WHITE", "PAINT_GRAY", "PAINT_TERRACOTTA", "PAINT_SAGE"]), floorMat: rng.pick(["FLOOR_TILE_GRAY", "FLOOR_CONCRETE", "FLOOR_TERRAZZO", "FLOOR_OAK"]), shop: true });
  const door = grid.addDoor(shop, null, { kind: "shopfront", width: 10, place: "center", leaf: "glass" });
  if (!door) grid.addDoor(shop, null, { kind: "shopfront", width: 8, place: "auto", leaf: "glass" });
  if (withBack) {
    const back = { ...rect, y0: salesEnd + 2 };
    if (width >= 5 * M) {
      const wcW = 12;
      const wc = grid.addRoom("wc", [{ ...back, x1: back.x0 + wcW - 1 }], { paint: "WALL_TILE_WHITE", floorMat: "FLOOR_TILE_WHITE" });
      const br = grid.addRoom("backroom", [{ ...back, x0: back.x0 + wcW + 1 }], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" });
      grid.addDoor(br, shop, { width: 7 }) ?? grid.addDoor(br, shop, { width: 6, margin: 1 });
      grid.addDoor(wc, br, { width: 6, margin: 1 }) ?? grid.addDoor(wc, shop, { width: 6, margin: 1 });
      if (backCirc) grid.addDoor(br, backCirc, { width: 7, leaf: "metal" });
    } else {
      const br = grid.addRoom("backroom", [back], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" });
      grid.addDoor(br, shop, { width: 6, margin: 1 });
    }
  }
  return shop;
}

function serviceRooms(grid, ctx, corr, zone, blocked) {
  const types = ["bike", "storage", "laundry", "mechanical", "storage", "trash"];
  let k = 0;
  for (const [a0, a1] of freeIntervals(zone.x0, zone.x1, blocked)) {
    for (const [p0, p1] of splitLength(a0, a1, 5 * M, 20, ctx.rng)) {
      const r = grid.addRoom(types[k % types.length], [{ x0: p0, y0: zone.y0, x1: p1, y1: zone.y1 }], { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" });
      // (a sliver too narrow for a door, beside a stair in a narrow house, is a service shaft)
      if (!grid.addDoor(r, corr, { width: 7, leaf: "metal" }) && !grid.addDoor(r, corr, { width: 6, margin: 1 })) r.type = "shaft";
      k += 1;
    }
  }
}

/**
 * Balcony doors: living rooms on a facade get a glass door onto a balcony
 * (the slab + railing are emitted by fixtures). Probability comes from the
 * building's style.
 */
function addBalconies(grid, env, rng) {
  const style = STYLES.maybe(env.style);
  const p = style ? style.balcony : 0;
  const want = new Rng(Math.floor(rng.next() * 2 ** 31)).chance(Math.min(0.95, p * 1.6));
  if (!want) return;
  for (const room of grid.rooms) {
    if (room.type !== "living" && room.type !== "studio") continue;
    const r = room.rects[0];
    const front = r.y0 < 6;
    // (the angled world's wings, S5: no balcony where one is cast into the facade)
    if (env.wings && nearWing(env, { x0: r.x0, x1: r.x1, y0: front ? -12 : env.V, y1: front ? 0 : env.V + 12 })) continue;
    grid.addDoor(room, null, { kind: "balcony", width: 8, leaf: "glass", place: "near", near: { u: (r.x0 + r.x1) / 2, v: front ? -12 : env.V + 12 } });
  }
}
