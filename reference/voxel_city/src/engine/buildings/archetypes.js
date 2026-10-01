import { ARCHETYPES, STYLES } from "../world/registry.js";
import { vx } from "../core/units.js";
import { Frame, frameOf, lotFrameOf } from "./frame.js";
import { lerp, clamp } from "../core/math.js";
import "./styles.js";
import { houseColumns } from "./interior/houses.js";
import { cabinLayout } from "./interior/cabins.js";
import { hash32 } from "../core/hash.js";
import { wrapOf } from "../world/wrap.js";
import "./civic.js";
import { MAT } from "../voxel/materials.js";

/**
 * Building archetypes: massing ("envelope") rules per building type.
 *
 * `envelope(ctx)` runs during cell planning (cheap: footprint tiers, floor
 * counts, heights, program) and is all that coarse LODs and maps need. The
 * full interior is produced later, lazily, by `plan` functions registered
 * in buildings/interior.
 *
 * ctx: { lot, frame (lot frame), district, rng, u, core, groundZ, config }
 * Footprints are returned in LOT-canonical coords (u along the frontage,
 * v from the street), then normalized by `finalizeEnvelope`.
 */

const S = {
  house: vx(2.75),
  res: vx(3.0),
  office: vx(3.75),
  retail: vx(4.5),
  industrial: vx(8.0),
};

function floorsFor(ctx, extraScale = 1) {
  const [fMin, fMax] = ctx.district.floors;
  const t = clamp(ctx.core * 1.1 + ctx.rng.float(-0.2, 0.25), 0, 1);
  const f = Math.round(lerp(fMin, fMax, t * t) * extraScale);
  return clamp(f, fMin, fMax);
}

ARCHETYPES.register({
  id: "house",
  fits: (U, V) => U >= vx(12) && V >= vx(21),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const setF = vx(rng.float(4.5, 7));
    const setS = vx(rng.float(1.5, 2.5));
    const width = Math.min(U - 2 * setS, vx(rng.float(8.5, 12)));
    const depth = Math.min(V - setF - vx(6), vx(rng.float(10.75, 12.5)));
    if (width < vx(8.25) || depth < vx(10.5)) return null;
    const garage = U - width - 2 * setS >= vx(4) && rng.chance(0.55);
    const leftSide = rng.chance(0.5);
    const u0 = garage ? (leftSide ? U - setS - width : setS) : Math.round((U - width) / 2);
    const main = { x0: u0, y0: setF, x1: u0 + width - 1, y1: setF + depth - 1 };
    const floors = rng.chance(0.25) ? 1 : 2;
    const tiers = [{ f0: 0, f1: floors - 1, rects: [main] }];
    const annexes = [];
    if (garage) {
      const gw = vx(3.75);
      const gu0 = leftSide ? main.x0 - gw : main.x1 + 1;
      annexes.push({ kind: "garage", rect: { x0: gu0, y0: setF + vx(0.5), x1: gu0 + gw - 1, y1: setF + vx(0.5) + vx(6.5) - 1 }, height: vx(2.75) });
    }
    return {
      tiers,
      annexes,
      floors,
      storyH: new Array(floors).fill(S.house),
      basements: 0,
      roof: { type: rng.chance(0.65) ? "gable" : "hip", pitch: 1 },
      program: { ground: "house", upper: "house" },
      entranceSide: "F",
    };
  },
});

ARCHETYPES.register({
  id: "rowhouse",
  fits: (U, V) => U >= vx(7) && U <= vx(11) && V >= vx(14),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const setF = ctx.lot.corner ? 0 : vx(rng.pick([0, 1.5, 2.5]));
    const depth = Math.min(V - setF - vx(4), vx(rng.float(10.5, 14)));
    if (depth < vx(10)) return null;
    const floors = clamp(floorsFor(ctx), 2, 4);
    const main = { x0: 0, y0: setF, x1: U - 1, y1: setF + depth - 1 };
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [main] }],
      annexes: [],
      floors,
      storyH: new Array(floors).fill(S.res),
      basements: rng.chance(0.4) ? 1 : 0,
      roof: { type: rng.chance(0.25) ? "gable" : "flat" },
      program: { ground: "house", upper: "house" },
      entranceSide: "F",
      stoop: setF > 0,
    };
  },
});

