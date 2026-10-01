import { planHall, planCorridor, planKiosk, planDepartmentStore } from "./civic.js";

/**
 * Room programs of the civic buildings, venues and big shops (the planners
 * live in civic.js, the envelopes in buildings/civic.js). A program lists
 * which rooms go where; the planner fits them to the footprint.
 */

export const HALL_PROGRAMS = {
  supermarket: { foyer: 3.5, foyerSide: ["office"], hall: "sales", hallOpen: true, hallDoor: 16, back: ["storage", "breakroom", "wc", "storage"], backDepth: 5, backW: 6, dock: "rollup" },
  concertHall: {
    foyer: 8,
    foyerSide: ["cloakroom", "wc"],
    hall: "auditorium",
    hallProps: { noWindows: true, classical: true },
    back: ["dressing", "dressing", "storage", "wc"],
    backDepth: 5,
    dock: "metal",
    upperFoyer: "foyerBar",
    upperSide: ["office"],
  },
  musicClub: { foyer: 4, foyerSide: ["cloakroom", "wc"], hall: "venueFloor", hallProps: { noWindows: true }, back: ["dressing", "storage"], backDepth: 4, dock: "metal" },
  cinema: { foyer: 7, foyerSide: ["kiosk", "wc"], hall: "cinema", halls: [2, 3], hallMinW: 9, hallProps: { noWindows: true }, back: ["storage", "wc"], backDepth: 3.5, dock: "metal", upperFoyer: "foyerBar" },
  marketHall: { foyer: 3.5, foyerSide: ["wc"], hall: "marketHall", hallOpen: true, hallDoor: 20, back: ["storage", "office"], backDepth: 4.5, dock: "rollup" },
  houseOfCulture: {
    foyer: 8,
    foyerSide: ["cloakroom", "wc"],
    hall: "auditorium",
    hallProps: { noWindows: true, classical: true },
    back: ["dressing", "clubroom", "storage"],
    backDepth: 5,
    dock: "metal",
    upperFoyer: "danceHall",
    upperSide: ["clubroom", "clubroom"],
  },
};

const HOSPITAL = {
  bay: 6.5,
  corr: 3,
  corrPaint: "PAINT_MINT",
  corrFloor: "FLOOR_LINOLEUM",
  ground: {
    front: [{ type: "lobby", at: "mid", lobby: true }, "waiting", "pharmacy", "exam"],
    back: [{ type: "emergency", span: 2, ext: "back", at: "start" }, "radiology", "restroom"],
    fill: ["exam", "office"],
  },
  first: {
    front: [{ type: "nurses", at: "mid" }],
    back: [{ type: "operating", span: 2, at: "start" }, "operating", "storage", "restroom"],
    fill: ["ward"],
  },
  upper: { front: [{ type: "nurses", at: "mid" }], back: ["restroom"], fill: ["ward"] },
};

/** A polyclinic (outpatients): a registry desk, doctors' rooms and waiting corners on every floor, x-ray, no wards. */
const POLYCLINIC = {
  ...HOSPITAL,
  ground: {
    front: [{ type: "lobby", at: "mid", lobby: true }, "pharmacy", "waiting"],
    back: [{ type: "radiology", at: "start" }, "restroom", "registry"],
    fill: ["exam"],
  },
  first: null,
  upper: { front: [{ type: "waiting", at: "mid" }], back: ["restroom", "office"], fill: ["exam"] },
};

export const CORRIDOR_PROGRAMS = {
  hospital: HOSPITAL,
  polyclinic: POLYCLINIC,
  policeStation: {
    bay: 4.5,
    corr: 2.5,
    ground: {
      front: [{ type: "policeDesk", at: "mid", lobby: true, span: 2 }, "interview", "interview"],
      back: [{ type: "cell", split: 3 }, { type: "cell", split: 2 }, "lockerRoom", { type: "garageBay", span: 2, ext: "rollupBack", at: "end" }],
      fill: ["office"],
    },
    upper: { front: [{ type: "briefing", span: 2, at: "mid" }], back: ["evidence", "restroom"], fill: ["office"] },
  },
  fireStation: {
    bay: 5,
    corr: 2.5,
    ground: {
      front: [{ type: "garageBay", span: 3, ext: "rollupFront", at: "start", props: { fire: true } }, { type: "reception", lobby: true, at: "end" }],
      back: ["lockerRoom", "storage", "restroom"],
      fill: ["office"],
    },
    upper: { front: ["dorm", "dorm"], back: ["kitchen", "dining", "restroom"], fill: ["office"] },
  },
  museum: {
    bay: 8,
    corr: 3.5,
    corrFloor: "FLOOR_MARBLE",
    ground: { front: [{ type: "lobby", at: "mid", lobby: true }, "museumShop", "cloakroom"], back: [{ type: "exhibit", span: 2 }, { type: "exhibit", span: 2 }], fill: ["exhibit"] },
    upper: { front: [{ type: "exhibit", span: 2 }, "cafe"], back: [{ type: "exhibit", span: 2 }, { type: "exhibit", span: 2 }], fill: ["exhibit"] },
  },
  artGallery: {
    bay: 9,
    corr: 3.5,
    corrFloor: "FLOOR_OAK",
    corrPaint: "PAINT_WHITE",
    ground: { front: [{ type: "lobby", at: "mid", lobby: true }, "museumShop", "cafe"], back: [{ type: "gallery", span: 2 }], fill: ["gallery"] },
    upper: { front: [{ type: "gallery", span: 2 }], back: [{ type: "gallery", span: 2 }], fill: ["gallery"] },
  },
  library: {
    bay: 7,
    ground: { front: [{ type: "reception", at: "mid", lobby: true }, "library", "study"], back: [{ type: "library", span: 2 }, "restroom"], fill: ["library"] },
    upper: { front: ["study", "meeting"], back: [{ type: "library", span: 2 }], fill: ["library"] },
  },
  townHall: {
    bay: 5.5,
    corrFloor: "FLOOR_PARQUET",
    ground: { front: [{ type: "lobby", at: "mid", lobby: true }, "waiting", "registry"], back: ["registry", "office", "restroom"], fill: ["office"] },
    first: { front: [{ type: "council", span: 3, at: "mid" }], back: ["office", "meeting", "restroom"], fill: ["office"] },
    upper: { back: ["restroom"], fill: ["office"] },
  },
  hotel: {
    bay: 4.5,
    corr: 2,
    corrFloor: "FLOOR_CARPET_RED",
    ground: { front: [{ type: "reception", at: "mid", lobby: true }, { type: "restaurant", span: 2 }, "pub"], back: ["kitchen", "office", "restroom", "storage"], fill: ["pub"] },
    upper: { back: ["storage"], fill: ["hotelRoom"] },
  },
};

export const KIOSK_PROGRAMS = {
  petrolStation: { shop: "grocery" },
};

/** Interior planners of the civic archetypes (plan.js PLANNERS). */
export const CIVIC_PLANNERS = {};
for (const [id, P] of Object.entries(HALL_PROGRAMS)) CIVIC_PLANNERS[id] = (ctx) => planHall(ctx, P);
for (const [id, P] of Object.entries(CORRIDOR_PROGRAMS)) CIVIC_PLANNERS[id] = (ctx) => planCorridor(ctx, P);
for (const [id, P] of Object.entries(KIOSK_PROGRAMS)) CIVIC_PLANNERS[id] = (ctx) => planKiosk(ctx, P);
CIVIC_PLANNERS.departmentStore = planDepartmentStore;
