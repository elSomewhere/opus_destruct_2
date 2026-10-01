// Stage "grading": site grading (city/grading.js) - levelLot, and SiteGrading over scripted cells:
// clusters of lots (plain and turned) of three blocks, carrying envelopes of every kind of pad
// (footprints and annexes, a pylon left out, civic forecourts; level lots: yards, car parks, schools,
// wharf houses, petrol forecourts), some without a building, with an envelope the cell lacks, under
// a highway or underground; then SiteGrading.at round every building (on its own lot, another lot or
// none; its block or another; bases above and below the pads, fractional ones; columns no pad
// reaches). tests/city/test_grading.cpp grades the same.
import { REF, line, samples } from "../lib/rec.mjs";
import { districtList, envLine } from "../lib/buildings.mjs";
import { shellWorld } from "../lib/shells.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const G = await import(REF + "city/grading.js");
const Fr = await import(REF + "buildings/frame.js");
const { Rng } = await import(REF + "core/hash.js");

const KINDS = ["house", "school", "wharfhouse", "petrolStation", "supermarket", "walkup", "townHall", "museum", "library", "cabin", "garage", "midrise", "barn",
  "warehouse", "church", "hotel"];
const FRONTS = ["N", "E", "S", "W"];

/** A lot of a cluster round (ox, oy) of a size archetype a fits (plain or turned), in block k % 3. */
function clusterLot(r, ox, oy, a, k, d) {
  let U = 0;
  let V = 0;
  for (let t = 0; t < 60; t += 1) {
    U = 60 + Math.floor(r() * 500);
    V = 60 + Math.floor(r() * 500);
    if (a.fits(U, V)) break;
  }
  const turned = r() < 0.3;
  const x0 = ox + Math.floor(r() * 1600);
  const y0 = oy + Math.floor(r() * 1600);
  const fi = Math.floor(r() * 4);
  const yaw = Math.floor(r() * 132);
  const corner = r() < 0.3;
  const lot = { id: `C0_0/b${k % 3}/l${k}`, district: d.id, corner, micro: false, block: `C0_0/b${k % 3}` };
  if (turned) {
    lot.front = Fr.nominalFront(yaw);
    lot.turn = { yaw, origin: { x: x0, y: y0 }, ou: 0, ov: 0, U, V };
    lot.rect = { ...Fr.lotFrameOf(lot).R };
  } else {
    lot.front = FRONTS[fi];
    const ns = lot.front === "N" || lot.front === "S";
    lot.rect = { x0, y0, x1: x0 + (ns ? U : V) - 1, y1: y0 + (ns ? V : U) - 1 };
  }
  return lot;
}

export default function* grading() {
  const r = samples(73);
  const DS = districtList();
  const styles = STYLES.all().map((s) => s.id);
  for (let cell = 0; cell < 40; cell += 1) {
    const w = shellWorld(cell % 3);
    const ox = Math.floor((r() - 0.5) * 100000);
    const oy = Math.floor((r() - 0.5) * 100000);
    const n = 4 + Math.floor(r() * 8);
    const lots = [];
    const envs = [];
    const buildings = new Map();
    for (let k = 0; k < n; k += 1) {
      const a = ARCHETYPES.get(KINDS[Math.floor(r() * KINDS.length)]);
      const d = DS[Math.floor(r() * DS.length)];
      const lot = clusterLot(r, ox, oy, a, k, d);
      lot.groundZ = 400 + Math.floor(r() * 200);
      lot.building = null;
      const style = styles[Math.floor(r() * styles.length)];
      const extra = { u: r(), core: r(), groundZ: lot.groundZ, config: w.config };
      const env = A.planBuildingEnvelopeAs(lot, a.id, style, d, new Rng(Math.floor(r() * 4294967296)), extra);
      const miss = r() < 0.08;
      const hw = r() < 0.06;
      const ug = r() < 0.05;
      if (env) {
        lot.building = env.id;
        if (!miss) buildings.set(env.id, env);
      }
      lot.underHighway = hw;
      if (ug) lot.underground = true;
      lots.push(lot);
      envs.push(env);
      yield `${line("lot", lot.id, lot.rect.x0, lot.rect.y0, lot.rect.x1, lot.rect.y1, lot.groundZ, lot.block, lot.building, miss, hw, ug, G.levelLot(lot, env))} ${envLine(env)}`;
    }
    const grading = new G.SiteGrading(lots, buildings);
    for (let k = 0; k < n; k += 1) {
      const lot = lots[k];
      const env = envs[k];
      const b = env ? env.bounds : lot.rect;
      for (let q = 0; q < 40; q += 1) {
        const x = Math.floor(b.x0 - 200 + r() * (b.x1 - b.x0 + 400));
        const y = Math.floor(b.y0 - 200 + r() * (b.y1 - b.y0 + 400));
        const frac = r() < 0.3;
        const dz = (r() - 0.5) * 240;
        const base = lot.groundZ + (frac ? dz : Math.floor(dz));
        const blockId = r() < 0.8 ? lot.block : "C0_0/b9";
        const lt = r();
        const other = lots[Math.floor(r() * lots.length)];
        const on = lt < 0.3 ? lot : lt < 0.4 ? other : null;
        yield line("at", x, y, base, blockId, on ? on.id : "-", grading.at(x, y, base, blockId, on));
      }
    }
  }
}