ARCHETYPES.register({
  id: "walkup",
  fits: (U, V) => U >= vx(9) && V >= vx(13),
  envelope(ctx) {
    const { rng, frame, district } = ctx;
    const U = frame.U;
    const V = frame.V;
    const depth = Math.min(V - vx(3), vx(rng.float(11, 16)));
    if (depth < vx(9.5)) return null;
    const floors = clamp(floorsFor(ctx), 3, 6);
    const retail = (district.id === "mixed" || district.id === "midtown") && rng.chance(0.7);
    const main = { x0: 0, y0: 0, x1: U - 1, y1: depth - 1 };
    const storyH = new Array(floors).fill(S.res);
    if (retail) storyH[0] = vx(4.0);
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [main] }],
      annexes: [],
      floors,
      storyH,
      basements: rng.chance(0.5) ? 1 : 0,
      roof: { type: "flat" },
      program: { ground: retail ? "retail" : "apartments", upper: "apartments" },
      entranceSide: "F",
    };
  },
});

ARCHETYPES.register({
  id: "midrise",
  fits: (U, V) => U >= vx(16) && V >= vx(17),
  envelope(ctx) {
    const { rng, frame, district } = ctx;
    const U = frame.U;
    const V = frame.V;
    const depth = Math.min(V - vx(2), vx(rng.float(15.5, 20)));
    if (depth < vx(14.5)) return null;
    const floors = clamp(floorsFor(ctx, 1.1), 5, 16);
    const retail = district.id !== "residential" && rng.chance(0.75);
    const main = { x0: 0, y0: 0, x1: U - 1, y1: depth - 1 };
    const storyH = new Array(floors).fill(S.res);
    storyH[0] = retail ? S.retail : vx(3.5);
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [main] }],
      annexes: [],
      floors,
      storyH,
      basements: 1,
      roof: { type: "flat" },
      program: { ground: retail ? "retail" : "lobbyApartments", upper: "apartments" },
      entranceSide: "F",
    };
  },
});

/**
 * Panel slab (Soviet khrushchyovka / brezhnevka, social housing slab): a
 * long, shallow freestanding block with one stair section per entrance,
 * 5, 9 or 12-16 floors of flats with low ceilings. The lot is the footprint.
 */
ARCHETYPES.register({
  id: "panelSlab",
  fits: (U, V) => U >= vx(24) && V >= vx(12),
  envelope(ctx) {
    const { rng, frame, district } = ctx;
    const depth = Math.min(frame.V - vx(1), vx(rng.float(12, 13.5)));
    const [fMin, fMax] = district.floors;
    const options = [5, 9, 9, 12, 14, 16].filter((f) => f >= fMin && f <= fMax);
    const floors = options.length ? rng.pick(options) : clamp(fMin, 2, 16);
    const y0 = Math.round((frame.V - depth) / 2);
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [{ x0: 0, y0, x1: frame.U - 1, y1: y0 + depth - 1 }] }],
      annexes: [],
      floors,
      storyH: new Array(floors).fill(vx(2.875)),
      basements: 1,
      roof: { type: "flat" },
      program: { ground: "lobbyApartments", upper: "apartments" },
      entranceSide: "F",
    };
  },
});

/** Point tower of the same system: a square block of flats round one core. */
ARCHETYPES.register({
  id: "panelTower",
  fits: (U, V) => U >= vx(20) && V >= vx(20),
  envelope(ctx) {
    const { rng, frame, district } = ctx;
    const side = Math.min(frame.U, frame.V, vx(rng.float(21, 25)));
    const [fMin, fMax] = district.floors;
    const floors = clamp(rng.int(Math.max(fMin, 12), Math.max(fMin, fMax + 6)), 9, 24);
    const x0 = Math.round((frame.U - side) / 2);
    const y0 = Math.round((frame.V - side) / 2);
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [{ x0, y0, x1: x0 + side - 1, y1: y0 + side - 1 }] }],
      annexes: [],
      floors,
      storyH: new Array(floors).fill(vx(2.875)),
      basements: 1,
      roof: { type: "flat" },
      program: { ground: "lobbyApartments", upper: "apartments" },
      entranceSide: "F",
    };
  },
});

ARCHETYPES.register({
  id: "office",
  fits: (U, V) => U >= vx(20) && V >= vx(20),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const depth = Math.min(V - vx(1), vx(rng.float(20, 30)));
    const floors = clamp(floorsFor(ctx, 0.8), 4, 18);
    const main = { x0: 0, y0: 0, x1: U - 1, y1: depth - 1 };
    const storyH = new Array(floors).fill(S.office);
    storyH[0] = S.retail;
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [main] }],
      annexes: [],
      floors,
      storyH,
      basements: rng.chance(0.6) ? 2 : 1,
      roof: { type: "flat" },
      program: { ground: "officeLobby", upper: "office" },
      entranceSide: "F",
    };
  },
});

