import { Registry, STYLES, ARCHETYPES, DISTRICTS } from "../world/registry.js";

/**
 * City flavors: a settlement-level character that re-weights architectural
 * styles per district and scales building heights. A flavor may also shape
 * the street network: `mainRoad` (class of the main streets), `mainRoadWobble`
 * (0..1, how much they bend), `cobbleWithin` (main streets inside this
 * normalized distance are cobbled), `collectors: false` (no collector grid),
 * `patterns` (street pattern per district, e.g. "organic"), `adaptive`
 * (the whole town is laid out organically, each part at the block size of
 * its own district), `pitched` (chance per district that a flat-roofed
 * block gets a gabled or hipped roof), `oldCore` (radius, as a share of the
 * town's, of a historic core of irregular cobbled blocks: the `oldcore`
 * district) and `churchStyle`. A futuristic or a
 * historic city is data, not code: register a flavor and settlements pick
 * one by weight (the spawn city is always "modern"). Flavors with a `when`
 * climate predicate take precedence where it holds (desert towns, northern
 * towns), so the look of a city follows its biome.
 */
export const FLAVORS = new Registry("flavor");

/**
 * Modern city: a gridded downtown of towers round a small historic core
 * (irregular cobbled blocks), mixed quarters laid out organically, grids
 * that jog here and there.
 */
FLAVORS.register({
  id: "modern",
  weight: 5,
  floorScale: 1,
  oldCore: 0.1,
  patterns: { mixed: "organic" },
  pitched: { mixed: 0.2, residential: 0.35 },
  styles: {},
});

/** Historic city: a big old core, the whole town grown organically, pitched roofs. */
FLAVORS.register({
  id: "historic",
  weight: 2,
  floorScale: 0.75,
  oldCore: 0.22,
  adaptive: true,
  pitched: { mixed: 0.6, midtown: 0.4, residential: 0.65 },
  styles: {
    downtown: [["deco", 4], ["brick", 3], ["concrete", 1]],
    midtown: [["brick", 4], ["deco", 2], ["plaster", 2]],
    mixed: [["brick", 5], ["plaster", 3]],
    residential: [["brick", 4], ["plaster", 4]],
  },
});

FLAVORS.register({
  id: "futuristic",
  weight: 1,
  floorScale: 1.35,
  styles: {
    downtown: [["futurist", 5], ["glass", 3]],
    midtown: [["futurist", 4], ["glass", 3], ["concrete", 1]],
    mixed: [["futurist", 3], ["concrete", 2], ["plaster", 1]],
    residential: [["futurist", 2], ["plaster", 2], ["concrete", 1]],
    suburban: [["futurist", 2], ["siding", 1]],
    industrial: [["futurist", 1], ["industrial", 2]],
  },
});

/**
 * Soviet town: a Stalinist centre on wide avenues, everything else
 * microdistricts of panel slabs and towers; no skyscrapers.
 */
FLAVORS.register({
  id: "soviet",
  weight: 2,
  floorScale: 0.6,
  // Orthodox churches: onion domes (gilded, green, blue)
  churchDome: ["GOLD", "DOME_GREEN", "DOME_BLUE", "GOLD"],
  districts: { midtown: "microdistrict", mixed: "microdistrict", residential: "microdistrict", suburban: "microdistrict" },
  styles: {
    downtown: [["stalinist", 5], ["panel", 2], ["concrete", 1]],
    midtown: [["stalinist", 3], ["panel", 2]],
    industrial: [["industrial", 3], ["concrete", 1]],
  },
});

FLAVORS.register({
  id: "desert",
  weight: 0,
  floorScale: 0.8,
  when: (s) => s.t > 0.66 && s.m < 0.36,
  // (a medina: the old core grows organically)
  oldCore: 0.18,
  styles: {
    oldcore: [["adobe", 5], ["plaster", 2]],
    downtown: [["adobe", 3], ["concrete", 2], ["glass", 1]],
    midtown: [["adobe", 4], ["plaster", 2], ["concrete", 1]],
    mixed: [["adobe", 5], ["plaster", 2]],
    residential: [["adobe", 5], ["plaster", 2]],
    suburban: [["adobe", 3], ["plaster", 2]],
  },
});

