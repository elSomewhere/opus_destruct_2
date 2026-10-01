// Stage "units": the apartment unit planner (buildings/interior/units.js) on scripted floors -
// double-loaded corridors (units both sides, split as apartments.js splits them, some with an entry
// point), stair-hall sections (units left and right of the hall, the entry near its middle) and
// single units on every side of a circulation strip with exterior or interior walls round them
// (through, band, rail and open templates, every bedroom count, a wrong entry side now and then);
// where a unit fails, the fallback studio apartments.js puts there. Each unit's facades, template,
// rooms and entry door, the stream's next draw, and every floor's grid; pickUnitStyle and floorFor.
// tests/city/test_units.cpp plans the same floors the same way.
import { REF, f, line, samples } from "../lib/rec.mjs";
import { gridRecords, rectStr } from "../lib/buildings.mjs";

const G = await import(REF + "buildings/interior/grid.js");
const UN = await import(REF + "buildings/interior/units.js");
const C = await import(REF + "buildings/interior/common.js");
const { Rng } = await import(REF + "core/hash.js");

const TYPES = ["bath", "wc", "kitchen", "laundry", "closet", "storage", "living", "bedroom", "foyer", "hall", "studio", "nope"];
const SIDES = ["N", "S", "E", "W"];

/** One unit planned (or its fallback studio), as a record. */
function place(grid, rng, rect, entry, circ, unitId, near) {
  const facades = C.facadeSidesOf(grid, rect);
  const res = UN.planUnit(grid, rect, entry, circ, facades, { rng, unit: unitId, entryNear: near });
  const out = res ? `${res.template}/${res.rooms.map((m) => m.id).join(",")}/${res.entryDoor.id}` : "-";
  const rec = line("u", entry, rectStr(rect), [...facades].join("") || "-", near ? `${f(near.u)},${f(near.v)}` : "-", f(unitId), out, rng.next());
  if (!res) {
    const room = grid.addRoom("studio", [rect], { unit: unitId, paint: "PAINT_WHITE", floorMat: "FLOOR_OAK" });
    if (!grid.addDoor(room, circ, { width: 8, kind: "entry" })) grid.addDoor(room, circ, { width: 6, margin: 1, kind: "entry" });
  }
  return rec;
}

// a cell (a along the entry wall, b away from it) of a footprint A x B whose circulation strip is on `side`
function cell(side, a, b, A, B) {
  if (side === "N") return [a, b];
  if (side === "S") return [a, B - 1 - b];
  if (side === "W") return [b, a];
  return [B - 1 - b, a];
}
function rectOf(side, a0, b0, a1, b1, A, B) {
  const [x0, y0] = cell(side, a0, b0, A, B);
  const [x1, y1] = cell(side, a1, b1, A, B);
  return { x0: Math.min(x0, x1), y0: Math.min(y0, y1), x1: Math.max(x0, x1), y1: Math.max(y0, y1) };
}

function extraOf(r) {
  const dr = r();
  const dy = r();
  return dr < 0.2 ? G.doorExtraOf({ turn: { yaw: Math.floor(dy * 132) } }) : 0;
}

