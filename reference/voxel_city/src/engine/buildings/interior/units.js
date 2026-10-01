import { Frame } from "../frame.js";

/**
 * Apartment unit planner.
 *
 * A unit is a rect in the building-canonical frame with an ENTRY side (the
 * wall shared with the corridor / stair hall) and zero or more FACADE sides.
 * Planning happens in a unit-local frame where the entry is at v = 0.
 *
 * Templates (tried in order of fit, with fewer bedrooms as fallback):
 *   band     entry opposite the facade: service row (kitchen, foyer, bath),
 *            a hall row, then living + bedrooms along the facade
 *   through  facades on both ends (walk-up "through" units): living on one
 *            facade, bedrooms on the other, foyer/bath/kitchen between
 *   rail     narrow deep unit: hall down one side, living at the facade
 *   open     studio fallback: foyer + bath + one main room
 *
 * Every template is painted into the real FloorGrid and must be able to
 * place every required door on an actual shared wall; otherwise the grid is
 * rolled back and the next candidate is tried. So a returned unit is always
 * fully connected.
 */

const M = 8; // voxels per meter

/** Split [a0, a1] into spans of the given widths separated by 1-cell walls; last span absorbs the rest. */
function spans(a0, a1, widths) {
  const out = [];
  let a = a0;
  for (let k = 0; k < widths.length; k += 1) {
    const last = k === widths.length - 1;
    const e = last ? a1 : a + widths[k] - 1;
    out.push([a, e]);
    a = e + 2;
  }
  return out;
}

/** Split [a0,a1] into n near-equal spans separated by walls. */
function evenSpans(a0, a1, n) {
  const total = a1 - a0 + 1 - (n - 1);
  const base = Math.floor(total / n);
  const widths = new Array(n).fill(base);
  return spans(a0, a1, widths);
}

function rectOf(u0, v0, u1, v1) {
  return { x0: u0, y0: v0, x1: u1, y1: v1 };
}

function bedroomsForArea(areaM2) {
  if (areaM2 < 33) return 0;
  if (areaM2 < 52) return 1;
  if (areaM2 < 80) return 2;
  return 3;
}

// --- templates (unit frame: u in [0,U-1], v in [0,V-1], entry at v=0) ---

