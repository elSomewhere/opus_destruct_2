// Stage "interiors": the interior planners (buildings/interior/plan.js, apartments.js, offices.js,
// industrial.js, garage.js, school.js, civic.js, civicPrograms.js) - planBuilding on envelopes of
// every archetype (planBuildingEnvelopeAs on scripted plain and turned lots of every district as
// every flavor sees it, every style, the configs of every world; some with a chamfered corner, a
// town flavor, sky doors, wings, pitched ramps, the other roof), and on the envelopes
// planBuildingEnvelope chooses; odd envelopes for the rare branches (shallow, narrow and short
// footprints, an archetype without a planner); the planners called directly (PLANNERS) on streams
// of the stage's, with the stream's next draw; validatePlan on scripted plans (stairs, links,
// mezzanine links, closed elevator doors, floors sharing an index). Each plan's stairs, elevators,
// ramps, links, floors (each grid once: a hash of its cells, its rooms and doors) and issues. The
// worlds plan with no roads (the street levels: stage interiorstreets). tests/city/test_interiors.cpp
// plans the same.
import { REF, f, line, samples } from "../lib/rec.mjs";
import { districtList, configList, scriptedLot, envLine } from "../lib/buildings.mjs";
import { planRecords } from "../lib/interiors.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const PL = await import(REF + "buildings/interior/plan.js");
const AP = await import(REF + "buildings/interior/apartments.js");
const OF = await import(REF + "buildings/interior/offices.js");
const IN = await import(REF + "buildings/interior/industrial.js");
const GA = await import(REF + "buildings/interior/garage.js");
const SC = await import(REF + "buildings/interior/school.js");
const HO = await import(REF + "buildings/interior/houses.js");
const CA = await import(REF + "buildings/interior/cabins.js");
const CP = await import(REF + "buildings/interior/civicPrograms.js");
const G = await import(REF + "buildings/interior/grid.js");
const { World } = await import(REF + "world/World.js");
const { Rng } = await import(REF + "core/hash.js");

/** plan.js PLANNERS, from the planners' exports. */
const PLANNERS = {
  walkup: AP.planApartmentBuilding,
  midrise: AP.planApartmentBuilding,
  tower: (ctx) => (ctx.env.program.upper === "apartments" ? AP.planApartmentBuilding(ctx) : OF.planOfficeBuilding(ctx)),
  office: OF.planOfficeBuilding,
  house: HO.planHouse,
  rowhouse: HO.planHouse,
  warehouse: IN.planIndustrial,
  factory: IN.planIndustrial,
  barn: IN.planIndustrial,
  panelSlab: AP.planApartmentBuilding,
  panelTower: AP.planApartmentBuilding,
  garage: GA.planGarage,
  school: SC.planSchool,
  townhouse: (ctx) => (ctx.env.program.upper === "house" ? HO.planHouse(ctx) : AP.planApartmentBuilding(ctx)),
  wharfhouse: AP.planApartmentBuilding,
  cabin: CA.planCabin,
  church: CA.planChurch,
  ...CP.CIVIC_PLANNERS,
};

// town flavors a shop reads (and some it does not)
const FLAVORS = [undefined, "nordic", "nordicHarbour", "harbourTown", "nordicBleak", "soviet", "nordicVillage", "sovietVillage", "historic", "modern"];
const PITCHED = [0.08, 0.35, 0.65, 1];

/** A sky door (city/skybridges.js) on floor 1 .. floors (the last: none such) across a span of the building's world box. */
function skyDoorOf(env, a, b, c, d, bridge) {
  const floor = 1 + Math.floor(a * Math.max(1, env.floors));
  const alongX = env.front === "N" || env.front === "S";
  const spanX = d < 0.85 ? alongX : !alongX;
  const lo = spanX ? env.R.x0 : env.R.y0;
  const hi = spanX ? env.R.x1 : env.R.y1;
  const s0 = lo + Math.floor(b * Math.max(1, hi - lo - 24));
  const s1 = s0 + 10 + Math.floor(c * 24);
  return { floor, span: spanX ? { x0: s0, x1: s1 } : { y0: s0, y1: s1 }, bridge };
}

const skyStr = (d) => `${d.floor}/${d.span.x0 !== undefined ? "x" : "y"}/${d.span.x0 ?? d.span.y0}/${d.span.x1 ?? d.span.y1}/${d.bridge}`;

