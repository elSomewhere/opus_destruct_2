import { MAT } from "../voxel/materials.js";
import { vx } from "../core/units.js";
import { hash32 } from "../core/hash.js";
import { Frame, frameOf } from "../buildings/frame.js";
import { industrySurface } from "./industry.js";
import { parkSurface } from "./parks.js";

/**
 * Ground surfaces of lots and open spaces (yards, driveways, plazas, parks,
 * parking lots, sports courts). Pure functions of the plan records, so they
 * can be sampled per column at any LOD.
 */

function envFrame(env) {
  return frameOf(env);
}

function inRects(rects, u, v) {
  for (const r of rects) if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return true;
  return false;
}

/**
 * @returns {number} material id for the lot surface at world column (x, y)
 */
export function lotSurface(lot, env, x, y, seed, hx = x, hy = y) {
  // (hx, hy: the position for texture hashes, canonical in a wrapping world)
  if (!env) {
    const h = hash32(Math.floor(hx / 16), Math.floor(hy / 16), seed, 5);
    return (h & 7) === 0 ? MAT.GRAVEL : (h & 7) === 1 ? MAT.DIRT : MAT.GRASS_DRY;
  }
  const f = envFrame(env);
  const [u, v] = f.fromWorld(x, y);
  const ground = env.tiers[0].rects;
  if (inRects(ground, u, v)) return MAT.CONCRETE;
  for (const a of env.annexes) {
    const r = a.canon;
    if (r ? u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1 : x >= a.world.x0 && x <= a.world.x1 && y >= a.world.y0 && y <= a.world.y1) return MAT.CONCRETE;
  }
  const entU = env.entranceU ?? Math.floor(env.U / 2);
  const arch = env.archetype;
  if (arch === "cabin") {
    // a clearing of forest floor and moss, a gravel path from the porch
    if (v < 0 && Math.abs(u - entU) <= vx(0.6)) return MAT.GRAVEL;
    return (hash32(Math.floor(hx / 12), Math.floor(hy / 12), seed, 6) & 3) === 0 ? MAT.MOSS : MAT.FOREST_FLOOR;
  }
  if (arch === "church") {
    // churchyard: a gravel path to the tower door and round the church, grass between the graves
    if (v < 0 && Math.abs(u - entU) <= vx(1.25)) return MAT.GRAVEL;
    if (u > -vx(1.5) && u < env.U + vx(1.5) && v > -vx(1.5) && v < env.V + vx(1.5)) return MAT.GRAVEL;
    return MAT.GRASS_DRY;
  }
  if (arch === "house") {
    // driveway from the garage straight to the street
    for (const a of env.annexes) {
      if (a.kind !== "garage") continue;
      const g = a.canon ?? f.rectFromWorld(a.world);
      if (u >= g.x0 + 2 && u <= g.x1 - 2 && v < g.y0) return MAT.CONCRETE_LIGHT;
    }
    if (v < 0 && Math.abs(u - entU) <= vx(0.6)) return MAT.PAVER_GRAY;
    if (v > env.V && v < env.V + vx(3.5) && Math.abs(u - entU) < vx(2.5)) return MAT.PAVER_RED;
    return flowerBed(u, v, env) ?? MAT.GRASS_LAWN;
  }
  if (arch === "rowhouse") {
    if (v < 0) return Math.abs(u - entU) <= vx(0.75) ? MAT.PAVER_GRAY : MAT.GRASS_LAWN;
    return (hash32(Math.floor(u / 12), Math.floor(v / 12), seed) & 3) === 0 ? MAT.PAVER_RED : MAT.GRASS_LAWN;
  }
  if (arch === "school") {
    // paved forecourt, schoolyard with a court behind the building, lawn at the sides
    if (v < 0) return Math.abs(u - entU) < vx(3) ? MAT.PAVER_GRAY : MAT.GRASS_LAWN;
    if (v > env.V + vx(2)) {
      const yv = v - env.V - vx(2);
      const court = u > vx(4) && u < Math.min(env.U - vx(4), vx(32)) && yv > vx(2) && yv < vx(17);
      if (court) {
        const cu = u - vx(4);
        const cv = yv - vx(2);
        const edge = cu < 1 || cv < 1 || cu > Math.min(env.U - vx(8), vx(28)) - 1 || cv > vx(15) - 1 || Math.abs(cv - vx(7.5)) < 0.5;
        return edge ? MAT.LINE_WHITE : MAT.COURT_ORANGE;
      }
      return MAT.ASPHALT;
    }
    return MAT.GRASS_LAWN;
  }
  if (arch === "warehouse" || arch === "factory") {
    if (v < 0) {
      // front parking with stall lines
      const su = ((u % 20) + 20) % 20;
      return su === 0 ? MAT.LINE_WHITE : MAT.ASPHALT;
    }
    return MAT.CONCRETE_DARK;
  }
  if (arch === "townhouse" || arch === "wharfhouse") {
    // old-town plots: flagstones in front, behind the house a cobbled or
    // gravel court, or a little kitchen garden with beds
    if (v < 0) return MAT.FLAGSTONE;
    const kind = hash32(env.R.x0, env.R.y0, seed, 21) % 3;
    if (kind === 0) return cobble(hx, hy);
    if (kind === 1) return (hash32(Math.floor(hx / 10), Math.floor(hy / 10), seed, 22) & 7) === 0 ? MAT.GRASS : MAT.GRAVEL;
    const back = v - env.V;
    if (back > vx(2) && ((u >> 3) & 1) === 0 && Math.abs(u - env.U / 2) < env.U * 0.35) return MAT.SOIL_BED;
    return back < vx(1.5) ? MAT.GRAVEL : MAT.GRASS_LAWN;
  }
  if (env.civic) return civicYard(env, u, v, hx, hy, seed);
  if (arch === "office" || arch === "tower") {
    const a = ((x % 16) + 16) % 16;
    const b = ((y % 16) + 16) % 16;
    return a === 0 || b === 0 ? MAT.PLAZA_STONE_DARK : MAT.PLAZA_STONE;
  }
  // apartments: paved front, green courtyard behind
  if (v < 0) return MAT.PAVER_GRAY;
  const cu = ((u % 48) + 48) % 48;
  if (cu < 8) return MAT.PAVER_GRAY;
  return MAT.GRASS_LAWN;
}