ARCHETYPES.register({
  id: "tower",
  fits: (U, V) => U >= vx(30) && V >= vx(30),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const podiumFloors = rng.int(2, 4);
    const floors = clamp(floorsFor(ctx, 1), 12, 46);
    const residential = rng.chance(0.3);
    const inset = vx(rng.float(3, 6));
    const tw = Math.min(U - 2 * inset, vx(rng.float(24, 38)));
    const td = Math.min(V - 2 * inset, vx(rng.float(24, 34)));
    if (tw < vx(20) || td < vx(20)) return null;
    const tu0 = Math.round((U - tw) / 2);
    const tv0 = Math.round((V - td) / 2 + rng.float(-0.15, 0.1) * (V - td));
    const tower = { x0: tu0, y0: tv0, x1: tu0 + tw - 1, y1: tv0 + td - 1 };
    const podium = { x0: 0, y0: 0, x1: U - 1, y1: Math.min(V - 1, tower.y1 + vx(rng.float(2, 8))) };
    const tiers = [
      { f0: 0, f1: podiumFloors - 1, rects: [podium] },
      { f0: podiumFloors, f1: floors - 1, rects: [tower] },
    ];
    // upper setback for tall towers
    if (floors > 28 && rng.chance(0.6)) {
      const cut = Math.round(floors * rng.float(0.65, 0.8));
      const s = vx(rng.float(2.5, 4.5));
      const upper = { x0: tower.x0 + s, y0: tower.y0 + s, x1: tower.x1 - s, y1: tower.y1 - s };
      tiers[1].f1 = cut - 1;
      tiers.push({ f0: cut, f1: floors - 1, rects: [upper] });
    }
    const storyH = new Array(floors).fill(residential ? S.res : S.office);
    for (let f = 0; f < podiumFloors; f += 1) storyH[f] = f === 0 ? S.retail : S.office;
    return {
      tiers,
      annexes: [],
      floors,
      storyH,
      basements: 2,
      roof: { type: "flat", crown: rng.chance(0.5) },
      program: { ground: "officeLobby", podium: "office", upper: residential ? "apartments" : "office" },
      podiumFloors,
      entranceSide: "F",
    };
  },
});

ARCHETYPES.register({
  id: "warehouse",
  fits: (U, V) => U >= vx(25) && V >= vx(30),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const setF = vx(rng.float(10, 18));
    const setS = vx(rng.float(3, 6));
    const w = U - 2 * setS;
    const d = Math.min(V - setF - vx(5), vx(rng.float(24, 70)));
    if (w < vx(18) || d < vx(16)) return null;
    const hall = { x0: setS, y0: setF, x1: setS + w - 1, y1: setF + d - 1 };
    return {
      tiers: [{ f0: 0, f1: 0, rects: [hall] }],
      annexes: [],
      floors: 1,
      storyH: [vx(rng.float(7.5, 10))],
      basements: 0,
      roof: { type: rng.chance(0.3) ? "sawtooth" : "flat" },
      program: { ground: warehouseKind(ctx), upper: null },
      entranceSide: "F",
      yard: true,
    };
  },
});

/**
 * What a town's warehouse holds: pallet racks, or a distribution centre
 * (conveyors, parcel cages), self-storage units, a cold store, a timber
 * merchant's hall (drawn from its own stream: the envelope stays the same).
 * Warehouses on military and research sites stay plain.
 */
const WAREHOUSE_KINDS = [["warehouse", 5], ["distribution", 2], ["selfStorage", 1.5], ["coldStore", 1], ["timberYard", 1]];
const TOWN_WAREHOUSE_DISTRICTS = new Set(["industrial", "heavyIndustry", "harbour", "port", "mixed", "suburban", "residential", "projects"]);
function warehouseKind(ctx) {
  if (!TOWN_WAREHOUSE_DISTRICTS.has(ctx.district?.id)) return "warehouse";
  return ctx.rng.fork("kind").weighted(WAREHOUSE_KINDS);
}

/** Farm barn: one tall hall under a gable roof (hay, stalls), a small office and loft stair in a corner. */
ARCHETYPES.register({
  id: "barn",
  fits: (U, V) => U >= vx(16) && V >= vx(20),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const setF = vx(rng.float(3, 6));
    const setS = vx(rng.float(1.5, 3));
    const w = Math.min(frame.U - 2 * setS, vx(rng.float(16, 24)));
    const d = Math.min(frame.V - setF - vx(3), vx(rng.float(20, 32)));
    if (w < vx(14) || d < vx(16)) return null;
    const u0 = Math.round((frame.U - w) / 2);
    return {
      tiers: [{ f0: 0, f1: 0, rects: [{ x0: u0, y0: setF, x1: u0 + w - 1, y1: setF + d - 1 }] }],
      annexes: [],
      floors: 1,
      storyH: [vx(rng.float(6.5, 8))],
      basements: 0,
      roof: { type: "gable", pitch: 1 },
      program: { ground: "barn", upper: null },
      entranceSide: "F",
      yard: true,
    };
  },
});

