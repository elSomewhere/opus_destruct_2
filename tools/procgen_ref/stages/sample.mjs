// Stage "sample": stageArchetype (buildings/sample.js) on scripted worlds - a world's cell plans
// are scripted lots (plain and turned, within their cell) with the buildings planBuildingEnvelope
// puts on them, made from (seed, "plan", i, j); its envelopesIn the buildings of the cells round
// the origin overlapping a rect; its building plan cache logs what it drops. On each world the
// reference's nordic stagings (town houses from walk-ups and row houses, cabins on cabin plots of
// house lots, churches), then every archetype in a style from the lot's own district, an empty
// `from` and n = 0: the envelopes staged, the plans dropped, and envelopesIn round each staged
// building and over sample rects once staged. tests/city/test_sample.cpp stages the same.
import { REF, line, samples } from "../lib/rec.mjs";
import { districtList, configList, envLine, rectStr } from "../lib/buildings.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const SA = await import(REF + "buildings/sample.js");
const F = await import(REF + "buildings/frame.js");
const { Rng } = await import(REF + "core/hash.js");
const { vx } = await import(REF + "core/units.js");

const CELL = 4096;
const FRONTS = ["N", "E", "S", "W"];
const OLDTOWN = { id: "oldtown", floors: [2, 3], archetypes: [], styles: [] };
const FOREST = { id: "forest", floors: [1, 1], archetypes: [], styles: [] };

/** A cabin-sized square plot (14-22 m) at the front of a larger lot (the reference's nordic test). */
const cabinPlot = (lot, f) => {
  const side = vx(14 + (((lot.rect.x0 * 7 + lot.rect.y0 * 3) & 0xffff) % 9));
  return side <= f.U && side <= f.V ? { x0: 0, y0: 0, x1: side - 1, y1: side - 1 } : null;
};

function sampleLot(next, i, j, k, districtId) {
  const U = 60 + Math.floor(next() * 260);
  const V = 80 + Math.floor(next() * 260);
  const turned = next() < 0.2;
  const x0 = i * CELL + Math.floor(next() * 3000);
  const y0 = j * CELL + Math.floor(next() * 3000);
  const lot = { id: `C${i}_${j}/b${k % 3}/l${k}`, district: districtId, corner: next() < 0.3, micro: false };
  if (turned) {
    const yaw = Math.floor(next() * 132);
    lot.front = F.nominalFront(yaw);
    lot.turn = { yaw, origin: { x: x0, y: y0 }, ou: 0, ov: 0, U, V };
    lot.rect = { ...F.lotFrameOf(lot).R };
  } else {
    lot.front = FRONTS[Math.floor(next() * 4)];
    const ns = lot.front === "N" || lot.front === "S";
    lot.rect = { x0, y0, x1: x0 + (ns ? U : V) - 1, y1: y0 + (ns ? V : U) - 1 };
  }
  lot.u = next();
  lot.core = next();
  lot.groundZ = Math.floor(next() * 900);
  return lot;
}

function makeWorld(config, DS) {
  const seed = config.seed;
  const plans = new Map();
  const dropped = [];
  const world = {
    seed,
    config,
    cellPlan(i, j) {
      const key = `${i},${j}`;
      let p = plans.get(key);
      if (!p) {
        const rng = Rng.from(seed, "plan", i, j);
        const next = () => rng.next();
        const n = 3 + Math.floor(next() * 6);
        const lots = [];
        const buildings = [];
        for (let k = 0; k < n; k += 1) {
          const d = DS[Math.floor(next() * DS.length)];
          const lot = sampleLot(next, i, j, k, d.id);
          lot.building = null;
          const env = A.planBuildingEnvelope(lot, d, Rng.from(seed, lot.id, "building"), { u: lot.u, core: lot.core, groundZ: lot.groundZ, config });
          if (env) {
            lot.building = env.id;
            buildings.push(env);
          }
          lots.push(lot);
        }
        p = { lots, buildings, buildingById: new Map(buildings.map((b) => [b.id, b])) };
        plans.set(key, p);
      }
      return p;
    },
    envelopesIn(rect) {
      const out = [];
      for (let j = -6; j <= 6; j += 1) for (let i = -6; i <= 6; i += 1) for (const b of world.cellPlan(i, j).buildings) if (b.bounds.x0 <= rect.x1 && rect.x0 <= b.bounds.x1 && b.bounds.y0 <= rect.y1 && rect.y0 <= b.bounds.y1) out.push(b);
      return out;
    },
    buildingPlans: {
      map: {
        delete(id) {
          dropped.push(id);
          return true;
        },
      },
    },
  };
  return { world, dropped };
}

export default function* sample() {
  const r = samples(47);
  const DS = districtList();
  const CFG = configList();
  const styles = STYLES.all().map((s) => s.id);
  for (const w of [0, 3, 17, 21, 27]) {
    const config = CFG[w];
    const { world, dropped } = makeWorld(config, DS);
    const runs = [
      ["townhouse", "nordicWood", { n: 5, from: ["walkup", "rowhouse"], district: OLDTOWN }],
      ["townhouse", "nordicPlaster", { n: 2, from: ["rowhouse", "walkup"], district: { ...OLDTOWN, id: "mixed" } }],
      ["cabin", "cabin", { n: 4, from: ["house"], district: FOREST, radius: 5, lotRect: cabinPlot }],
      ["church", "nordicChurch", { n: 3, from: ["walkup", "midrise", "house"], district: OLDTOWN }],
      ["house", "siding", { n: 3, from: [] }],
      ["office", "glass", { n: 0 }],
    ];
    for (const a of ARCHETYPES.all()) {
      const opts = { n: 1 + Math.floor(r() * 3), radius: Math.floor(r() * 4) };
      if (r() < 0.3) opts.lotRect = cabinPlot;
      runs.push([a.id, styles[Math.floor(r() * styles.length)], opts]);
    }
    const staged = [];
    for (const [id, style, opts] of runs) {
      const envs = SA.stageArchetype(world, id, style, opts);
      yield line("stage", w, id, style, opts.n ?? 3, opts.radius ?? 3, opts.from ? opts.from.join(",") || "[]" : "-", opts.district ? opts.district.id : "-", !!opts.lotRect,
        envs.length, dropped.length);
      for (const e of envs) {
        yield envLine(e);
        staged.push(e);
      }
    }
    yield line("dropped", dropped.join(","));
    // envelopesIn once staged: round each staged building, and over sample rects
    const rects = staged.map((e) => e.bounds);
    for (let k = 0; k < 20; k += 1) {
      const x0 = Math.floor((r() - 0.5) * 6 * CELL);
      const y0 = Math.floor((r() - 0.5) * 6 * CELL);
      rects.push({ x0, y0, x1: x0 + Math.floor(r() * 3000), y1: y0 + Math.floor(r() * 3000) });
    }
    for (const q of rects) yield line("in", rectStr(q), world.envelopesIn(q).map((e) => e.id).join(",") || "-");
  }
}
