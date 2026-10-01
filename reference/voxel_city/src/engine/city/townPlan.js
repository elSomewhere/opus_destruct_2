import { hashFloat } from "../core/hash.js";
import { vx } from "../core/units.js";
import { DISTRICTS } from "../world/registry.js";
import { CIVIC, civicGroup } from "../buildings/civic.js";
import { flavorOf } from "./flavors.js";

/**
 * Town plan: the landmarks that give a town its character, placed once per
 * settlement from its centre, size, harbour and industrial side:
 *
 *   square      the market square (torg) near the centre, towards the
 *               harbour on an island
 *   church      the main church on its own block near the centre, on the
 *               highest ground of a few candidates (churches like hills)
 *   cemetery    on the edge of the town, away from the works and the harbour
 *   wharf       gabled warehouses along the harbour front (island towns)
 *   park, school, sports
 *   allotments  garden plots with huts on the fringe
 *   garages     rows of lock-up garages by the bleak estates and the works
 *   wasteland   a vacant lot of rubble and scrub by the works
 *
 * A landmark is a point with a kind; the nearest block that suits it (by
 * district and size, from the stage-1 cell networks, so there is no cycle
 * with the cell plans) takes that use (`useFor`). Everything is a pure
 * function of the settlement record and cached on it.
 */

/** Block programs a landmark kind turns into, with the districts and block sizes (m) it accepts. */
const KINDS = {
  square: { districts: ["oldtown", "mixed", "downtown", "midtown", "residential", "harbour"], min: 22, max: 120 },
  church: { districts: ["oldtown", "mixed", "midtown", "residential", "suburban", "downtown"], min: 20, max: 150, long: 30 },
  wharf: { districts: ["oldtown", "mixed", "harbour"], min: 20, max: 160 },
  cemetery: { districts: ["residential", "suburban", "mixed", "oldtown", "projects", "microdistrict"], min: 45, max: 400, long: 60 },
  park: { districts: ["oldtown", "mixed", "downtown", "midtown", "residential", "suburban", "microdistrict"], min: 35, max: 400 },
  school: { districts: ["mixed", "midtown", "residential", "suburban", "microdistrict", "projects", "oldtown"], min: 38, max: 400, long: 48 },
  sports: { districts: ["residential", "suburban", "microdistrict", "projects", "mixed", "industrial"], min: 45, max: 400, long: 70 },
  allotments: { districts: ["residential", "suburban", "projects", "microdistrict", "industrial"], min: 35, max: 400 },
  garages: { districts: ["projects", "microdistrict", "industrial", "residential", "suburban"], min: 28, max: 300 },
  wasteland: { districts: ["industrial", "heavyIndustry", "projects", "harbour"], min: 30, max: 400 },
};

/**
 * Civic buildings a town gets (buildings/civic.js): the districts a lot for
 * one may be carved from. The block must hold the archetype's lot (its
 * `lot` size, or at least its minimum footprint and setbacks).
 */
const CIVIC_DISTRICTS = {
  supermarket: ["residential", "suburban", "mixed", "projects", "microdistrict", "industrial"],
  departmentStore: ["downtown", "midtown", "mixed", "oldcore", "oldtown"],
  petrolStation: ["suburban", "residential", "industrial", "mixed", "projects", "microdistrict", "heavyIndustry"],
  hospital: ["residential", "suburban", "mixed", "midtown", "microdistrict"],
  polyclinic: ["microdistrict", "residential", "mixed", "projects", "suburban"],
  policeStation: ["mixed", "midtown", "residential", "downtown", "oldcore", "oldtown"],
  fireStation: ["mixed", "residential", "suburban", "industrial", "midtown", "oldtown"],
  museum: ["oldcore", "oldtown", "downtown", "midtown", "mixed"],
  artGallery: ["oldcore", "oldtown", "downtown", "midtown", "mixed"],
  concertHall: ["downtown", "midtown", "mixed", "oldcore"],
  houseOfCulture: ["mixed", "microdistrict", "downtown", "midtown", "oldtown"],
  cinema: ["downtown", "midtown", "mixed", "oldtown", "oldcore", "microdistrict"],
  library: ["mixed", "midtown", "oldcore", "oldtown", "residential"],
  townHall: ["oldcore", "oldtown", "downtown", "midtown", "mixed"],
  hotel: ["oldcore", "oldtown", "downtown", "midtown", "mixed", "harbour"],
  marketHall: ["oldtown", "oldcore", "harbour", "mixed", "downtown"],
  musicClub: ["mixed", "midtown", "oldcore", "industrial", "downtown", "oldtown"],
};
for (const [id, districts] of Object.entries(CIVIC_DISTRICTS)) {
  const c = CIVIC[id];
  const needW = c.w[0] + 2 * (c.setS?.[0] ?? 0);
  const needD = c.d[0] + c.setF[0] + 1;
  KINDS[id] = { districts, min: Math.min(needW, needD), long: Math.max(needW, needD), max: 9999, civic: true };
}

