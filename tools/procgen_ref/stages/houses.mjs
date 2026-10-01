// Stage "houses": the house and cabin floor planners (buildings/interior/houses.js, cabins.js) -
// houseColumns of every width and cabinLayout of every footprint, both mirrored; then the plans of
// house, row house, town house, cabin and church envelopes (planBuildingEnvelopeAs on scripted
// lots, plain and turned, some with a chamfered corner) planned through plan.js's PlanBuilder
// (planHouse with every front door margin, planCabin, planChurch): the floors, the stairs with
// their flights and every floor's grid as stage floorgrid records it (cells, rooms, doors, door
// leaves, the door graph); cabins and wharf houses planned as houses too (the narrow house's
// one-room fallback). tests/city/test_houses.cpp plans the same envelopes the same way.
import { REF, f, line, samples } from "../lib/rec.mjs";
import { districtList, configList, scriptedLot, envLine, rectStr, gridRecords } from "../lib/buildings.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const H = await import(REF + "buildings/interior/houses.js");
const C = await import(REF + "buildings/interior/cabins.js");
const { PlanBuilder } = await import(REF + "buildings/interior/plan.js");
const { Rng } = await import(REF + "core/hash.js");

/** A plan's records: its stairs, its floors with their grids. */
function* planRecords(tag, pb) {
  for (const s of pb.stairs) {
    const flights = (s.flights ?? []).map((q) => `${f(q.f)}/${f(q.z0)}/${f(q.H)}`).join(";");
    yield line(`${tag}s`, s.id, rectStr(s.rect), s.axis, s.dir, s.laneLow, s.lane, s.landing, s.f0, s.f1, s.L, s.W, s.open, flights);
  }
  for (const fl of pb.floors) {
    yield line(`${tag}f`, fl.index, fl.z, fl.height, fl.kind);
    yield* gridRecords(tag, fl.grid);
  }
}

// [archetype, plans, planner] (cabins and wharf houses planned as houses too: narrow enough for the
// house's one-room fallback)
const KINDS = [
  ["house", 100, "house"],
  ["rowhouse", 100, "house"],
  ["townhouse", 100, "house"],
  ["cabin", 120, "cabin"],
  ["church", 80, "church"],
  ["cabin", 40, "house"],
  ["wharfhouse", 30, "house"],
];
const MARGINS = [undefined, 0, 1, 2];

export default function* houses() {
  const r = samples(37);
  // ---- house columns and cabin layouts
  for (let U = 16; U <= 260; U += 1)
    for (const m of [false, true]) {
      const c = H.houseColumns(U, m);
      yield line("hc", U, m, c.stairCol, c.hall, c.rooms, c.entranceU);
    }
  for (let U = 24; U <= 120; U += 1)
    for (let V = 24; V <= 120; V += 3)
      for (const m of [false, true]) {
        const L = C.cabinLayout(U, V, m);
        yield line("cl", U, V, m, L.rooms.map((q) => `${q.key}/${q.type}/${rectStr(q.rect)}`).join(";"), L.entranceU);
      }
  // ---- plans
  const DS = districtList();
  const CFG = configList();
  const styles = STYLES.all().map((s) => s.id);
  let k = 0;
  for (const [id, n, planner] of KINDS) {
    const a = ARCHETYPES.get(id);
    for (let s = 0; s < n; s += 1) {
      let U = 0;
      let V = 0;
      for (let t = 0; t < 60; t += 1) {
        U = 40 + Math.floor(r() * 360);
        V = 40 + Math.floor(r() * 360);
        if (a.fits(U, V)) break;
      }
      const d = DS[Math.floor(r() * DS.length)];
      const lot = scriptedLot(r, U, V, (k += 1), d.id);
      const style = styles[Math.floor(r() * styles.length)];
      const config = CFG[Math.floor(r() * CFG.length)];
      const extra = { u: r(), core: r() * 1.2, groundZ: Math.floor(r() * 2000), config };
      if (r() < 0.3) extra.chapel = true;
      if (r() < 0.4) extra.dome = ["GOLD", "DOME_GREEN"];
      const env = A.planBuildingEnvelopeAs(lot, id, style, d, new Rng(Math.floor(r() * 4294967296)), extra);
      const ch = r();
      const cs = r();
      const ck = r();
      if (env && ch < 0.2) env.chamfer = { side: cs < 0.5 ? "L" : "R", a: 20 * (1 + Math.floor(ck * 2)), b: 21 * (1 + Math.floor(ck * 2)) };
      const margin = MARGINS[Math.floor(r() * MARGINS.length)];
      yield `${line("h", id, planner, U, V, d.id, f(margin), env?.chamfer ? `${env.chamfer.side}${env.chamfer.a}/${env.chamfer.b}` : "-")} ${envLine(env)}`;
      if (!env) continue;
      const rng = Rng.from(config.seed, env.id, "interior");
      const pb = new PlanBuilder(env, rng);
      if (planner === "cabin") C.planCabin({ env, rng, pb });
      else if (planner === "church") C.planChurch({ env, pb });
      else H.planHouse(margin === undefined ? { env, rng, pb } : { env, rng, pb, frontDoorMargin: margin });
      yield* planRecords("x", pb);
      yield line("xe", rng.next());
    }
  }
}