/**
 * Forecourts and yards of civic buildings: a supermarket's car park, a
 * petrol station's asphalt, an engine apron, a stone forecourt in front of
 * museums and town halls, a drive-up in front of a hospital, lawn round the
 * sides.
 */
function civicYard(env, u, v, hx, hy, seed) {
  const c = env.civic;
  if (v >= 0) {
    // sides and back: service yard behind shops and stations, lawn elsewhere
    if (c === "supermarket" || c === "petrolStation" || c === "fireStation" || c === "policeStation" || c === "marketHall") return v > env.V ? MAT.ASPHALT_WORN : MAT.PAVER_GRAY;
    return (hash32(Math.floor(hx / 12), Math.floor(hy / 12), seed, 31) & 7) === 0 ? MAT.GRASS : MAT.GRASS_LAWN;
  }
  const entU = env.entranceU ?? Math.floor(env.U / 2);
  switch (c) {
    case "supermarket": {
      // rows of stalls with drive aisles, a walkway to the doors
      if (Math.abs(u - entU) < vx(1.5)) return MAT.PAVER_GRAY;
      const row = Math.floor(-v / vx(6));
      const inRow = -v % vx(6);
      if (row % 2 === 1) return MAT.ASPHALT;
      if (inRow === 0) return MAT.LINE_WHITE;
      return (((u % 20) + 20) % 20) === 0 ? MAT.LINE_WHITE : MAT.ASPHALT_WORN;
    }
    case "petrolStation":
      return (hash32(Math.floor(hx / 24), Math.floor(hy / 24), seed, 32) & 7) === 0 ? MAT.ASPHALT_PATCH : MAT.ASPHALT;
    case "fireStation":
      return Math.abs(v + vx(0.5)) < 2 ? MAT.HAZARD_YELLOW : MAT.CONCRETE;
    case "policeStation":
    case "hospital":
    case "polyclinic":
      return Math.abs(u - entU) < vx(3) ? MAT.PAVER_GRAY : v > -vx(4) ? MAT.PAVER_GRAY : MAT.ASPHALT;
    case "museum":
    case "townHall":
    case "concertHall":
    case "houseOfCulture":
    case "library": {
      const a = ((hx % 16) + 16) % 16;
      const b = ((hy % 16) + 16) % 16;
      return a === 0 || b === 0 ? MAT.PLAZA_STONE_DARK : MAT.PLAZA_STONE;
    }
    default:
      return MAT.PAVER_GRAY;
  }
}

