import { ARCHETYPES, STYLES } from "../world/registry.js";
import { MAT } from "../voxel/materials.js";
import { vx } from "../core/units.js";

/**
 * Civic buildings, venues and big shops: archetypes whose envelopes come
 * from one table (CIVIC). Each entry gives the footprint (m, along the
 * street and deep), floors and story heights, setbacks (a forecourt, a car
 * park in front), and what the front shows (a shop window, a sign, a
 * portico, a petrol canopy). `lot` is the lot a town carves for it (m).
 * The interiors are planned by interior/civic.js from the programs in
 * interior/civicPrograms.js; signs, porticos and canopies are drawn by
 * interior/civicFixtures.js.
 *
 * `civicStyle(id, flavorId, rng)` picks the facade style by town flavor
 * (classical stone museums and town halls, wooden northern ones,
 * Stalinist houses of culture...).
 */

export const CIVIC = {
  supermarket: { w: [28, 56], d: [26, 42], floors: [1, 1], story: [5.5], setF: [16, 28], setS: [3, 6], lot: [52, 64], front: "shop", sign: "fascia", parking: true },
  departmentStore: { w: [30, 52], d: [26, 40], floors: [3, 5], story: [4.5, 4], setF: [0, 1], setS: [0, 0], lot: [44, 36], front: "shop", sign: "fascia" },
  petrolStation: { w: [10, 15], d: [8, 11], floors: [1, 1], story: [3.5], setF: [15, 19], setS: [4, 8], lot: [34, 34], front: "shop", sign: "fascia", canopy: true },
  marketHall: { w: [26, 44], d: [22, 34], floors: [1, 1], story: [7], setF: [2, 5], setS: [1, 3], lot: [44, 38], front: "shop", sign: "plate", roof: "gable" },
  concertHall: { w: [30, 46], d: [34, 48], floors: [2, 2], story: [4.75, 4.25], setF: [6, 9], setS: [2, 4], lot: [50, 56], sign: "marquee", portico: true },
  houseOfCulture: { w: [34, 52], d: [34, 46], floors: [2, 2], story: [4.75, 4.25], setF: [7, 10], setS: [3, 5], lot: [56, 56], sign: "plate", portico: true },
  musicClub: { w: [15, 22], d: [20, 30], floors: [1, 1], story: [5], setF: [0, 2], setS: [0, 1], lot: [22, 32], sign: "neon" },
  cinema: { w: [26, 40], d: [28, 40], floors: [2, 2], story: [4.25, 4], setF: [2, 4], setS: [1, 2], lot: [42, 44], sign: "marquee", front: "shop" },
  hospital: { w: [44, 76], d: [22, 30], floors: [3, 6], story: [4, 3.75], setF: [10, 16], setS: [3, 6], lot: [80, 48], sign: "cross" },
  polyclinic: { w: [40, 64], d: [20, 26], floors: [3, 5], story: [3.75, 3.5], setF: [8, 12], setS: [3, 6], lot: [68, 40], sign: "cross" },
  policeStation: { w: [28, 40], d: [17, 22], floors: [2, 3], story: [4.25, 3.5], setF: [4, 7], setS: [1, 3], lot: [44, 30], sign: "police" },
  fireStation: { w: [30, 40], d: [17, 22], floors: [2, 2], story: [5, 3.25], setF: [9, 12], setS: [1, 3], lot: [44, 36], sign: "fire" },
  museum: { w: [38, 64], d: [24, 32], floors: [2, 3], story: [5, 5], setF: [7, 10], setS: [3, 6], lot: [66, 44], sign: "banners", portico: true },
  artGallery: { w: [28, 50], d: [22, 30], floors: [2, 3], story: [5, 4.5], setF: [3, 7], setS: [2, 4], lot: [52, 38], sign: "banners", front: "shop" },
  library: { w: [28, 46], d: [20, 26], floors: [2, 3], story: [4.25, 4], setF: [3, 6], setS: [2, 4], lot: [48, 34], sign: "plate" },
  townHall: { w: [30, 50], d: [18, 26], floors: [2, 4], story: [4.5, 4.25], setF: [5, 8], setS: [2, 4], lot: [52, 36], sign: "plate", portico: true },
  hotel: { w: [24, 44], d: [16, 22], floors: [3, 7], story: [4.25, 3], setF: [0, 3], setS: [0, 2], lot: [44, 26], sign: "plate" },
};