/** Decorations of an envelope (every number drawn whether used or not): a chamfer, a flavor, sky doors, wings, pitched ramps, the other roof. */
function decorate(r, env, k) {
  const ch = r();
  const cs = r();
  const ck = r();
  const fl = r();
  const sk = r();
  const sa = r();
  const sb = r();
  const sc = r();
  const sd = r();
  const s2 = r();
  const wg = r();
  const wu = r();
  const wv = r();
  const pr = r();
  const rf = r();
  if (!env) return "dec -";
  if (ch < 0.15) env.chamfer = { side: cs < 0.5 ? "L" : "R", a: 20 * (1 + Math.floor(ck * 2)), b: 21 * (1 + Math.floor(ck * 2)) };
  const flavor = FLAVORS[Math.floor(fl * FLAVORS.length)];
  if (flavor !== undefined) env.flavor = flavor;
  if (sk < 0.6) {
    env.skyDoors = [skyDoorOf(env, sa, sb, sc, sd, `B${k}a`)];
    if (s2 < 0.3) env.skyDoors.push(skyDoorOf(env, sb, sc, sa, s2, `B${k}b`));
  }
  if (wg < 0.3) {
    const u = Math.floor(wu * env.U);
    const [y0, y1] = wv < 0.4 ? [-30, -2] : wv < 0.8 ? [env.V + 2, env.V + 30] : [env.V + 100, env.V + 140];
    env.wings = [{ canon: { x0: u - 20, y0, x1: u + 20, y1 } }];
  }
  if (pr < 0.5) env.pitchedRamps = true;
  if (rf < 0.1) env.roof = { ...env.roof, type: env.roof.type === "flat" ? "gable" : "flat" };
  const c = env.chamfer;
  const w = env.wings?.[0]?.canon;
  return line("dec", c ? `${c.side}${c.a}/${c.b}` : "-", env.flavor, env.skyDoors ? env.skyDoors.map(skyStr).join(",") : "-", w ? `${w.x0},${w.y0},${w.x1},${w.y1}` : "-", !!env.pitchedRamps, env.roof.type);
}

/** The world of a config (its seed for the plans' streams), with no roads (no street levels). */
function worldsOf(CFG) {
  const worlds = new Map();
  return (ci) => {
    let w = worlds.get(ci);
    if (!w) {
      w = new World(CFG[ci]);
      w.cellNet = () => ({ roads: [] });
      worlds.set(ci, w);
    }
    return w;
  };
}

/** The planners called directly on a stream of the stage's: the plan's size and the stream's next draw. */
function direct(env, seed) {
  const rng = new Rng(seed);
  const pb = new PL.PlanBuilder(env, rng);
  const planner = PLANNERS[env.archetype];
  if (!planner) return line("direct", "-", rng.next());
  planner({ env, rng, pb, world: null });
  const plan = pb.build();
  let rooms = 0;
  for (const fl of plan.floors) rooms += fl.grid.rooms.length;
  return line("direct", plan.floors.length, rooms, plan.issues.length, rng.next());
}

// samples per archetype (civic ones and big ones fewer)
const COUNT = { walkup: 60, midrise: 50, tower: 40, office: 40, panelSlab: 40, panelTower: 30, townhouse: 50, wharfhouse: 30, garage: 30, school: 24, warehouse: 30, factory: 20, barn: 20 };
const TYPES = ["office", "hall", "stair", "elevator", "shaft", "void", "mechanicalShaft", "storage"];
const DOOR_KINDS = ["interior", "entrance", "elevator", "balcony", "window", "opening", "stair"];