/** Landmark kind -> block use (cellPlan). */
export const LANDMARK_USE = {
  square: "square",
  church: "church",
  wharf: "wharf",
  cemetery: "cemetery",
  park: "park",
  school: "school",
  sports: "sports",
  allotments: "allotments",
  garages: "garages",
  wasteland: "wasteland",
};
for (const id of Object.keys(CIVIC_DISTRICTS)) LANDMARK_USE[id] = `civic:${id}`;

/** The landmarks of settlement `s` (world voxels), cached on it. Villages get none. */
export function townPlan(world, s) {
  if (s.plan) return s.plan;
  const plan = { anchors: [] };
  s.plan = plan;
  if (s.village) return plan;
  const seed = world.config.seed;
  // (hash the canonical centre: a wrapping world's laps share the plan)
  const hx = Math.round(s.cx ?? s.x);
  const hy = Math.round(s.cy ?? s.y);
  const rnd = (k) => hashFloat(seed, hx, hy, 8800 + k);
  const R = s.radius;
  const Rm = R / 8;
  const isl = world.fields.island;
  const onLand = (x, y, m = 40) => !isl || isl.coast(x / 8, y / 8) > m;
  const at = (ang, d) => [s.x + Math.cos(ang) * d * R, s.y + Math.sin(ang) * d * R];
  const add = (kind, x, y, pri) => {
    if (onLand(x, y)) plan.anchors.push({ kind, x: Math.round(x), y: Math.round(y), pri });
  };
  // the harbour (island towns) and the industrial side (fields.industryNoise)
  const h = isl && s.i === 0 && s.j === 0 ? isl.harbour() : null;
  const harbourAng = h ? Math.atan2(h.dy, h.dx) : null;
  const indAng = hashFloat(seed, s.i, s.j, 17) * Math.PI * 2;
  const angGap = (a, b) => Math.abs(Math.atan2(Math.sin(a - b), Math.cos(a - b)));
  // a direction away from the works and the water
  const quiet = (k) => {
    let best = null;
    for (let q = 0; q < 12; q += 1) {
      const a = ((q + rnd(k)) / 12) * Math.PI * 2;
      const score = angGap(a, indAng) + (harbourAng !== null ? angGap(a, harbourAng) : 1) + rnd(k + q + 1) * 0.6;
      if (!best || score > best.score) best = { a, score };
    }
    return best.a;
  };

  // market square: at the heart, pulled towards the harbour
  if (h) {
    const hd = Math.hypot(h.x * 8 - s.x, h.y * 8 - s.y) / R;
    const d = Math.min(0.28, Math.max(0.06, hd * 0.55));
    add("square", ...at(harbourAng, d), 0);
    // the wharf: gabled warehouses along the harbour front on either side of the square's axis
    for (const side of [-1, 1]) {
      const a = harbourAng + side * (0.35 + 0.25 * rnd(3 + side));
      add("wharf", ...at(a, Math.max(0.1, hd * 0.9)), 1);
    }
  } else add("square", ...at(rnd(1) * Math.PI * 2, 0.05 + 0.08 * rnd(2)), 0);
  // main church: the highest of a few spots near the centre
  {
    let best = null;
    for (let k = 0; k < 7; k += 1) {
      const [x, y] = at(rnd(10 + k) * Math.PI * 2, 0.12 + 0.22 * rnd(20 + k));
      if (!onLand(x, y, 80)) continue;
      const z = world.terrain.sample(x, y).h;
      if (!best || z > best.z) best = { x, y, z };
    }
    if (best) add("church", best.x, best.y, 1);
  }
  // cemetery on the quiet edge of town
  add("cemetery", ...at(quiet(30), 0.78 + 0.14 * rnd(31)), 2);
  // parks and schools through the town, more in a bigger one
  const nPark = 1 + Math.floor(Rm / 900);
  for (let k = 0; k < nPark; k += 1) add("park", ...at(rnd(40 + k) * Math.PI * 2, 0.32 + 0.35 * rnd(50 + k)), 3);
  const nSchool = Math.max(1, Math.round(Rm / 800));
  for (let k = 0; k < nSchool; k += 1) add("school", ...at(rnd(60 + k) * Math.PI * 2, 0.3 + 0.4 * rnd(70 + k)), 3);
  add("sports", ...at(quiet(80) + 1.4 + rnd(81), 0.62 + 0.25 * rnd(82)), 4);
  // allotment gardens out on the fringe, garages and a vacant lot by the works
  const nAllot = 1 + Math.floor(Rm / 1500);
  for (let k = 0; k < nAllot; k += 1) add("allotments", ...at(quiet(90 + k) + (rnd(95 + k) - 0.5) * 2.2, 0.86 + 0.14 * rnd(100 + k)), 5);
  add("garages", ...at(indAng + (rnd(110) - 0.5) * 1.1, 0.6 + 0.18 * rnd(111)), 5);
  add("wasteland", ...at(indAng + (rnd(120) - 0.5) * 0.9, 0.78 + 0.15 * rnd(121)), 6);
  civicAnchors(s, Rm, add, at, rnd, quiet, h ? harbourAng : null, plan);
  return plan;
}

