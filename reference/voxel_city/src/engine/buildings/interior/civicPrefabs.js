import { MAT } from "../../voxel/materials.js";

/**
 * Furniture of civic buildings and shops (hospitals, police stations,
 * museums and galleries, venues and cinemas, supermarkets, shops, fire
 * stations, hotels, warehouses). Same frame as prefabs.js: a along the
 * wall, b into the room, z above the finished floor; `build(rng, opt)`
 * returns boxes {a0,a1,b0,b1,z0,z1,m} (m = 0 carves).
 */

const B = (a0, a1, b0, b1, z0, z1, m) => ({ a0, a1, b0, b1, z0, z1, m });
const ART = [MAT.ART_BLUE, MAT.ART_OCHRE, MAT.ART_CRIMSON, MAT.ART_TEAL, MAT.FABRIC_MUSTARD, MAT.PAINT_DARK, MAT.PLAZA_STONE];

export const CIVIC_PREFABS = {
  // ---------------------------------------------------------------- hospital
  hospitalBed: {
    w: 8,
    d: 17,
    build(rng) {
      return [
        B(0, 7, 0, 16, 2, 2, MAT.METAL_CHROME),
        B(0, 0, 0, 0, 0, 1, MAT.METAL_BLACK),
        B(7, 7, 0, 0, 0, 1, MAT.METAL_BLACK),
        B(0, 0, 16, 16, 0, 1, MAT.METAL_BLACK),
        B(7, 7, 16, 16, 0, 1, MAT.METAL_BLACK),
        B(0, 7, 0, 0, 3, 7, MAT.PLASTIC_WHITE),
        B(0, 7, 1, 16, 3, 3, MAT.MATTRESS),
        B(0, 7, 6, 16, 4, 4, rng.pick([MAT.BEDSHEET_WHITE, MAT.BEDSHEET_BLUE, MAT.MEDICAL_GREEN])),
        B(1, 6, 1, 3, 4, 4, MAT.PILLOW),
        B(0, 0, 5, 12, 4, 5, MAT.METAL_CHROME),
        B(7, 7, 5, 12, 4, 5, MAT.METAL_CHROME),
        // drip stand
        B(9, 9, 2, 2, 0, 15, MAT.METAL_CHROME),
        B(9, 9, 2, 2, 13, 14, MAT.GLASS),
      ];
    },
    dExtra: 0,
  },
  bedCurtain: {
    w: 1,
    d: 17,
    tall: true,
    build() {
      return [B(0, 0, 0, 16, 1, 15, MAT.CURTAIN_BLUE), B(0, 0, 0, 16, 16, 16, MAT.METAL_CHROME)];
    },
  },
  medCart: {
    w: 5,
    d: 4,
    build(rng) {
      return [B(0, 4, 0, 3, 1, 6, rng.pick([MAT.PLASTIC_WHITE, MAT.CURTAIN_BLUE])), B(0, 4, 0, 3, 7, 7, MAT.METAL_CHROME), B(0, 0, 0, 0, 0, 0, MAT.METAL_BLACK), B(4, 4, 3, 3, 0, 0, MAT.METAL_BLACK)];
    },
  },
  examCouch: {
    w: 6,
    d: 15,
    build() {
      return [B(0, 5, 0, 14, 0, 3, MAT.PLASTIC_WHITE), B(0, 5, 0, 14, 4, 4, MAT.MEDICAL_GREEN), B(0, 5, 0, 3, 5, 6, MAT.MEDICAL_GREEN), B(0, 5, 2, 12, 5, 5, MAT.BEDSHEET_WHITE)];
    },
  },
  opTable: {
    w: 6,
    d: 16,
    free: true,
    pad: 6,
    build() {
      return [
        B(2, 3, 6, 9, 0, 5, MAT.METAL_CHROME),
        B(0, 5, 0, 15, 6, 6, MAT.MEDICAL_GREEN),
        B(0, 5, 0, 15, 5, 5, MAT.METAL_CHROME),
        // surgical lamp hanging from the ceiling
        B(2, 3, 7, 8, 16, 21, MAT.METAL_CHROME),
        B(0, 5, 5, 10, 15, 15, MAT.STAGE_LIGHT),
        // instrument trolley and monitor stack
        B(-5, -2, 2, 5, 0, 7, MAT.METAL_CHROME),
        B(8, 11, 1, 4, 0, 12, MAT.PLASTIC_WHITE),
        B(8, 11, 1, 1, 9, 12, MAT.SCREEN),
      ];
    },
  },
  xray: {
    w: 12,
    d: 14,
    free: true,
    pad: 4,
    build() {
      return [
        B(3, 8, 0, 13, 0, 5, MAT.PLASTIC_WHITE),
        B(3, 8, 0, 13, 6, 6, MAT.BEDSHEET_WHITE),
        B(0, 1, 5, 7, 0, 18, MAT.PLASTIC_WHITE),
        B(0, 7, 5, 7, 17, 18, MAT.PLASTIC_WHITE),
        B(4, 7, 5, 8, 14, 16, MAT.METAL_PANEL),
      ];
    },
  },
  waitingChairs: {
    w: 0,
    d: 4,
    build(rng, opt) {
      const w = opt.w;
      const c = rng.pick([MAT.PLASTIC_BLACK, MAT.FABRIC_BLUE, MAT.PLASTIC_WHITE, MAT.FABRIC_RED]);
      const out = [B(0, w - 1, 1, 3, 3, 3, MAT.METAL_CHROME)];
      for (let a = 0; a + 2 < w; a += 4) out.push(B(a, a + 2, 1, 3, 4, 4, c), B(a, a + 2, 0, 0, 5, 8, c));
      out.push(B(0, 0, 1, 3, 0, 2, MAT.METAL_BLACK), B(w - 1, w - 1, 1, 3, 0, 2, MAT.METAL_BLACK));
      return out;
    },
  },
  serviceCounter: {
    // a counter with a glass screen (police front desk, bank, pharmacy, ticket office)
    w: 0,
    d: 6,
    build(rng, opt) {
      const w = opt.w;
      const top = opt.top ?? rng.pick([MAT.WOOD_DARK, MAT.LAMINATE_GRAY, MAT.WOOD_MED]);
      const out = [B(0, w - 1, 3, 5, 0, 7, top), B(0, w - 1, 3, 5, 8, 8, MAT.COUNTERTOP)];
      if (opt.screen !== false) out.push(B(0, w - 1, 4, 4, 9, 15, MAT.GLASS));
      for (let a = 2; a + 3 < w; a += 8) out.push(B(a, a + 2, 1, 1, 5, 7, MAT.SCREEN), B(a, a + 2, 0, 1, 3, 3, MAT.FABRIC_GRAY));
      return out;
    },
  },
  // ---------------------------------------------------------------- police / fire
  bunk: {
    w: 7,
    d: 16,
    build(rng, opt = {}) {
      const frame = opt.steel ? MAT.METAL_PANEL : MAT.METAL_BLACK;
      const out = [B(0, 6, 0, 15, 2, 2, frame), B(0, 6, 0, 15, 3, 3, MAT.MATTRESS), B(0, 6, 5, 15, 4, 4, opt.steel ? MAT.FABRIC_GRAY : rng.pick([MAT.BEDSHEET_BLUE, MAT.FABRIC_GRAY]))];
      if (opt.double) out.push(B(0, 6, 0, 15, 10, 10, frame), B(0, 6, 0, 15, 11, 11, MAT.MATTRESS), B(0, 0, 0, 0, 0, 12, frame), B(6, 6, 15, 15, 0, 12, frame), B(0, 0, 15, 15, 0, 12, frame), B(6, 6, 0, 0, 0, 12, frame));
      else out.push(B(0, 0, 0, 0, 0, 1, frame), B(6, 6, 15, 15, 0, 1, frame));
      return out;
    },
  },
  steelToilet: {
    w: 4,
    d: 5,
    build() {
      return [B(0, 3, 0, 1, 0, 6, MAT.METAL_CHROME), B(1, 2, 2, 4, 0, 3, MAT.METAL_CHROME), B(1, 2, 3, 3, 3, 3, 0)];
    },
  },
  cellBars: {
    // a row of bars (the front of a holding cell)
    w: 0,
    d: 1,
    build(rng, opt) {
      const out = [B(0, opt.w - 1, 0, 0, 17, 17, MAT.METAL_BLACK), B(0, opt.w - 1, 0, 0, 0, 0, MAT.METAL_BLACK)];
      for (let a = 0; a < opt.w; a += 2) out.push(B(a, a, 0, 0, 1, 16, MAT.METAL_BLACK));
      return out;
    },
  },
  fireTruck: {
    w: 20,
    d: 64,
    free: true,
    pad: 2,
    build() {
      const out = [
        B(0, 19, 2, 61, 2, 20, MAT.FIRE_RED),
        B(1, 18, 0, 12, 2, 18, MAT.FIRE_RED),
        B(2, 17, 0, 0, 10, 16, MAT.CAR_GLASS),
        B(2, 17, 0, 0, 5, 6, MAT.HEADLIGHT),
        B(0, 19, 14, 58, 21, 22, MAT.METAL_CHROME),
        B(3, 16, 16, 56, 23, 24, MAT.METAL_PANEL),
        B(4, 15, 12, 12, 21, 22, MAT.SIGNAL_RED),
        B(0, 0, 20, 58, 6, 14, MAT.METAL_PANEL),
        B(19, 19, 20, 58, 6, 14, MAT.METAL_PANEL),
      ];
      for (const b of [6, 44, 52]) out.push(B(-1, 2, b, b + 5, 0, 5, MAT.TIRE), B(17, 20, b, b + 5, 0, 5, MAT.TIRE));
      return out;
    },
  },
  hoseRack: {
    w: 10,
    d: 3,
    tall: true,
    build() {
      const out = [B(0, 9, 0, 2, 0, 16, MAT.METAL_PANEL_DARK)];
      for (let z = 2; z <= 12; z += 5) out.push(B(1, 8, 1, 2, z, z + 3, MAT.FIRE_RED));
      out.push(B(1, 8, 0, 0, 14, 15, MAT.HAZARD_YELLOW));
      return out;
    },
  },
  // ---------------------------------------------------------------- museum / gallery
  painting: {
    w: 0,
    d: 1,
    tall: true,
    build(rng, opt) {
      const w = opt.w;
      const h = Math.max(5, Math.min(12, Math.round(w * rng.float(0.5, 1.1))));
      const z0 = 12 - Math.floor(h / 2);
      const frame = rng.pick([MAT.GOLD, MAT.WOOD_DARK, MAT.METAL_BLACK, MAT.PLASTIC_WHITE]);
      const out = [B(0, w - 1, 0, 0, z0, z0 + h - 1, frame)];
      // the canvas: a field of colour with a band or a blob in a second colour
      const c1 = rng.pick(ART);
      const c2 = rng.pick(ART);
      out.push(B(1, w - 2, 0, 0, z0 + 1, z0 + h - 2, c1));
      if (w > 4 && h > 4) {
        const a = rng.int(1, w - 3);
        const z = rng.int(z0 + 1, z0 + h - 3);
        out.push(B(a, Math.min(w - 2, a + rng.int(1, 3)), 0, 0, z, Math.min(z0 + h - 2, z + rng.int(1, 3)), c2));
      }
      out.push(B(Math.floor(w / 2), Math.floor(w / 2), 0, 0, z0 - 2, z0 - 2, MAT.SIGN_WHITE));
      return out;
    },
  },
  displayCase: {
    w: 7,
    d: 5,
    free: true,
    pad: 3,
    build(rng) {
      const obj = rng.pick([MAT.GOLD, MAT.CLAY, MAT.BONE, MAT.GRANITE, MAT.BOOKS, MAT.STEEL_RUST, MAT.DOME_GREEN]);
      return [B(0, 6, 0, 4, 0, 5, MAT.WOOD_DARK), B(0, 6, 0, 4, 6, 9, MAT.GLASS), B(0, 6, 0, 4, 10, 10, MAT.METAL_BLACK), B(2, 4, 1, 3, 6, 6 + rng.int(1, 2), obj)];
    },
  },
  vitrine: {
    w: 12,
    d: 4,
    tall: true,
    build(rng) {
      const out = [B(0, 11, 0, 3, 0, 4, MAT.WOOD_DARK), B(0, 11, 0, 3, 5, 15, MAT.GLASS), B(0, 11, 0, 3, 16, 16, MAT.WOOD_DARK), B(0, 11, 0, 0, 5, 15, MAT.PAINT_DARK)];
      for (let a = 1; a < 11; a += 3) out.push(B(a, a + 1, 1, 2, 5, 5 + rng.int(1, 5), rng.pick([MAT.GOLD, MAT.CLAY, MAT.BONE, MAT.STEEL_RUST, MAT.GRANITE])));
      out.push(B(1, 10, 1, 2, 10, 10, MAT.GLASS));
      return out;
    },
  },
  sculpture: {
    w: 4,
    d: 4,
    free: true,
    pad: 4,
    build(rng) {
      const m = rng.pick([MAT.LIMESTONE, MAT.GRANITE, MAT.METAL_PANEL_DARK, MAT.STEEL_RUST, MAT.GOLD, MAT.PLASTER_WHITE]);
      const out = [B(0, 3, 0, 3, 0, 5, MAT.PLASTER_WHITE)];
      const kind = rng.int(0, 2);
      if (kind === 0) for (let z = 6; z < 16; z += 1) out.push(B(1 + (z % 3 === 0 ? 1 : 0), 2, 1, 2 + (z % 4 === 0 ? 1 : 0), z, z, m));
      else if (kind === 1) out.push(B(0, 3, 1, 2, 6, 7, m), B(1, 2, 0, 3, 8, 11, m), B(1, 2, 1, 2, 12, 14, m));
      else out.push(B(1, 2, 1, 2, 6, 10, m), B(0, 3, 0, 3, 11, 12, m), B(1, 1, 1, 1, 13, 17, m));
      return out;
    },
  },
  skeleton: {
    // a dinosaur skeleton on a low platform (local b runs head to tail)
    w: 10,
    d: 28,
    free: true,
    pad: 4,
    build() {
      const out = [B(0, 9, 0, 27, 0, 1, MAT.GRANITE_LIGHT)];
      const bone = MAT.BONE;
      // legs
      for (const [a, b] of [[2, 9], [7, 9], [2, 18], [7, 18]]) out.push(B(a, a, b, b, 2, 11, bone), B(a - 1, a + 1, b - 1, b + 1, 2, 2, bone));
      // spine from the skull down the tail
      for (let b = 2; b < 27; b += 1) {
        const h = b < 6 ? 16 : b < 20 ? 14 - Math.max(0, Math.abs(b - 13) - 5) * 0 : 14 - (b - 20);
        out.push(B(4, 5, b, b, h, h, bone));
        if (b >= 7 && b <= 19 && b % 2 === 0) out.push(B(2, 2, b, b, h - 5, h - 1, bone), B(7, 7, b, b, h - 5, h - 1, bone), B(3, 6, b, b, h - 6, h - 6, bone));
      }
      // skull and neck
      out.push(B(3, 6, 0, 4, 15, 18, bone), B(4, 5, -2, 0, 15, 16, bone), B(4, 5, 5, 7, 14, 16, bone));
      return out;
    },
  },
  // ---------------------------------------------------------------- venues
  stage: {
    w: 0,
    d: 0,
    build(rng, opt) {
      const w = opt.w;
      const d = opt.d;
      const out = [B(0, w - 1, 0, d - 1, 0, 3, MAT.STAGE_BLACK), B(0, w - 1, d - 1, d - 1, 0, 3, MAT.WOOD_DARK)];
      // backdrop curtain, speaker stacks, a truss with lights
      out.push(B(0, w - 1, 0, 0, 4, 20, opt.curtain ?? MAT.VELVET_RED));
      for (const a of [1, w - 5]) out.push(B(a, a + 3, d - 4, d - 1, 4, 13, MAT.PLASTIC_BLACK), B(a + 1, a + 2, d - 1, d - 1, 6, 11, MAT.METAL_PANEL_DARK));
      out.push(B(0, w - 1, d - 2, d - 2, 21, 21, MAT.METAL_BLACK));
      for (let a = 2; a < w - 2; a += 5) out.push(B(a, a, d - 2, d - 2, 20, 20, MAT.STAGE_LIGHT));
      if (opt.band) {
        // drum riser, a keyboard and mic stands
        const c = Math.floor(w / 2);
        out.push(B(c - 4, c + 3, 2, 7, 4, 5, MAT.STAGE_BLACK), B(c - 2, c + 1, 3, 6, 6, 8, MAT.METAL_CHROME), B(c - 3, c - 3, 4, 4, 6, 10, MAT.METAL_CHROME));
        out.push(B(c + 5, c + 9, 5, 6, 8, 8, MAT.PLASTIC_BLACK), B(c + 6, c + 6, 5, 5, 4, 7, MAT.METAL_BLACK));
        for (const a of [c - 7, c, c + 7]) if (a > 1 && a < w - 2) out.push(B(a, a, d - 5, d - 5, 4, 14, MAT.METAL_BLACK));
      }
      if (opt.piano) out.push(B(Math.floor(w / 2) - 5, Math.floor(w / 2) + 5, 4, 12, 7, 8, MAT.PLASTIC_BLACK), B(Math.floor(w / 2) - 5, Math.floor(w / 2) + 5, 4, 5, 4, 6, MAT.PLASTIC_BLACK), B(Math.floor(w / 2) - 4, Math.floor(w / 2) - 4, 8, 11, 4, 6, MAT.PLASTIC_BLACK));
      return out;
    },
  },
  seatRow: {
    w: 0,
    d: 5,
    free: true,
    pad: 0,
    build(rng, opt) {
      const w = opt.w;
      const c = opt.seat ?? MAT.VELVET_RED;
      const out = [];
      for (let a = 0; a + 2 < w; a += 3) out.push(B(a, a + 2, 1, 3, 3, 3, c), B(a, a + 2, 4, 4, 4, 7, c), B(a, a, 1, 4, 0, 4, MAT.METAL_BLACK));
      return out;
    },
  },
  projectionScreen: {
    w: 0,
    d: 1,
    build(rng, opt) {
      return [B(0, opt.w - 1, 0, 0, 5, Math.max(12, opt.h ?? 20), MAT.PROJECTION), B(0, opt.w - 1, 0, 0, 4, 4, MAT.STAGE_BLACK)];
    },
  },
  mixingDesk: {
    w: 10,
    d: 6,
    free: true,
    pad: 3,
    build() {
      return [B(0, 9, 0, 5, 0, 5, MAT.STAGE_BLACK), B(0, 9, 1, 4, 6, 6, MAT.METAL_PANEL_DARK), B(1, 8, 2, 3, 7, 7, MAT.SIGNAL_GREEN), B(0, 9, 5, 5, 6, 8, MAT.PLASTIC_BLACK)];
    },
  },
  grandPiano: {
    w: 12,
    d: 14,
    free: true,
    pad: 3,
    build() {
      return [B(0, 11, 2, 13, 5, 7, MAT.PLASTIC_BLACK), B(0, 11, 0, 3, 5, 6, MAT.PLASTIC_BLACK), B(1, 10, 0, 1, 7, 7, MAT.PLASTIC_WHITE), B(1, 1, 4, 4, 0, 4, MAT.PLASTIC_BLACK), B(10, 10, 4, 4, 0, 4, MAT.PLASTIC_BLACK), B(5, 5, 12, 12, 0, 4, MAT.PLASTIC_BLACK), B(4, 7, -4, -2, 3, 3, MAT.LEATHER)];
    },
  },
  poolTable: {
    w: 9,
    d: 16,
    free: true,
    pad: 5,
    build() {
      return [B(0, 8, 0, 15, 5, 6, MAT.WOOD_DARK), B(1, 7, 1, 14, 6, 6, MAT.FABRIC_GREEN), B(1, 1, 1, 1, 0, 4, MAT.WOOD_DARK), B(7, 7, 14, 14, 0, 4, MAT.WOOD_DARK), B(1, 1, 14, 14, 0, 4, MAT.WOOD_DARK), B(7, 7, 1, 1, 0, 4, MAT.WOOD_DARK), B(3, 5, 7, 8, 18, 18, MAT.LAMP_SHADE)];
    },
  },
  bottleShelf: {
    w: 12,
    d: 2,
    tall: true,
    build() {
      const out = [B(0, 11, 0, 1, 0, 5, MAT.WOOD_DARK), B(0, 11, 0, 0, 6, 16, MAT.MIRROR)];
      for (let z = 7; z <= 15; z += 4) out.push(B(0, 11, 1, 1, z, z, MAT.WOOD_DARK), B(0, 11, 1, 1, z + 1, z + 2, MAT.BOTTLES));
      return out;
    },
  },
  // ---------------------------------------------------------------- shops
  fridgeCase: {
    w: 10,
    d: 5,
    tall: true,
    build(rng) {
      const out = [B(0, 9, 0, 4, 0, 15, MAT.APPLIANCE_STEEL), B(0, 9, 4, 4, 2, 14, MAT.GLASS_TINT)];
      for (let z = 3; z <= 12; z += 3) out.push(B(1, 8, 1, 3, z, z + 1, rng.pick([MAT.GOODS, MAT.BOTTLES, MAT.PLASTIC_WHITE, MAT.PRODUCE_RED])));
      out.push(B(0, 9, 3, 3, 15, 15, MAT.LIGHT_STRIP));
      return out;
    },
  },
  freezerChest: {
    w: 12,
    d: 5,
    free: true,
    pad: 4,
    build() {
      return [B(0, 11, 0, 4, 0, 5, MAT.PLASTIC_WHITE), B(1, 10, 1, 3, 6, 6, MAT.GLASS_TINT), B(1, 10, 1, 3, 4, 5, MAT.CARDBOARD)];
    },
  },
  produceStand: {
    w: 12,
    d: 7,
    free: true,
    pad: 4,
    build(rng) {
      const out = [B(0, 11, 0, 6, 0, 3, MAT.WOOD_LIGHT)];
      for (let a = 0; a < 12; a += 4) {
        const m = rng.pick([MAT.PRODUCE_GREEN, MAT.PRODUCE_RED, MAT.PRODUCE_YELLOW, MAT.BREAD, MAT.CLAY]);
        out.push(B(a, a + 3, 0, 2, 4, 5, m), B(a, a + 3, 3, 6, 4, 6, rng.pick([MAT.PRODUCE_GREEN, MAT.PRODUCE_RED, MAT.PRODUCE_YELLOW])));
      }
      return out;
    },
  },
  trolleys: {
    w: 6,
    d: 12,
    build() {
      const out = [];
      for (let b = 0; b < 12; b += 3) out.push(B(0, 5, b, b + 5, 2, 6, MAT.METAL_CHROME), B(1, 4, b + 1, b + 4, 3, 5, 0));
      return out;
    },
  },
  shopCounter: {
    // a glass counter with goods inside (bakery, butcher, fishmonger, deli)
    w: 0,
    d: 6,
    build(rng, opt) {
      const w = opt.w;
      const goods = opt.goods ?? MAT.BREAD;
      const out = [B(0, w - 1, 2, 5, 0, 4, MAT.PLASTIC_WHITE), B(0, w - 1, 2, 5, 5, 8, MAT.GLASS), B(1, w - 2, 3, 4, 5, 5, goods), B(0, w - 1, 2, 3, 9, 9, MAT.COUNTERTOP)];
      out.push(B(w - 5, w - 3, 3, 4, 10, 10, MAT.PLASTIC_BLACK));
      return out;
    },
  },
  goodsShelf: {
    w: 10,
    d: 3,
    tall: true,
    build(rng, opt = {}) {
      const frame = opt.frame ?? rng.pick([MAT.WOOD_LIGHT, MAT.WOOD_DARK, MAT.SHELF_METAL]);
      const goods = opt.goods ?? [MAT.GOODS];
      const out = [B(0, 0, 0, 2, 0, 15, frame), B(9, 9, 0, 2, 0, 15, frame), B(0, 9, 0, 0, 0, 15, frame)];
      for (let z = 0; z <= 12; z += 4) out.push(B(0, 9, 0, 2, z, z, frame), B(1, 8, 1, 2, z + 1, z + 2, rng.pick(goods)));
      return out;
    },
  },
  clothesRack: {
    w: 10,
    d: 4,
    free: true,
    pad: 3,
    build(rng) {
      const out = [B(0, 0, 1, 2, 0, 12, MAT.METAL_CHROME), B(9, 9, 1, 2, 0, 12, MAT.METAL_CHROME), B(0, 9, 1, 2, 12, 12, MAT.METAL_CHROME)];
      for (let a = 1; a < 9; a += 1) out.push(B(a, a, 0, 3, 5, 11, rng.pick([MAT.CLOTHES, MAT.FABRIC_RED, MAT.FABRIC_BLUE, MAT.FABRIC_BEIGE, MAT.FABRIC_GRAY])));
      return out;
    },
  },
  flowerBuckets: {
    w: 12,
    d: 3,
    build(rng) {
      const out = [B(0, 11, 0, 2, 0, 2, MAT.WOOD_WEATHERED)];
      for (let a = 0; a < 12; a += 3) out.push(B(a, a + 2, 0, 2, 3, 4, MAT.METAL_CHROME), B(a, a + 2, 0, 2, 5, 7, rng.pick([MAT.FLOWER_RED, MAT.FLOWER_YELLOW, MAT.FLOWER_WHITE, MAT.FLOWER_PURPLE, MAT.FLOWER_PINK, MAT.PLANT])));
      return out;
    },
  },
  barberChair: {
    w: 7,
    d: 9,
    build() {
      return [B(0, 6, 0, 0, 5, 14, MAT.MIRROR), B(0, 6, 0, 1, 4, 4, MAT.COUNTERTOP), B(2, 4, 4, 7, 3, 4, MAT.LEATHER), B(2, 4, 7, 7, 5, 9, MAT.LEATHER), B(3, 3, 5, 6, 0, 2, MAT.METAL_CHROME)];
    },
  },
  lumberStack: {
    w: 20,
    d: 8,
    free: true,
    pad: 3,
    build(rng) {
      const out = [];
      const n = rng.int(3, 7);
      for (let k = 0; k < n; k += 1) out.push(B(0, 19, 0, 7, k * 2, k * 2 + 1, k % 2 ? MAT.WOOD_LIGHT : MAT.WOOD_MED), B(2, 3, 0, 7, k * 2 + 1, k * 2 + 1, MAT.WOOD_DARK));
      return out;
    },
  },
  conveyor: {
    w: 0,
    d: 4,
    free: true,
    pad: 2,
    build(rng, opt) {
      const w = opt.w;
      const out = [B(0, w - 1, 0, 3, 4, 4, MAT.PLASTIC_BLACK), B(0, w - 1, 0, 0, 5, 5, MAT.HAZARD_YELLOW), B(0, w - 1, 3, 3, 5, 5, MAT.HAZARD_YELLOW)];
      for (let a = 0; a < w; a += 8) out.push(B(a, a, 0, 0, 0, 3, MAT.METAL_PANEL), B(a, a, 3, 3, 0, 3, MAT.METAL_PANEL));
      for (let a = 3; a + 3 < w; a += rng.int(6, 12)) out.push(B(a, a + 3, 1, 2, 5, 7, MAT.CARDBOARD));
      return out;
    },
  },
  storageUnits: {
    // self-storage: a block of lock-up units, doors on both long sides
    w: 0,
    d: 12,
    free: true,
    pad: 0,
    build(rng, opt) {
      const w = opt.w;
      const out = [B(0, w - 1, 0, 11, 0, 17, MAT.CORRUGATED), B(0, w - 1, 0, 11, 18, 18, MAT.METAL_PANEL)];
      for (let a = 1; a + 6 < w; a += 8) {
        const door = rng.pick([MAT.ROLLUP_DOOR, MAT.CORRUGATED_BLUE, MAT.CORRUGATED_RUST, MAT.DOOR_METAL]);
        out.push(B(a, a + 5, 0, 0, 0, 13, door), B(a, a + 5, 11, 11, 0, 13, door));
      }
      return out;
    },
  },
  whiteboard: {
    w: 16,
    d: 1,
    build() {
      return [B(0, 15, 0, 0, 7, 14, MAT.PANEL_WHITE), B(0, 15, 0, 0, 6, 6, MAT.METAL_CHROME)];
    },
  },
  danceBarre: {
    w: 0,
    d: 2,
    tall: true,
    build(rng, opt) {
      return [B(0, opt.w - 1, 0, 0, 2, 16, MAT.MIRROR), B(0, opt.w - 1, 1, 1, 7, 7, MAT.HANDRAIL_WOOD)];
    },
  },
  atm: {
    w: 5,
    d: 3,
    build() {
      return [B(0, 4, 0, 2, 0, 12, MAT.METAL_PANEL), B(1, 3, 2, 2, 7, 9, MAT.SCREEN), B(1, 3, 2, 2, 5, 5, MAT.PLASTIC_BLACK)];
    },
  },
  icon: {
    // an icon screen (iconostasis panel) of an orthodox church: gilded frames round dark panels
    w: 0,
    d: 2,
    tall: true,
    build(rng, opt) {
      const w = opt.w;
      const out = [B(0, w - 1, 0, 1, 0, 22, MAT.WOOD_DARK)];
      for (let a = 1; a + 3 < w; a += 5) for (const z of [3, 12]) out.push(B(a, a + 3, 1, 1, z, z + 7, MAT.GOLD), B(a + 1, a + 2, 1, 1, z + 1, z + 6, rng.pick([MAT.ART_CRIMSON, MAT.ART_BLUE, MAT.ART_OCHRE])));
      out.push(B(Math.floor(w / 2) - 2, Math.floor(w / 2) + 2, 1, 1, 0, 11, 0));
      return out;
    },
  },
};
