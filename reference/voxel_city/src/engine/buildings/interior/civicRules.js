import { MAT } from "../../voxel/materials.js";

/**
 * Furnishing rules of civic rooms and shops (see furnish.js for the room
 * context API: wall / free / opposite placements with door clearance and
 * a walker check). `makeCivicRules(RULES)` returns the rules; RULES gives
 * access to the base rules to build on.
 */

const OPP = { N: "S", S: "N", E: "W", W: "E" };

/**
 * Rows of an item across a room, facing the `front` side (seats, desks):
 * steps of `da` along the side and `db` away from it, starting at (a0, b0).
 */
function rows(c, key, front, { a0 = 6, b0 = 12, da = 10, db = 10, w = null, aEnd = 6, bEnd = 6, prefab = {} } = {}) {
  const L = c.sideLen(front);
  const D = c.sideDepth(front);
  const iw = w ?? 6;
  let n = 0;
  for (let b = b0; b + db - 2 < D - bEnd; b += db)
    for (let a = a0; a + iw <= L - aEnd; a += da) if (c.free(key, { side: front, a, b, exact: true, pad: 0, ...(w ? { w } : {}), prefab })) n += 1;
  return n;
}

/** The side of a room furthest from its doors (a stage, a screen, an altar goes there). */
function farSide(c) {
  let best = null;
  for (const side of ["N", "S", "E", "W"]) {
    let doors = 0;
    for (let a = 0; a < c.sideLen(side); a += 1) if (c.wallBehind(side, a) === "door") doors += 1;
    const score = doors * 100 - c.sideLen(side) * 0.1;
    if (!best || score < best.score) best = { side, score };
  }
  return best.side;
}

/** Paintings along every free wall stretch, hung at even spacing. */
function hangPaintings(c, n = 12) {
  for (const side of ["N", "E", "S", "W"]) {
    const L = c.sideLen(side);
    for (let a = 4; a + 10 < L - 3; a += c.rng.int(14, 20)) {
      if (n <= 0) return;
      if (c.wall("painting", { sides: [side], at: a, w: c.rng.int(6, 11), keepDepth: 2 })) n -= 1;
    }
  }
}

