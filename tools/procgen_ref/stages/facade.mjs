// Stage "facade": facade rules (buildings/facade.js) - buildingLook of envelopes of every archetype
// in every style (planBuildingEnvelopeAs on scripted lots: shop fronts, civic storefronts, nordic
// boards and seams, panel joints, curtain walls, open decks), the class of every facade cell
// (facadeCell, wallClass through it) over side lengths, floors and the envelope's story heights,
// and the material of every class at sample floors and heights (facadeMaterial). Class rasters are
// written out for the first looks and as an FNV-1a hash after. tests/city/test_facade.cpp makes the
// same looks.
import { REF, line, samples } from "../lib/rec.mjs";
import { districtList, configList, scriptedLot } from "../lib/buildings.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const FA = await import(REF + "buildings/facade.js");
const { Rng } = await import(REF + "core/hash.js");

const LENS = [5, 13, 24, 37, 61, 157];
const FLOORS = [0, 1, 3];
const ZRS = [0, 1, 2, 3, 5, 6, 7, 99];
const CODE = "0123456789abcdef";
const RAW = 60;

function fnv(s) {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i += 1) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

export default function* facade() {
  yield line("win", ...Object.entries(FA.WIN).map(([k, v]) => `${k}:${v}`));
  const r = samples(43);
  const DS = districtList();
  const CFG = configList();
  const styles = STYLES.all().map((s) => s.id);
  const all = ARCHETYPES.all();
  let k = 0;
  for (let s = 0; s < 300; s += 1) {
    const style = styles[s % styles.length];
    let env = null;
    let config = null;
    for (let tries = 0; !env && tries < 20; tries += 1) {
      const a = all[Math.floor(r() * all.length)];
      let U = 0;
      let V = 0;
      for (let t = 0; t < 40; t += 1) {
        U = 40 + Math.floor(r() * 680);
        V = 40 + Math.floor(r() * 680);
        if (a.fits(U, V)) break;
      }
      const d = DS[Math.floor(r() * DS.length)];
      const lot = scriptedLot(r, U, V, (k += 1), d.id);
      config = CFG[Math.floor(r() * CFG.length)];
      const rng = new Rng(Math.floor(r() * 4294967296));
      const extra = { u: r(), core: r() * 1.2, groundZ: 0, config };
      env = A.planBuildingEnvelopeAs(lot, a.id, style, d, rng, extra);
    }
    if (!env) {
      yield line("look", "-");
      continue;
    }
    const look = FA.buildingLook(env, config.seed);
    const st = look.style;
    yield line("look", env.id, env.archetype, env.style, env.program.ground, !!env.storefront, st.id, st.wall, st.base, st.trim, st.glass, st.frame, st.roof, st.pitched,
      st.storefront, st.fireEscape, st.balcony, st.waterTank, st.awning, st.shutters, st.accentStrips, st.joint, st.seam, st.seamDir, st.corners, st.shopBase, st.baseH,
      look.bay, look.winW, look.sill, look.head, look.type, look.lintel, look.storefront, look.litSeed, look.joint, look.casing, look.transom, look.boards,
      FA.buildingLook(env, config.seed) === look);
    for (const len of LENS)
      for (const fl of FLOORS) {
        const H = env.storyH[Math.min(fl, env.storyH.length - 1)];
        let row = "";
        for (let zr = 0; zr <= H + 1; zr += 1) for (let t = 0; t < len; t += 1) row += CODE[FA.facadeCell(look, len, t, zr, H, fl)];
        yield line("fc", len, fl, H, s < RAW ? row : fnv(row));
      }
    const wall = [];
    for (const len of [4, 9]) for (let t = 0; t < len; t += 1) for (let zr = 0; zr < 7; zr += 1) wall.push(CODE[FA.wallClass(look, len, t, zr)]);
    const mats = [];
    for (let cls = 0; cls <= 11; cls += 1) for (const fl of FLOORS) for (const zr of ZRS) mats.push(FA.facadeMaterial(look, cls, fl, zr));
    mats.push(FA.facadeMaterial(look, 6, 3), FA.facadeMaterial(look, 8, 0), FA.facadeMaterial(look, 10, 0));
    yield line("fm", wall.join(""), mats);
  }
}