export default function* interiors() {
  const r = samples(53);
  const DS = districtList();
  const CFG = configList();
  const styles = STYLES.all().map((s) => s.id);
  const worldOf = worldsOf(CFG);
  let k = 0;
  // ---- every archetype on scripted lots
  for (const a of ARCHETYPES.all()) {
    const n = a.civic ? 14 : (COUNT[a.id] ?? 16);
    for (let s = 0; s < n; s += 1) {
      let U = 0;
      let V = 0;
      for (let t = 0; t < 60; t += 1) {
        U = 40 + Math.floor(r() * 400);
        V = 40 + Math.floor(r() * 400);
        if (a.fits(U, V)) break;
      }
      const d = DS[Math.floor(r() * DS.length)];
      const lot = scriptedLot(r, U, V, (k += 1), d.id);
      const style = styles[Math.floor(r() * styles.length)];
      const ci = Math.floor(r() * CFG.length);
      const extra = { u: r(), core: r() * 1.2, groundZ: Math.floor(r() * 2000), config: CFG[ci] };
      if (r() < 0.3) extra.chapel = true;
      if (r() < 0.4) extra.dome = ["GOLD", "DOME_GREEN"];
      if (r() < 0.5) extra.pitchedCivic = PITCHED[Math.floor(r() * PITCHED.length)];
      const env = A.planBuildingEnvelopeAs(lot, a.id, style, d, new Rng(Math.floor(r() * 4294967296)), extra);
      yield `${line("b", a.id, U, V, d.id, style, ci)} ${envLine(env)}`;
      yield decorate(r, env, k);
      const seed = Math.floor(r() * 4294967296);
      if (!env) continue;
      yield* planRecords(PL.planBuilding(worldOf(ci), env));
      if (s % 4 === 0) yield direct(env, seed);
    }
  }
  // ---- the archetypes planBuildingEnvelope chooses (by district, flavors' pitched roofs, styles)
  for (let s = 0; s < 300; s += 1) {
    const U = 40 + Math.floor(r() * 400);
    const V = 40 + Math.floor(r() * 400);
    const d = DS[Math.floor(r() * DS.length)];
    const lot = scriptedLot(r, U, V, (k += 1), d.id);
    const ci = Math.floor(r() * CFG.length);
    const extra = { u: r(), core: r() * 1.2, groundZ: Math.floor(r() * 2000), config: CFG[ci] };
    const env = A.planBuildingEnvelope(lot, d, new Rng(Math.floor(r() * 4294967296)), extra);
    yield `${line("p", U, V, d.id, ci)} ${envLine(env)}`;
    if (!env) continue;
    yield* planRecords(PL.planBuilding(worldOf(ci), env));
  }
  // ---- odd envelopes: footprints cut shallow or narrow (every tier's rects clipped), taller
  // stories, no basements, one floor, a style or a ground program nothing registers, no planner
  const CORRIDORS = new Set(["hospital", "policeStation", "fireStation"]);
  const ODD = [
    ["walkup", "shallow", 30],
    ["townhouse", "shallow", 16],
    ["wharfhouse", "shallow", 10],
    ["panelSlab", "shallow", 10],
    ["garage", "shallow", 30],
    ["office", "shallow", 16],
    ["school", "narrow", 24],
    ["midrise", "narrow", 14],
    ["office", "narrow", 14],
    ["tower", "narrow", 10],
    ["warehouse", "narrow", 10],
    ["hospital", "narrow", 12],
    ["policeStation", "narrow", 12],
    ["fireStation", "narrow", 12],
    ["supermarket", "narrow", 8],
    ["cinema", "narrow", 8],
    ["departmentStore", "narrow", 8],
    ["midrise", "tall", 24],
    ["garage", "tall", 12],
    ["walkup", "style", 8],
    ["office", "nobasement", 8],
    ["warehouse", "program", 6],
    ["school", "onefloor", 6],
    ["hospital", "onefloor", 6],
    ["house", "bogus", 4],
  ];
  for (const [id, cut, n] of ODD) {
    const a = ARCHETYPES.get(id);
    for (let s = 0; s < n; s += 1) {
      let U = 0;
      let V = 0;
      for (let t = 0; t < 60; t += 1) {
        U = 40 + Math.floor(r() * 400);
        V = 40 + Math.floor(r() * 400);
        if (a.fits(U, V)) break;
      }
      const d = DS[Math.floor(r() * DS.length)];
      const lot = scriptedLot(r, U, V, (k += 1), d.id);
      const style = styles[Math.floor(r() * styles.length)];
      const ci = Math.floor(r() * CFG.length);
      const extra = { u: r(), core: r() * 1.2, groundZ: Math.floor(r() * 2000), config: CFG[ci] };
      const env = A.planBuildingEnvelopeAs(lot, id, style, d, new Rng(Math.floor(r() * 4294967296)), extra);
      const size = r();
      const pr = r();
      if (env) {
        if (cut === "shallow") {
          const depth = id === "garage" ? 150 + Math.floor(size * 180) : id === "office" ? 40 + Math.floor(size * 80) : 30 + Math.floor(size * 90);
          for (const t of env.tiers) for (const q of t.rects) q.y1 = Math.min(q.y1, q.y0 + depth - 1);
        } else if (cut === "narrow") {
          const width = CORRIDORS.has(id) ? 60 + Math.floor(size * size * 260) : 90 + Math.floor(size * 260);
          for (const t of env.tiers) for (const q of t.rects) q.x1 = Math.min(q.x1, q.x0 + width - 1);
        } else if (cut === "tall") {
          const h = id === "garage" ? 28 + Math.floor(size * 12) : 40 + Math.floor(size * 24);
          if (id === "garage") env.storyH = env.storyH.map(() => h);
          else env.storyH[0] = h;
        } else if (cut === "style") env.style = "nope";
        else if (cut === "nobasement") env.basements = 0;
        else if (cut === "program") env.program = { ...env.program, ground: "nope" };
        else if (cut === "onefloor") {
          env.floors = 1;
          env.storyH = [env.storyH[0]];
          for (const t of env.tiers) t.f1 = 0;
        } else env.archetype = "bogus";
        if (pr < 0.6) env.pitchedRamps = true;
      }
      yield `${line("odd", id, cut, U, V, d.id, style, ci, size, pr)} ${envLine(env)}`;
      if (!env) continue;
      yield* planRecords(PL.planBuilding(worldOf(ci), env));
    }
  }
  // ---- validatePlan on scripted plans, built by PlanBuilder.build (floors sorted by z, then index)
  const env0 = { baseZ: 0, basementH: 26, storyH: [30, 30, 30, 30, 30] };
  for (let c = 0; c < 400; c += 1) {
    const nR = 2 + Math.floor(r() * 7);
    const nG = 1 + Math.floor(r() * 3);
    const nS = Math.floor(r() * 3);
    const pb = new PL.PlanBuilder(env0, null);
    for (let s = 0; s < nS; s += 1) {
      const f0 = Math.floor(r() * 3) - 1;
      pb.stairs.push({ id: s, f0, f1: f0 + Math.floor(r() * 4) });
    }
    const grids = [];
    for (let g = 0; g < nG; g += 1) {
      const grid = new G.FloorGrid(64, 64, [{ x0: 0, y0: 0, x1: 63, y1: 63 }]);
      for (let q = 0; q < nR; q += 1) {
        let type = TYPES[Math.floor(r() * TYPES.length)];
        const st = Math.floor(r() * Math.max(1, nS));
        if (type === "stair" && !nS) type = "office";
        const i = q % 3;
        const j = Math.floor(q / 3);
        grid.addRoom(type, [{ x0: 2 + i * 20, y0: 2 + j * 20, x1: 19 + i * 20, y1: 19 + j * 20 }], type === "stair" ? { stair: st } : {});
      }
      const nD = Math.floor(r() * 12);
      for (let q = 0; q < nD; q += 1) {
        const da = Math.floor(r() * nR);
        const db = r() < 0.3 ? -1 : Math.floor(r() * nR);
        const kind = DOOR_KINDS[Math.floor(r() * DOOR_KINDS.length)];
        grid.doors.push({ id: grid.doors.length, a: da, b: db, kind });
      }
      grids.push(grid);
    }
    const nF = 1 + Math.floor(r() * 5);
    for (let q = 0; q < nF; q += 1) {
      const index = Math.floor(r() * 5) - 1;
      const z = r() < 0.2 ? 0 : index * 30;
      pb.addFloor(index, grids[Math.floor(r() * nG)], "x", { z, height: 30 });
    }
    const nL = Math.floor(r() * 3);
    for (let q = 0; q < nL; q += 1) {
      const fa = pb.floors[Math.floor(r() * nF)].index;
      const ra = Math.floor(r() * nR);
      const fb = pb.floors[Math.floor(r() * nF)].index;
      const rb = Math.floor(r() * nR);
      pb.links.push([[fa, ra], [fb, rb], { ramp: q }]);
    }
    for (const g of grids)
      for (const m of g.rooms) {
        const lk = r();
        const lf = pb.floors[Math.floor(r() * nF)].index;
        const lr = Math.floor(r() * nR);
        if (lk < 0.15) m.linkTo = { floor: lf, room: lr };
      }
    const plan = pb.build();
    const fl = plan.floors.map((q) => `${q.index}@${q.z}:${grids.indexOf(q.grid)}:${plan.floorByIndex.get(q.index) === q ? 1 : 0}`).join(",");
    yield line("vp", c, nR, nG, nS, fl, plan.issues.map((q) => `${q.floor}/${q.room}/${q.type}/${q.msg}`).join(",") || "-");
  }
  // ---- roomAtCell on the first plans' floors
  const w0 = worldOf(0);
  for (let s = 0; s < 40; s += 1) {
    const a = ARCHETYPES.all()[s % ARCHETYPES.all().length];
    let U = 0;
    let V = 0;
    for (let t = 0; t < 60; t += 1) {
      U = 40 + Math.floor(r() * 300);
      V = 40 + Math.floor(r() * 300);
      if (a.fits(U, V)) break;
    }
    const d = DS[Math.floor(r() * DS.length)];
    const lot = scriptedLot(r, U, V, (k += 1), d.id);
    const env = A.planBuildingEnvelopeAs(lot, a.id, "concrete", d, new Rng(Math.floor(r() * 4294967296)), { u: 0.5, core: 0.5, groundZ: 100, config: CFG[0] });
    if (!env) {
      yield line("rc", a.id, "-");
      continue;
    }
    const plan = PL.planBuilding(w0, env);
    const out = [];
    for (const fl of plan?.floors ?? []) {
      for (let q = 0; q < 12; q += 1) {
        const u = Math.floor(r() * (env.U + 4)) - 2;
        const v = Math.floor(r() * (env.V + 4)) - 2;
        const x = PL.roomAtCell(fl, u, v);
        out.push(x === null ? "-" : x === "door" ? "D" : f(x.id));
      }
    }
    yield line("rc", a.id, out.join(",") || "-");
  }
}