function tBand(U, V, nb, rng) {
  const rooms = [];
  const doors = [];
  const s1 = Math.max(18, Math.min(23, Math.round(V * 0.3)));
  const fw = rng.int(11, 14);
  if (nb === 0) {
    const bw = Math.max(14, Math.min(20, U - fw - 1));
    if (U < fw + 1 + 14 || V < s1 + 1 + 24) return null;
    const [ba, f] = spans(0, U - 1, [bw, fw]);
    rooms.push({ key: "Ba", type: "bath", rect: rectOf(ba[0], 0, ba[1], s1 - 1) });
    rooms.push({ key: "F", type: "foyer", rect: rectOf(f[0], 0, f[1], s1 - 1), entry: true });
    rooms.push({ key: "Main", type: "studio", rect: rectOf(0, s1 + 1, U - 1, V - 1), facade: true });
    doors.push(["F", "Main", { kind: "opening", width: Math.min(12, f[1] - f[0] - 1), place: "center" }]);
    doors.push(["F", "Ba", { width: 6 }]);
    return { rooms, doors };
  }
  const hall = nb >= 2;
  const hd = hall ? rng.int(9, 10) : 0;
  const bedTop = hall ? s1 + hd + 2 : s1 + 1;
  if (V - bedTop < 24) return null;
  const kw = rng.int(20, 26);
  let lw;
  if (nb === 1) lw = Math.max(32, kw + 1 + fw);
  else lw = rng.int(32, 38);
  const bedsW = U - lw - 1;
  if (bedsW < nb * 22 + (nb - 1)) return null;
  if (nb === 1) {
    const baW = U - (kw + fw + 2);
    if (baW < 14) return null;
    rooms.push({ key: "K", type: "kitchen", rect: rectOf(0, 0, kw - 1, s1 - 1) });
    rooms.push({ key: "F", type: "foyer", rect: rectOf(kw + 1, 0, kw + fw, s1 - 1), entry: true });
    rooms.push({ key: "Ba", type: "bath", rect: rectOf(kw + fw + 2, 0, U - 1, s1 - 1) });
    rooms.push({ key: "L", type: "living", rect: rectOf(0, s1 + 1, lw - 1, V - 1), facade: true });
    rooms.push({ key: "B1", type: "bedroom", rect: rectOf(lw + 1, s1 + 1, U - 1, V - 1), facade: true });
    doors.push(["F", "L", { kind: "opening", width: Math.min(10, fw - 2), place: "center" }]);
    doors.push(["K", "L", { kind: "opening", width: Math.min(14, kw - 4), place: "center" }]);
    doors.push(["F", "Ba", { width: 6 }]);
    doors.push(["L", "B1", {}]);
    return { rooms, doors };
  }
  // 2-3 bedrooms with a hall
  const svcW = U - (lw + fw + 2);
  if (svcW < 14) return null;
  rooms.push({ key: "K", type: "kitchen", rect: rectOf(0, 0, lw - 1, s1 - 1) });
  rooms.push({ key: "F", type: "foyer", rect: rectOf(lw + 1, 0, lw + fw, s1 - 1), entry: true });
  // service rooms right of the foyer: bath (+ wc / closet when wide)
  const sv0 = lw + fw + 2;
  if (svcW >= 14 + 1 + 10 + (nb >= 3 ? 11 : 0)) {
    const bath = Math.max(14, Math.min(22, svcW - 11 - (nb >= 3 ? 12 : 0)));
    const parts = nb >= 3 && svcW >= bath + 1 + 10 + 1 + 10 ? [bath, 10, 10] : [bath, 10];
    const sp = spans(sv0, U - 1, parts);
    rooms.push({ key: "Ba", type: "bath", rect: rectOf(sp[0][0], 0, sp[0][1], s1 - 1) });
    rooms.push({ key: "C1", type: nb >= 3 ? "wc" : "closet", rect: rectOf(sp[1][0], 0, sp[1][1], s1 - 1) });
    if (sp[2]) rooms.push({ key: "C2", type: "closet", rect: rectOf(sp[2][0], 0, sp[2][1], s1 - 1) });
  } else {
    rooms.push({ key: "Ba", type: "bath", rect: rectOf(sv0, 0, U - 1, s1 - 1) });
  }
  rooms.push({ key: "H", type: "hall", rect: rectOf(lw + 1, s1 + 1, U - 1, s1 + hd) });
  rooms.push({ key: "L", type: "living", rect: rectOf(0, s1 + 1, lw - 1, V - 1), facade: true });
  const beds = evenSpans(lw + 1, U - 1, nb);
  beds.forEach(([a, b], k) => rooms.push({ key: `B${k + 1}`, type: "bedroom", rect: rectOf(a, bedTop, b, V - 1), facade: true }));
  doors.push(["F", "H", { kind: "opening", width: Math.min(9, fw - 2), place: "center" }]);
  doors.push(["H", "L", { place: "center" }]);
  doors.push(["K", "L", { kind: "opening", width: Math.min(16, lw - 6), place: "center" }]);
  doors.push(["Ba", "H", { width: 6 }]);
  if (rooms.some((r) => r.key === "C1")) doors.push(["C1", "H", { width: 6 }]);
  if (rooms.some((r) => r.key === "C2")) doors.push(["C2", "H", { width: 6 }]);
  for (let k = 1; k <= nb; k += 1) doors.push([`B${k}`, "H", {}]);
  return { rooms, doors };
}

function tThrough(U, V, nb, rng) {
  // facades at u=0 (living) and u=U-1 (bedrooms), entry at v=0 in the middle
  const rooms = [];
  const doors = [];
  const lw = rng.int(30, 36);
  const bw = rng.int(24, 30);
  const mid0 = lw + 1;
  const mid1 = U - bw - 2;
  const midW = mid1 - mid0 + 1;
  const nbr = Math.min(nb, V >= 2 * 24 + 1 ? 2 : 1);
  if (nbr < 1) return null;
  const needHall = nbr >= 2;
  const hw = needHall ? 10 : 0;
  if (midW < (needHall ? hw + 1 + 14 : 14)) return null;
  const fd = rng.int(12, 16);
  if (V < fd + 1 + 16 + 1 + 18) return null;
  rooms.push({ key: "L", type: "living", rect: rectOf(0, 0, lw - 1, V - 1), facade: true });
  const beds = evenSpans(0, V - 1, nbr);
  beds.forEach(([a, b], k) => rooms.push({ key: `B${k + 1}`, type: "bedroom", rect: rectOf(U - bw, a, U - 1, b), facade: true }));
  const svc1 = needHall ? mid1 - hw - 1 : mid1;
  rooms.push({ key: "F", type: "foyer", rect: rectOf(mid0, 0, mid1, fd - 1), entry: true });
  if (needHall) rooms.push({ key: "H", type: "hall", rect: rectOf(svc1 + 2, fd + 1, mid1, V - 1) });
  const bd = Math.max(16, Math.min(22, Math.round((V - fd - 1) * 0.45)));
  rooms.push({ key: "Ba", type: "bath", rect: rectOf(mid0, fd + 1, svc1, fd + bd) });
  rooms.push({ key: "K", type: "kitchen", rect: rectOf(mid0, fd + bd + 2, svc1, V - 1) });
  doors.push(["F", "L", { place: "center" }]);
  doors.push(["F", "Ba", { width: 6 }]);
  doors.push(["K", "L", { kind: "opening", width: 10, place: "center" }]);
  if (needHall) {
    doors.push(["F", "H", { kind: "opening", width: Math.min(8, hw - 2), place: "center" }]);
    doors.push(["B1", "H", {}]);
    doors.push(["B2", "H", {}]);
  } else {
    doors.push(["F", "B1", {}]);
  }
  return { rooms, doors };
}