FLAVORS.register({
  id: "nordic",
  weight: 0,
  floorScale: 0.85,
  when: (s) => s.t < 0.3,
  oldCore: 0.14,
  churchStyle: "nordicChurch",
  pitched: { mixed: 0.5, residential: 0.8 },
  styles: {
    oldcore: [["nordicPlaster", 4], ["nordicWood", 3], ["brick", 2]],
    downtown: [["brick", 3], ["glass", 2], ["concrete", 2]],
    midtown: [["brick", 4], ["plaster", 2]],
    mixed: [["brick", 3], ["siding", 2], ["plaster", 2]],
    residential: [["siding", 4], ["brick", 2]],
    suburban: [["siding", 5], ["suburbanBrick", 1]],
  },
});

/**
 * Harbour town of a temperate island: a low old centre of rendered and
 * brick houses, the usual residential rings, a small harbour.
 */
FLAVORS.register({
  id: "harbourTown",
  weight: 0,
  floorScale: 1,
  oldCore: 0.3,
  adaptive: true,
  pitched: { downtown: 0.4, midtown: 0.5, mixed: 0.6, residential: 0.8 },
  districts: { port: "harbour" },
  floors: { downtown: [3, 6], midtown: [3, 5], mixed: [2, 4], residential: [2, 3] },
  archetypes: {
    downtown: [["midrise", 3], ["walkup", 4], ["office", 1]],
    midtown: [["walkup", 5], ["midrise", 2], ["rowhouse", 1]],
  },
  styles: {
    downtown: [["plaster", 4], ["brick", 3], ["nordicPlaster", 2]],
    midtown: [["plaster", 4], ["brick", 3]],
    mixed: [["plaster", 4], ["brick", 3], ["siding", 1]],
    residential: [["siding", 3], ["plaster", 2], ["brick", 2]],
  },
});

/**
 * Bleak northern harbour town (Norway, Karelia, the White Sea coast): an
 * old centre of wooden and rendered town houses behind the harbour, a ring
 * of post-war Stalinist and brick blocks, then wide streets through
 * panel-block microdistricts in some quarters and wooden houses in others,
 * an industrial quarter with its housing projects, a small harbour.
 * `districts` is a function of the district picked from the macro fields
 * and the sub-cell's context { d (distance to the town centre in radii),
 * dn (district noise), u, core, ind }.
 */
FLAVORS.register({
  id: "nordicBleak",
  weight: 0,
  floorScale: 1,
  churchStyle: "nordicChurch",
  // Karelian wooden churches: onion domes of aspen shingle (now and then a green one)
  churchDome: ["DOME_SHINGLE", "DOME_SHINGLE", "DOME_GREEN"],
  // cobbled lanes in the wooden old centre, broad Soviet streets through the estates
  cobbleWithin: 0.3,
  mainRoadWobble: 0.12,
  adaptive: true,
  collectors: false,
  pitched: { oldtown: 1, mixed: 0.35, residential: 0.9 },
  districts(id, f) {
    if (id === "port") return "harbour";
    // the works (a sawmill, a fish plant) with bleak blocks of flats round them
    if (id === "industrial" && (f.ind ?? 1) < 0.42) return "projects";
    if (id === "industrial" || id === "heavyIndustry" || id === "projects" || id === "park" || id === "rural") return id;
    if (f.d < 0.36) return "oldtown";
    if (id === "downtown" || id === "midtown") return "mixed";
    if (f.d < 0.62) return id === "mixed" || f.dn > 0.12 ? "mixed" : f.dn < -0.05 ? "microdistrict" : "residential";
    return f.dn < -0.12 ? "microdistrict" : id === "mixed" ? "residential" : id;
  },
  floors: { mixed: [3, 5], residential: [1, 2], microdistrict: [5, 9], projects: [5, 12] },
  blockUse: {
    microdistrict: [["micro", 0.8], ["school", 0.07], ["garages", 0.07], ["park", 0.03], ["wasteland", 0.03]],
    projects: [["micro", 0.75], ["garages", 0.12], ["wasteland", 0.08], ["parking", 0.05]],
    industrial: [["lots", 0.86], ["wasteland", 0.08], ["garages", 0.06]],
    mixed: [["lots", 0.9], ["square", 0.03], ["park", 0.04], ["school", 0.03]],
  },
  archetypes: {
    mixed: [["walkup", 5], ["midrise", 2], ["townhouse", 1]],
    residential: [["house", 6], ["townhouse", 1], ["rowhouse", 1]],
    suburban: [["house", 8]],
  },
  styles: {
    mixed: [["stalinist", 3], ["nordicPlaster", 3], ["brick", 1], ["panel", 1]],
    residential: [["nordicWood", 6], ["siding", 1]],
    suburban: [["nordicWood", 6], ["siding", 1]],
    industrial: [["industrial", 4], ["concrete", 2]],
    heavyIndustry: [["industrial", 4], ["concrete", 3]],
    projects: [["panel", 3], ["projects", 2]],
    village: [["nordicWood", 6], ["siding", 1]],
  },
});