function flowerBed(u, v, env) {
  if (v < -vx(1.0) && v > -vx(2.2) && (u < env.U * 0.35 || u > env.U * 0.65) && u > 4 && u < env.U - 4) {
    return ((u >> 2) & 1) === 0 ? MAT.FLOWER_RED : MAT.FLOWER_YELLOW;
  }
  return null;
}

/**
 * Open-space surfaces: returns { mat, dz, water }.
 */
export function spaceSurface(space, x, y, out, hx = x, hy = y) {
  const r = space.rect;
  out.dz = 0;
  out.water = false;
  const w = r.x1 - r.x0;
  const h = r.y1 - r.y0;
  const u = x - r.x0;
  const v = y - r.y0;
  switch (space.kind) {
    case "plaza": {
      // a paved city plaza: large slabs in a running bond, a border band,
      // a basin off-centre, rows of tree beds along two sides
      const k = hash32(r.x0, r.y0, 0x71a) >>> 0;
      const fu = w * (0.35 + 0.3 * ((k & 255) / 255));
      const fv = h * (0.35 + 0.3 * (((k >>> 8) & 255) / 255));
      const d = Math.hypot(u - fu, v - fv);
      if (d < vx(4)) {
        out.mat = d < vx(3.4) ? MAT.WATER : MAT.GRANITE_LIGHT;
        out.water = d < vx(3.4);
        out.dz = d < vx(3.4) ? 0 : 2;
        return out;
      }
      const e = Math.min(u, v, w - u, h - v);
      if (e < vx(1.2)) {
        out.mat = MAT.PLAZA_STONE_DARK;
        return out;
      }
      // tree beds every 8 m, 3 m in from the long sides
      const long = w >= h;
      const along = long ? u : v;
      const across = long ? Math.min(v, h - v) : Math.min(u, w - u);
      if (Math.abs(across - vx(4)) < vx(0.9) && ((along % vx(8)) + vx(8)) % vx(8) < vx(1.8)) {
        out.mat = MAT.TREE_PIT;
        return out;
      }
      const row = Math.floor(v / 12);
      const off = row & 1 ? 12 : 0;
      const joint = v % 12 === 0 || (u + off) % 24 === 0;
      out.mat = joint ? MAT.SIDEWALK_JOINT : (hash32(Math.floor((hx + off) / 24), row, 0x71b) & 7) === 0 ? MAT.PLAZA_STONE_DARK : MAT.PLAZA_STONE;
      return out;
    }
    case "quay": {
      // a paved harbour apron: concrete slabs with joints, worn tracks
      const ju = u % vx(6);
      const jv = v % vx(6);
      out.mat = ju === 0 || jv === 0 ? MAT.CONCRETE_DARK : (hash32(Math.floor(hx / vx(6)), Math.floor(hy / vx(6)), 913) & 15) === 0 ? MAT.CONCRETE_LIGHT : MAT.CONCRETE;
      return out;
    }
    case "parking": {
      if (u < vx(1.5) || v < vx(1.5) || w - u < vx(1.5) || h - v < vx(1.5)) {
        out.mat = MAT.GRASS_LAWN;
        return out;
      }
      const row = Math.floor((v - vx(1.5)) / vx(12));
      const inRow = (v - vx(1.5)) % vx(12);
      if (inRow >= vx(5) && inRow < vx(7)) {
        out.mat = MAT.ASPHALT;
        return out;
      }
      const su = (u - vx(1.5)) % 20;
      out.mat = su === 0 ? MAT.LINE_WHITE : row % 2 ? MAT.ASPHALT : MAT.ASPHALT_WORN;
      return out;
    }
    case "sports": {
      const m = vx(3);
      if (u < m || v < m || w - u < m || h - v < m) {
        out.mat = MAT.GRASS_LAWN;
        return out;
      }
      const iu = u - m;
      const iv = v - m;
      const cw = w - 2 * m;
      const ch = h - 2 * m;
      const line = iu < 2 || iv < 2 || cw - iu < 2 || ch - iv < 2 || Math.abs(iu - cw / 2) < 1;
      out.mat = line ? MAT.LINE_WHITE : MAT.SPORT_COURT_GREEN;
      return out;
    }
    case "courtyard": {
      // parking along the block edge, then lawns crossed by footpaths;
      // worn patches of bare earth (more of them in the projects)
      const e = Math.min(u, v, w - u, h - v);
      if (e < vx(6)) {
        const along = e === u || e === w - u ? v : u;
        out.mat = e < vx(0.5) ? MAT.CURB : e < vx(5.5) && ((along % 20) + 20) % 20 === 0 ? MAT.LINE_WHITE : MAT.ASPHALT_WORN;
        return out;
      }
      if (e < vx(7.5)) {
        out.mat = MAT.PAVER_GRAY;
        return out;
      }
      const pu = ((u + 90) % 360 + 360) % 360;
      const pv = ((v + 90) % 360 + 360) % 360;
      if (pu < 18 || pv < 18) {
        out.mat = MAT.ASPHALT_WORN;
        return out;
      }
      const h1 = hash32(Math.floor(hx / 24), Math.floor(hy / 24), 911) & 15;
      const bleak = space.district === "projects";
      out.mat = h1 < (bleak ? 4 : 2) ? MAT.DIRT : bleak && h1 < 8 ? MAT.GRASS_DRY : MAT.GRASS_LAWN;
      return out;
    }
    case "square": {
      // market square (torg): cobbles, a flagstone border along the houses,
      // a ring of dark setts round the monument in the middle
      // (market square: cobbles, a flagstone border and a flagstone walk
      // across it, a granite plinth round the monument)
      const e = Math.min(u, v, w - u, h - v);
      const d = Math.hypot(u - w / 2, v - h / 2);
      if (e < vx(1.5)) out.mat = MAT.FLAGSTONE;
      else if (d < vx(2.5)) out.mat = MAT.GRANITE_LIGHT;
      else if (Math.abs((w >= h ? v - h / 2 : u - w / 2)) < vx(1.2)) out.mat = MAT.FLAGSTONE;
      else out.mat = cobble(hx, hy);
      return out;
    }
    case "garden": {
      // a small town garden: lawn, a gravel cross and a round bed in the middle, beds along the edge
      const e = Math.min(u, v, w - u, h - v);
      const d = Math.hypot(u - w / 2, v - h / 2);
      if (e < vx(1.2)) out.mat = MAT.HEDGE;
      else if (e < vx(2.4)) out.mat = (Math.floor(hx / 6) + Math.floor(hy / 6)) & 1 ? MAT.FLOWER_RED : MAT.SOIL_BED;
      else if (d < vx(2.5)) out.mat = d < vx(1.6) ? MAT.FLOWER_YELLOW : MAT.SOIL_BED;
      else if (Math.abs(u - w / 2) < vx(0.8) || Math.abs(v - h / 2) < vx(0.8) || Math.abs(d - vx(3.2)) < vx(0.7)) out.mat = MAT.GRAVEL;
      else out.mat = MAT.GRASS_LAWN;
      return out;
    }
    case "cemetery":
      cemeterySurface(space, x, y, hx, hy, out);
      return out;
    case "allotments":
      allotmentSurface(space, x, y, hx, hy, out);
      return out;
    case "garages": {
      // a garage cooperative's yard: worn asphalt, gravel and puddles of mud
      const n = hash32(Math.floor(hx / 24), Math.floor(hy / 24), 931) & 15;
      out.mat = n < 5 ? MAT.GRAVEL : n < 7 ? MAT.MUD : n < 8 ? MAT.ASPHALT_PATCH : MAT.ASPHALT_WORN;
      return out;
    }
    case "wasteland": {
      // a vacant lot by the works: gravel, bare earth, rubble, dry grass and weeds
      const a = hash32(Math.floor(hx / 40), Math.floor(hy / 40), 941) & 7;
      const b = hash32(Math.floor(hx / 12), Math.floor(hy / 12), 942) & 7;
      out.mat = a < 2 ? (b < 3 ? MAT.GRAVEL : MAT.DIRT) : a < 4 ? (b < 2 ? MAT.CONCRETE_DARK : MAT.GRASS_DRY) : b < 2 ? MAT.MUD : b < 4 ? MAT.GRASS_DRY : MAT.GRASS;
      return out;
    }
    case "tankFarm":
    case "containerYard":
      industrySurface(space, x, y, out);
      return out;
    case "riverside": {
      // lawn with a paved promenade grid; the quay itself is cut by the river
      const pu = ((u % 96) + 96) % 96;
      const pv = ((v % 96) + 96) % 96;
      out.mat = pu < 12 || pv < 12 ? MAT.PLAZA_STONE : MAT.GRASS_LAWN;
      return out;
    }
    case "park":
    default:
      return parkSurface(space, u, v, out, hx, hy);
  }
}