ARCHETYPES.register({
  id: "factory",
  fits: (U, V) => U >= vx(30) && V >= vx(35),
  envelope(ctx) {
    const env = ARCHETYPES.get("warehouse").envelope(ctx);
    if (!env) return null;
    env.roof = { type: "sawtooth", chimney: ctx.rng.chance(0.7) };
    env.program = { ground: "factory", upper: null };
    return env;
  },
});

ARCHETYPES.register({
  id: "garage",
  label: "Parking garage",
  fits: (U, V) => U >= vx(32) && U <= vx(70) && V >= vx(40),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const depth = Math.min(V - vx(1), vx(rng.float(40, 52)));
    const floors = rng.int(3, 6);
    const main = { x0: 0, y0: 0, x1: U - 1, y1: depth - 1 };
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [main] }],
      annexes: [],
      floors,
      storyH: new Array(floors).fill(vx(3.0)),
      basements: 0,
      roof: { type: "flat" },
      program: { ground: "garage", upper: "garage" },
      entranceSide: "F",
      forceStyle: "parkingDeck",
    };
  },
});

ARCHETYPES.register({
  id: "school",
  label: "School",
  // civic: placed on whole blocks by the cell plan, never picked by weight
  fits: (U, V) => U >= vx(44) && V >= vx(36),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const setF = vx(rng.float(5, 8));
    const width = Math.min(U - vx(10), vx(rng.float(46, 72)));
    const depth = Math.min(V - setF - vx(14), vx(rng.float(19, 22)));
    if (depth < vx(18)) return null;
    const u0 = Math.round((U - width) / 2);
    const floors = rng.int(2, 3);
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [{ x0: u0, y0: setF, x1: u0 + width - 1, y1: setF + depth - 1 }] }],
      annexes: [],
      floors,
      storyH: new Array(floors).fill(vx(3.75)),
      basements: 0,
      roof: { type: "flat" },
      program: { ground: "school", upper: "school" },
      entranceSide: "F",
    };
  },
});

// --- nordic -------------------------------------------------------------------
//
// Optional envelope keys used here (absent on every other archetype):
//   roof.slope     rise per voxel of a pitched roof (default 0.7, ~35°)
//   roof.ridge     "u" (default: ridge along the street) | "v" (gable to the street)
//   roof.overhang  eaves overhang in voxels, a number or {F, B, L, R} (default 3)
//   plinth         stone plinth from the ground up to a raised ground floor
//   porch          wooden porch + steps at the front door instead of a canopy
//   steeple        {rect, shaft, spire}: ground-tier rect `rect` rises as a
//                  church tower `shaft` voxels above the eaves, then a spire
// and an archetype may define `entranceU(env, U, V, mirror)` so the lot
// dressing knows where its front door is before the interior is planned.

/**
 * Nordic wooden town house (old town centres of Norwegian, Swedish and
 * Karelian towns) on a perimeter-block lot: 2-3 low floors built to the
 * street line under a steep gable roof, the ridge along the street or, on
 * narrow lots, the gable to the street; gable ends and eaves stay flush with
 * the party walls. In an "oldtown" district the ground floor is often a shop
 * with flats above; otherwise one family house (house planner) or small
 * flats (apartment planner). Entrance on the front facade.
 */