export function makeCivicRules(RULES) {
  const R = {
    // ------------------------------------------------------------ hospital
    ward(c) {
      // beds head to the long walls, curtains between them, a chair and a cart by each
      for (const side of c.longSides().slice(0, 2)) {
        const L = c.sideLen(side);
        for (let a = 4; a + 10 < L - 3; a += 16) {
          const bed = c.wall("hospitalBed", { sides: [side], at: a, keepDepth: 8 });
          if (!bed) continue;
          c.wall("bedCurtain", { sides: [side], at: bed.a + bed.w + 3 });
        }
      }
      c.wall("medCart", { at: "random" });
      c.wall("basin", { at: "random" });
    },
    emergency(c) {
      R.ward(c);
      c.wall("shelfUnit", { at: "random", prefab: { metal: true } });
    },
    exam(c) {
      c.wall("examCouch", { at: "start" });
      c.wall("desk", { at: "end" });
      c.wall("shelfUnit", { at: "random", prefab: { metal: true } });
      c.wall("basin", { at: "random" });
    },
    operating(c) {
      c.free("opTable", { range: 10 });
      for (let k = 0; k < 3; k += 1) c.wall("medCart", { at: "random" });
      c.wall("shelfUnit", { at: "random", prefab: { metal: true } });
    },
    radiology(c) {
      c.free("xray", { range: 10 });
      c.wall("desk", { at: "start" });
    },
    waiting(c) {
      for (const side of c.longSides()) {
        const w = Math.min(28, c.sideLen(side) - 10);
        if (w >= 8) c.wall("waitingChairs", { sides: [side], w, at: "center" });
      }
      c.wall("plant", { at: "start" });
      c.wall("plant", { at: "end" });
    },
    nurses(c) {
      const side = c.longSides()[0];
      c.wall("serviceCounter", { sides: [side], w: Math.min(20, c.sideLen(side) - 8), at: "center", prefab: { screen: false } });
      c.wall("shelfUnit", { at: "random", prefab: { metal: true } });
      c.wall("medCart", { at: "random" });
    },
    // ------------------------------------------------------------ police / fire / civic offices
    policeDesk(c) {
      const side = farSide(c);
      c.wall("serviceCounter", { sides: [side], w: Math.min(24, c.sideLen(side) - 8), at: "center", prefab: { top: MAT.POLICE_BLUE } });
      const s2 = OPP[side];
      c.wall("waitingChairs", { sides: [s2], w: Math.min(16, c.sideLen(s2) - 12), at: "start" });
      c.wall("plant", { at: "random" });
    },
    cell(c) {
      c.wall("bunk", { at: "start", prefab: { steel: true } });
      c.wall("steelToilet", { at: "end" });
    },
    interview(c) {
      c.free("diningTable", { range: 4 });
      c.wall("shelfUnit", { at: "random" });
    },
    briefing(c) {
      const side = farSide(c);
      c.wall("whiteboard", { sides: [side], at: "center", keepDepth: 2 });
      rows(c, "schoolDesk", side, { b0: 12, da: 9, db: 11 });
    },
    evidence(c) {
      for (let k = 0; k < 6; k += 1) if (!c.wall("shelfUnit", { at: "random", prefab: { metal: true } })) break;
      c.wall("boxes", { at: "random" });
    },
    lockerRoom(c) {
      for (let k = 0; k < 4; k += 1) if (!c.wall("lockers", { at: "random" })) break;
      c.free("bench", { range: 8 });
    },
    garageBay(c) {
      // painted bays, a vehicle in each (when vehicles are on), equipment on the walls
      const side = c.W >= c.H ? "N" : "W";
      const L = c.sideLen(side);
      const r = c.r;
      const line = (x0, y0, x1, y1, m = MAT.LINE_YELLOW) => c.boxes.push({ x0, y0, x1, y1, z0: c.zf - 1, z1: c.zf - 1, m });
      for (let a = 26; a < L - 8; a += 26) {
        const [u0, v0] = c.cell(side, a, 4);
        const [u1, v1] = c.cell(side, a, c.sideDepth(side) - 5);
        line(Math.min(u0, u1), Math.min(v0, v1), Math.max(u0, u1), Math.max(v0, v1));
      }
      void r;
      const fire = c.room.fire;
      for (let a = 4; a + 22 < L; a += 26) {
        if (c.cars) c.free(fire ? "fireTruck" : "car", { side: OPP[side], a: a + 2, b: 6, exact: true, pad: 0 });
      }
      for (let k = 0; k < 3; k += 1) c.wall(fire ? "hoseRack" : "shelfUnit", { sides: [side === "N" ? "S" : "E"], at: "random", prefab: { metal: true } });
    },
    dorm(c) {
      for (const side of c.longSides().slice(0, 2)) {
        for (let k = 0; k < 4; k += 1) if (!c.wall("bunk", { sides: [side], at: "random", prefab: { double: true } })) break;
      }
      c.wall("lockers", { at: "random" });
    },
    council(c) {
      const side = farSide(c);
      c.wall("serviceCounter", { sides: [side], w: Math.min(30, c.sideLen(side) - 10), at: "center", prefab: { screen: false, top: MAT.WOOD_DARK } });
      rows(c, "seatRow", side, { b0: 16, da: 30, w: Math.min(24, c.sideLen(side) - 14), db: 7, prefab: { seat: MAT.LEATHER } });
      c.wall("plant", { at: "start" });
    },
    registry(c) {
      RULES.office(c);
      c.wall("shelfUnit", { at: "random" });
    },
    // ------------------------------------------------------------ museum / gallery
    exhibit(c) {
      // vitrines along the walls, display cases in rows, a skeleton in a big hall
      for (let k = 0; k < 6; k += 1) if (!c.wall("vitrine", { at: "random", keepDepth: 10 })) break;
      if (c.W >= 60 && c.H >= 50 && c.rng.chance(0.6)) c.free("skeleton", { side: c.W >= c.H ? "E" : "N", range: 8 });
      else c.free("sculpture", { range: 8 });
      for (let b = 14; b + 5 < c.H - 12; b += 16) for (let a = 14; a + 7 < c.W - 12; a += 18) c.free("displayCase", { a, b, exact: true });
      c.free("bench", { range: 20 });
    },
    gallery(c) {
      hangPaintings(c, 14);
      const n = c.area() > 1500 ? 3 : 1;
      for (let k = 0; k < n; k += 1) c.free("sculpture", { range: 20, a: Math.round(c.W * (0.3 + 0.2 * k)) });
      if (c.area() > 900) c.free("bench", { range: 6 });
    },
    museumShop(c) {
      for (let k = 0; k < 4; k += 1) if (!c.wall("goodsShelf", { at: "random", prefab: { goods: [MAT.BOOKS, MAT.GOODS, MAT.ART_BLUE] } })) break;
      c.wall("checkout", { at: "start" });
    },
    cloakroom(c) {
      const side = farSide(c);
      c.wall("serviceCounter", { sides: [side], w: Math.min(20, c.sideLen(side) - 8), at: "center", prefab: { screen: false } });
      c.free("clothesRack", { side, b: 1, range: 6 });
    },
    // ------------------------------------------------------------ venues
    auditorium(c) {
      // a stage at the far wall, rows of seats facing it, a mixing desk at the back
      const side = farSide(c);
      const L = c.sideLen(side);
      const d = Math.min(28, Math.max(12, Math.round(c.sideDepth(side) * 0.22)));
      c.wall("stage", { sides: [side], w: L - 4, d, at: "center", keepDepth: 8, prefab: { band: !c.room.classical, piano: !!c.room.classical, curtain: MAT.VELVET_RED } });
      const w = Math.min(36, Math.floor((L - 14) / 2));
      if (w >= 9) {
        rows(c, "seatRow", side, { a0: 4, b0: d + 8, da: w + 6, w, db: 6, bEnd: 14 });
      }
      c.free("mixingDesk", { side: OPP[side], b: 4, range: 10 });
    },
    venueFloor(c) {
      // a club: stage in a corner, bar along a side wall, standing room, a few tables
      const side = farSide(c);
      const L = c.sideLen(side);
      const d = Math.min(18, Math.max(10, Math.round(c.sideDepth(side) * 0.25)));
      c.wall("stage", { sides: [side], w: Math.min(L - 6, 44), d, at: "center", keepDepth: 8, prefab: { band: true, curtain: MAT.STAGE_BLACK } });
      for (const s2 of ["E", "W", "N", "S"]) {
        if (s2 === side || s2 === OPP[side]) continue;
        const w = Math.min(30, c.sideLen(s2) - d - 12);
        if (w >= 10 && c.wall("barCounter", { sides: [s2], w, at: "end" })) break;
      }
      for (let k = 0; k < 4; k += 1) c.free("cafeTable", { side: OPP[side], range: 10, a: 8 + k * 10, b: 6 });
    },
    cinema(c) {
      const side = farSide(c);
      const L = c.sideLen(side);
      c.wall("projectionScreen", { sides: [side], w: L - 8, at: "center", keepDepth: 8, prefab: { h: 26 } });
      const w = Math.min(40, L - 14);
      rows(c, "seatRow", side, { a0: Math.round((L - w) / 2), b0: 16, da: 100, w, db: 6, bEnd: 8 });
    },
    foyerBar(c) {
      const side = c.longSides()[0];
      c.wall("barCounter", { sides: [side], w: Math.min(28, c.sideLen(side) - 10), at: "center" });
      c.wall("bottleShelf", { sides: [OPP[side]], at: "center" });
      for (let k = 0; k < 5; k += 1) c.free("cafeTable", { range: 20, a: 8 + ((k * 13) % Math.max(8, c.W - 16)), b: 8 + ((k * 7) % Math.max(8, c.H - 16)) });
    },
    dressing(c) {
      for (let k = 0; k < 3; k += 1) if (!c.wall("barberChair", { at: "random" })) break;
      c.free("clothesRack", { range: 10 });
      c.wall("sofa", { at: "random" });
    },
    // ------------------------------------------------------------ supermarket / market
    sales(c) {
      // fridge cabinets along the back, aisles of gondolas, produce by the entrance, checkouts at the front
      const front = c.room.front ?? "N";
      const back = OPP[front];
      const L = c.sideLen(back);
      for (let a = 4; a + 10 < L - 4; a += 11) c.wall("fridgeCase", { sides: [back], at: a, keepDepth: 10 });
      const D = c.sideDepth(front);
      for (let a = 8; a + 12 < L - 8; a += 14) c.free("checkout", { side: front, a, b: 6, exact: true, pad: 1 });
      for (let a = 10; a + 16 < L - 10; a += 22) for (let b = 26; b + 5 < D - 16; b += 13) c.free("gondola", { side: front, a, b, exact: true, pad: 1 });
      c.free("produceStand", { side: front, a: 6, b: 18, range: 6 });
      c.free("freezerChest", { side: front, a: L - 20, b: 18, range: 6 });
      c.wall("trolleys", { sides: [front], at: "end" });
    },
    marketHall(c) {
      // stalls in rows: counters of bread, meat, fish, produce, flowers
      const side = c.W >= c.H ? "N" : "W";
      const L = c.sideLen(side);
      const D = c.sideDepth(side);
      const goods = [MAT.FISH, MAT.MEAT, MAT.BREAD, MAT.PRODUCE_RED, MAT.PRODUCE_GREEN, MAT.FLOWER_YELLOW];
      let k = 0;
      for (let b = 10; b + 6 < D - 10; b += 18)
        for (let a = 8; a + 16 < L - 8; a += 22) c.free("marketCounter", { side, a, b, exact: true, pad: 1, prefab: { goods: goods[k++ % goods.length] } });
    },
    // ------------------------------------------------------------ shops (ground-floor tenants)
    bakery(c) {
      const side = farSide(c);
      c.wall("shopCounter", { sides: [side], w: Math.min(20, c.sideLen(side) - 8), at: "center", prefab: { goods: MAT.BREAD } });
      c.wall("goodsShelf", { at: "random", prefab: { goods: [MAT.BREAD] } });
      c.free("cafeTable", { range: 10 });
    },
    butcher(c) {
      const side = farSide(c);
      c.wall("shopCounter", { sides: [side], w: Math.min(22, c.sideLen(side) - 8), at: "center", prefab: { goods: MAT.MEAT } });
      c.wall("fridgeCase", { at: "random" });
    },
    fishmonger(c) {
      const side = farSide(c);
      c.wall("shopCounter", { sides: [side], w: Math.min(22, c.sideLen(side) - 8), at: "center", prefab: { goods: MAT.FISH } });
      c.wall("fridgeCase", { at: "random" });
    },
    pharmacy(c) {
      const side = farSide(c);
      c.wall("serviceCounter", { sides: [side], w: Math.min(18, c.sideLen(side) - 10), at: "center", prefab: { screen: false, top: MAT.PLASTIC_WHITE } });
      for (let k = 0; k < 4; k += 1) if (!c.wall("goodsShelf", { at: "random", prefab: { frame: MAT.PLASTIC_WHITE, goods: [MAT.PLASTIC_WHITE, MAT.CURTAIN_BLUE, MAT.MEDICAL_GREEN] } })) break;
    },
    bookshop(c) {
      for (let k = 0; k < 6; k += 1) if (!c.wall("bookshelf", { at: "random" })) break;
      c.free("diningTable", { range: 8 });
      c.wall("checkout", { at: "start" });
    },
    clothing(c) {
      for (let k = 0; k < 4; k += 1) c.free("clothesRack", { range: 24, a: 6 + k * 12, b: Math.round(c.H / 2) });
      c.wall("dresser", { at: "random" });
      c.wall("checkout", { at: "start" });
    },
    florist(c) {
      for (let k = 0; k < 3; k += 1) c.wall("flowerBuckets", { at: "random" });
      c.wall("plant", { at: "start" });
      c.wall("plant", { at: "end" });
      c.wall("checkout", { at: "random" });
    },
    hardware(c) {
      for (let k = 0; k < 5; k += 1) if (!c.wall("shelfUnit", { at: "random", prefab: { metal: true, goods: MAT.METAL_PANEL_DARK } })) break;
      c.free("gondola", { range: 12 });
      c.wall("checkout", { at: "start" });
    },
    barber(c) {
      const side = c.longSides()[0];
      for (let k = 0; k < 3; k += 1) if (!c.wall("barberChair", { sides: [side], at: "random" })) break;
      c.wall("waitingChairs", { sides: [OPP[side]], w: 12, at: "start" });
    },
    pub(c) {
      const side = c.longSides()[0];
      c.wall("barCounter", { sides: [side], w: Math.min(28, c.sideLen(side) - 10), at: "center" });
      c.wall("bottleShelf", { sides: [OPP[side]], at: "center" });
      if (c.area() > 900) c.free("poolTable", { range: 12 });
      for (let k = 0; k < 5; k += 1) c.free("cafeTable", { range: 18, a: 6 + ((k * 11) % Math.max(8, c.W - 12)), b: 6 + ((k * 9) % Math.max(8, c.H - 12)) });
    },
    bank(c) {
      const side = farSide(c);
      c.wall("serviceCounter", { sides: [side], w: Math.min(24, c.sideLen(side) - 8), at: "center" });
      c.wall("atm", { sides: [OPP[side]], at: "start" });
      c.wall("plant", { at: "random" });
      c.free("bench", { range: 8 });
    },
    laundromat(c) {
      for (const side of c.longSides().slice(0, 2)) for (let k = 0; k < 5; k += 1) if (!c.wall("washer", { sides: [side], at: "start" })) break;
      c.free("bench", { range: 6 });
    },
    grocery(c) {
      for (let k = 0; k < 3; k += 1) if (!c.wall("fridgeCase", { at: "random" })) break;
      for (let k = 0; k < 4; k += 1) if (!c.wall("goodsShelf", { at: "random" })) break;
      c.free("produceStand", { range: 10 });
      c.wall("checkout", { at: "start" });
    },
    produkty(c) {
      // a Soviet-style grocery: long counters, goods on the shelves behind them
      const side = farSide(c);
      c.wall("goodsShelf", { sides: [side], at: "start", prefab: { goods: [MAT.BOTTLES, MAT.GOODS, MAT.BREAD] } });
      c.wall("goodsShelf", { sides: [side], at: "end", prefab: { goods: [MAT.BOTTLES, MAT.GOODS, MAT.BREAD] } });
      c.free("marketCounter", { side, b: 6, range: 8, prefab: { goods: MAT.MEAT } });
      c.wall("checkout", { sides: [OPP[side]], at: "start" });
    },
    souvenir(c) {
      for (let k = 0; k < 4; k += 1) if (!c.wall("goodsShelf", { at: "random", prefab: { goods: [MAT.ART_BLUE, MAT.ART_CRIMSON, MAT.GOODS, MAT.BOOKS] } })) break;
      c.free("clothesRack", { range: 10 });
      c.wall("checkout", { at: "start" });
    },
    kiosk(c) {
      c.wall("serviceCounter", { at: "center", w: Math.min(14, c.sideLen("N") - 6), prefab: { screen: false } });
      c.wall("goodsShelf", { at: "random", prefab: { goods: [MAT.BOOKS, MAT.GOODS, MAT.BOTTLES] } });
      c.wall("fridgeCase", { at: "random" });
    },
    // ------------------------------------------------------------ hotel / culture
    hotelRoom(c) {
      RULES.bedroom(c);
      c.wall("tvUnit", { at: "random" });
      c.wall("armchair", { at: "random" });
    },
    clubroom(c) {
      c.free("meetingTable", { w: Math.max(10, Math.min(c.W - 16, 30)), d: Math.max(6, Math.min(c.H - 16, 10)), range: 4 });
      c.wall("bookshelf", { at: "random" });
      if (c.rng.chance(0.4)) c.wall("tvUnit", { at: "random" });
    },
    danceHall(c) {
      const side = c.longSides()[0];
      c.wall("danceBarre", { sides: [side], w: c.sideLen(side) - 10, at: "center", keepDepth: 4 });
      c.free("grandPiano", { side: OPP[side], b: 4, a: 6, range: 6 });
    },
    // ------------------------------------------------------------ warehouses
    selfStorage(c) {
      const side = c.W >= c.H ? "N" : "W";
      const L = c.sideLen(side);
      const D = c.sideDepth(side);
      for (let b = 10; b + 12 < D - 8; b += 22) c.free("storageUnits", { side, a: 8, b, w: L - 16, exact: true });
    },
    coldStore(c) {
      RULES.warehouse(c);
      for (let k = 0; k < 3; k += 1) c.free("freezerChest", { range: 30 });
    },
    distribution(c) {
      const side = c.W >= c.H ? "N" : "W";
      const L = c.sideLen(side);
      const D = c.sideDepth(side);
      c.free("conveyor", { side, a: 10, b: 14, w: L - 20, exact: true });
      for (let b = 28; b + 9 < D - 10; b += 34) for (let a = 10; a + 22 < L - 10; a += 26) c.free("rack", { side, a, b, exact: true, pad: 0, prefab: { h: 34 } });
      for (let k = 0; k < 8; k += 1) c.free("crates", { range: 40, a: c.rng.int(4, Math.max(5, L - 12)), b: c.rng.int(4, Math.max(5, D - 12)) });
    },
    timberYard(c) {
      const side = c.W >= c.H ? "N" : "W";
      const L = c.sideLen(side);
      const D = c.sideDepth(side);
      for (let b = 10; b + 8 < D - 8; b += 16) for (let a = 8; a + 20 < L - 8; a += 26) c.free("lumberStack", { side, a, b, exact: true });
    },
    // ------------------------------------------------------------ churches
    orthodoxNave(c) {
      // an icon screen across the east end, candle stands, no pews
      const side = farSide(c);
      c.wall("icon", { sides: [side], w: c.sideLen(side) - 4, at: "center", keepDepth: 6 });
      for (let k = 0; k < 3; k += 1) c.free("candleStand", { range: 14 });
    },
  };
  return R;
}