function tRail(U, V, nb, rng) {
  const rooms = [];
  const doors = [];
  const hw = rng.int(9, 11);
  const ld = rng.int(30, 40);
  if (V < ld + 1 + 18 + 1 + 16 || U < hw + 1 + 18) return null;
  const withBed = nb >= 1 && U >= 32 + 1 + 22;
  const colEnd = V - ld - 2;
  rooms.push({ key: "H", type: "foyer", rect: rectOf(0, 0, hw - 1, colEnd), entry: true });
  if (withBed) {
    const [l, b] = spans(0, U - 1, [U - 22 - 1 - Math.max(0, U - 60), 22 + Math.max(0, U - 60)]);
    rooms.push({ key: "L", type: "living", rect: rectOf(l[0], V - ld, l[1], V - 1), facade: true });
    rooms.push({ key: "B1", type: "bedroom", rect: rectOf(b[0], V - ld, b[1], V - 1), facade: true });
  } else {
    rooms.push({ key: "L", type: "living", rect: rectOf(0, V - ld, U - 1, V - 1), facade: true });
  }
  const colLen = colEnd + 1;
  const [ba, k] = colLen >= 16 + 1 + 18 ? spans(0, colEnd, [16, colLen]) : [[0, colEnd], null];
  rooms.push({ key: "Ba", type: "bath", rect: rectOf(hw + 1, ba[0], U - 1, ba[1]) });
  if (k) rooms.push({ key: "K", type: "kitchen", rect: rectOf(hw + 1, k[0], U - 1, k[1]) });
  doors.push(["H", "L", { place: "center" }]);
  doors.push(["H", "Ba", { width: 6 }]);
  if (k) {
    doors.push(["H", "K", { kind: "opening", width: 9 }]);
  }
  if (withBed) doors.push(["L", "B1", {}]);
  return { rooms, doors };
}

function tOpen(U, V) {
  if (U < 24 || V < 26) return null;
  const rooms = [];
  const doors = [];
  const bw = Math.min(18, Math.floor(U * 0.45));
  const bd = Math.min(18, Math.floor(V * 0.4));
  rooms.push({ key: "Ba", type: "bath", rect: rectOf(0, 0, bw - 1, bd - 1) });
  rooms.push({ key: "Main", type: "studio", rect: rectOf(bw + 1, 0, U - 1, V - 1), entry: true, facade: true });
  rooms.push({ key: "Main2", type: "studio", rect: rectOf(0, bd + 1, bw, V - 1), merge: "Main" });
  doors.push(["Main", "Ba", { width: 6 }]);
  return { rooms, doors };
}

const TEMPLATES = { band: tBand, through: tThrough, rail: tRail, open: tOpen };

/**
 * Plan one unit into the grid.
 * @param grid       FloorGrid
 * @param rect       unit interior rect (building canonical)
 * @param entrySide  'N'|'S'|'E'|'W' side of rect touching `circ` (N = -v)
 * @param circ       circulation room the unit door opens onto
 * @param facades    Set of sides ('N','S','E','W') of rect lying on the exterior wall
 * @param opts       { rng, unit (id), entryNear {u,v}, kinds (template preference) }
 */
export function planUnit(grid, rect, entrySide, circ, facades, opts) {
  const { rng } = opts;
  const frame = new Frame(rect, entrySide);
  const U = frame.U;
  const V = frame.V;
  const fs = new Set([...facades].map((s) => frame.canonSide(s)));
  const areaM2 = (U * V) / (M * M);
  const nbMax = bedroomsForArea(areaM2);
  let order;
  if (fs.has("L") && fs.has("R") && !fs.has("B")) order = ["through", "open"];
  else if (fs.has("B")) order = U < 56 && V > 64 ? ["rail", "band", "open"] : ["band", "rail", "open"];
  else if (fs.has("L") || fs.has("R")) order = ["through", "band", "open"];
  else order = ["band", "open"];

  const unitStyle = pickUnitStyle(rng);
  const entryRange = entryCells(grid, frame, circ);
  if (!entryRange.length) return null;
  for (const name of order) {
    for (let nb = nbMax; nb >= 0; nb -= 1) {
      const variants = name === "open" ? 1 : 3;
      for (let attempt = 0; attempt < variants; attempt += 1) {
        const layout = TEMPLATES[name](U, V, nb, rng);
        if (!layout) continue;
        // try the layout and its mirror image; best entry overlap first
        const cands = [layout, mirrorLayout(layout, U)]
          .map((l) => ({ l, ov: entryOverlap(l, entryRange) }))
          .filter((c) => c.ov >= 10)
          .sort((a, b) => b.ov - a.ov);
        for (const c of cands) {
          const res = tryLayout(grid, frame, c.l, circ, opts, unitStyle, name);
          if (res) return res;
        }
      }
      if (name === "open") break;
    }
  }
  return null;
}

