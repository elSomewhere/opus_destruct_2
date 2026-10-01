import { vx } from "../core/units.js";
import { Rng } from "../core/hash.js";
import { resolveStyle } from "./styles.js";
import { MAT } from "../voxel/materials.js";

/**
 * Facade rules shared by every LOD. A facade is described per building side
 * by a bay grid (window centers every `bay` voxels, aligned to that side's
 * length) and per floor by a window band (sill..head). Interior planners
 * align partition walls to the same bay grid, so windows seen from far away
 * are the windows you find from inside.
 */

export const WIN = {
  WALL: 0,
  GLASS: 1,
  FRAME: 2,
  SILL: 3,
  LINTEL: 4,
  STOREFRONT: 5,
  SPANDREL: 6,
  JOINT: 7,
  // nordic styles: board seams, window casings, corner boards (trim)
  SEAM: 8,
  CASING: 9,
  CORNER: 10,
};

const styleCache = new WeakMap();

/** Per-building resolved style + facade parameters (cached on the envelope). */
export function buildingLook(env, seed) {
  let look = styleCache.get(env);
  if (look) return look;
  const rng = Rng.from(seed, env.id, "look");
  const style = resolveStyle(env.style, rng);
  const w = style.window;
  const bay = Math.max(vx(w.bay), vx(1.0));
  look = {
    style,
    bay,
    winW: Math.min(vx(w.width), bay - 2),
    sill: vx(w.sill) + 2,
    head: vx(w.head) + 2,
    type: w.type,
    lintel: w.lintel,
    storefront: env.program.ground === "retail" || env.program.ground === "officeLobby" || !!env.storefront,
    litSeed: rng.int(0, 1 << 30),
    joint: style.joint,
    casing: !!w.casing,
    transom: !!w.transom,
    // any of the nordic wall treatments (seams, corner boards)
    boards: !!style.seam || style.corners,
  };
  styleCache.set(env, look);
  return look;
}

/**
 * Classify a facade voxel.
 * @param look   buildingLook()
 * @param len    length of this facade side (voxels)
 * @param t      position along the side (0..len-1)
 * @param zr     height above the floor slab bottom (0 = slab bottom)
 * @param H      story height (voxels)
 * @param floor  floor index (0 = ground)
 */
export function facadeCell(look, len, t, zr, H, floor) {
  const cls = facadeCellBase(look, len, t, zr, H, floor);
  return cls === WIN.WALL ? wallClass(look, len, t, zr) : cls;
}

/** Plain wall cell, or its board seam / corner board in the nordic styles. */
export function wallClass(look, len, t, zr) {
  if (!look.boards) return WIN.WALL;
  const st = look.style;
  if (st.corners && (t < 2 || t >= len - 2)) return WIN.CORNER;
  if (st.seam && (st.seamDir === "h" ? zr : t) % 3 === 0) return WIN.SEAM;
  return WIN.WALL;
}

function facadeCellBase(look, len, t, zr, H, floor) {
  if (zr < 2) return WIN.SPANDREL; // slab edge band
  if (floor === 0 && look.storefront) {
    if (zr < 3) return WIN.WALL;
    if (zr >= H - 4) return WIN.LINTEL;
    const m = t % vx(3.0);
    if (m === 0 || t < 3 || t > len - 4) return WIN.FRAME;
    return WIN.STOREFRONT;
  }
  const bay = look.bay;
  const nb = Math.max(1, Math.floor(len / bay));
  const margin = Math.floor((len - nb * bay) / 2);
  const tt = t - margin;
  if (tt < 0 || tt >= nb * bay) return WIN.WALL;
  const inBay = tt % bay;
  if (look.type === "open") {
    if (zr >= look.sill && zr < look.head) return inBay < 3 ? WIN.FRAME : WIN.GLASS;
    if (zr === look.sill - 1) return WIN.SILL;
    return WIN.WALL;
  }
  if (look.type === "curtain") {
    if (zr >= H - 2) return WIN.SPANDREL;
    if (inBay === 0) return WIN.FRAME;
    return WIN.GLASS;
  }
  const w0 = Math.floor((bay - look.winW) / 2);
  const inWin = inBay >= w0 && inBay < w0 + look.winW;
  // prefab panels: a joint line round every panel (one per bay and floor)
  if (look.joint && !inWin && (inBay === 0 || zr === 2)) return WIN.JOINT;
  if (look.joint && inWin && zr === 2) return WIN.JOINT;
  if (look.type === "ribbon") {
    if (zr >= look.sill && zr < look.head) return inBay === 0 ? WIN.FRAME : WIN.GLASS;
    return WIN.WALL;
  }
  if (!inWin) {
    // casing: a trim surround beside the window, from the sill to the lintel
    if (look.casing && (inBay === w0 - 1 || inBay === w0 + look.winW) && zr >= look.sill - 1 && zr <= look.head) return WIN.CASING;
    return WIN.WALL;
  }
  if (zr === look.sill - 1) return WIN.SILL;
  if (zr >= look.sill && zr < look.head) {
    const mid = w0 + Math.floor(look.winW / 2);
    if (look.winW >= 8 && inBay === mid) return WIN.FRAME;
    if (look.transom && zr === look.sill + Math.round((look.head - look.sill) * 0.62)) return WIN.FRAME;
    return WIN.GLASS;
  }
  if (zr === look.head && look.lintel) return WIN.LINTEL;
  return WIN.WALL;
}

/** Map a facade class to a material for this building. */
export function facadeMaterial(look, cls, floor, zr = 99) {
  const st = look.style;
  switch (cls) {
    case WIN.GLASS:
    case WIN.STOREFRONT:
      return st.glass;
    case WIN.FRAME:
      return floor === 0 && look.storefront ? st.storefront : st.frame;
    case WIN.SILL:
    case WIN.LINTEL:
      return st.trim;
    case WIN.JOINT:
      return st.joint ?? st.wall;
    case WIN.SEAM:
      if (floor === 0 && ((look.storefront && st.shopBase) || zr < st.baseH)) return st.base;
      return st.seam;
    case WIN.CASING:
      return st.trim;
    case WIN.CORNER:
      if (floor === 0 && zr < st.baseH) return st.base;
      return st.trim;
    case WIN.SPANDREL:
      if (st.accentStrips && floor > 0 && floor % 3 === 0 && zr === 1) return st.trim;
      return look.type === "curtain" ? st.frame : floor === 0 ? st.base : st.wall;
    default:
      if (floor === 0 && ((look.storefront && st.shopBase) || zr < st.baseH)) return st.base;
      return st.wall;
  }
}

export { MAT };