/** Cobbles (setts of ~25 cm) in three shades, from a canonical position hash. */
export function cobble(hx, hy) {
  const k = hash32(Math.floor(hx / 2), Math.floor(hy / 2), 0xc0b) & 15;
  return k < 3 ? MAT.COBBLE_DARK : k < 5 ? MAT.COBBLE_LIGHT : MAT.COBBLE;
}

const spaceFrames = new WeakMap();
function spaceFrame(space) {
  let f = spaceFrames.get(space);
  if (!f) {
    f = new Frame(space.rect, space.front ?? "S");
    spaceFrames.set(space, f);
  }
  return f;
}

/**
 * Cemetery ground in its gate frame (u along the street, v inwards): a
 * gravel main path from the gate to the chapel, cross paths every ~16 m, a
 * path along the inside of the wall; grass between the rows of graves.
 */
function cemeterySurface(space, x, y, hx, hy, out) {
  const f = spaceFrame(space);
  const [u, v] = f.fromWorld(x, y);
  const U = f.U;
  const V = f.V;
  const e = Math.min(u, v, U - 1 - u, V - 1 - v);
  const main = Math.abs(u - (U - 1) / 2) < vx(1.4);
  const cross = v > vx(4) && ((v - vx(4)) % vx(16)) < vx(1.1);
  if (e < vx(2) && e >= vx(0.8)) out.mat = MAT.GRAVEL;
  else if (main || (cross && e > vx(2))) out.mat = MAT.GRAVEL;
  else out.mat = (hash32(Math.floor(hx / 10), Math.floor(hy / 10), 951) & 7) === 0 ? MAT.GRASS : MAT.GRASS_LAWN;
}

