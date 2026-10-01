import { MAT } from "../voxel/materials.js";
import { carBoxes } from "../buildings/interior/prefabs.js";
import { INDUSTRY_PROPS } from "./industry.js";

/**
 * Street prop prefabs. Local frame: a runs along the street, b points from
 * the prop towards the carriageway, z is height above the ground surface
 * voxel (0 = first voxel above ground).
 */
const B = (a0, a1, b0, b1, z0, z1, m) => ({ a0, a1, b0, b1, z0, z1, m });

export const PROPS = {
  streetlight(rng, o = {}) {
    const h = o.h ?? 52;
    const reach = o.reach ?? 12;
    const pole = o.pole ?? MAT.POLE_METAL;
    return [
      B(-1, 1, -1, 1, 0, 1, MAT.CONCRETE_DARK),
      B(0, 0, 0, 0, 2, h, pole),
      B(0, 0, 1, reach, h, h, pole),
      B(-1, 1, reach - 3, reach, h - 1, h - 1, MAT.LAMP_LIGHT),
      B(-1, 1, reach - 3, reach, h + 1, h + 1, pole),
    ];
  },
  parkLamp() {
    return [B(0, 0, 0, 0, 0, 26, MAT.POLE_GREEN), B(-1, 1, -1, 1, 27, 29, MAT.LAMP_LIGHT), B(-1, 1, -1, 1, 30, 30, MAT.POLE_GREEN)];
  },
  signal(rng, o = {}) {
    const reach = o.reach ?? 28;
    const out = [B(-1, 1, -1, 1, 0, 1, MAT.CONCRETE_DARK), B(0, 0, 0, 0, 2, 44, MAT.SIGNAL_BOX), B(0, 0, 1, reach, 44, 44, MAT.SIGNAL_BOX)];
    for (const b of [Math.round(reach * 0.55), reach]) {
      out.push(B(-1, 1, b - 1, b, 36, 43, MAT.SIGNAL_BOX));
      out.push(B(-1, 1, b - 1, b - 1, 41, 42, rng.chance(0.5) ? MAT.SIGNAL_RED : MAT.SIGNAL_BOX));
      out.push(B(-1, 1, b - 1, b - 1, 37, 38, rng.chance(0.5) ? MAT.SIGNAL_GREEN : MAT.SIGNAL_BOX));
    }
    // pedestrian signal + push button
    out.push(B(0, 0, -1, -1, 18, 21, MAT.SIGNAL_BOX), B(0, 0, -1, -1, 19, 19, MAT.SIGNAL_AMBER));
    return out;
  },
  delineator() {
    // white roadside post with a reflector facing the traffic
    return [B(0, 0, 0, 0, 0, 7, MAT.PLASTIC_WHITE), B(0, 0, 0, 0, 5, 6, MAT.SIGNAL_AMBER), B(0, 0, 0, 0, 7, 7, MAT.METAL_BLACK)];
  },
  hydrant() {
    return [B(0, 1, 0, 1, 0, 4, MAT.HYDRANT), B(-1, 2, 0, 1, 3, 3, MAT.HYDRANT), B(0, 1, 0, 1, 5, 5, MAT.METAL_BLACK)];
  },
  bench() {
    return [
      B(0, 11, 0, 3, 3, 3, MAT.WOOD_MED),
      B(0, 11, 0, 0, 4, 6, MAT.WOOD_MED),
      B(1, 1, 0, 3, 0, 2, MAT.METAL_BLACK),
      B(10, 10, 0, 3, 0, 2, MAT.METAL_BLACK),
    ];
  },
  bin() {
    return [B(0, 2, 0, 2, 0, 6, MAT.BIN_GREEN), B(0, 2, 0, 2, 7, 7, MAT.METAL_BLACK)];
  },
  /** a headstone, now and then a wooden grave cross (churchyards) */
  gravestone(rng) {
    if (rng.chance(0.3)) return [B(1, 1, 0, 0, 0, 7, MAT.WOOD_DARK), B(0, 2, 0, 0, 5, 5, MAT.WOOD_DARK)];
    const m = rng.pick([MAT.GRANITE, MAT.ROCK_DARK, MAT.STONE]);
    const h = rng.int(3, 6);
    return [B(0, 3, 0, 0, 0, h, m), B(0, 3, -1, 1, 0, 0, m)];
  },
  mailbox() {
    return [B(0, 3, 0, 2, 0, 1, MAT.METAL_BLACK), B(0, 3, 0, 2, 2, 8, MAT.SIGN_BLUE)];
  },
  signPost(rng) {
    return [B(0, 0, 0, 0, 0, 22, MAT.POLE_METAL), B(-3, 3, 0, 0, 20, 21, MAT.SIGN_GREEN), B(0, 0, -3, 3, 22, 23, MAT.SIGN_GREEN), B(-2, 2, 1, 1, 14, 17, rng.chance(0.5) ? MAT.SIGN_RED : MAT.SIGN_WHITE)];
  },
  busShelter() {
    return [
      B(0, 23, -8, -8, 0, 20, MAT.GLASS),
      B(0, 0, -8, -1, 0, 20, MAT.GLASS),
      B(23, 23, -8, -1, 0, 20, MAT.GLASS),
      B(-1, 24, -9, 1, 21, 21, MAT.METAL_PANEL_DARK),
      B(2, 21, -7, -5, 3, 3, MAT.WOOD_MED),
      B(26, 26, 0, 0, 0, 24, MAT.POLE_METAL),
      B(25, 27, 0, 0, 20, 24, MAT.SIGN_BLUE),
    ];
  },
  bollard() {
    return [B(0, 0, 0, 0, 0, 5, MAT.BOLLARD)];
  },
  planter() {
    return [B(0, 7, 0, 3, 0, 3, MAT.CONCRETE_LIGHT), B(1, 6, 1, 2, 3, 4, MAT.BUSH), B(2, 5, 1, 2, 5, 5, MAT.FLOWER_RED)];
  },
  // parked car: a = along the street (length), b = across
  car(rng) {
    return carBoxes(rng).map((q) => ({ a0: q.b0, a1: q.b1, b0: q.a0, b1: q.a1, z0: q.z0, z1: q.z1, m: q.m }));
  },
  fountain() {
    const out = [];
    for (let r = 0; r <= 20; r += 1) {
      const w = Math.round(Math.sqrt(Math.max(0, 400 - r * r)));
      out.push(B(-w, w, r, r, 0, 2, MAT.PLAZA_STONE_DARK), B(-w, w, -r, -r, 0, 2, MAT.PLAZA_STONE_DARK));
    }
    for (let r = 0; r <= 18; r += 1) {
      const w = Math.round(Math.sqrt(Math.max(0, 324 - r * r)));
      out.push(B(-w, w, r, r, 1, 2, MAT.WATER), B(-w, w, -r, -r, 1, 2, MAT.WATER));
    }
    out.push(B(-2, 2, -2, 2, 0, 10, MAT.LIMESTONE), B(-4, 4, -4, 4, 10, 11, MAT.LIMESTONE), B(-1, 1, -1, 1, 12, 16, MAT.WATER));
    return out;
  },
  playground(rng) {
    const out = [B(0, 47, 0, 39, -1, -1, MAT.RUBBER_MAT)];
    // swing set
    out.push(B(4, 4, 4, 4, 0, 18, MAT.PLAY_RED), B(20, 20, 4, 4, 0, 18, MAT.PLAY_RED), B(4, 20, 4, 4, 18, 18, MAT.PLAY_RED));
    out.push(B(8, 10, 3, 5, 5, 5, MAT.PLAY_BLUE), B(14, 16, 3, 5, 5, 5, MAT.PLAY_BLUE));
    // slide tower
    out.push(B(28, 36, 20, 28, 0, 1, MAT.PLAY_YELLOW), B(28, 36, 20, 28, 12, 12, MAT.WOOD_MED));
    for (const [a, b] of [[28, 20], [36, 20], [28, 28], [36, 28]]) out.push(B(a, a, b, b, 0, 20, MAT.PLAY_BLUE));
    out.push(B(28, 36, 20, 28, 20, 21, MAT.PLAY_RED));
    for (let k = 0; k < 12; k += 1) out.push(B(30, 34, 29 + k, 29 + k, 11 - k, 11 - k, MAT.PLAY_YELLOW));
    // sandbox
    out.push(B(4, 16, 22, 34, 0, 0, MAT.WOOD_LIGHT), B(5, 15, 23, 33, -1, 0, MAT.SAND_BOX));
    void rng;
    return out;
  },
  /** an old-town lantern: a black iron post with a four-sided lamp (a runs along the street) */
  oldLamp() {
    return [
      B(-1, 1, -1, 1, 0, 1, MAT.GRANITE),
      B(0, 0, 0, 0, 2, 28, MAT.METAL_BLACK),
      B(-1, 1, -1, 1, 22, 22, MAT.METAL_BLACK),
      B(-1, 1, -1, 1, 29, 29, MAT.METAL_BLACK),
      B(-1, 1, -1, 1, 30, 32, MAT.LAMP_LIGHT),
      B(-2, 2, -2, 2, 33, 33, MAT.METAL_BLACK),
      B(-1, 1, -1, 1, 34, 34, MAT.METAL_BLACK),
    ];
  },
  /** a statue on a granite plinth in the middle of a square */
  monument(rng) {
    const out = [B(-10, 10, -10, 10, 0, 1, MAT.GRANITE_LIGHT), B(-7, 7, -7, 7, 2, 3, MAT.GRANITE), B(-4, 4, -4, 4, 4, 16, MAT.GRANITE_LIGHT), B(-5, 5, -5, 5, 17, 17, MAT.GRANITE)];
    // the figure: a dark bronze body, head and a raised arm (or an obelisk)
    if (rng.chance(0.3)) {
      for (let k = 0; k < 20; k += 1) {
        const w = Math.max(0, 3 - Math.floor(k / 7));
        out.push(B(-w, w, -w, w, 18 + k, 18 + k, MAT.GRANITE));
      }
      return out;
    }
    const m = MAT.METAL_PANEL_DARK;
    out.push(B(-2, 2, -1, 1, 18, 20, m), B(-2, 2, -2, 2, 21, 29, m), B(-1, 1, -1, 1, 30, 32, m), B(3, 3, 0, 0, 26, 34, m), B(-3, -3, 0, 0, 22, 27, m));
    return out;
  },
  /** a market stall: a trestle table with crates under a striped canvas roof */
  marketStall(rng) {
    const cloth = rng.pick([MAT.AWNING_RED, MAT.AWNING_GREEN, MAT.AWNING_BLUE, MAT.AWNING_STRIPE]);
    const out = [];
    for (const [a, b] of [[0, 0], [23, 0], [0, 15], [23, 15]]) out.push(B(a, a, b, b, 0, 17, MAT.WOOD_MED));
    out.push(B(1, 22, 2, 9, 6, 6, MAT.WOOD_LIGHT));
    for (let a = -1; a <= 24; a += 1) out.push(B(a, a, -1, 16, 18, 18, (a >> 1) & 1 ? cloth : MAT.AWNING_STRIPE));
    out.push(B(3, 7, 3, 7, 7, 8, rng.pick([MAT.GOODS, MAT.CARDBOARD, MAT.FLOWER_YELLOW, MAT.FLOWER_RED])));
    out.push(B(10, 14, 3, 7, 7, 8, rng.pick([MAT.GOODS, MAT.LEAVES_LIGHT, MAT.FLOWER_PURPLE])));
    out.push(B(16, 20, 3, 7, 7, 7, MAT.PALLET));
    return out;
  },
  /** an allotment hut (kolonihage / dacha shed): boards, a door, a window, a pitched roof */
  allotmentHut(rng) {
    const wall = rng.pick([MAT.CLAD_FALU, MAT.CLAD_GREEN, MAT.CLAD_OCHRE, MAT.CLAD_WHITE, MAT.CLAD_GREYBLUE, MAT.WOOD_WEATHERED]);
    const roof = rng.pick([MAT.ROOF_BLACK_METAL, MAT.CORRUGATED, MAT.CORRUGATED_RUST, MAT.ROOF_SHINGLE]);
    const out = [B(0, 23, 0, 19, 0, 0, MAT.WOOD_WEATHERED), B(0, 23, 0, 19, 1, 17, wall), B(9, 14, 20, 20, 1, 15, MAT.DOOR_WOOD), B(3, 6, 20, 20, 7, 11, MAT.GLASS), B(17, 20, 20, 20, 7, 11, MAT.GLASS)];
    // pitched roof along a, ridge in the middle of b
    for (let k = 0; k <= 11; k += 1) out.push(B(-1, 24, -1 + k, 20 - k, 18 + k, 18 + k, roof));
    return out;
  },
  /** a low picket fence post pair (used in runs) */
  picket() {
    return [B(0, 0, 0, 0, 0, 6, MAT.FENCE_WHITE)];
  },
  /** a pile of rubble: broken concrete, bricks and a rusty drum */
  rubble(rng) {
    const out = [];
    const n = rng.int(4, 8);
    for (let k = 0; k < n; k += 1) {
      const a = rng.int(-10, 8);
      const b = rng.int(-10, 8);
      const h = rng.int(1, 5);
      out.push(B(a, a + rng.int(2, 6), b, b + rng.int(2, 6), 0, h, rng.pick([MAT.CONCRETE_DARK, MAT.CONCRETE, MAT.BRICK_RED, MAT.GRAVEL, MAT.CINDERBLOCK])));
    }
    if (rng.chance(0.5)) out.push(B(10, 13, -3, 0, 0, 7, MAT.STEEL_RUST));
    return out;
  },
  /** a woodpile under a lean-to roof against a back fence */
  woodpile() {
    return [B(0, 23, 0, 6, 0, 10, MAT.WOOD_MED), B(0, 23, 0, 0, 11, 13, MAT.WOOD_DARK), B(-1, 24, -1, 8, 14, 14, MAT.CORRUGATED_RUST)];
  },
  /** a boathouse (naust): a steep-roofed shed of tarred or red boards, its gable doors to the water (+b) */
  boathouse(rng) {
    const wall = rng.pick([MAT.CLAD_FALU, MAT.CLAD_FALU, MAT.LOG_TARRED, MAT.WOOD_WEATHERED, MAT.CLAD_WHITE]);
    const roof = rng.pick([MAT.ROOF_BLACK_TILE, MAT.ROOF_SOD, MAT.ROOF_BLACK_METAL, MAT.ROOF_SLATE]);
    const out = [B(-18, 18, 0, 63, -16, 0, MAT.STONE), B(-18, 18, 0, 63, 1, 22, wall), B(-10, 10, 63, 63, 1, 18, MAT.WOOD_DARK), B(-17, 17, 1, 62, 1, 1, MAT.WOOD_WEATHERED)];
    for (let k = 0; k <= 20; k += 1) out.push(B(-20 + k, 20 - k, -1, 64, 23 + k, 23 + k, k === 20 ? MAT.WOOD_DARK : roof));
    // gable wall triangle at both ends
    for (let k = 0; k < 19; k += 1) out.push(B(-18 + k, 18 - k, 0, 0, 23 + k, 23 + k, wall), B(-18 + k, 18 - k, 63, 63, 23 + k, 23 + k, wall));
    return out;
  },
  /** a lighthouse: a white round tower with red bands, a gallery and a lantern */
  lighthouse() {
    const out = [];
    for (let z = 0; z < 120; z += 1) {
      const r = 14 - Math.floor(z / 22);
      const band = Math.floor(z / 14) % 3 === 1;
      for (let a = -r; a <= r; a += 1) {
        const w = Math.round(Math.sqrt(r * r - a * a));
        out.push(B(a, a, -w, w, z, z, band ? MAT.PAINT_RED : MAT.PLASTER_WHITE));
      }
    }
    out.push(B(-12, 12, -12, 12, 120, 121, MAT.METAL_BLACK));
    for (const [a, b] of [[-12, -12], [12, -12], [-12, 12], [12, 12]]) out.push(B(a, a, b, b, 122, 128, MAT.RAILING));
    out.push(B(-6, 6, -6, 6, 122, 134, MAT.LAMP_LIGHT), B(-7, 7, -7, 7, 135, 137, MAT.PAINT_RED), B(-3, 3, -3, 3, 138, 142, MAT.PAINT_RED), B(0, 0, 0, 0, 143, 150, MAT.METAL_BLACK));
    return out;
  },
  /** a cairn (varde) on a fell top: a stack of flat stones */
  cairn(rng) {
    const out = [];
    const h = rng.int(10, 16);
    for (let z = 0; z < h; z += 1) {
      const r = Math.max(1, Math.round(5 - (z * 4) / h));
      out.push(B(-r, r, -r, r, z, z, z % 3 === 0 ? MAT.ROCK_DARK : MAT.GRANITE));
    }
    return out;
  },
  /** a fish-drying rack (hjell): posts and rails, stockfish hanging in rows */
  fishRack(rng) {
    const out = [];
    const L = rng.int(48, 96);
    for (let a = 0; a <= L; a += 16) out.push(B(a, a, -6, -6, 0, 26, MAT.WOOD_WEATHERED), B(a, a, 6, 6, 0, 26, MAT.WOOD_WEATHERED), B(a, a, -6, 6, 26, 26, MAT.WOOD_WEATHERED));
    out.push(B(0, L, 0, 0, 24, 24, MAT.WOOD_WEATHERED));
    for (let a = 1; a < L; a += 2) if (rng.chance(0.8)) out.push(B(a, a, -1, 1, 16, 23, MAT.WOOD_LIGHT));
    return out;
  },
};

Object.assign(PROPS, INDUSTRY_PROPS);