/**
 * Norwegian harbour town (Bergen, Ålesund, Stavanger, the Lofoten towns):
 * a big old town of wooden houses on cobbled streets and lanes round the
 * harbour, a ring of rendered stone merchant houses and blocks from the
 * turn of the century, wooden villas beyond; a small works with a few
 * bleak blocks of flats; a harbour of quays, warehouses and fish sheds.
 */
FLAVORS.register({
  id: "nordicHarbour",
  weight: 0,
  floorScale: 1,
  churchStyle: "nordicChurch",
  // narrow main streets bending through the town (cobbled in the old town), no collector grid,
  // irregular blocks well beyond the old centre
  mainRoad: "village",
  mainRoadWobble: 0.22,
  cobbleWithin: 0.42,
  collectors: false,
  adaptive: true,
  pitched: { oldtown: 1, mixed: 0.85, residential: 0.9, harbour: 0.4 },
  districts(id, f) {
    if (id === "port") return "harbour";
    if (id === "heavyIndustry") return "industrial";
    // a small works: only the heart of the industrial quarter; round it a
    // few bleak blocks of flats, then ordinary housing
    if (id === "industrial" && (f.ind ?? 1) < 0.45) return f.d > 0.55 ? "projects" : "mixed";
    if (id === "projects" && (f.ind ?? 1) < 0.28) return f.d < 0.66 ? "mixed" : "residential";
    if (id === "industrial" || id === "projects" || id === "park" || id === "rural") return id;
    if (id === "microdistrict") return "residential";
    if (f.d < 0.46) return "oldtown";
    if (f.d < 0.66) return f.dn > -0.08 || id === "downtown" || id === "midtown" ? "mixed" : "oldtown";
    return id === "downtown" || id === "midtown" || (id === "mixed" && f.dn > 0.15) ? "mixed" : "residential";
  },
  floors: { mixed: [2, 4], residential: [1, 2], projects: [4, 8] },
  archetypes: {
    mixed: [["townhouse", 4], ["walkup", 3], ["midrise", 0.4]],
    residential: [["house", 7], ["townhouse", 2], ["rowhouse", 0.5]],
    suburban: [["house", 8]],
  },
  styles: {
    oldtown: [["nordicWood", 7], ["nordicPlaster", 2]],
    mixed: [["nordicPlaster", 4], ["nordicWood", 3], ["brick", 1], ["plaster", 1]],
    residential: [["nordicWood", 8], ["siding", 1]],
    suburban: [["nordicWood", 8]],
    projects: [["projects", 2], ["panel", 1], ["concrete", 1]],
    industrial: [["industrial", 3], ["nordicWood", 1]],
    harbour: [["nordicWood", 3], ["industrial", 2]],
    village: [["nordicWood", 6], ["siding", 1]],
  },
  blockUse: {
    harbour: [["lots", 0.62], ["quay", 0.22], ["parking", 0.1], ["containerYard", 0.06]],
    mixed: [["lots", 0.9], ["square", 0.03], ["park", 0.04], ["garden", 0.03]],
    projects: [["micro", 0.8], ["garages", 0.1], ["wasteland", 0.05], ["sports", 0.05]],
  },
});

