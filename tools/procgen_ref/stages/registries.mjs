// Stage "registries": the data registries and what reads them - DISTRICTS (city/districts.js),
// FLAVORS (city/flavors.js), STYLES (buildings/styles.js), the CIVIC table with its ARCHETYPES and
// its style (buildings/civic.js): every entry's data in registration order, classifyDistrict on
// sample macro fields, flavorOf on sample settlements, flavoredDistrict for every district and
// flavor (village flavors too), flavoredDistrictId on sample places, resolveStyle, civicGroup,
// civicStyle and the civic archetypes' fits and envelopes on seeded streams.
//
// The registries hold what the port registers (world/register_all.cpp): the registering modules
// it has ported, imported here in the reference's order (docs/CITY.md §2.2). What flavoredDistrict
// keeps depends on what is registered, so a port that registers more into these registries
// (buildings/archetypes.js; the sites' districts) imports its module here, in that order, and
// records the stage again.
import { REF, line, f, samples } from "../lib/rec.mjs";

const { DISTRICTS, STYLES, ARCHETYPES } = await import(REF + "world/registry.js");
const D = await import(REF + "city/districts.js");
const F = await import(REF + "city/flavors.js");
const S = await import(REF + "buildings/styles.js");
const C = await import(REF + "buildings/civic.js");
const { Rng } = await import(REF + "core/hash.js");

/** A weighted list: [id:weight,...]. */
const W = (list) => `[${list.map(([id, w]) => `${id}:${f(w)}`).join(",")}]`;
/** An object keyed by district id: {key:value,...} in its key order (undefined values as -). */
const M = (obj, fmt = f) => (obj ? `{${Object.keys(obj).map((k) => `${k}:${obj[k] === undefined ? "-" : fmt(obj[k])}`).join(",")}}` : "{}");

const flavorLine = (fl) =>
  line("flavor", fl.id, fl.weight, fl.floorScale, !!fl.when, fl.oldCore, !!fl.adaptive, fl.collectors !== false, fl.mainRoad, fl.mainRoadWobble, fl.cobbleWithin, fl.churchStyle, fl.churchDome,
    M(fl.patterns), M(fl.pitched), M(fl.styles, W), M(fl.archetypes, W), M(fl.floors), M(fl.blockUse, W), typeof fl.districts === "function" ? "fn" : M(fl.districts));

const rectStr = (q) => `${q.x0},${q.y0},${q.x1},${q.y1}`;
function envLine(env) {
  if (!env) return line("env", null);
  const tiers = env.tiers.map((t) => `${t.f0}/${t.f1}/${t.rects.map(rectStr).join(";")}`).join("|");
  const annexes = env.annexes.map((a) => `${a.kind}/${rectStr(a.rect)}/${a.height}`).join("|") || "none";
  const x = env.extra;
  return line("env", tiers, annexes, env.floors, env.storyH, env.basements, env.roof.type, env.roof.ridge, env.roof.slope, env.roof.overhang, env.roof.pitch, env.program.ground, env.program.upper,
    env.entranceSide, x.civic, x.storefront, x.sign, x.signColor, x.portico, x.parking, x.setF);
}

