import { stairDims, makeStair } from "./stairs.js";
import { addStairRoom, stairDoor, splitLength } from "./common.js";
import { tierRects } from "../archetypes.js";
import { EXT_T } from "./grid.js";
import { rSubtractAll } from "../../core/rect.js";
import { officeCore, planOpenFloor } from "./offices.js";
import { shopWithBackroom } from "./apartments.js";
import { vx } from "../../core/units.js";
import { MAT } from "../../voxel/materials.js";

/**
 * Interiors of civic buildings, venues and big shops. Three planners, each
 * driven by a program table:
 *
 *   hall       a foyer band along the street (entrance, side rooms, the
 *              stair), one to three big halls behind it (a supermarket
 *              floor, an auditorium, cinema screens, a market hall), a
 *              service band at the back (storage with a loading door,
 *              dressing rooms, staff rooms, WCs). With two floors the halls
 *              are double height: the upper floor keeps a gallery / bar
 *              over the foyer and a `void` over the halls (slab open).
 *   corridor   a double-loaded corridor with a stair at each end (like the
 *              school): rooms by floor from the program (hospital wards and
 *              theatres, police cells and interview rooms, museum and
 *              gallery halls spanning several bays, hotel rooms...), a
 *              lobby with the street entrance on the ground floor, back or
 *              front doors and roll-up doors where the program asks.
 *   kiosk      one small shop (petrol station shop, market kiosk).
 *
 * Every room is reachable through doors and stairs; `validatePlan` and the
 * voxel walker check it like any other building.
 */

const WHITE = { paint: "PAINT_WHITE", floorMat: "FLOOR_TERRAZZO" };