/** u' coordinates (unit frame) along the entry wall that face the circulation room. */
function entryCells(grid, frame, circ) {
  const out = [];
  const lab = 16 + circ.id;
  for (let u = 0; u < frame.U; u += 1) {
    const [x, y] = frame.toWorld(u, 0);
    const [ox, oy] = frame.toWorld(u, -2);
    if (grid.get(ox, oy) === lab && grid.get(x, y) === 2) out.push(u);
  }
  return out;
}

function entryOverlap(layout, entryRange) {
  const e = layout.rooms.find((r) => r.entry);
  if (!e || e.rect.y0 !== 0) return 0;
  let n = 0;
  for (const u of entryRange) if (u >= e.rect.x0 && u <= e.rect.x1) n += 1;
  return n;
}

function mirrorLayout(layout, U) {
  const m = (r) => ({ x0: U - 1 - r.x1, y0: r.y0, x1: U - 1 - r.x0, y1: r.y1 });
  return { rooms: layout.rooms.map((r) => ({ ...r, rect: m(r.rect) })), doors: layout.doors };
}

function tryLayout(grid, frame, layout, circ, opts, unitStyle, templateName) {
  const snap = grid.snapshot();
  const byKey = new Map();
  for (const r of layout.rooms) {
    const w = frame.rectToWorld(r.rect);
    if (r.merge) {
      const target = byKey.get(r.merge);
      if (!target || !grid.extendRoom(target, w)) {
        grid.restore(snap);
        return null;
      }
      continue;
    }
    if (!grid.isFree(w)) {
      grid.restore(snap);
      return null;
    }
    const room = grid.addRoom(r.type, [w], {
      unit: opts.unit,
      paint: r.type === "bath" || r.type === "wc" ? unitStyle.tile : unitStyle.paint,
      floorMat: floorFor(r.type, unitStyle),
      template: templateName,
    });
    byKey.set(r.key, room);
  }
  const entryRoom = layout.rooms.find((r) => r.entry);
  const entry = byKey.get(entryRoom.key);
  const entryDoor = grid.addDoor(entry, circ, {
    width: 8,
    margin: 1,
    kind: "entry",
    place: opts.entryNear ? "near" : "center",
    near: opts.entryNear,
    leaf: "wood",
  });
  if (!entryDoor) {
    grid.restore(snap);
    return null;
  }
  for (const [a, b, o] of layout.doors) {
    const ra = byKey.get(a);
    const rb = byKey.get(b);
    if (!ra || !rb) continue;
    const d = grid.addDoor(ra, rb, o);
    if (!d) {
      // an opening that does not fit may fall back to a normal door
      const d2 = o.kind === "opening" ? grid.addDoor(ra, rb, { width: 7 }) : null;
      if (!d2) {
        grid.restore(snap);
        return null;
      }
    }
  }
  return { rooms: [...byKey.values()], entryDoor, template: templateName };
}

const PAINTS = ["PAINT_WHITE", "PAINT_CREAM", "PAINT_GRAY", "PAINT_SAGE", "PAINT_BLUE", "PAINT_PEACH", "PAINT_MINT", "PAINT_LAVENDER"];
const TILES = ["WALL_TILE_WHITE", "WALL_TILE_BLUE", "WALL_TILE_GREEN"];
const WOODS = ["FLOOR_OAK", "FLOOR_WALNUT", "FLOOR_PARQUET", "FLOOR_CARPET_BEIGE", "FLOOR_CARPET_GRAY"];
const WETS = ["FLOOR_TILE_WHITE", "FLOOR_TILE_GRAY", "FLOOR_TILE_TERRA"];

export function pickUnitStyle(rng) {
  return {
    paint: rng.pick(PAINTS),
    tile: rng.pick(TILES),
    wood: rng.pick(WOODS),
    wet: rng.pick(WETS),
  };
}

export function floorFor(type, st) {
  switch (type) {
    case "bath":
    case "wc":
    case "kitchen":
    case "laundry":
      return st.wet;
    case "closet":
    case "storage":
      return "FLOOR_OAK";
    default:
      return st.wood;
  }
}