/** Facade styles of the civic buildings by flavor group (first match wins; unregistered styles are skipped). */
const STYLE_TABLE = {
  default: {
    supermarket: ["concrete", "industrial", "glass"],
    departmentStore: ["deco", "glass", "concrete"],
    petrolStation: ["concrete"],
    marketHall: ["brick", "classical"],
    concertHall: ["classical", "glass", "deco"],
    houseOfCulture: ["classical"],
    musicClub: ["brick", "concrete", "industrial"],
    cinema: ["deco", "concrete", "brick"],
    hospital: ["concrete", "plaster"],
    polyclinic: ["concrete"],
    policeStation: ["brick", "concrete"],
    fireStation: ["brick"],
    museum: ["classical", "deco"],
    artGallery: ["glass", "concrete"],
    library: ["brick", "classical", "glass"],
    townHall: ["classical", "brick"],
    hotel: ["plaster", "deco", "brick"],
  },
  nordic: {
    supermarket: ["concrete", "nordicWood"],
    departmentStore: ["nordicPlaster", "brick"],
    marketHall: ["nordicWood"],
    concertHall: ["nordicWood", "glass"],
    musicClub: ["nordicWood", "brick"],
    cinema: ["nordicPlaster"],
    hospital: ["concrete", "nordicPlaster"],
    policeStation: ["brick", "nordicPlaster"],
    fireStation: ["nordicWood", "brick"],
    museum: ["nordicPlaster", "classical"],
    artGallery: ["nordicWood", "glass"],
    library: ["nordicPlaster", "nordicWood"],
    townHall: ["nordicPlaster"],
    hotel: ["nordicWood", "nordicPlaster"],
  },
  soviet: {
    supermarket: ["concrete", "panel"],
    departmentStore: ["stalinist", "concrete"],
    houseOfCulture: ["stalinist"],
    concertHall: ["stalinist"],
    cinema: ["stalinist", "concrete"],
    polyclinic: ["panel", "concrete"],
    hospital: ["panel", "concrete"],
    policeStation: ["panel", "stalinist"],
    fireStation: ["brick", "stalinist"],
    museum: ["stalinist", "classical"],
    library: ["stalinist"],
    townHall: ["stalinist"],
    hotel: ["panel", "stalinist"],
    musicClub: ["concrete", "brick"],
  },
};

const FLAVOR_GROUP = { nordic: "nordic", nordicHarbour: "nordic", harbourTown: "nordic", nordicBleak: "soviet", soviet: "soviet" };

/** The flavor group of a town flavor id: default, nordic or soviet. */
export function civicGroup(flavorId) {
  return FLAVOR_GROUP[String(flavorId ?? "").replace(/Village$/, "")] ?? "default";
}

/** A facade style for civic building `id` in a town of flavor `flavorId`. */
export function civicStyle(id, flavorId, rng) {
  const group = civicGroup(flavorId);
  const list = (STYLE_TABLE[group][id] ?? STYLE_TABLE.default[id] ?? ["concrete"]).filter((s) => STYLES.has(s));
  return list.length ? rng.pick(list) : "concrete";
}

for (const [id, spec] of Object.entries(CIVIC)) {
  ARCHETYPES.register({
    id,
    label: id,
    civic: true,
    fits: (U, V) => U >= vx(spec.w[0] + 2 * (spec.setS?.[0] ?? 0)) && V >= vx(spec.d[0] + spec.setF[0] + 1),
    envelope: (ctx) => civicEnvelope(ctx, id, spec),
  });
}