export default function* units() {
  const r = samples(41);
  // ---- styles and floors
  for (let s = 0; s < 24; s += 1) {
    const rng = new Rng(500 + s * 7907);
    const st = UN.pickUnitStyle(rng);
    yield line("st", st.paint, st.tile, st.wood, st.wet, ...TYPES.map((t) => UN.floorFor(t, st)), rng.next());
  }
  // ---- double-loaded corridors
  for (let i = 0; i < 70; i += 1) {
    const U = 120 + Math.floor(r() * 300);
    const V = 110 + Math.floor(r() * 130);
    const g = new G.FloorGrid(U, V, [{ x0: 0, y0: 0, x1: U - 1, y1: V - 1 }]);
    g.doorExtra = extraOf(r);
    const inner = { x0: 2, y0: 2, x1: U - 3, y1: V - 3 };
    const cw = 10 + Math.floor(r() * 8);
    const cy0 = inner.y0 + 40 + Math.floor(r() * (V - 4 - cw - 80));
    const corr = g.addRoom("corridor", [{ x0: inner.x0, y0: cy0, x1: inner.x1, y1: cy0 + cw - 1 }], { paint: "PAINT_CREAM", floorMat: "FLOOR_TERRAZZO" });
    const rng = new Rng(Math.floor(r() * 4294967296));
    const mode = r();
    yield line("fa", i, U, V, cw, cy0, g.doorExtra, mode);
    let unitNo = 0;
    const zones = [
      [{ x0: inner.x0, y0: inner.y0, x1: inner.x1, y1: cy0 - 2 }, "S"],
      [{ x0: inner.x0, y0: cy0 + cw + 1, x1: inner.x1, y1: inner.y1 }, "N"],
    ];
    for (const [zone, entry] of zones) {
      const target = Math.round(rng.float(7.5, 10.5) * 8);
      for (const [p0, p1] of C.splitLength(zone.x0, zone.x1, target, 5 * 8, rng)) {
        const rect = { x0: p0, y0: zone.y0, x1: p1, y1: zone.y1 };
        const near = mode < 0.3 ? { u: Math.round((p0 + p1) / 2) + 3, v: entry === "S" ? cy0 : cy0 + cw - 1 } : null;
        const id = mode < 0.8 ? `${entry}${unitNo}` : undefined;
        unitNo += 1;
        yield place(g, rng, rect, entry, corr, id, near);
      }
    }
    yield* gridRecords("a", g);
    yield line("ae", rng.next());
  }
  // ---- stair-hall sections
  for (let i = 0; i < 60; i += 1) {
    const U = 100 + Math.floor(r() * 260);
    const V = 60 + Math.floor(r() * 90);
    const g = new G.FloorGrid(U, V, [{ x0: 0, y0: 0, x1: U - 1, y1: V - 1 }]);
    g.doorExtra = extraOf(r);
    const inner = { x0: 2, y0: 2, x1: U - 3, y1: V - 3 };
    const hw = 20 + Math.floor(r() * 16);
    const c0 = inner.x0 + 30 + Math.floor(r() * (U - 4 - hw - 60));
    const c1 = c0 + hw - 1;
    const hall = { x0: c0, y0: inner.y0, x1: c1, y1: inner.y1 };
    const hallRoom = g.addRoom("hall", [hall], { paint: "PAINT_GRAY", floorMat: "FLOOR_TERRAZZO" });
    const rng = new Rng(Math.floor(r() * 4294967296));
    yield line("fb", i, U, V, c0, c1, g.doorExtra);
    const near = { u: (hall.x0 + hall.x1) / 2, v: (hall.y0 + hall.y1) / 2 };
    let unitNo = 0;
    const zones = [
      [{ x0: inner.x0, y0: inner.y0, x1: c0 - 2, y1: inner.y1 }, "E"],
      [{ x0: c1 + 2, y0: inner.y0, x1: inner.x1, y1: inner.y1 }, "W"],
    ];
    for (const [zone, entry] of zones) {
      if (zone.x1 - zone.x0 + 1 < 24) continue;
      yield place(g, rng, zone, entry, hallRoom, `s${unitNo}`, near);
      unitNo += 1;
    }
    yield* gridRecords("b", g);
    yield line("be", rng.next());
  }
  // ---- single units
  for (let i = 0; i < 400; i += 1) {
    const w = 24 + Math.floor(r() * 100);
    const dpt = 24 + Math.floor(r() * 100);
    const side = SIDES[Math.floor(r() * 4)];
    const cw = 8 + Math.floor(r() * 6);
    const pad = () => (r() < 0.5 ? 0 : 4 + Math.floor(r() * 16));
    const padL = pad();
    const padR = pad();
    const padB = pad();
    const A = 2 + padL + w + padR + 2;
    const B = 2 + cw + 1 + dpt + padB + 2;
    const ns = side === "N" || side === "S";
    const g = new G.FloorGrid(ns ? A : B, ns ? B : A, [{ x0: 0, y0: 0, x1: (ns ? A : B) - 1, y1: (ns ? B : A) - 1 }]);
    g.doorExtra = extraOf(r);
    const circ = g.addRoom("hall", [rectOf(side, 2, 2, A - 3, 2 + cw - 1, A, B)], { paint: "PAINT_GRAY", floorMat: "FLOOR_TERRAZZO" });
    const rect = rectOf(side, 2 + padL, 2 + cw + 1, 2 + padL + w - 1, 2 + cw + dpt, A, B);
    const wrong = r() < 0.05;
    const entry = wrong ? SIDES[(SIDES.indexOf(side) + 1) % 4] : side;
    const nr = r();
    const near = nr < 0.3 ? { u: rect.x0 + Math.floor(nr * 40), v: rect.y0 + Math.floor(nr * 30) } : null;
    const rng = new Rng(Math.floor(r() * 4294967296));
    yield line("fc", i, w, dpt, side, cw, padL, padR, padB, g.doorExtra, entry);
    yield place(g, rng, rect, entry, circ, `c${i}`, near);
    yield* gridRecords("c", g);
  }
}