const villageFlavors = new Map();

/** Villages keep their region's look (desert adobe, nordic siding...) at village height. */
function villageFlavor(f) {
  let v = villageFlavors.get(f.id);
  if (!v) {
    v = { ...f, id: `${f.id}Village`, floorScale: Math.min(f.floorScale, 0.45), styles: { mixed: [["plaster", 3], ["brick", 3], ["siding", 1]], ...f.styles, downtown: undefined } };
    villageFlavors.set(f.id, v);
  }
  return v;
}

export function flavorOf(settlement) {
  if (!settlement) return FLAVORS.get("modern");
  if (settlement.village) return villageFlavor(baseFlavor(settlement));
  return baseFlavor(settlement);
}

function baseFlavor(settlement) {
  // a flavor set by the world (an island's town) wins
  if (settlement.flavor && FLAVORS.has(settlement.flavor)) return FLAVORS.get(settlement.flavor);
  if (settlement.i === 0 && settlement.j === 0 && !settlement.island) return FLAVORS.get("modern");
  for (const f of FLAVORS.all()) if (f.when && settlement.t !== undefined && f.when(settlement)) return f;
  const all = FLAVORS.all().filter((f) => f.weight > 0);
  const total = all.reduce((s, f) => s + f.weight, 0);
  let r = ((settlement.style >>> 0) % 10000) / 10000 * total;
  for (const f of all) {
    if (r < f.weight) return f;
    r -= f.weight;
  }
  return all[0];
}

/**
 * District definition as seen through a flavor: styles, archetype weights,
 * floor range and block programs (`blockUse`) per district (unregistered
 * style / archetype ids are skipped, so a flavor may name optional kits).
 */
export function flavoredDistrict(district, flavor) {
  const styles = flavor.styles[district.id];
  const archetypes = flavor.archetypes?.[district.id];
  const floors = flavor.floors?.[district.id];
  const blockUse = flavor.blockUse?.[district.id];
  const pitched = flavor.pitched?.[district.id];
  const complete = (list, reg) => list.every(([id]) => reg.has(id));
  if (!styles && !archetypes && !floors && !blockUse && !pitched && flavor.floorScale === 1 && complete(district.styles, STYLES) && complete(district.archetypes, ARCHETYPES)) return district;
  const f = floors ?? district.floors;
  // the flavor's list (its registered entries), else the district's own
  const pick = (own, base, reg) => {
    const ok = own ? own.filter(([id]) => reg.has(id)) : [];
    return ok.length ? ok : base.filter(([id]) => reg.has(id));
  };
  return {
    ...district,
    styles: pick(styles, district.styles, STYLES),
    archetypes: pick(archetypes, district.archetypes, ARCHETYPES),
    floors: [f[0], Math.max(f[0], Math.round(f[1] * flavor.floorScale))],
    ...(blockUse ? { blockUse } : {}),
    ...(pitched ? { pitched } : {}),
  };
}

/**
 * District id as re-mapped by a settlement's flavor (Soviet towns build
 * microdistricts); `ctx` { d, dn, u, core, ind } lets a flavor map by place.
 */
export function flavoredDistrictId(districtId, settlement, ctx = null) {
  if (!settlement) return districtId;
  const flavor = flavorOf(settlement);
  const map = flavor.districts;
  if (settlement.village) return typeof map === "function" && districtId === "port" ? map(districtId, ctx ?? {}) : districtId;
  // the historic core round the centre (`oldCore`: its radius as a share of the town's)
  if (flavor.oldCore && ctx && ctx.d < flavor.oldCore && !KEEP_IN_CORE.has(districtId) && DISTRICTS_HAVE_OLDCORE()) return "oldcore";
  if (typeof map === "function") return map(districtId, ctx ?? { d: 1, dn: 0 });
  return map?.[districtId] ?? districtId;
}

/** Districts an old core never replaces. */
const KEEP_IN_CORE = new Set(["port", "park", "rural", "industrial", "heavyIndustry", "oldtown"]);
let hasOldCore = null;
function DISTRICTS_HAVE_OLDCORE() {
  hasOldCore ??= DISTRICTS.has("oldcore");
  return hasOldCore;
}