/** Finishes per room type. */
const FINISH = {
  foyer: { paint: "PAINT_CREAM", floorMat: "FLOOR_MARBLE" },
  lobby: { paint: "PAINT_WHITE", floorMat: "FLOOR_TERRAZZO" },
  sales: { paint: "PAINT_WHITE", floorMat: "FLOOR_TILE_GRAY" },
  marketHall: { paint: "PAINT_CREAM", floorMat: "FLOOR_TILE_TERRA" },
  auditorium: { paint: "PAINT_DARK", floorMat: "FLOOR_CARPET_RED" },
  cinema: { paint: "PAINT_DARK", floorMat: "FLOOR_CARPET_BLUE" },
  venueFloor: { paint: "PAINT_DARK", floorMat: "FLOOR_CONCRETE" },
  foyerBar: { paint: "PAINT_TERRACOTTA", floorMat: "FLOOR_PARQUET" },
  danceHall: { paint: "PAINT_CREAM", floorMat: "FLOOR_PARQUET" },
  dressing: { paint: "PAINT_PEACH", floorMat: "FLOOR_LINOLEUM" },
  cloakroom: { paint: "PAINT_WHITE", floorMat: "FLOOR_MARBLE" },
  kiosk: { paint: "PAINT_WHITE", floorMat: "FLOOR_TILE_GRAY" },
  storage: { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" },
  breakroom: { paint: "PAINT_MINT", floorMat: "FLOOR_LINOLEUM" },
  office: { paint: "PAINT_WHITE", floorMat: "FLOOR_CARPET_GRAY" },
  wc: { paint: "WALL_TILE_WHITE", floorMat: "FLOOR_TILE_WHITE" },
  restroom: { paint: "WALL_TILE_WHITE", floorMat: "FLOOR_TILE_GRAY" },
  ward: { paint: "PAINT_MINT", floorMat: "FLOOR_LINOLEUM" },
  emergency: { paint: "PAINT_MINT", floorMat: "FLOOR_LINOLEUM" },
  exam: { paint: "PAINT_WHITE", floorMat: "FLOOR_LINOLEUM" },
  operating: { paint: "WALL_TILE_GREEN", floorMat: "FLOOR_EPOXY" },
  radiology: { paint: "PAINT_BLUE", floorMat: "FLOOR_LINOLEUM" },
  waiting: { paint: "PAINT_SAGE", floorMat: "FLOOR_LINOLEUM" },
  nurses: { paint: "PAINT_WHITE", floorMat: "FLOOR_LINOLEUM" },
  pharmacy: { paint: "PAINT_WHITE", floorMat: "FLOOR_TILE_WHITE" },
  policeDesk: { paint: "PAINT_BLUE", floorMat: "FLOOR_TERRAZZO" },
  cell: { paint: "CONCRETE", floorMat: "FLOOR_CONCRETE" },
  interview: { paint: "PAINT_GRAY", floorMat: "FLOOR_LINOLEUM" },
  briefing: { paint: "PAINT_WHITE", floorMat: "FLOOR_CARPET_BLUE" },
  evidence: { paint: "PAINT_GRAY", floorMat: "FLOOR_CONCRETE" },
  lockerRoom: { paint: "PAINT_GRAY", floorMat: "FLOOR_TILE_GRAY" },
  garageBay: { paint: "CONCRETE", floorMat: "FLOOR_EPOXY" },
  dorm: { paint: "PAINT_CREAM", floorMat: "FLOOR_LINOLEUM" },
  kitchen: { paint: "WALL_TILE_WHITE", floorMat: "FLOOR_TILE_WHITE" },
  dining: { paint: "PAINT_PEACH", floorMat: "FLOOR_OAK" },
  exhibit: { paint: "PAINT_CREAM", floorMat: "FLOOR_PARQUET" },
  gallery: { paint: "PAINT_WHITE", floorMat: "FLOOR_OAK" },
  museumShop: { paint: "PAINT_SAGE", floorMat: "FLOOR_OAK" },
  cafe: { paint: "PAINT_TERRACOTTA", floorMat: "FLOOR_OAK" },
  restaurant: { paint: "PAINT_PEACH", floorMat: "FLOOR_OAK" },
  pub: { paint: "WOOD_PANEL", floorMat: "FLOOR_WALNUT" },
  library: { paint: "PAINT_SAGE", floorMat: "FLOOR_CARPET_BLUE" },
  study: { paint: "PAINT_CREAM", floorMat: "FLOOR_CARPET_BEIGE" },
  reception: { paint: "PAINT_WHITE", floorMat: "FLOOR_MARBLE" },
  council: { paint: "WOOD_PANEL", floorMat: "FLOOR_CARPET_RED" },
  registry: { paint: "PAINT_CREAM", floorMat: "FLOOR_PARQUET" },
  meeting: { paint: "PAINT_WHITE", floorMat: "FLOOR_CARPET_BLUE" },
  hotelRoom: { paint: "PAINT_CREAM", floorMat: "FLOOR_CARPET_BEIGE" },
  clubroom: { paint: "PAINT_CREAM", floorMat: "FLOOR_PARQUET" },
  departmentFloor: { paint: "PAINT_WHITE", floorMat: "FLOOR_TERRAZZO" },
};

function finish(type) {
  return FINISH[type] ?? WHITE;
}

function inset(r, d) {
  return { x0: r.x0 + d, y0: r.y0 + d, x1: r.x1 - d, y1: r.y1 - d };
}

/** Door to a neighbour, falling back to a narrower one; a room that gets no door becomes a shaft. */
function connect(grid, room, to, opts = {}) {
  if (grid.addDoor(room, to, { width: 8, ...opts })) return true;
  if (grid.addDoor(room, to, { ...opts, width: 6, margin: 1 })) return true;
  return false;
}

// ============================================================ hall buildings

/**
 * program: {
 *   foyer: depth (m) of the street band; foyerSide: side room types at the
 *   end opposite the stair; hall: room type; halls: [min, max] count;
 *   hallProps: extra room props (noWindows, classical...); back: service
 *   room types; backDepth (m); dock: 'rollup' | 'metal' (the back door of
 *   the first back room); upperFoyer: type of the upper landing;
 *   upperSide: rooms beside it
 * }
 */
export function planHall({ env, rng, pb }, P) {
  const nF = env.floors;
  const fp = tierRects(env, 0)[0];
  const inner = inset(fp, EXT_T);
  const Vi = inner.y1 - inner.y0 + 1;
  const Ui = inner.x1 - inner.x0 + 1;
  let maxH = 0;
  for (const h of env.storyH) maxH = Math.max(maxH, h);
  const sd = nF > 1 ? stairDims(maxH) : null;
  const fd = Math.round(Math.max(sd ? sd.W + 14 : 24, Math.min(vx(P.foyer ?? 5), Vi * 0.34)));
  const bd = P.back?.length ? Math.round(Math.max(26, Math.min(vx(P.backDepth ?? 4.5), Vi * 0.28))) : 0;
  const foyerBand = { x0: inner.x0, y0: inner.y0, x1: inner.x1, y1: inner.y0 + fd - 1 };
  const backBand = bd ? { x0: inner.x0, y0: inner.y1 - bd + 1, x1: inner.x1, y1: inner.y1 } : null;
  const hallZone = { x0: inner.x0, y0: foyerBand.y1 + 2, x1: inner.x1, y1: backBand ? backBand.y0 - 2 : inner.y1 };
  // the stair in a front corner of the foyer band, its near end towards the middle
  const stairLeft = rng.chance(0.5);
  let stair = null;
  let sRect = null;
  if (sd) {
    sRect = stairLeft ? { x0: inner.x0, y0: inner.y0, x1: inner.x0 + sd.L - 1, y1: inner.y0 + sd.W - 1 } : { x0: inner.x1 - sd.L + 1, y0: inner.y0, x1: inner.x1, y1: inner.y0 + sd.W - 1 };
    stair = pb.addStair(makeStair({ rect: sRect, axis: "u", dir: stairLeft ? -1 : 1, laneLow: false, f0: 0, f1: nF - 1 }));
  }
  // side rooms at the other end of the foyer band
  const sideW = Math.round(Math.min(vx(5), Math.max(28, Ui * 0.14)));
  const sideTypes = (P.foyerSide ?? []).slice(0, Math.max(0, Math.floor((Ui - (sd ? sd.L : 0) - 60) / (sideW + 1))));
  const sideRects = [];
  let edge = stairLeft ? inner.x1 : inner.x0;
  // (in a deep foyer the side rooms leave a passage behind them)
  const sideY1 = fd >= 40 ? foyerBand.y1 - 14 : foyerBand.y1;
  for (const t of sideTypes) {
    const r = stairLeft ? { x0: edge - sideW + 1, x1: edge, y0: foyerBand.y0, y1: sideY1 } : { x0: edge, x1: edge + sideW - 1, y0: foyerBand.y0, y1: sideY1 };
    sideRects.push({ type: t, rect: r });
    edge = stairLeft ? r.x0 - 2 : r.x1 + 2;
  }
  const cut = (r) => ({ x0: r.x0 - 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1 });
  const foyerRects = rSubtractAll([foyerBand], [...sideRects.map((s) => cut(s.rect)), ...(sRect ? [cut(sRect)] : [])]).filter((r) => r.x1 - r.x0 >= 3 && r.y1 - r.y0 >= 3);
  // halls side by side, backs rooms in pieces behind them
  const nHalls = Math.max(1, Math.min(P.halls?.[1] ?? 1, Math.max(P.halls?.[0] ?? 1, Math.floor(Ui / vx(P.hallMinW ?? 9)))));
  const hallPieces = splitLength(hallZone.x0, hallZone.x1, Math.floor((Ui - (nHalls - 1)) / nHalls), 24, rng, 0.1);
  const backPieces = backBand ? splitLength(backBand.x0, backBand.x1, Math.round(vx(P.backW ?? 5)), 24, rng, 0.2) : [];

  // ---- ground floor
  const g0 = pb.newGrid(0);
  const sRoom0 = stair ? addStairRoom(g0, stair) : null;
  const foyer = g0.addRoom(P.foyerType ?? "foyer", foyerRects, finish(P.foyerType ?? "foyer"));
  const halls = hallPieces.map(([a0, a1]) => g0.addRoom(P.hall, [{ x0: a0, x1: a1, y0: hallZone.y0, y1: hallZone.y1 }], { ...finish(P.hall), ...(P.hallProps ?? {}), tall: nF > 1, front: "N" }));
  const sides = sideRects.map((s) => g0.addRoom(s.type, [s.rect], finish(s.type)));
  const backs = backPieces.map(([a0, a1], k) => {
    const type = P.back[k % P.back.length];
    return g0.addRoom(type, [{ x0: a0, x1: a1, y0: backBand.y0, y1: backBand.y1 }], finish(type));
  });
  // entrance, halls off the foyer, side rooms off the foyer, back rooms off the halls
  const fr = foyerRects.slice().sort((a, b) => b.x1 - b.x0 - (a.x1 - a.x0))[0];
  g0.addDoor(foyer, null, { kind: "entrance", width: Math.min(16, fr.x1 - fr.x0 - 4), place: "near", near: { u: (fr.x0 + fr.x1) / 2, v: -12 }, leaf: "glass" });
  for (const h of halls) {
    if (!(g0.addDoor(h, foyer, { width: P.hallDoor ?? 14, kind: P.hallOpen ? "opening" : "interior", leaf: P.hallOpen ? "none" : "wood", place: "center" }) || connect(g0, h, foyer))) connect(g0, h, halls[0] === h ? halls[1] ?? foyer : halls[0]);
  }
  for (const s of sides) if (!connect(g0, s, foyer)) s.type = "shaft";
  for (const b of backs) {
    const [ha] = halls.filter((h) => h.rects[0].x0 <= b.rects[0].x1 && h.rects[0].x1 >= b.rects[0].x0).sort((p, q) => overlap(q, b) - overlap(p, b));
    if (!(ha && connect(g0, b, ha, { leaf: "metal" })) && !halls.some((h) => connect(g0, b, h, { leaf: "metal" }))) b.type = "shaft";
  }
  // the loading / stage door at the back
  if (backs.length && P.dock) {
    const dockRoom = backs.find((b) => b.type !== "shaft" && b.type !== "wc") ?? backs[0];
    const r = dockRoom.rects[0];
    const rollup = P.dock === "rollup" && r.x1 - r.x0 >= 36;
    g0.addDoor(dockRoom, null, rollup ? { kind: "rollup", width: 28, margin: 4, place: "near", near: { u: (r.x0 + r.x1) / 2, v: env.V + 12 }, leaf: "rollup", height: 28 } : { kind: "entrance", width: 8, place: "near", near: { u: (r.x0 + r.x1) / 2, v: env.V + 12 }, leaf: "metal" });
  }
  // fire exits from the end halls onto the side streets
  for (const h of [halls[0], halls[halls.length - 1]]) {
    const r = h.rects[0];
    const onLeft = r.x0 === inner.x0;
    if (onLeft || r.x1 === inner.x1) g0.addDoor(h, null, { kind: "entrance", width: 8, place: "near", near: { u: onLeft ? -12 : env.U + 12, v: (r.y0 + r.y1) / 2 }, leaf: "metal" });
  }
  if (sRoom0) stairDoor(g0, sRoom0, stair, foyer);
  pb.addFloor(0, g0, "ground");

  // ---- upper floor: a gallery / bar over the foyer, voids over the halls
  if (nF > 1) {
    const g1 = pb.newGrid(1);
    const sRoom1 = addStairRoom(g1, stair);
    const upSide = (P.upperSide ?? []).slice(0, sideRects.length);
    const upRects = sideRects.slice(0, upSide.length);
    const landRects = rSubtractAll([foyerBand], [...upRects.map((s) => cut(s.rect)), cut(sRect)]).filter((r) => r.x1 - r.x0 >= 3 && r.y1 - r.y0 >= 3);
    const land = g1.addRoom(P.upperFoyer ?? "foyerBar", landRects, finish(P.upperFoyer ?? "foyerBar"));
    stairDoor(g1, sRoom1, stair, land);
    upSide.forEach((t, k) => {
      const room = g1.addRoom(t, [upRects[k].rect], finish(t));
      if (!connect(g1, room, land)) room.type = "shaft";
    });
    for (const h of halls) g1.addRoom("void", h.rects, { noWindows: !!P.hallProps?.noWindows });
    if (backBand) g1.addRoom("shaft", [backBand]);
    pb.addFloor(1, g1, "upper");
  }
}

function overlap(h, b) {
  return Math.min(h.rects[0].x1, b.rects[0].x1) - Math.max(h.rects[0].x0, b.rects[0].x0);
}

// ============================================================ corridor buildings

/**
 * program: {
 *   bay (m): room width target; corr (m): corridor width;
 *   ground / first / upper: { front: [spec], back: [spec], fill: [types] }
 *   spec: type string or { type, span (bays), at ('mid' | 'start' | 'end'),
 *         ext ('front' | 'back' | 'rollupFront' | 'rollupBack'), split (n
 *         rooms side by side in one bay), lobby (entrance + opening) }
 * }
 */
export function planCorridor({ env, rng, pb }, P) {
  const nF = env.floors;
  const fp = tierRects(env, 0)[0];
  const inner = inset(fp, EXT_T);
  const Vi = inner.y1 - inner.y0 + 1;
  let maxH0 = 0;
  for (const h of env.storyH) maxH0 = Math.max(maxH0, h);
  // (the stairs sit in the corridor band: it is at least as wide as they are)
  const CORR = Math.max(Math.round(vx(P.corr ?? 3)), nF > 1 ? stairDims(maxH0).W + 1 : 0);
  const depth = Math.floor((Vi - CORR - 2) / 2);
  const band = { x0: inner.x0, x1: inner.x1, y0: inner.y0 + depth + 1, y1: inner.y0 + depth + CORR };
  const front = { x0: inner.x0, x1: inner.x1, y0: inner.y0, y1: band.y0 - 2 };
  const back = { x0: inner.x0, x1: inner.x1, y0: band.y1 + 2, y1: inner.y1 };
  let maxH = 0;
  for (const h of env.storyH) maxH = Math.max(maxH, h);
  const stairs = [];
  let corrRects = [band];
  let minEnd = 24;
  if (nF > 1) {
    const sd = stairDims(maxH);
    const sy0 = band.y0 + Math.floor((CORR - sd.W) / 2);
    const left = { x0: inner.x0, x1: inner.x0 + sd.L - 1, y0: sy0, y1: sy0 + sd.W - 1 };
    const right = { x0: inner.x1 - sd.L + 1, x1: inner.x1, y0: sy0, y1: sy0 + sd.W - 1 };
    const top = env.roof.type === "flat" ? nF : nF - 1;
    stairs.push(pb.addStair(makeStair({ rect: left, axis: "u", dir: -1, laneLow: true, f0: 0, f1: top })));
    stairs.push(pb.addStair(makeStair({ rect: right, axis: "u", dir: 1, laneLow: false, f0: 0, f1: top })));
    corrRects = rSubtractAll([band], [left, right].map((q) => ({ x0: q.x0 - 1, y0: band.y0, x1: q.x1 + 1, y1: band.y1 }))).filter((q) => q.x1 >= q.x0 && q.y1 >= q.y0);
    minEnd = sd.L + 24;
  }
  const bay = Math.round(vx(P.bay ?? 6));
  const fPieces = endSafe(splitLength(front.x0, front.x1, bay, 28, rng), minEnd);
  const bPieces = endSafe(splitLength(back.x0, back.x1, bay, 28, rng), minEnd);

  const floorPlan = (f) => {
    const grid = pb.newGrid(f);
    const stairRooms = stairs.map((st) => addStairRoom(grid, st));
    const corridor = grid.addRoom("corridor", corrRects, { paint: P.corrPaint ?? "PAINT_CREAM", floorMat: P.corrFloor ?? "FLOOR_TERRAZZO" });
    stairRooms.forEach((sr, k) => stairDoor(grid, sr, stairs[k], corridor));
    const prog = f === 0 ? P.ground : f === 1 && P.first ? P.first : P.upper ?? P.first ?? P.ground;
    for (const [sideName, pieces, zone] of [["front", fPieces, front], ["back", bPieces, back]]) {
      const specs = assign(pieces.length, (prog[sideName] ?? []).map((sp) => (typeof sp === "string" ? { type: sp } : sp)), prog.fill ?? ["office"], rng);
      for (const a of specs) {
        const rect = { x0: pieces[a.from][0], x1: pieces[a.to][1], y0: zone.y0, y1: zone.y1 };
        // (split rooms, e.g. holding cells, stay at least ~3 m wide)
        const n = Math.max(1, Math.min(a.spec.split ?? 1, Math.floor((rect.x1 - rect.x0 + 2) / 26)));
        const parts = n > 1 ? offCorridor(splitLength(rect.x0, rect.x1, Math.floor((rect.x1 - rect.x0 + 1 - (n - 1)) / n), 16, rng, 0), corrRects) : [[rect.x0, rect.x1]];
        for (const [p0, p1] of parts) {
          const t = a.spec.type;
          const r = { ...rect, x0: p0, x1: p1 };
          const room = grid.addRoom(t, [r], { ...finish(t), ...(a.spec.props ?? {}), front: sideName === "front" ? "N" : "S" });
          const wide = p1 - p0 > 60;
          if (a.spec.lobby) {
            grid.addDoor(room, null, { kind: "entrance", width: Math.min(16, p1 - p0 - 6), place: "near", near: { u: (p0 + p1) / 2, v: -12 }, leaf: "glass" });
            grid.addDoor(room, corridor, { width: Math.min(16, p1 - p0 - 6), place: "center", kind: "opening", leaf: "none" }) || connect(grid, room, corridor);
          } else if (!connect(grid, room, corridor, { width: wide ? 12 : 8, leaf: a.spec.leaf ?? "wood" })) {
            room.type = "shaft";
            continue;
          }
          const ext = a.spec.ext;
          if (ext === "front" || ext === "back") grid.addDoor(room, null, { kind: "entrance", width: 10, place: "near", near: { u: (p0 + p1) / 2, v: ext === "front" ? -12 : env.V + 12 }, leaf: ext === "front" ? "glass" : "metal" });
          else if (ext === "rollupFront" || ext === "rollupBack") {
            const w = Math.min(28, p1 - p0 - 10);
            const nDoors = Math.max(1, Math.floor((p1 - p0 + 1) / 40));
            for (let k = 0; k < nDoors; k += 1) {
              const u = p0 + ((k + 0.5) * (p1 - p0)) / nDoors;
              const dr = grid.addDoor(room, null, { kind: "rollup", width: w, margin: 3, place: "near", near: { u, v: ext === "rollupFront" ? -12 : env.V + 12 }, leaf: "rollup", height: Math.min(30, env.storyH[0] - 6) });
              if (dr && a.spec.props?.fire) dr.color = MAT.FIRE_RED;
            }
          }
        }
      }
    }
    return grid;
  };

  pb.addFloor(0, floorPlan(0), "ground");
  if (nF > 1) pb.addFloor(1, floorPlan(1), "upper");
  let upper = null;
  for (let f = 2; f < nF; f += 1) {
    upper ??= floorPlan(f);
    pb.addFloor(f, upper, "upper");
  }
}

/** Split parts [[u0, u1]...] with the ones out of the corridor's reach (behind an end stair) joined to their neighbour. */
function offCorridor(parts, corrRects) {
  const lo = Math.min(...corrRects.map((r) => r.x0)) + 10;
  const hi = Math.max(...corrRects.map((r) => r.x1)) - 10;
  const out = parts.map((q) => [...q]);
  while (out.length > 1 && out[0][1] < lo) out.splice(0, 2, [out[0][0], out[1][1]]);
  while (out.length > 1 && out[out.length - 1][0] > hi) out.splice(out.length - 2, 2, [out[out.length - 2][0], out[out.length - 1][1]]);
  return out;
}

/**
 * Assign room specs to n pieces: those with `at` first (mid / start / end),
 * then the rest in order; spans take consecutive pieces; free pieces get
 * the fill types in turn. Returns [{from, to, spec}] in piece order.
 */
function assign(n, specs, fill, rng) {
  const used = new Array(n).fill(null);
  const out = [];
  const place = (spec, from) => {
    const span = Math.max(1, Math.min(spec.span ?? 1, n > 1 ? n - 1 : 1));
    for (let s = from; s + span <= n; s += 1) {
      let ok = true;
      for (let k = s; k < s + span; k += 1) if (used[k]) ok = false;
      if (!ok) continue;
      for (let k = s; k < s + span; k += 1) used[k] = spec;
      out.push({ from: s, to: s + span - 1, spec });
      return true;
    }
    return false;
  };
  const placeExact = (spec, s, span) => {
    for (let k = s; k < s + span; k += 1) if (used[k]) return false;
    for (let k = s; k < s + span; k += 1) used[k] = spec;
    out.push({ from: s, to: s + span - 1, spec });
    return true;
  };
  // (a wide room never takes every piece: the others need a place too)
  const spanOf = (spec) => Math.max(1, Math.min(spec.span ?? 1, n > 1 ? n - 1 : 1));
  const tryAt = (spec) => {
    const span = spanOf(spec);
    const start = spec.at === "mid" ? Math.max(0, Math.floor((n - span) / 2)) : spec.at === "end" ? n - span : 0;
    const order = [start];
    for (let d = 1; d < n; d += 1) order.push(start + d, start - d);
    for (const s of order) if (s >= 0 && s + span <= n && placeExact(spec, s, span)) return;
  };
  for (const spec of specs) if (spec.at) tryAt(spec);
  for (const spec of specs) if (!spec.at) place(spec, 0);
  let k = 0;
  for (let i = 0; i < n; i += 1) {
    if (used[i]) continue;
    const spec = { type: fill[k % fill.length] };
    k += 1;
    used[i] = spec;
    out.push({ from: i, to: i, spec });
  }
  void rng;
  return out.sort((a, b) => a.from - b.from);
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

// ============================================================ small buildings

/** A kiosk / petrol station shop: one shop room with a back room and a WC. */
export function planKiosk({ env, rng, pb }, P) {
  const g = pb.newGrid(0);
  const inner = inset(tierRects(env, 0)[0], EXT_T);
  shopWithBackroom(g, { rng, env }, inner, null, P.shop ?? "grocery");
  pb.addFloor(0, g, "ground");
}

/**
 * Department store (Kaufhaus, univermag): the office core (stairs,
 * elevators, restrooms) with open sales floors round it on every floor.
 */
export function planDepartmentStore({ env, rng, pb }) {
  const nF = env.floors;
  const top = tierRects(env, nF - 1)[0];
  const inner = inset(top, EXT_T);
  let maxH = 0;
  for (const h of env.storyH) maxH = Math.max(maxH, h);
  const sd = stairDims(maxH);
  const core = officeCore(inner, sd, nF);
  const layout = { stairs: [], elevators: [], restrooms: [], shafts: [] };
  for (const c of core.comps) {
    if (c.kind === "stair") layout.stairs.push({ rect: c.rect, stair: pb.addStair(makeStair({ rect: c.rect, axis: "v", dir: 1, laneLow: rng.chance(0.5), f0: 0, f1: nF })) });
    else if (c.kind === "restroom") layout.restrooms.push(c.rect);
    else {
      for (let k = 0; k < core.nElev; k += 1) {
        const ex0 = c.rect.x0 + k * 17;
        const rect = { x0: ex0, y0: c.rect.y0, x1: ex0 + 15, y1: c.rect.y0 + 15 };
        layout.elevators.push({ rect, elev: pb.addElevator({ rect, f0: 0, f1: nF - 1, doorSide: "N" }) });
      }
      layout.shafts.push({ x0: c.rect.x0, y0: c.rect.y0 + 17, x1: c.rect.x1, y1: c.rect.y1 });
    }
  }
  const ctx = { env, rng, pb, inner, layout };
  let typical = null;
  for (let f = 0; f < nF; f += 1) {
    if (f > 1 && typical) {
      pb.addFloor(f, typical, "store");
      continue;
    }
    const g = pb.newGrid(f);
    const stairRooms = layout.stairs.map((s) => addStairRoom(g, s.stair));
    const elevRooms = layout.elevators.map((e) => g.addRoom("elevator", [e.rect], { elevator: e.elev.id }));
    for (const s of layout.shafts) g.addRoom("shaft", [s]);
    const restrooms = layout.restrooms.map((r) => g.addRoom("restroom", [r], finish("restroom")));
    const open = planOpenFloor(g, ctx, { stairRooms, elevRooms, restrooms, layout }, "store");
    if (f === 0) g.addDoor(open, null, { kind: "entrance", width: 16, place: "near", near: { u: (inner.x0 + inner.x1) / 2, v: 0 }, leaf: "glass" });
    pb.addFloor(f, g, "store");
    if (f === 1) typical = g;
  }
}
