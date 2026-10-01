// Stage "interiorstreets": the street levels of building plans (plan.js streetLevels: the street in
// front of each street door of the ground floor, recorded where it is off the building's level, the
// door raised to it where a walker could not step up) - buildings of every archetype on scripted
// lots (plain and turned) of worlds of World.js (a town, the angled world, an old harbour town),
// each in a cell of its own with scripted roads along its front (mostly), back and sides (their
// profiles fitted to the terrain, made pure: lib/worlds.mjs pureTerrain), its ground level set
// -14..14 voxels off the street's in front of its middle; each ground-floor door to the outside with
// its street and sill. tests/city/test_interiorstreets.cpp plans the same.
import { REF, line, samples } from "../lib/rec.mjs";
import { districtList, envLine } from "../lib/buildings.mjs";
import { pureTerrain } from "../lib/worlds.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const PL = await import(REF + "buildings/interior/plan.js");
const F = await import(REF + "buildings/frame.js");
const { World } = await import(REF + "world/World.js");
const { Rng } = await import(REF + "core/hash.js");
const { roadLevelAt } = await import(REF + "network/roadLevel.js");
const { localPointToWorld } = await import(REF + "core/obb.js");
const { presetConfig } = await import(REF + "config/presets.js");

/** [world key, preset, size] */
export const STREET_WORLDS = [
  ["cities", "cities", null],
  ["angledCities", "angledCities", null],
  ["oldHarbourTown", "oldHarbourTown", null],
];
const FRONTS = ["N", "E", "S", "W"];
const SIDE_P = [0.95, 0.5, 0.35, 0.35];

/** A lot whose frame is U x V cells at (x0, y0): plain (a front of four) or turned (a table yaw). */
function streetLot(r, U, V, x0, y0, k, districtId) {
  const turned = r() < 0.3;
  const fi = Math.floor(r() * 4);
  const corner = r() < 0.3;
  const lot = { id: `C9_9/b${k % 13}/l${k}`, district: districtId, corner, micro: false };
  if (turned) {
    const yaw = Math.floor(r() * 132);
    lot.front = F.nominalFront(yaw);
    lot.turn = { yaw, origin: { x: x0, y: y0 }, ou: 0, ov: 0, U, V };
    lot.rect = { ...F.lotFrameOf(lot).R };
  } else {
    lot.front = FRONTS[fi];
    const ns = lot.front === "N" || lot.front === "S";
    lot.rect = { x0, y0, x1: x0 + (ns ? U : V) - 1, y1: y0 + (ns ? V : U) - 1 };
  }
  return lot;
}

/** A canonical point of a building's frame in the world (streetLevels' choice). */
function toWorld(frame, u, v) {
  return frame.pointToWorld ? frame.pointToWorld(u, v) : localPointToWorld(frame.placement, u, v);
}

export default function* interiorstreets() {
  const r = samples(59);
  const DS = districtList();
  const styles = STYLES.all().map((s) => s.id);
  const all = ARCHETYPES.all();
  for (const [key, id, size] of STREET_WORLDS) {
    const w = pureTerrain(new World(presetConfig(id, { size })));
    const cellRoads = new Map();
    w.cellNet = (i, j) => ({ roads: cellRoads.get(`${i},${j}`) ?? [] });
    yield line("world", key);
    // ---- the buildings and their roads
    const built = [];
    for (let q = 0; q < 3 * all.length; q += 1) {
      const a = all[q % all.length];
      let U = 0;
      let V = 0;
      for (let t = 0; t < 60; t += 1) {
        U = 40 + Math.floor(r() * 300);
        V = 40 + Math.floor(r() * 300);
        if (a.fits(U, V)) break;
      }
      const d = DS[Math.floor(r() * DS.length)];
      const style = styles[Math.floor(r() * styles.length)];
      const ci = 2 + 3 * (q % 10);
      const cj = 2 + 3 * Math.floor(q / 10);
      const rc = w.arterials.cellRect(ci, cj);
      const x0 = Math.floor(rc.x0 + 800 + r() * Math.max(1, rc.x1 - rc.x0 - 1600));
      const y0 = Math.floor(rc.y0 + 800 + r() * Math.max(1, rc.y1 - rc.y0 - 1600));
      const lot = streetLot(r, U, V, x0, y0, q, d.id);
      const extra = { u: r(), core: r(), groundZ: 0, config: w.config };
      const env = A.planBuildingEnvelopeAs(lot, a.id, style, d, new Rng(Math.floor(r() * 4294967296)), extra);
      const sides = [];
      for (let s = 0; s < 4; s += 1) {
        const has = r() < SIDE_P[s];
        const off = 4 + Math.floor(r() * 30);
        const walk = r() < 0.3 ? 0 : 12 + Math.floor(r() * 8);
        sides.push({ has, off, walk });
      }
      yield `${line("sb", q, a.id, U, V, d.id, style, ci, cj, sides.map((s) => (s.has ? `${s.off}/${s.walk}` : "-")).join(","))} ${envLine(env)}`;
      if (!env) continue;
      const frame = F.frameOf(env);
      const EU = env.U;
      const EV = env.V;
      const roads = [];
      sides.forEach((s, k) => {
        if (!s.has) return;
        const o = s.off;
        const ends = [
          [[-60, -o], [EU + 60, -o]],
          [[EU + 60, EV - 1 + o], [-60, EV - 1 + o]],
          [[-o, EV + 60], [-o, -60]],
          [[EU - 1 + o, -60], [EU - 1 + o, EV + 60]],
        ][k];
        const pts = ends.map(([u, v]) => {
          const [x, y] = toWorld(frame, u, v);
          return { x, y };
        });
        roads.push({ id: `S${q}/r${k}`, cell: `S${q}`, cls: "local", pts, hc: 24, hr: 24 + s.walk, corner: 6, median: 0, parking: 0, lanes: 2, lane: 24, sidewalk: s.walk, shoulder: 0 });
      });
      cellRoads.set(`${ci},${cj}`, roads);
      built.push({ q, env, frame });
    }
    // ---- the plans, each building's level off its front street's
    for (const { q, env, frame } of built) {
      const [x, y] = toWorld(frame, env.U / 2, -6);
      const c = w.cellAt(x, y);
      const lv = roadLevelAt(w, w.roadView(c.i, c.j), x, y, 24);
      const L0 = lv ? Math.round(lv.z) + (lv.sidewalk ? 1 : 0) : 0;
      const delta = Math.floor(r() * 29) - 14;
      env.groundZ = L0 - delta;
      env.baseZ = env.groundZ + (env.stoop ? 4 : 1);
      const plan = PL.planBuilding(w, env);
      yield line("lv", q, c.i, c.j, lv ? lv.z : "-", L0, delta, env.groundZ, env.baseZ, plan ? plan.issues.length : "-");
      const F0 = plan?.floorByIndex.get(0);
      for (const d of F0?.grid.doors ?? []) if (d.b === -1) yield line("sd", d.id, d.kind, d.orient, d.u0, d.u1, d.v0, d.v1, d.street, d.sill);
    }
  }
}
