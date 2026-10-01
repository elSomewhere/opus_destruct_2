// Stage "archetypes": the building archetypes (buildings/archetypes.js, after civic.js's) - every
// archetype's fits() over every lot size up to 720 x 720 cells; every archetype's envelope(ctx) on
// scripted contexts (plain and turned lot frames, the districts as every flavor sees them, the
// configs of every world, chapels, church domes, pitched civic roofs; tall downtown towers for
// their upper setback); planBuildingEnvelope on scripted lots of every size (the archetype
// weighted by district, fallbacks, a flavor's pitched roofs, the style) and planBuildingEnvelopeAs
// for every archetype; on each envelope tierRects, floorZ, floorHeight and envelopeFrame. Lots and
// districts come from lib/buildings.mjs (the C++ twin: tests/city/building_records.hpp).
import { REF, line, f, samples } from "../lib/rec.mjs";
import { districtList, configList, scriptedLot, specLine, envLine, rectStr } from "../lib/buildings.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const { Rng } = await import(REF + "core/hash.js");
const F = await import(REF + "buildings/frame.js");

function rle(bits) {
  const out = [];
  let i = 0;
  while (i < bits.length) {
    let j = i;
    while (j < bits.length && bits[j] === bits[i]) j += 1;
    out.push(`${bits[i]}:${j - i}`);
    i = j;
  }
  return out.join(",");
}

// a flavor's church dome materials (and a list with a name the palette lacks, and an empty one)
const DOMES = [["GOLD", "DOME_GREEN", "DOME_BLUE", "GOLD"], ["DOME_SHINGLE", "DOME_SHINGLE", "DOME_GREEN"], ["NOPE"], []];
const PITCHED = [0.08, 0.35, 0.65, 1];

/** An envelope's queries: tierRects, floorZ, floorHeight and its frame. */
function* queries(e) {
  const tiers = [];
  for (let fl = -3; fl <= e.floors + 1; fl += 1) tiers.push(A.tierRects(e, fl).map(rectStr).join(";") || "-");
  const zs = [];
  for (let fl = -e.basements - 1; fl <= e.floors; fl += 1) zs.push(A.floorZ(e, fl));
  const hs = [];
  for (let fl = -1; fl < e.floors; fl += 1) hs.push(A.floorHeight(e, fl));
  const fr = A.envelopeFrame(e);
  yield line("q", tiers.join("|"), zs, hs, rectStr(fr.R), fr.U, fr.V, !!fr.turned, ...fr.toWorld(0, 0), ...fr.toWorld(e.U - 1, e.V - 1), ...fr.fromWorld(e.R.x0, e.R.y0));
}

export default function* archetypes() {
  const r = samples(31);
  const DS = districtList();
  const CFG = configList();
  const styles = STYLES.all().map((s) => s.id);
  const all = ARCHETYPES.all();
  // ---- fits over every size
  for (const a of all) {
    const bits = [];
    for (let U = 0; U <= 720; U += 1) for (let V = 0; V <= 720; V += 1) bits.push(a.fits(U, V) ? 1 : 0);
    yield line("fits", a.id, !!a.entranceU, rle(bits));
  }
  let k = 0;
  const extraOf = () => {
    const extra = { u: r(), core: r() * 1.2, groundZ: Math.floor(r() * 4000) - 200, config: CFG[Math.floor(r() * CFG.length)] };
    if (r() < 0.3) extra.chapel = true;
    if (r() < 0.5) extra.dome = DOMES[Math.floor(r() * DOMES.length)];
    if (r() < 0.5) extra.pitchedCivic = PITCHED[Math.floor(r() * PITCHED.length)];
    return extra;
  };
  // ---- every archetype's envelope on scripted contexts (sizes that fit, mostly)
  for (const a of all) {
    const n = a.civic ? 30 : 240;
    for (let s = 0; s < n; s += 1) {
      let U = 0;
      let V = 0;
      for (let t = 0; t < 40; t += 1) {
        U = 40 + Math.floor(r() * 680);
        V = 40 + Math.floor(r() * 680);
        if (a.fits(U, V) && r() < 0.95) break;
      }
      const d = DS[Math.floor(r() * DS.length)];
      const lot = scriptedLot(r, U, V, (k += 1), d.id);
      const rng = new Rng(Math.floor(r() * 4294967296));
      const extra = extraOf();
      const lotFrame = F.lotFrameOf(lot);
      const env = a.envelope({ lot, frame: lotFrame, district: d, rng, ...extra });
      yield `${line("a", a.id, U, V, d.id, lot.id, !!lot.turn)} ${specLine(env)} ${f(rng.next())}`;
    }
  }
  // ---- tall towers: downtown as every flavor sees it, a high core (the upper setback above 28 floors)
  const downtown = DS.filter((d) => d.id === "downtown");
  const tower = ARCHETYPES.get("tower");
  for (let s = 0; s < 80; s += 1) {
    const U = 240 + Math.floor(r() * 400);
    const V = 240 + Math.floor(r() * 400);
    const d = downtown[Math.floor(r() * downtown.length)];
    const lot = scriptedLot(r, U, V, (k += 1), d.id);
    const rng = new Rng(Math.floor(r() * 4294967296));
    const extra = extraOf();
    extra.core = 0.75 + r() * 0.5;
    const env = tower.envelope({ lot, frame: F.lotFrameOf(lot), district: d, rng, ...extra });
    yield `${line("t", U, V, d.id, lot.id, !!lot.turn)} ${specLine(env)} ${f(rng.next())}`;
  }
  // ---- planBuildingEnvelope: the district's archetypes
  for (let s = 0; s < 4000; s += 1) {
    const U = 30 + Math.floor(r() * 640);
    const V = 30 + Math.floor(r() * 640);
    const d = DS[Math.floor(r() * DS.length)];
    const lot = scriptedLot(r, U, V, (k += 1), d.id);
    const rng = new Rng(Math.floor(r() * 4294967296));
    const extra = extraOf();
    const env = A.planBuildingEnvelope(lot, d, rng, extra);
    yield `${line("p", U, V, d.id, lot.id, !!lot.turn)} ${envLine(env)} ${f(rng.next())}`;
    if (env) yield* queries(env);
  }
  // ---- planBuildingEnvelopeAs: every archetype in a style
  for (const a of all) {
    for (let s = 0; s < 50; s += 1) {
      let U = 0;
      let V = 0;
      for (let t = 0; t < 40; t += 1) {
        U = 40 + Math.floor(r() * 680);
        V = 40 + Math.floor(r() * 680);
        if (a.fits(U, V) && r() < 0.9) break;
      }
      const d = DS[Math.floor(r() * DS.length)];
      const lot = scriptedLot(r, U, V, (k += 1), d.id);
      const style = styles[Math.floor(r() * styles.length)];
      const rng = new Rng(Math.floor(r() * 4294967296));
      const extra = extraOf();
      const env = A.planBuildingEnvelopeAs(lot, a.id, style, d, rng, extra);
      yield `${line("as", a.id, style, U, V, d.id, lot.id, !!lot.turn)} ${envLine(env)} ${f(rng.next())}`;
      if (env) yield* queries(env);
    }
  }
}
