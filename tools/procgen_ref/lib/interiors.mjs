// Records of building plans (buildings/interior/plan.js planBuilding's record) for the interior
// stages (interiors, interiorstreets); the C++ twin is tests/city/interior_records.hpp.
import { f, line } from "./rec.mjs";
import { rectStr } from "./buildings.mjs";

/** FNV-1a over a label grid's cells (16-bit values), as a uint32. */
export function cellHash(cells) {
  let h = 2166136261;
  for (let i = 0; i < cells.length; i += 1) h = Math.imul(h ^ cells[i], 16777619) >>> 0;
  return h;
}

const join = (list, sep, fmt) => (list && list.length ? list.map(fmt).join(sep) : "-");

/** A room: every field a planner gives one, and its area. */
export function roomLine(g, m) {
  const stalls = join(m.stalls, ";", (s) => [s.x0, s.x1, s.y0, s.y1, s.noseU, s.car ? 1 : 0].join(","));
  return line("rm", m.id, m.type, join(m.rects, "|", rectStr), m.paint, m.floorMat, m.stair, m.elevator, m.unit, m.template, m.ceiling, m.deckH, m.front, !!m.shop, !!m.tall,
    !!m.noWindows, !!m.classical, !!m.fire, stalls, join(m.pillars, ";", rectStr), m.rampRect ? rectStr(m.rampRect) : "-", m.linkTo ? `${f(m.linkTo.floor)}/${f(m.linkTo.room)}` : "-",
    g.area(m));
}

/** A door: its cells, rooms, kind and leaf, what the street levels and the planners add, its leaf's resting rect. */
export function doorLine(g, d) {
  const leaf = g.doorLeafRect(d);
  return line("dr", d.id, d.u0, d.u1, d.v0, d.v1, d.orient, d.a, d.b, d.kind, d.sideA, d.width, d.leaf, d.height, d.street, d.sill, d.color, !!d.misplaced, leaf ? rectStr(leaf) : "-");
}

/** A floor grid: its size, door extra, footprint, cut, a hash of its cells, its rooms and doors. */
export function* gridRecords(g) {
  yield line("gc", g.U, g.V, g.doorExtra, join(g.footprint, "|", rectStr), !!g.cut, cellHash(g.cells), g.rooms.length, g.doors.length);
  for (const m of g.rooms) yield roomLine(g, m);
  for (const d of g.doors) yield doorLine(g, d);
}

/** A plan: its stairs, elevators, ramps, links, its floors (each distinct grid once) and issues. */
export function* planRecords(plan) {
  if (!plan) {
    yield "plan -";
    return;
  }
  yield line("plan", plan.floors.length, plan.stairs.length, plan.elevators.length, plan.ramps.length, plan.links.length, plan.issues.length);
  for (const s of plan.stairs) {
    const flights = join(s.flights, ";", (q) => `${f(q.f)}/${f(q.z0)}/${f(q.H)}`);
    yield line("st", s.id, rectStr(s.rect), s.axis, s.dir, s.laneLow, s.lane, s.landing, s.f0, s.f1, s.L, s.W, s.open, flights);
  }
  for (const e of plan.elevators) yield line("el", e.id, rectStr(e.rect), e.f0, e.f1, e.doorSide);
  for (const r of plan.ramps) yield line("ra", r.id, rectStr(r.rect), r.f, r.H, r.pitch);
  for (const [a, b, x] of plan.links) yield line("ln", a[0], a[1], b[0], b[1], x.ramp);
  const grids = [];
  for (const fl of plan.floors) {
    let gi = grids.indexOf(fl.grid);
    const fresh = gi < 0;
    if (fresh) {
      gi = grids.length;
      grids.push(fl.grid);
    }
    const low = join(fl.lowRegions, ";", (q) => `${rectStr(q.rect)}/${f(q.height)}`);
    yield line("fl", fl.index, fl.z, fl.height, fl.kind, gi, !!fl.mezzanine, low, join(fl.ramps, ",", (q) => f(q.id)), plan.floorByIndex.get(fl.index) === fl);
    if (fresh) yield* gridRecords(fl.grid);
  }
  for (const q of plan.issues) yield line("is", q.floor, q.room, q.type, q.msg);
}