/** Extra prefabs used by the rules above. */
export const RULE_PREFABS = {
  marketCounter: {
    w: 16,
    d: 6,
    free: true,
    pad: 3,
    build(rng, opt = {}) {
      const goods = opt.goods ?? MAT.PRODUCE_GREEN;
      return [
        { a0: 0, a1: 15, b0: 0, b1: 5, z0: 0, z1: 5, m: MAT.WOOD_MED },
        { a0: 0, a1: 15, b0: 0, b1: 5, z0: 6, z1: 6, m: goods },
        { a0: 0, a1: 0, b0: 0, b1: 0, z0: 7, z1: 16, m: MAT.WOOD_DARK },
        { a0: 15, a1: 15, b0: 0, b1: 0, z0: 7, z1: 16, m: MAT.WOOD_DARK },
        { a0: 0, a1: 15, b0: 0, b1: 5, z0: 17, z1: 17, m: rng.pick([MAT.AWNING_RED, MAT.AWNING_GREEN, MAT.AWNING_BLUE, MAT.AWNING_STRIPE]) },
      ];
    },
  },
  candleStand: {
    w: 3,
    d: 3,
    free: true,
    pad: 3,
    build() {
      return [
        { a0: 1, a1: 1, b0: 1, b1: 1, z0: 0, z1: 6, m: MAT.GOLD },
        { a0: 0, a1: 2, b0: 0, b1: 2, z0: 7, z1: 7, m: MAT.GOLD },
        { a0: 0, a1: 2, b0: 0, b1: 2, z0: 8, z1: 8, m: MAT.LAMP_LIGHT },
      ];
    },
  },
};