ARCHETYPES.register({
  id: "townhouse",
  label: "Town house",
  // (old-town block ends and corner plots run to ~26 m: a broad merchant's house)
  fits: (U, V) => U >= vx(7.5) && U <= vx(26) && V >= vx(11),
  envelope(ctx) {
    const { rng, frame, district } = ctx;
    const U = frame.U;
    const V = frame.V;
    const depth = Math.min(V - vx(2), vx(rng.float(9, 12.5)));
    if (depth < vx(8.5)) return null;
    const floors = clamp(floorsFor(ctx), 2, 3);
    const shop = district.id === "oldtown" && rng.chance(0.55);
    // an old street line is never quite straight: some houses stand a little
    // back, and a passage (a fire lane, a gate to the back yard) opens
    // beside some of them
    const old = district.id === "oldtown" || district.id === "mixed";
    const setF = old && !ctx.lot.corner ? rng.pick([0, 0, 0, 0, 1, 2, 3, 4]) : 0;
    const gap = old && U >= vx(10) && rng.chance(0.3) ? vx(rng.float(1.5, 2.5)) : 0;
    const gapLeft = rng.chance(0.5);
    const W = U - gap;
    // (a broad plot holds flats: the house planner is for narrow houses)
    const house = !shop && W <= vx(17) && (W < vx(10) || rng.chance(0.45));
    const storyH = new Array(floors).fill(S.res);
    if (shop) storyH[0] = vx(3.5);
    const gableFront = W <= vx(12) && rng.chance(0.5);
    const roof = gableFront
      ? { type: "gable", ridge: "v", slope: rng.float(1.0, 1.2), overhang: { F: 2, B: 2, L: gap ? 2 : 0, R: gap ? 2 : 0 } }
      : { type: "gable", ridge: "u", slope: rng.float(0.9, 1.1), overhang: { F: 3, B: 3, L: 0, R: 0 } };
    const x0 = gap && gapLeft ? gap : 0;
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [{ x0, y0: setF, x1: x0 + W - 1, y1: setF + depth - 1 }] }],
      annexes: [],
      floors,
      storyH,
      basements: rng.chance(0.3) ? 1 : 0,
      roof,
      program: shop ? { ground: "retail", upper: "apartments" } : house ? { ground: "house", upper: "house" } : { ground: "apartments", upper: "apartments" },
      entranceSide: "F",
    };
  },
  entranceU: (env, U, V, mirror) => (env.program.upper === "house" ? houseColumns(U, mirror).entranceU : Math.floor(U / 2)),
});

/**
 * Wharf warehouse (Bryggen in Bergen): a narrow, deep wooden house of three
 * or four storeys with its steep gable to the street, in falu red, ochre
 * or white boards, a shop or workshop below and rooms above; a row of them
 * lines the harbour front.
 */
ARCHETYPES.register({
  id: "wharfhouse",
  label: "Wharf warehouse",
  fits: (U, V) => U >= vx(6.5) && U <= vx(13) && V >= vx(14),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const depth = Math.min(V - vx(1), vx(rng.float(13, 22)));
    if (depth < vx(12)) return null;
    const floors = rng.chance(0.35) ? 4 : 3;
    const storyH = new Array(floors).fill(vx(2.875));
    storyH[0] = vx(3.25);
    return {
      tiers: [{ f0: 0, f1: floors - 1, rects: [{ x0: 0, y0: 0, x1: U - 1, y1: depth - 1 }] }],
      annexes: [],
      floors,
      storyH,
      basements: 0,
      roof: { type: "gable", ridge: "v", slope: rng.float(1.2, 1.45), overhang: { F: 3, B: 2, L: 0, R: 0 } },
      program: { ground: "retail", upper: "apartments" },
      entranceSide: "F",
    };
  },
});

/**
 * Forest cabin (hytte): one low floor of tarred logs or falu-red boards on a
 * stone plinth under a steep gable roof with deep eaves, a wooden porch at
 * the front door; living room, kitchen, one or two bedrooms and a bathroom
 * (interior/cabins.js). Freestanding and set back on a ~14-22 m lot.
 */
ARCHETYPES.register({
  id: "cabin",
  label: "Cabin",
  fits: (U, V) => U >= vx(10) && V >= vx(11),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    const w = Math.min(U - vx(3), vx(rng.float(6.25, 8.25)));
    const d = Math.min(V - vx(4), vx(rng.float(7, 9.75)));
    if (w < vx(6) || d < vx(6.5)) return null;
    // set back far enough for the porch and its steps
    const setF = Math.min(V - d - vx(1.5), vx(rng.float(2.75, 5)));
    const u0 = Math.round((U - w) / 2 + rng.float(-0.2, 0.2) * (U - w));
    return {
      tiers: [{ f0: 0, f1: 0, rects: [{ x0: u0, y0: setF, x1: u0 + w - 1, y1: setF + d - 1 }] }],
      annexes: [],
      floors: 1,
      storyH: [vx(2.625)],
      basements: 0,
      roof: { type: "gable", ridge: rng.chance(0.6) ? "u" : "v", slope: rng.float(0.95, 1.25), overhang: 4 },
      program: { ground: "cabin", upper: null },
      entranceSide: "F",
      stoop: true,
      plinth: true,
      porch: true,
    };
  },
  entranceU: (env, U, V, mirror) => cabinLayout(U, V, mirror).entranceU,
});

/**
 * Wooden church (Norwegian long church) for an old-town square: one tall
 * nave under a steep roof with its gable to the street, a square west tower
 * in front (the entrance vestibule) rising above the ridge as a boarded
 * belfry with a pyramid spire and a finial cross (massing.js steeple).
 * Interior: vestibule + one open nave with pews and an altar.
 */