/**
 * The civic buildings of a town, by its size and flavor group: a town
 * hall by the square, a museum, galleries, a concert hall (a house of
 * culture in Soviet and Karelian towns), cinemas, libraries, hotels and a
 * department store round the centre, police and fire stations, hospitals
 * (polyclinics), supermarkets further out, petrol stations on the main
 * roads at the edge, a market hall in a harbour town, music clubs.
 */
function civicAnchors(s, Rm, add, at, rnd, quiet, harbourAng, plan) {
  const group = civicGroup(flavorOf(s).id);
  const sq = plan.anchors.find((a) => a.kind === "square");
  const sqAng = sq ? Math.atan2(sq.y - s.y, sq.x - s.x) : rnd(200) * Math.PI * 2;
  let k = 300;
  const some = (kind, n, d0, d1, pri = 7, ang = null) => {
    for (let q = 0; q < n; q += 1) {
      const a = ang !== null ? ang + (rnd(k++) - 0.5) * 0.8 : rnd(k++) * Math.PI * 2;
      add(kind, ...at(a, d0 + (d1 - d0) * rnd(k++)), pri);
    }
  };
  const soviet = group === "soviet";
  some("townHall", 1, 0.04, 0.12, 6, sqAng);
  some("hotel", 1 + Math.floor(Rm / 1200), 0.05, 0.3);
  if (Rm >= 450) some("museum", 1 + Math.floor(Rm / 2500), 0.08, 0.35);
  if (Rm >= 800) some("artGallery", 1 + Math.floor(Rm / 2000), 0.1, 0.4);
  if (Rm >= 600) some(soviet ? "houseOfCulture" : "concertHall", 1 + Math.floor(Rm / 2500), 0.1, 0.35);
  if (Rm >= 450) some("cinema", 1 + Math.floor(Rm / 1800), 0.1, 0.45);
  if (Rm >= 450) some("library", 1 + Math.floor(Rm / 2000), 0.15, 0.45);
  if (Rm >= 600) some("musicClub", 1 + Math.floor(Rm / 1500), 0.15, 0.5);
  if (Rm >= 1200 || (soviet && Rm >= 500)) some("departmentStore", 1 + Math.floor(Rm / 2000), 0.05, 0.22);
  if (Rm >= 400) some("policeStation", 1 + Math.floor(Rm / 1800), 0.15, 0.45);
  if (Rm >= 400) some("fireStation", 1 + Math.floor(Rm / 2000), 0.3, 0.65);
  if (Rm >= 900 && !soviet) some("hospital", 1 + Math.floor(Rm / 2500), 0.35, 0.7, 7, quiet(310));
  if (Rm >= 450 && (soviet || Rm < 900)) some("polyclinic", 1 + Math.floor(Rm / 1200), 0.3, 0.7, 7, quiet(320));
  if (harbourAng !== null || Rm >= 1500) some("marketHall", 1, 0.05, 0.2, 7, harbourAng ?? sqAng);
  some("supermarket", 1 + Math.floor(Rm / 800), 0.35, 0.85, 8);
  some("petrolStation", 1 + Math.floor(Rm / 900), 0.55, 0.95, 8);
}

