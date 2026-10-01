import { MAT } from "../../voxel/materials.js";

/**
 * Furniture & fixture prefabs at 12.5 cm resolution.
 *
 * Local frame: a runs along the wall (0..w-1), b runs away from the wall
 * into the room (0..d-1), z is height above the finished floor (0 = first
 * air voxel). `build(rng, opt)` returns boxes {a0,a1,b0,b1,z0,z1,m}; a box
 * with m = 0 carves. Heights: seat ≈ z3 (0.45 m), table top z5 (0.75 m),
 * counter top z6 (0.9 m).
 */

const B = (a0, a1, b0, b1, z0, z1, m) => ({ a0, a1, b0, b1, z0, z1, m });

const FABRICS = [MAT.FABRIC_GRAY, MAT.FABRIC_BLUE, MAT.FABRIC_RED, MAT.FABRIC_GREEN, MAT.FABRIC_BEIGE, MAT.FABRIC_MUSTARD, MAT.LEATHER];
const WOODS = [MAT.WOOD_LIGHT, MAT.WOOD_MED, MAT.WOOD_DARK, MAT.LAMINATE_WHITE];
const SHEETS = [MAT.BEDSHEET_BLUE, MAT.BEDSHEET_GREEN, MAT.BEDSHEET_WHITE, MAT.FABRIC_BEIGE, MAT.FABRIC_RED];