ARCHETYPES.register({
  id: "church",
  label: "Church",
  fits: (U, V) => U >= vx(13) && V >= vx(24),
  envelope(ctx) {
    const { rng, frame } = ctx;
    const U = frame.U;
    const V = frame.V;
    // (a cemetery chapel is smaller than a town's church)
    const chapel = !!ctx.chapel;
    const naveW = Math.min(U - vx(3), chapel ? vx(rng.float(9.5, 10.5)) : vx(rng.float(9.5, 12.5)));
    const towerW = clamp(Math.round(naveW * 0.5), vx(4.75), vx(6));
    const setF = Math.min(vx(rng.float(3, 6)), V - towerW - vx(15));
    const naveL = Math.min(V - setF - towerW - vx(2), chapel ? vx(rng.float(12, 15)) : vx(rng.float(16, 22)));
    if (naveW < vx(9.5) || naveL < vx(12) || setF < vx(1.5)) return null;
    const u0 = Math.round((U - naveW) / 2);
    const tu0 = Math.round((U - towerW) / 2);
    // the tower overlaps the nave's front wall: vestibule and nave share a partition
    const ny0 = setF + towerW - 2;
    const nave = { x0: u0, y0: ny0, x1: u0 + naveW - 1, y1: ny0 + naveL - 1 };
    const tower = { x0: tu0, y0: setF, x1: tu0 + towerW - 1, y1: ny0 + 1 };
    const slope = rng.float(1.3, 1.6);
    const ridge = Math.floor((naveW / 2 + 3) * slope) + 1;
    const storyH = [vx(rng.float(6, 7))];
    const steeple = { rect: 1, shaft: ridge + vx(chapel ? rng.float(1, 2) : rng.float(2.5, 4.5)), spire: vx(chapel ? rng.float(4, 6) : rng.float(8, 12)) };
    // an Orthodox church (Russian and Karelian towns: `ctx.dome`, the flavor's
    // dome materials): an onion dome on the bell tower (or a tented spire with
    // a small onion on top), one to three onions on drums along the nave ridge
    let domes = null;
    if (ctx.dome && ctx.dome.length) {
      const m = MAT[rng.pick(ctx.dome)] ?? MAT.DOME_GREEN;
      steeple.dome = m;
      steeple.tent = !chapel && rng.chance(0.35);
      if (!chapel) {
        const r = clamp(Math.round(naveW * 0.24), 12, vx(2.25));
        domes = [{ rect: 0, fv: 0.5, r, drum: ridge + vx(rng.float(1.25, 2.25)), h: Math.round(r * 2.4), m }];
        if (naveL >= vx(17) && rng.chance(0.6)) {
          const r2 = Math.round(r * 0.62);
          for (const fv of [0.2, 0.8]) domes.push({ rect: 0, fv, r: r2, drum: ridge + vx(1), h: Math.round(r2 * 2.4), m });
        }
      }
    }
    return {
      tiers: [{ f0: 0, f1: 0, rects: [nave, tower] }],
      annexes: [],
      floors: 1,
      storyH,
      basements: 0,
      roof: { type: "gable", ridge: "v", slope, overhang: { F: 2, B: 3, L: 3, R: 3 } },
      program: { ground: "church", upper: null },
      entranceSide: "F",
      steeple,
      ...(domes ? { domes } : {}),
    };
  },
});

/**
 * Choose an archetype for a lot and build its normalized envelope, or null
 * (lot stays open: yard / parking).
 */
export function planBuildingEnvelope(lot, district, rng, extra) {
  const lotFrame = lotFrameOf(lot);
  const U = lotFrame.U;
  const V = lotFrame.V;
  const options = district.archetypes.filter(([id]) => ARCHETYPES.get(id).fits(U, V));
  let choice = options.length ? rng.weighted(options) : null;
  if (!choice) {
    const fallbacks = ["walkup", "rowhouse", "house", "warehouse"].filter((id) => ARCHETYPES.get(id).fits(U, V));
    if (!fallbacks.length || district.archetypes.length === 0) return null;
    choice = fallbacks[0];
  }
  const arch = ARCHETYPES.get(choice);
  const ctx = { lot, frame: lotFrame, district, rng, ...extra };
  const env = arch.envelope(ctx);
  if (!env) return null;
  pitchRoof(env, choice, lot, district, rng);
  const styleId = env.forceStyle ?? (district.styles.length ? rng.weighted(district.styles) : "concrete");
  return finalizeEnvelope(lot, lotFrame, choice, styleId, env, extra);
}

/** Archetypes whose flat roof a flavor may turn into a pitched one (one rect, apartment or house plans). */
const PITCHABLE = new Set(["walkup", "midrise", "rowhouse"]);