/** Allotment plots of ~8 x 11 m in rows off gravel paths (plot grid in the block's gate frame). */
export const ALLOT = { w: vx(8), d: vx(11), path: vx(1.5) };

function allotmentSurface(space, x, y, hx, hy, out) {
  const f = spaceFrame(space);
  const [u, v] = f.fromWorld(x, y);
  const pu = u % (ALLOT.w + ALLOT.path);
  const pv = v % (ALLOT.d + ALLOT.path);
  if (pu < ALLOT.path || pv < ALLOT.path) {
    out.mat = MAT.GRAVEL;
    return;
  }
  // each plot: beds in rows at the back, grass at the front (by the hut)
  const cu = Math.floor(u / (ALLOT.w + ALLOT.path));
  const cv = Math.floor(v / (ALLOT.d + ALLOT.path));
  const kind = hash32(cu, cv, 961) & 3;
  const iv = pv - ALLOT.path;
  if (kind !== 3 && iv > ALLOT.d * 0.45) {
    const row = Math.floor((pu - ALLOT.path) / 4);
    out.mat = row & 1 ? MAT.SOIL_BED : kind === 0 ? MAT.LEAVES_LIGHT : kind === 1 ? MAT.GRASS : MAT.SOIL_BED;
    return;
  }
  out.mat = (hash32(Math.floor(hx / 8), Math.floor(hy / 8), 962) & 7) === 0 ? MAT.GRASS : MAT.GRASS_LAWN;
}