function rectDist(r, x, y) {
  const dx = Math.max(r.x0 - x, 0, x - r.x1);
  const dy = Math.max(r.y0 - y, 0, y - r.y1);
  return Math.hypot(dx, dy);
}

function suits(kind, block) {
  const k = KINDS[kind];
  if (!k.districts.includes(block.district)) return false;
  const p = block.prop;
  const w = (p.x1 - p.x0) / 8;
  const h = (p.y1 - p.y0) / 8;
  const lo = Math.min(w, h);
  const hi = Math.max(w, h);
  return lo >= k.min && hi <= k.max && hi >= (k.long ?? k.min);
}

/**
 * The block (id) each landmark of a town lands on: the nearest suitable
 * block within reach that no more important landmark took, or null.
 * Resolved for the whole town at once (in order of importance), cached.
 */
function resolveTown(world, plan) {
  if (plan.resolved) return;
  plan.resolved = true;
  const claimed = new Set();
  const order = plan.anchors.slice().sort((p, q) => p.pri - q.pri);
  // (a civic building keeps off the blocks a highway crosses)
  const crossed = (b) => !!world.highways && world.highways.corridorsNear(b.prop).some((c) => c.hitsRect(b.prop));
  for (const a of order) {
    const civic = !!KINDS[a.kind].civic;
    const reach = vx(a.kind === "cemetery" || a.kind === "allotments" || a.kind === "school" || civic ? 240 : 150);
    let best = null;
    for (const { i, j } of world.cellsOverlapping({ x0: a.x - reach, y0: a.y - reach, x1: a.x + reach, y1: a.y + reach })) {
      for (const b of world.cellNet(i, j).blocks) {
        if (claimed.has(b.id) || !DISTRICTS.has(b.district) || !suits(a.kind, b)) continue;
        if (civic && crossed(b)) continue;
        const d = rectDist(b.prop, a.x, a.y);
        if (d > reach) continue;
        if (!best || d < best.d || (d === best.d && b.id < best.b.id)) best = { b, d };
      }
    }
    a.block = best ? best.b.id : null;
    if (best) claimed.add(best.b.id);
  }
}

function resolve(world, a, plan) {
  resolveTown(world, plan);
  return a.block;
}

/**
 * The landmark use of a block, or null: the most important landmark of
 * any town nearby that lands on it.
 */
export function landmarkUse(world, block) {
  const p = block.prop;
  const pad = vx(250);
  let pick = null;
  for (const s of world.fields.settlementsIn({ x0: p.x0 - pad, y0: p.y0 - pad, x1: p.x1 + pad, y1: p.y1 + pad })) {
    const plan = townPlan(world, s);
    for (const a of plan.anchors) {
      if (Math.abs(a.x - (p.x0 + p.x1) / 2) > pad * 2 || Math.abs(a.y - (p.y0 + p.y1) / 2) > pad * 2) continue;
      if (resolve(world, a, plan) !== block.id) continue;
      if (!pick || a.pri < pick.pri) pick = a;
    }
  }
  return pick ? LANDMARK_USE[pick.kind] : null;
}