/**
 * Pitched roofs by flavor (`district.pitched`, the chance per district, set
 * by city/flavors.js): rendered blocks of old northern and European towns
 * carry gabled roofs along the street, freestanding and corner ones hips.
 */
function pitchRoof(env, archetype, lot, district, rng) {
  const pc = district.pitched ?? 0;
  if (pc <= 0 || env.roof.type !== "flat" || !PITCHABLE.has(archetype) || env.floors > 7) return;
  if (env.tiers.length !== 1 || env.tiers[0].rects.length !== 1 || !rng.chance(pc)) return;
  env.roof = lot.corner || lot.micro ? { type: "hip", slope: rng.float(0.6, 0.85), overhang: 2 } : { type: "gable", ridge: "u", slope: rng.float(0.65, 0.95), overhang: { F: 3, B: 3, L: 0, R: 0 } };
}

/** Envelope with a forced archetype + style (sites, special plans). */
export function planBuildingEnvelopeAs(lot, archetypeId, styleId, district, rng, extra) {
  const lotFrame = lotFrameOf(lot);
  const arch = ARCHETYPES.get(archetypeId);
  if (!arch.fits(lotFrame.U, lotFrame.V)) return null;
  const env = arch.envelope({ lot, frame: lotFrame, district, rng, ...extra });
  if (!env) return null;
  return finalizeEnvelope(lot, lotFrame, archetypeId, styleId, env, extra);
}

/** Convert lot-canonical footprints to world + a tight building frame. */
function finalizeEnvelope(lot, lotFrame, archetype, styleId, env, extra) {
  if (lotFrame.turned) return finalizeTurned(lot, lotFrame, archetype, styleId, env, extra);
  const worldRects = [];
  for (const t of env.tiers) for (const r of t.rects) worldRects.push(lotFrame.rectToWorld(r));
  let bx0 = Infinity;
  let by0 = Infinity;
  let bx1 = -Infinity;
  let by1 = -Infinity;
  for (const r of worldRects) {
    bx0 = Math.min(bx0, r.x0);
    by0 = Math.min(by0, r.y0);
    bx1 = Math.max(bx1, r.x1);
    by1 = Math.max(by1, r.y1);
  }
  const R = { x0: bx0, y0: by0, x1: bx1, y1: by1 };
  const frame = new Frame(R, lot.front);
  const tiers = env.tiers.map((t) => ({
    f0: t.f0,
    f1: t.f1,
    rects: t.rects.map((r) => frame.rectFromWorld(lotFrame.rectToWorld(r))),
  }));
  const annexes = (env.annexes ?? []).map((a) => ({ ...a, world: lotFrame.rectToWorld(a.rect) }));
  return finalizeWith(lot, archetype, styleId, env, extra, frame, R, tiers, annexes);
}

/** The envelope record from its building frame, world box, canonical tiers and annexes. */
function finalizeWith(lot, archetype, styleId, env, extra, frame, R, tiers, annexes) {
  const heightAbove = env.storyH.reduce((s, h) => s + h, 0);
  const baseZ = extra.groundZ + (env.stoop ? 4 : 1);
  let roofExtra = env.roof.type === "flat" ? vx(4.5) : roofRise(env.roof, R, frame);
  if (env.steeple) roofExtra = Math.max(roofExtra, env.steeple.shaft + env.steeple.spire + (env.steeple.dome ? 34 : 24));
  const margin = vx(2);
  const bounds = { x0: R.x0 - margin, y0: R.y0 - margin, x1: R.x1 + margin, y1: R.y1 + margin };
  for (const a of annexes) {
    bounds.x0 = Math.min(bounds.x0, a.world.x0 - margin);
    bounds.y0 = Math.min(bounds.y0, a.world.y0 - margin);
    bounds.x1 = Math.max(bounds.x1, a.world.x1 + margin);
    bounds.y1 = Math.max(bounds.y1, a.world.y1 + margin);
  }
  const basementH = vx(3.25);
  // (canonical lot position: a wrapping world mirrors the same buildings every lap)
  const W = wrapOf(extra.config);
  const mirror = (hash32(extra.config.seed, W.vi(lot.rect.x0), W.vi(lot.rect.y0), 3) & 1) === 1;
  const entranceAt = ARCHETYPES.get(archetype).entranceU;
  const entranceU = entranceAt
    ? entranceAt(env, frame.U, frame.V, mirror)
    : archetype === "house" || archetype === "rowhouse"
      ? houseColumns(frame.U, mirror).entranceU
      : Math.floor(frame.U / 2);
  return {
    mirror,
    entranceU,
    id: `${lot.id}/B`,
    lot: lot.id,
    archetype,
    style: styleId,
    district: lot.district,
    front: lot.front,
    R,
    U: frame.U,
    V: frame.V,
    tiers,
    annexes,
    floors: env.floors,
    storyH: env.storyH,
    basements: env.basements,
    basementH,
    baseZ,
    groundZ: extra.groundZ,
    roof: env.roof,
    program: env.program,
    podiumFloors: env.podiumFloors ?? 0,
    stoop: !!env.stoop,
    yard: !!env.yard,
    ...(env.plinth ? { plinth: true } : {}),
    ...(env.porch ? { porch: true } : {}),
    ...(env.steeple ? { steeple: { ...env.steeple } } : {}),
    ...(env.domes ? { domes: env.domes } : {}),
    // (civic buildings: sign, portico, shop front, car park; see civic.js)
    ...(env.extra ?? {}),
    topZ: baseZ + heightAbove + roofExtra,
    bottomZ: baseZ - env.basements * basementH - 2,
    bounds,
  };
}