function civicEnvelope(ctx, id, spec) {
  const { rng, frame } = ctx;
  const U = frame.U;
  const V = frame.V;
  const setS = vx(rng.float(spec.setS?.[0] ?? 0, spec.setS?.[1] ?? 0));
  const w = Math.min(U - 2 * setS, vx(rng.float(spec.w[0], spec.w[1])));
  const setF = Math.min(vx(rng.float(spec.setF[0], spec.setF[1])), V - vx(spec.d[0]) - vx(1));
  const d = Math.min(V - setF - vx(1), vx(rng.float(spec.d[0], spec.d[1])));
  if (w < vx(spec.w[0]) - 8 || d < vx(spec.d[0]) - 8 || setF < vx(spec.setF[0]) - 8) return null;
  const u0 = Math.round((U - w) / 2);
  const floors = rng.int(spec.floors[0], spec.floors[1]);
  const storyH = [];
  for (let f = 0; f < floors; f += 1) storyH.push(vx(spec.story[Math.min(f, spec.story.length - 1)]));
  const rect = { x0: u0, y0: setF, x1: u0 + w - 1, y1: setF + d - 1 };
  const annexes = [];
  if (spec.canopy) {
    // the petrol canopy over the pumps in front of the shop
    const cw = Math.min(U - vx(2), w + vx(rng.float(6, 12)));
    const cu0 = Math.round((U - cw) / 2);
    annexes.push({ kind: "canopy", rect: { x0: cu0, y0: vx(2.5), x1: cu0 + cw - 1, y1: setF - vx(2) }, height: vx(5) });
  }
  if (spec.parking || spec.canopy) {
    // a price / logo pylon at the lot's street corner
    const pu = rng.chance(0.5) ? vx(1.5) : U - vx(2.5);
    annexes.push({ kind: "pylon", rect: { x0: pu, y0: vx(1), x1: pu + 7, y1: vx(1) + 3 }, height: vx(spec.canopy ? 7 : 9) });
  }
  const pitched = spec.roof === "gable" || (ctx.pitchedCivic && rng.chance(ctx.pitchedCivic) && floors <= 4 && !spec.parking && !spec.canopy);
  const roof = pitched ? { type: spec.roof === "gable" ? "gable" : rng.chance(0.5) ? "hip" : "gable", ridge: "u", slope: spec.roof === "gable" ? 0.55 : rng.float(0.55, 0.8), overhang: 3 } : { type: "flat" };
  const signColor = rng.pick([MAT.SIGNAGE_RED, MAT.SIGNAGE_BLUE, MAT.SIGNAGE, MAT.SIGN_GREEN, MAT.NEON_RED]);
  return {
    tiers: [{ f0: 0, f1: floors - 1, rects: [rect] }],
    annexes,
    floors,
    storyH,
    basements: 0,
    roof,
    program: { ground: id, upper: null },
    entranceSide: "F",
    extra: {
      civic: id,
      storefront: spec.front === "shop",
      sign: spec.sign ?? null,
      signColor,
      portico: !!spec.portico && setF >= vx(4.5) && w >= vx(20),
      parking: !!spec.parking,
      // the building's distance from the lot front (voxels): room for a portico, pumps, a porch
      setF,
    },
  };
}

/** Classical stone front of museums, town halls and concert halls: limestone, tall windows, a cornice. */
STYLES.register({
  id: "classical",
  walls: [MAT.LIMESTONE, MAT.TRIM_STONE, MAT.PLASTER_CREAM, MAT.PLASTER_WHITE],
  base: [MAT.GRANITE, MAT.GRANITE_LIGHT],
  trim: [MAT.PLASTER_WHITE, MAT.TRIM_STONE, MAT.CORNICE],
  glass: [MAT.GLASS],
  frame: [MAT.FRAME_WHITE, MAT.FRAME_DARK],
  window: { type: "punched", width: 1.375, sill: 1.0, head: 3.125, bay: 3.5, lintel: true, casing: true },
  cornice: true,
  roof: [MAT.ROOF_MEMBRANE],
  pitched: [MAT.ROOF_METAL_GREEN, MAT.ROOF_SLATE],
  fireEscape: 0,
  balcony: 0,
  waterTank: 0,
  awning: 0,
  storefront: [MAT.FRAME_DARK],
});