export default function* registries() {
  const r = samples(17);
  // ---- districts
  for (const d of DISTRICTS.all()) {
    const s = d.streets;
    yield line("district", d.id, d.label, d.color, !!d.port, s.pattern, s.block, s.pedestrianChance, s.mergeChance, s.localClass, s.laneClass, s.laneChance, s.paving, W(d.blockUse), d.lots.mode,
      d.lots.width, d.lots.alleyChance, W(d.archetypes), d.floors, W(d.styles), d.pitched);
  }
  for (let i = 0; i < 800; i += 1) {
    const out = [];
    for (let k = 0; k < 25; k += 1) {
      const u = r();
      const core = r() * r() * 1.2;
      const dn = r() - 0.5;
      const ind = r();
      const seed = Math.floor(r() * 1e6);
      const key = Math.floor(r() * 4294967296);
      const village = r() < 0.1;
      const port = r() < 0.2;
      out.push(D.classifyDistrict({ u, core, dn, ind, seed, key, village, port }));
    }
    yield line("cd", ...out);
  }
  // ---- styles
  for (const s of STYLES.all()) {
    const w = s.window;
    yield line("style", s.id, s.walls, s.base, s.trim, s.glass, s.frame, w.type, w.width, w.sill, w.head, w.bay, w.lintel, w.shutters, !!w.casing, !!w.transom, s.cornice, s.roof, s.pitched,
      s.fireEscape, s.balcony, s.waterTank, s.awning, s.storefront, s.joint, !!s.accentStrips, !!s.boards, !!s.corners, s.shopBase, s.baseH);
  }
  for (let m = 0; m < 400; m += 1) {
    const s = S.SEAMS.get(m);
    if (s) yield line("seam", m, s.m, s.dir);
  }
  for (const s of STYLES.all())
    for (let k = 0; k < 8; k += 1) {
      const g = new Rng(1000 + k * 7919);
      const rs = S.resolveStyle(s.id, g);
      yield line("rs", rs.id, rs.wall, rs.base, rs.trim, rs.glass, rs.frame, rs.window.type, rs.window.bay, rs.cornice, rs.roof, rs.pitched, rs.storefront, rs.fireEscape, rs.balcony, rs.waterTank,
        rs.awning, rs.shutters, rs.accentStrips, rs.joint, rs.seam, rs.seamDir, rs.corners, rs.shopBase, rs.baseH, g.next());
    }
  // ---- flavors
  const ids = F.FLAVORS.all().map((x) => x.id);
  const villages = ids.map((id) => F.flavorOf({ village: true, flavor: id }));
  for (const fl of F.FLAVORS.all()) yield flavorLine(fl);
  for (const fl of villages) yield flavorLine(fl);
  const settlement = () => {
    const s = { flavor: null, i: 1, j: 2, island: false, village: false, style: 0 };
    if (r() < 0.15) s.flavor = r() < 0.8 ? ids[Math.floor(r() * ids.length)] : "nope";
    if (r() < 0.1) {
      s.i = 0;
      s.j = 0;
    }
    s.island = r() < 0.2;
    s.village = r() < 0.25;
    if (r() < 0.9) {
      s.t = r();
      s.m = r();
    }
    s.style = Math.floor(r() * 4294967296);
    return s;
  };
  yield line("fo", F.flavorOf(null).id);
  for (let i = 0; i < 3000; i += 1) {
    const s = settlement();
    yield line("fo", F.flavorOf(s).id, F.FLAVORS.get("desert").when(s), F.FLAVORS.get("nordic").when(s));
  }
  for (const d of DISTRICTS.all())
    for (const fl of [...F.FLAVORS.all(), ...villages]) {
      const fd = F.flavoredDistrict(d, fl);
      yield line("fd", d.id, fl.id, fd.id, W(fd.styles), W(fd.archetypes), fd.floors, W(fd.blockUse), fd.pitched);
    }
  const allIds = [...DISTRICTS.all().map((d) => d.id), "nope"];
  for (let i = 0; i < 2000; i += 1) {
    const s = r() < 0.05 ? null : settlement();
    const c = r();
    let ctx = null;
    if (c >= 0.3) {
      const d = r() * 1.2;
      const dn = r() - 0.5;
      const ind = r() < 0.1 ? undefined : r();
      const u = r();
      const core = r();
      ctx = { d, dn, ind, u, core };
    } else if (c >= 0.2) ctx = {};
    yield line("fdi", ...allIds.map((id) => F.flavoredDistrictId(id, s, ctx)));
  }
  // ---- civic
  for (const [id, s] of Object.entries(C.CIVIC))
    yield line("civic", id, s.w, s.d, s.floors, s.story, s.setF, s.setS, s.lot, s.front, s.sign, s.roof, !!s.parking, !!s.canopy, !!s.portico);
  for (const id of [...ids, ...ids.map((x) => `${x}Village`), null, undefined, "", "nope", "Village", "nordicVillageVillage"]) yield line("cg", id, C.civicGroup(id));
  let seed = 7;
  for (const cid of [...Object.keys(C.CIVIC), "school"])
    for (const fid of [...ids, ...ids.map((x) => `${x}Village`), null]) {
      const g = new Rng((seed += 31));
      yield line("cs", cid, fid, C.civicStyle(cid, fid, g), C.civicStyle(cid, fid, g), g.next());
    }
  for (const a of ARCHETYPES.all()) {
    const fits = [];
    for (let U = 40; U <= 640; U += 40) for (let V = 40; V <= 640; V += 60) fits.push(a.fits(U, V) ? 1 : 0);
    yield line("arch", a.id, a.label, !!a.civic, fits.join(""), !!a.entranceU);
    for (let k = 0; k < 40; k += 1) {
      const U = 60 + Math.floor(r() * 700);
      const V = 60 + Math.floor(r() * 700);
      const pc = [undefined, 0, 0.08, 0.35, 0.65, 1][Math.floor(r() * 6)];
      const rng = new Rng(Math.floor(r() * 1e9));
      const env = a.envelope({ rng, frame: { U, V }, pitchedCivic: pc });
      yield `${envLine(env)} ${rng.next()}`;
    }
  }
}