/**
 * A turned envelope (the angled world, ANGLED_WORLD_PLAN.md S3): the lot
 * and the building share the lot's placement, so the footprints stay
 * canonical, shifted to the building's own corner; `env.turn` records the
 * building frame (buildings/frame.js frameOf), `env.R` is the world box of
 * the turned footprint and annexes keep a canonical rect (`canon`) beside
 * their world box.
 */
function finalizeTurned(lot, lotFrame, archetype, styleId, env, extra) {
  let bu0 = Infinity;
  let bv0 = Infinity;
  let bu1 = -Infinity;
  let bv1 = -Infinity;
  for (const t of env.tiers)
    for (const r of t.rects) {
      bu0 = Math.min(bu0, r.x0);
      bv0 = Math.min(bv0, r.y0);
      bu1 = Math.max(bu1, r.x1);
      bv1 = Math.max(bv1, r.y1);
    }
  const shift = (r) => ({ ...r, x0: r.x0 - bu0, y0: r.y0 - bv0, x1: r.x1 - bu0, y1: r.y1 - bv0 });
  const frame = lotFrame.shifted(bu0, bv0, bu1 - bu0 + 1, bv1 - bv0 + 1);
  const R = frame.rectToWorld({ x0: 0, y0: 0, x1: frame.U - 1, y1: frame.V - 1 });
  const tiers = env.tiers.map((t) => ({ f0: t.f0, f1: t.f1, rects: t.rects.map(shift) }));
  const annexes = (env.annexes ?? []).map((a) => {
    const canon = shift(a.rect);
    return { ...a, canon, world: frame.rectToWorld(canon) };
  });
  const out = finalizeWith(lot, archetype, styleId, env, extra, frame, R, tiers, annexes);
  out.turn = frame.turn;
  return out;
}

/**
 * Rise (voxels) of a pitched roof above the top floor: exactly the highest
 * point massing.pitchedRoof draws (slope times the distance from the eaves,
 * over the building's span plus the overhangs), plus a little margin. The
 * building's rect bounds its top tier, so this is an upper bound for every
 * roof type (gable either way, hip, sawtooth).
 */
function roofRise(roof, R, frame) {
  const slope = roof.slope ?? 0.7;
  const o = roof.overhang ?? 3;
  const oh = typeof o === "number" ? { F: o, B: o, L: o, R: o } : { F: o.F ?? 3, B: o.B ?? 3, L: o.L ?? 3, R: o.R ?? 3 };
  const du = Math.floor((frame.U - 1 + oh.L + oh.R) / 2);
  const dv = Math.floor((frame.V - 1 + oh.F + oh.B) / 2);
  const d = roof.type === "hip" ? Math.min(du, dv) : roof.type === "sawtooth" ? Math.min(47, frame.V - 1 + oh.F + oh.B) / 3 : roof.ridge === "v" ? du : dv;
  return Math.floor(d * slope) + 1 + 4;
}

export function envelopeFrame(env) {
  return frameOf(env);
}

/** Footprint rects (canonical) of floor f; basements use the ground tier. */
export function tierRects(env, f) {
  const ff = Math.max(0, f);
  for (const t of env.tiers) if (ff >= t.f0 && ff <= t.f1) return t.rects;
  return [];
}

/** Z of the bottom of floor f's slab (negative f = basements). */
export function floorZ(env, f) {
  if (f < 0) return env.baseZ + f * env.basementH;
  let z = env.baseZ;
  for (let k = 0; k < f; k += 1) z += env.storyH[k];
  return z;
}

export function floorHeight(env, f) {
  return f < 0 ? env.basementH : env.storyH[f];
}

export { STYLES };