export const PREFABS = {
  bedDouble: {
    w: 13,
    d: 17,
    build(rng) {
      const wood = rng.pick(WOODS);
      const sheet = rng.pick(SHEETS);
      return [
        B(0, 12, 0, 16, 0, 1, wood),
        B(0, 12, 0, 0, 0, 6, wood),
        B(0, 12, 1, 16, 2, 2, MAT.MATTRESS),
        B(0, 12, 5, 16, 3, 3, sheet),
        B(1, 5, 1, 3, 3, 3, MAT.PILLOW),
        B(7, 11, 1, 3, 3, 3, MAT.PILLOW),
      ];
    },
  },
  bedSingle: {
    w: 8,
    d: 16,
    build(rng) {
      const wood = rng.pick(WOODS);
      return [
        B(0, 7, 0, 15, 0, 1, wood),
        B(0, 7, 0, 0, 0, 5, wood),
        B(0, 7, 1, 15, 2, 2, MAT.MATTRESS),
        B(0, 7, 5, 15, 3, 3, rng.pick(SHEETS)),
        B(1, 6, 1, 3, 3, 3, MAT.PILLOW),
      ];
    },
  },
  nightstand: {
    w: 4,
    d: 3,
    build(rng) {
      const wood = rng.pick(WOODS);
      return [B(0, 3, 0, 2, 0, 3, wood), B(1, 2, 1, 1, 4, 5, MAT.LAMP_SHADE)];
    },
  },
  wardrobe: {
    w: 10,
    d: 5,
    tall: true,
    build(rng) {
      const wood = rng.pick(WOODS);
      return [B(0, 9, 0, 4, 0, 15, wood), B(4, 5, 4, 4, 7, 9, MAT.METAL_CHROME)];
    },
  },
  dresser: {
    w: 8,
    d: 4,
    build(rng) {
      const wood = rng.pick(WOODS);
      return [B(0, 7, 0, 3, 0, 5, wood), B(1, 6, 0, 0, 8, 12, MAT.MIRROR)];
    },
  },
  desk: {
    w: 10,
    d: 5,
    build(rng, opt = {}) {
      const wood = rng.pick(WOODS);
      const out = [B(0, 9, 0, 4, 5, 5, wood), B(0, 0, 0, 4, 0, 4, wood), B(9, 9, 0, 4, 0, 4, wood)];
      if (opt.monitor !== false) out.push(B(3, 6, 1, 1, 6, 9, MAT.SCREEN), B(4, 5, 2, 2, 6, 6, MAT.PLASTIC_BLACK));
      // chair
      out.push(B(3, 6, 6, 9, 3, 3, MAT.FABRIC_GRAY), B(3, 6, 9, 9, 4, 7, MAT.FABRIC_GRAY), B(4, 5, 7, 8, 0, 2, MAT.METAL_BLACK));
      return out;
    },
    dExtra: 5,
  },
  sofa: {
    w: 16,
    d: 7,
    build(rng) {
      const f = rng.pick(FABRICS);
      return [
        B(0, 15, 0, 6, 0, 2, f),
        B(0, 15, 0, 1, 3, 6, f),
        B(0, 0, 2, 6, 3, 4, f),
        B(15, 15, 2, 6, 3, 4, f),
        B(1, 14, 2, 6, 3, 3, f),
      ];
    },
  },
  armchair: {
    w: 7,
    d: 7,
    build(rng) {
      const f = rng.pick(FABRICS);
      return [B(0, 6, 0, 6, 0, 2, f), B(0, 6, 0, 1, 3, 6, f), B(0, 0, 2, 6, 3, 4, f), B(6, 6, 2, 6, 3, 4, f)];
    },
  },
  tvUnit: {
    w: 12,
    d: 3,
    build(rng) {
      const wood = rng.pick(WOODS);
      return [B(0, 11, 0, 2, 0, 3, wood), B(1, 10, 1, 1, 4, 9, MAT.SCREEN_OFF), B(5, 6, 1, 1, 4, 4, MAT.PLASTIC_BLACK)];
    },
  },
  bookshelf: {
    w: 7,
    d: 3,
    tall: true,
    build(rng) {
      const wood = rng.pick(WOODS);
      const out = [B(0, 6, 0, 2, 0, 15, wood)];
      for (let z = 1; z <= 13; z += 3) out.push(B(1, 5, 1, 2, z, z + 1, MAT.BOOKS));
      return out;
    },
  },
  plant: {
    w: 3,
    d: 3,
    build() {
      return [B(0, 2, 0, 2, 0, 2, MAT.PLANT_POT), B(0, 2, 0, 2, 3, 7, MAT.PLANT), B(1, 1, 1, 1, 8, 9, MAT.PLANT)];
    },
  },
  floorLamp: {
    w: 2,
    d: 2,
    build() {
      return [B(0, 1, 0, 1, 0, 0, MAT.METAL_BLACK), B(0, 0, 0, 0, 1, 11, MAT.METAL_BLACK), B(0, 1, 0, 1, 12, 13, MAT.LAMP_SHADE)];
    },
  },
  counterRun: {
    // variable width: counters with sink + stove, upper cabinets
    w: 0,
    d: 5,
    build(rng, opt) {
      const w = opt.w;
      const cab = rng.pick([MAT.LAMINATE_WHITE, MAT.WOOD_LIGHT, MAT.LAMINATE_GRAY, MAT.WOOD_DARK]);
      const top = rng.pick([MAT.COUNTERTOP, MAT.COUNTERTOP_DARK]);
      const out = [B(0, w - 1, 0, 4, 0, 5, cab), B(0, w - 1, 0, 4, 6, 6, top)];
      if (w >= 12) {
        const s = Math.floor(w * 0.3);
        out.push(B(s, s + 3, 1, 3, 6, 6, 0), B(s, s + 3, 1, 3, 5, 5, MAT.METAL_CHROME), B(s + 1, s + 2, 0, 0, 7, 8, MAT.METAL_CHROME));
        const k = Math.floor(w * 0.65);
        out.push(B(k, k + 3, 0, 4, 0, 5, MAT.APPLIANCE_STEEL), B(k, k + 3, 1, 3, 6, 6, MAT.METAL_BLACK));
        if (opt.hood !== false) out.push(B(k, k + 3, 0, 2, 13, 15, MAT.APPLIANCE_STEEL));
      }
      if (opt.upper !== false) out.push(B(0, w - 1, 0, 2, 11, 15, cab));
      return out;
    },
  },
  fridge: {
    w: 5,
    d: 5,
    tall: true,
    build() {
      return [B(0, 4, 0, 4, 0, 14, MAT.APPLIANCE_STEEL), B(4, 4, 4, 4, 6, 10, MAT.METAL_CHROME)];
    },
  },
  diningTable: {
    w: 10,
    d: 7,
    free: true,
    build(rng) {
      const wood = rng.pick(WOODS);
      const chair = rng.pick([MAT.WOOD_DARK, MAT.WOOD_MED, MAT.PLASTIC_WHITE, MAT.FABRIC_GRAY]);
      const out = [B(0, 9, 0, 6, 5, 5, wood), B(1, 1, 1, 1, 0, 4, wood), B(8, 8, 1, 1, 0, 4, wood), B(1, 1, 5, 5, 0, 4, wood), B(8, 8, 5, 5, 0, 4, wood)];
      for (const a of [1, 6]) {
        out.push(B(a, a + 2, -3, -1, 3, 3, chair), B(a, a + 2, -3, -3, 4, 7, chair));
        out.push(B(a, a + 2, 7, 9, 3, 3, chair), B(a, a + 2, 9, 9, 4, 7, chair));
      }
      return out;
    },
    pad: 3,
  },
  coffeeTable: {
    w: 8,
    d: 5,
    free: true,
    build(rng) {
      const wood = rng.pick(WOODS);
      return [B(0, 7, 0, 4, 2, 2, wood), B(0, 0, 0, 0, 0, 1, wood), B(7, 7, 0, 0, 0, 1, wood), B(0, 0, 4, 4, 0, 1, wood), B(7, 7, 4, 4, 0, 1, wood)];
    },
  },
  rug: {
    w: 14,
    d: 10,
    free: true,
    flat: true,
    build(rng) {
      return [B(0, 13, 0, 9, -1, -1, rng.pick([MAT.RUG_RED, MAT.RUG_BLUE, MAT.RUG_BEIGE]))];
    },
  },
  toilet: {
    w: 4,
    d: 6,
    build() {
      return [B(0, 3, 0, 1, 2, 6, MAT.CERAMIC), B(1, 2, 2, 5, 0, 2, MAT.CERAMIC), B(1, 2, 3, 4, 2, 2, 0)];
    },
  },
  basin: {
    w: 5,
    d: 4,
    build() {
      return [B(0, 4, 0, 3, 5, 6, MAT.CERAMIC), B(1, 3, 1, 2, 6, 6, 0), B(2, 2, 1, 1, 0, 4, MAT.CERAMIC), B(1, 3, 0, 0, 8, 12, MAT.MIRROR), B(2, 2, 0, 0, 7, 7, MAT.METAL_CHROME)];
    },
  },
  bathtub: {
    w: 14,
    d: 6,
    build() {
      return [B(0, 13, 0, 5, 0, 3, MAT.CERAMIC), B(1, 12, 1, 4, 1, 3, 0), B(1, 12, 1, 4, 1, 1, MAT.WATER), B(12, 12, 0, 0, 4, 9, MAT.METAL_CHROME)];
    },
  },
  shower: {
    w: 7,
    d: 7,
    build() {
      return [B(0, 6, 0, 6, 0, 0, MAT.CERAMIC), B(0, 6, 6, 6, 1, 15, MAT.GLASS), B(6, 6, 0, 6, 1, 15, MAT.GLASS), B(3, 3, 0, 0, 13, 14, MAT.METAL_CHROME)];
    },
  },
  washer: {
    w: 5,
    d: 5,
    build() {
      return [B(0, 4, 0, 4, 0, 6, MAT.PLASTIC_WHITE), B(1, 3, 4, 4, 2, 4, MAT.GLASS_TINT)];
    },
  },
  shelfUnit: {
    w: 8,
    d: 3,
    tall: true,
    build(rng, opt = {}) {
      const frame = opt.metal ? MAT.SHELF_METAL : rng.pick(WOODS);
      const goods = opt.goods ?? MAT.GOODS;
      const out = [B(0, 0, 0, 2, 0, 14, frame), B(7, 7, 0, 2, 0, 14, frame)];
      for (let z = 0; z <= 12; z += 4) {
        out.push(B(0, 7, 0, 2, z, z, frame));
        if (rng.chance(0.85)) out.push(B(1, 6, 0, 2, z + 1, z + 2, goods));
      }
      return out;
    },
  },
  gondola: {
    // free-standing double-sided shop shelf
    w: 16,
    d: 5,
    free: true,
    build(rng) {
      const out = [B(0, 15, 0, 4, 0, 0, MAT.SHELF_METAL), B(0, 15, 2, 2, 1, 11, MAT.SHELF_METAL)];
      for (let z = 1; z <= 9; z += 4) {
        out.push(B(0, 15, 0, 1, z, z, MAT.SHELF_METAL), B(0, 15, 3, 4, z, z, MAT.SHELF_METAL));
        out.push(B(0, 15, 0, 1, z + 1, z + 2, rng.pick([MAT.GOODS, MAT.BOOKS, MAT.CARDBOARD])));
        out.push(B(0, 15, 3, 4, z + 1, z + 2, rng.pick([MAT.GOODS, MAT.BOOKS, MAT.CARDBOARD])));
      }
      return out;
    },
    pad: 5,
  },
  checkout: {
    w: 10,
    d: 5,
    build() {
      return [B(0, 9, 0, 4, 0, 6, MAT.WOOD_DARK), B(6, 8, 1, 3, 7, 8, MAT.PLASTIC_BLACK), B(1, 4, 1, 3, 7, 7, MAT.METAL_CHROME)];
    },
  },
  /** pupil's desk with a chair behind it (local b grows towards the back of the room) */
  schoolDesk: {
    w: 6,
    d: 8,
    free: true,
    build() {
      return [
        B(0, 5, 0, 3, 5, 5, MAT.LAMINATE_WHITE),
        B(0, 0, 0, 0, 0, 4, MAT.METAL_BLACK),
        B(5, 5, 0, 0, 0, 4, MAT.METAL_BLACK),
        B(0, 0, 3, 3, 0, 4, MAT.METAL_BLACK),
        B(5, 5, 3, 3, 0, 4, MAT.METAL_BLACK),
        B(1, 4, 5, 7, 3, 3, MAT.PLASTIC_BLACK),
        B(1, 4, 7, 7, 4, 7, MAT.PLASTIC_BLACK),
      ];
    },
    pad: 1,
  },
  chalkboard: {
    w: 24,
    d: 1,
    build(rng, opt = {}) {
      const w = opt.w ?? 24;
      return [B(0, w - 1, 0, 0, 7, 16, MAT.CHALKBOARD), B(0, w - 1, 0, 0, 6, 6, MAT.WOOD_MED), B(0, w - 1, 0, 0, 17, 17, MAT.WOOD_MED)];
    },
  },
  hoop: {
    w: 6,
    d: 5,
    build() {
      return [B(0, 5, 0, 0, 20, 25, MAT.PANEL_WHITE), B(2, 3, 1, 4, 20, 20, MAT.COURT_ORANGE), B(2, 3, 0, 0, 16, 19, MAT.METAL_BLACK)];
    },
  },
  cafeTable: {
    w: 4,
    d: 4,
    free: true,
    build(rng) {
      const t = rng.pick([MAT.WOOD_LIGHT, MAT.LAMINATE_WHITE, MAT.METAL_BLACK]);
      const c = rng.pick([MAT.WOOD_DARK, MAT.METAL_BLACK, MAT.FABRIC_RED]);
      return [
        B(0, 3, 0, 3, 5, 5, t),
        B(1, 2, 1, 2, 0, 4, MAT.METAL_BLACK),
        B(-3, -1, 1, 2, 3, 3, c),
        B(-3, -3, 1, 2, 4, 6, c),
        B(4, 6, 1, 2, 3, 3, c),
        B(6, 6, 1, 2, 4, 6, c),
      ];
    },
    pad: 4,
  },
  barCounter: {
    w: 0,
    d: 8, // counter + stools
    build(rng, opt) {
      const w = opt.w;
      const out = [B(0, w - 1, 0, 4, 0, 7, MAT.WOOD_DARK), B(0, w - 1, 0, 4, 8, 8, MAT.COUNTERTOP_DARK)];
      out.push(B(1, 3, 1, 3, 9, 11, MAT.APPLIANCE_STEEL));
      for (let a = 1; a < w - 1; a += 4) out.push(B(a, a + 1, 6, 7, 4, 4, MAT.FABRIC_RED), B(a, a, 6, 6, 0, 3, MAT.METAL_CHROME));
      return out;
    },
  },
  officeDesks: {
    // cluster of 4 desks facing each other with chairs and monitors
    w: 24,
    d: 22,
    free: true,
    build(rng) {
      const top = rng.pick([MAT.LAMINATE_WHITE, MAT.WOOD_LIGHT, MAT.LAMINATE_GRAY]);
      const chair = rng.pick([MAT.FABRIC_GRAY, MAT.FABRIC_BLUE, MAT.PLASTIC_BLACK]);
      const out = [];
      for (const [a0, flip] of [
        [0, false],
        [12, false],
        [0, true],
        [12, true],
      ]) {
        const b0 = flip ? 11 : 5;
        out.push(B(a0, a0 + 11, b0, b0 + 5, 5, 5, top));
        out.push(B(a0, a0, b0, b0, 0, 4, MAT.METAL_CHROME), B(a0 + 11, a0 + 11, b0 + 5, b0 + 5, 0, 4, MAT.METAL_CHROME));
        const mb = flip ? b0 + 1 : b0 + 4;
        out.push(B(a0 + 4, a0 + 7, mb, mb, 6, 8, MAT.SCREEN));
        const cb = flip ? b0 + 6 : b0 - 4;
        out.push(B(a0 + 4, a0 + 7, cb, cb + 3, 3, 3, chair), B(a0 + 5, a0 + 6, cb + 1, cb + 2, 0, 2, MAT.METAL_BLACK));
        out.push(flip ? B(a0 + 4, a0 + 7, cb + 3, cb + 3, 4, 7, chair) : B(a0 + 4, a0 + 7, cb, cb, 4, 7, chair));
      }
      out.push(B(0, 23, 10, 10, 6, 7, MAT.PANEL_WHITE));
      return out;
    },
    pad: 2,
  },
  meetingTable: {
    w: 0,
    d: 0,
    free: true,
    pad: 5, // chairs stick out 4 cells
    build(rng, opt) {
      const w = opt.w;
      const d = opt.d;
      const top = rng.pick([MAT.WOOD_DARK, MAT.WOOD_LIGHT, MAT.LAMINATE_WHITE]);
      const chair = rng.pick([MAT.FABRIC_GRAY, MAT.LEATHER, MAT.PLASTIC_BLACK]);
      const out = [B(0, w - 1, 0, d - 1, 5, 5, top), B(2, 2, 2, d - 3, 0, 4, MAT.METAL_CHROME), B(w - 3, w - 3, 2, d - 3, 0, 4, MAT.METAL_CHROME)];
      for (let a = 1; a + 3 < w; a += 5) {
        out.push(B(a, a + 2, -4, -2, 3, 3, chair), B(a, a + 2, -4, -4, 4, 7, chair));
        out.push(B(a, a + 2, d + 1, d + 3, 3, 3, chair), B(a, a + 2, d + 3, d + 3, 4, 7, chair));
      }
      return out;
    },
  },
  receptionDesk: {
    w: 16,
    d: 6,
    free: true,
    build() {
      return [B(0, 15, 0, 1, 0, 7, MAT.WOOD_DARK), B(0, 1, 0, 5, 0, 7, MAT.WOOD_DARK), B(0, 15, 0, 1, 8, 8, MAT.COUNTERTOP), B(6, 8, 3, 3, 5, 7, MAT.SCREEN), B(6, 9, 3, 5, 3, 3, MAT.FABRIC_GRAY)];
    },
  },
  bench: {
    w: 12,
    d: 4,
    build() {
      return [B(0, 11, 0, 3, 3, 3, MAT.WOOD_MED), B(1, 1, 0, 3, 0, 2, MAT.METAL_BLACK), B(10, 10, 0, 3, 0, 2, MAT.METAL_BLACK)];
    },
  },
  mailboxes: {
    w: 10,
    d: 2,
    build() {
      return [B(0, 9, 0, 1, 4, 11, MAT.METAL_CHROME), B(0, 9, 0, 0, 3, 3, MAT.METAL_BLACK)];
    },
  },
  rack: {
    // warehouse pallet rack
    w: 22,
    d: 9,
    tall: true,
    build(rng, opt = {}) {
      const h = opt.h ?? 40;
      const out = [];
      for (const a of [0, 21]) for (const b of [0, 8]) out.push(B(a, a, b, b, 0, h, MAT.SHELF_ORANGE));
      for (let z = 0; z <= h - 10; z += 12) {
        out.push(B(0, 21, 0, 0, z + 10, z + 10, MAT.SHELF_METAL), B(0, 21, 8, 8, z + 10, z + 10, MAT.SHELF_METAL));
        for (const a of [1, 11]) {
          if (!rng.chance(0.8)) continue;
          out.push(B(a, a + 8, 1, 7, z, z, MAT.PALLET));
          const bh = rng.int(3, 8);
          out.push(B(a + 1, a + 7, 1, 7, z + 1, z + bh, rng.pick([MAT.CARDBOARD, MAT.CARDBOARD, MAT.CRATE ?? MAT.CARDBOARD, MAT.PLASTIC_WHITE])));
        }
      }
      return out;
    },
  },
  stall: {
    // a stall pen: wooden rails on three sides, straw on the floor
    w: 20,
    d: 24,
    build() {
      return [
        B(0, 19, 0, 23, 0, 0, MAT.HAY),
        B(0, 0, 0, 23, 1, 10, MAT.WOOD_DARK),
        B(19, 19, 0, 23, 1, 10, MAT.WOOD_DARK),
        B(0, 19, 23, 23, 8, 10, MAT.WOOD_DARK),
        B(0, 19, 23, 23, 3, 4, MAT.WOOD_DARK),
        B(0, 0, 23, 23, 1, 10, MAT.WOOD_DARK),
        B(19, 19, 23, 23, 1, 10, MAT.WOOD_DARK),
        B(4, 10, 1, 3, 1, 4, MAT.WOOD_MED),
      ];
    },
  },
  hayStack: {
    // square bales stacked two to three high
    w: 12,
    d: 9,
    build(rng) {
      const out = [];
      const n = rng.int(2, 3);
      for (let k = 0; k < n; k += 1) out.push(B(0, 11, 0, 8, k * 5, k * 5 + 4, MAT.HAY));
      if (rng.chance(0.5)) out.push(B(2, 9, 1, 7, n * 5, n * 5 + 4, MAT.HAY));
      return out;
    },
  },
  crates: {
    w: 8,
    d: 8,
    free: true,
    build(rng) {
      const out = [B(0, 7, 0, 7, 0, 0, MAT.PALLET), B(0, 7, 0, 7, 1, rng.int(4, 9), MAT.CARDBOARD)];
      if (rng.chance(0.5)) out.push(B(1, 5, 1, 5, 10, 13, MAT.CARDBOARD));
      return out;
    },
    pad: 3,
  },
  machine: {
    w: 16,
    d: 12,
    free: true,
    build(rng) {
      const c = rng.pick([MAT.CONTAINER_BLUE, MAT.CONTAINER_GREEN, MAT.HAZARD_YELLOW, MAT.METAL_PANEL]);
      return [
        B(0, 15, 0, 11, 0, 9, c),
        B(2, 13, 2, 9, 10, 12, MAT.METAL_PANEL_DARK),
        B(3, 5, -1, -1, 4, 7, MAT.SCREEN),
        B(12, 13, 4, 5, 13, 22, MAT.PIPE),
        B(-8, -1, 4, 7, 4, 4, MAT.METAL_BLACK),
      ];
    },
    pad: 6,
  },
  car: {
    w: 15,
    d: 34,
    free: true,
    build(rng) {
      return carBoxes(rng);
    },
    pad: 2,
  },
  stallToilet: {
    w: 9,
    d: 11,
    build() {
      return [
        B(0, 0, 0, 10, 0, 14, MAT.LAMINATE_GRAY),
        B(8, 8, 0, 10, 0, 14, MAT.LAMINATE_GRAY),
        B(3, 5, 0, 1, 2, 6, MAT.CERAMIC),
        B(4, 4, 2, 5, 0, 2, MAT.CERAMIC),
      ];
    },
  },
  lockers: {
    w: 12,
    d: 3,
    tall: true,
    build() {
      const out = [B(0, 11, 0, 2, 0, 15, MAT.METAL_PANEL)];
      for (let a = 2; a < 12; a += 3) out.push(B(a, a, 0, 0, 0, 15, MAT.METAL_PANEL_DARK));
      return out;
    },
  },
  boxes: {
    w: 6,
    d: 5,
    build(rng) {
      return [B(0, 5, 0, 4, 0, rng.int(2, 5), MAT.CARDBOARD), B(1, 3, 1, 3, 6, 8, MAT.CARDBOARD)];
    },
  },
  // church: a pew (backrest at b = 0, facing +b) and an altar with a cloth and a cross
  pew: {
    w: 16,
    d: 5,
    build(rng, opt = {}) {
      const w = opt.w ?? 16;
      return [
        B(0, w - 1, 1, 3, 3, 3, MAT.WOOD_DARK),
        B(0, w - 1, 0, 0, 0, 7, MAT.WOOD_DARK),
        B(0, 0, 1, 3, 0, 5, MAT.WOOD_DARK),
        B(w - 1, w - 1, 1, 3, 0, 5, MAT.WOOD_DARK),
      ];
    },
  },
  altar: {
    w: 14,
    d: 5,
    build() {
      return [B(0, 13, 1, 4, 0, 6, MAT.WOOD_DARK), B(0, 13, 1, 4, 7, 7, MAT.BEDSHEET_WHITE), B(6, 7, 0, 0, 0, 16, MAT.WOOD_DARK), B(4, 9, 0, 0, 13, 13, MAT.WOOD_DARK)];
    },
  },
};

/** A parked car (local a = width across, b = length). */
export function carBoxes(rng) {
  const body = rng.pick([MAT.CAR_RED, MAT.CAR_BLUE, MAT.CAR_WHITE, MAT.CAR_BLACK, MAT.CAR_SILVER, MAT.CAR_SILVER, MAT.CAR_GREEN, MAT.CAR_YELLOW]);
  const w = 14;
  const l = rng.int(30, 36);
  const out = [
    B(0, w - 1, 2, l - 3, 2, 6, body),
    B(1, w - 2, 0, 1, 2, 5, body),
    B(1, w - 2, l - 2, l - 1, 2, 5, body),
    B(1, w - 2, 9, l - 10, 7, 10, body),
    B(2, w - 3, 8, 8, 7, 9, MAT.CAR_GLASS),
    B(2, w - 3, l - 9, l - 9, 7, 9, MAT.CAR_GLASS),
    B(1, 1, 10, l - 11, 7, 9, MAT.CAR_GLASS),
    B(w - 2, w - 2, 10, l - 11, 7, 9, MAT.CAR_GLASS),
    B(2, 4, 0, 0, 4, 5, MAT.HEADLIGHT),
    B(w - 5, w - 3, 0, 0, 4, 5, MAT.HEADLIGHT),
    B(2, 4, l - 1, l - 1, 4, 5, MAT.TAILLIGHT),
    B(w - 5, w - 3, l - 1, l - 1, 4, 5, MAT.TAILLIGHT),
  ];
  for (const b of [5, l - 8]) {
    out.push(B(-0, 1, b, b + 3, 0, 3, MAT.TIRE));
    out.push(B(w - 2, w - 1, b, b + 3, 0, 3, MAT.TIRE));
  }
  return out;
}
