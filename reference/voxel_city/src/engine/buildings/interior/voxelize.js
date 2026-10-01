import { MAT } from "../../voxel/materials.js";
import { P, P2 } from "../../voxel/chunk.js";
import { frameOf, voxelizeMassing, pitchedRoofOnly, roofSnowCover, snowAt } from "../massing.js";
import { buildingLook, facadeCell, facadeMaterial, wallClass, WIN } from "../facade.js";
import { OUT, EXT, WALL, DOOR, ROOM0 } from "./grid.js";
import { stairSlabOpen } from "./stairs.js";
import { buildingBoxes, floorBoxes } from "./fixtures.js";
import { floorZ } from "../archetypes.js";

/**
 * LOD0 building voxelizer: turns a BuildingPlan into voxels, column by
 * column, then stamps geometry boxes (stairs, doors, furniture, roof gear).
 *
 * Per floor and column (canonical cell label):
 *   slab      2 voxels: ceiling below + floor finish of the room above
 *   EXT       facade: outer layer = facade material with window recesses,
 *             inner layer = room paint with glass; windows follow the same
 *             bay grid as the massing LODs, suppressed behind partitions
 *   WALL      partition painted after the adjacent room (tiles in wet rooms)
 *   DOOR      opening (frames and leaves come from boxes)
 *   room      air, ceiling light panels on a grid
 */

const NO_WINDOW = new Set(["shaft", "elevator", "closet", "storage", "mechanical", "parking", "trash", "pantry"]);
const SMALL_WINDOW = new Set(["bath", "wc", "restroom", "laundry"]);
/** Rooms behind a shop window on a storefront ground floor. */
const STOREFRONT_ROOMS = new Set(["lobby", "reception", "door", "cafe", "restaurant", "retail", "foyer", "sales", "departmentFloor", "gallery", "marketHall", "museumShop", "foyerBar", "policeDesk", "waiting", "kiosk"]);
/** Big halls: ceiling lights on a wider grid. */
const WIDE_LIGHTS = new Set(["openOffice", "parking", "deck", "warehouse", "factory", "distribution", "selfStorage", "coldStore", "timberYard", "sales", "marketHall"]);
const DOOR_H = 17;

const matCache = new Map();
export function mat(name) {
  if (typeof name === "number") return name;
  let id = matCache.get(name);
  if (id === undefined) {
    id = MAT[name] ?? MAT.CONCRETE;
    matCache.set(name, id);
  }
  return id;
}

/** Per-grid lookup tables built once. */
function gridInfo(grid) {
  if (grid._info) return grid._info;
  const doorAt = new Map();
  for (const d of grid.doors) for (let v = d.v0; v <= d.v1; v += 1) for (let u = d.u0; u <= d.u1; u += 1) doorAt.set(u + v * grid.U, d);
  const roomPaint = grid.rooms.map((r) => mat(r.paint ?? "PAINT_WHITE"));
  const roomFloor = grid.rooms.map((r) => mat(r.floorMat ?? "FLOOR_CONCRETE"));
  grid._info = { doorAt, roomPaint, roomFloor };
  return grid._info;
}

function roomOf(grid, u, v) {
  const l = grid.get(u, v);
  return l >= ROOM0 ? grid.rooms[l - ROOM0] : null;
}

/** Nearest room across a wall cell (for paint / window decisions). */
function adjacentRoom(grid, u, v) {
  const n = [
    [1, 0],
    [-1, 0],
    [0, 1],
    [0, -1],
    [2, 0],
    [-2, 0],
    [0, 2],
    [0, -2],
    [3, 0],
    [-3, 0],
    [0, 3],
    [0, -3],
  ];
  let best = null;
  for (const [du, dv] of n) {
    const r = roomOf(grid, u + du, v + dv);
    if (!r) continue;
    if (!best || (r.type === "bath" || r.type === "wc" || r.type === "restroom") > (best.type === "bath" || best.type === "wc")) best = r;
    if (Math.abs(du) + Math.abs(dv) === 1) return best;
  }
  return best;
}

/** Facade info of an EXT cell: side, layer (0 outer, 1 inner), len, t. */
function extInfo(grid, u, v) {
  const outN = grid.get(u, v - 1) === OUT;
  const outS = grid.get(u, v + 1) === OUT;
  const outW = grid.get(u - 1, v) === OUT;
  const outE = grid.get(u + 1, v) === OUT;
  if (outN || outS || outW || outE) {
    return { side: outN ? "F" : outS ? "B" : outW ? "L" : "R", layer: 0 };
  }
  if (grid.get(u, v - 2) === OUT) return { side: "F", layer: 1 };
  if (grid.get(u, v + 2) === OUT) return { side: "B", layer: 1 };
  if (grid.get(u - 2, v) === OUT) return { side: "L", layer: 1 };
  if (grid.get(u + 2, v) === OUT) return { side: "R", layer: 1 };
  return { side: null, layer: 1 };
}

function footRectAt(grid, u, v) {
  for (const r of grid.footprint) if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return r;
  return grid.footprint[0];
}

export function voxelizeBuilding(world, env, chunk) {
  const plan = world.buildingPlan(env);
  if (!plan) {
    voxelizeMassing(world, env, chunk);
    return;
  }
  const box = chunk.worldBox;
  const b = env.bounds;
  if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1) return;
  if (env.topZ + 40 < box.z0 || env.bottomZ > box.z1) return;
  const frame = frameOf(env);
  const look = buildingLook(env, world.seed);
  const st = look.style;
  const d = chunk.data;
  const [i0, i1] = chunk.rangeX(Math.max(b.x0, box.x0), Math.min(b.x1, box.x1));
  const [j0, j1] = chunk.rangeY(Math.max(b.y0, box.y0), Math.min(b.y1, box.y1));
  const floors = plan.floors.filter((f) => f.z <= box.z1 + 2 && f.z + f.height + 8 >= box.z0);
  const zRoof = floorZ(env, env.floors);
  const topFloor = plan.floorByIndex.get(env.floors - 1);
  const snow = env.roof.type === "flat" ? roofSnowCover(world, env) : 0;
  const plinth = env.plinth ? plan.floorByIndex.get(0) : null;
  // (parts mode: a garage's ramps that are parts of their own are drawn in their lattices, garageRamps.js)
  const partRamps = env.rampParts && world.config.world.angles?.partsMode === "separate" ? env.rampParts : null;

  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j);
    for (let i = i0; i <= i1; i += 1) {
      const x = chunk.wx(i);
      const [u, v] = frame.fromWorld(x, y);
      if (u < 0 || v < 0 || u >= env.U || v >= env.V) continue;
      // raised ground floor on a stone plinth (cabins)
      if (plinth && plinth.grid.get(u, v) !== OUT) {
        const [k0, k1] = chunk.rangeZ(env.groundZ + 1, env.baseZ - 1);
        for (let k = k0; k <= k1; k += 1) d[i + j * P + k * P2] = st.base;
      }
      for (const F of floors) drawFloorColumn(chunk, d, i, j, F, u, v, plan, look, st, partRamps);
      // roof over the top floor (flat roofs; pitched roofs reuse the massing roof)
      if (topFloor && env.roof.type === "flat") drawFlatRoof(chunk, d, i, j, topFloor, u, v, zRoof, plan, st, env, snow && snowAt(world.seed, snow, x, y));
    }
  }
  if (env.roof.type !== "flat" || env.annexes.length) voxelizeRoofAndAnnexes(world, env, chunk);

  // geometry boxes
  const boxes = buildingBoxes(world, plan, env, frame, look);
  stampBoxes(chunk, boxes, box, frame);
  for (const F of floors) {
    if (F.z > box.z1 || F.z + F.height < box.z0) continue;
    stampBoxes(chunk, floorBoxes(world, plan, F, env, frame, look), box, frame);
  }
}

function stampBoxes(chunk, boxes, box, frame) {
  for (const q of boxes) {
    if (q.x1 < box.x0 || q.x0 > box.x1 || q.y1 < box.y0 || q.y0 > box.y1 || q.z1 < box.z0 || q.z0 > box.z1) continue;
    // (furniture: isolated voxels to the structvox export)
    chunk.isolating = !!q.iso;
    if (q.canon) stampTurned(chunk, frame, q);
    else chunk.fillBox(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode ?? 0);
  }
  chunk.isolating = false;
}

/**
 * The canonical-box rasterizer (ANGLED_WORLD_PLAN.md S3): a box of a turned
 * building fills every voxel whose canonical cell (the frame's exact map)
 * lies in it, as chunk.fillBox does in world space (mode 0 overwrite,
 * 1 only air, 2 only solid).
 */
function stampTurned(chunk, frame, q) {
  const [i0, i1] = chunk.rangeX(q.x0, q.x1);
  const [j0, j1] = chunk.rangeY(q.y0, q.y1);
  const [k0, k1] = chunk.rangeZ(q.z0, q.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const c = q.canon;
  const d = chunk.data;
  const iso = chunk.isolating ? chunk.iso : null;
  const mode = q.mode ?? 0;
  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j);
    for (let i = i0; i <= i1; i += 1) {
      const [u, v] = frame.fromWorld(chunk.wx(i), y);
      if (u < c.x0 || u > c.x1 || v < c.y0 || v > c.y1) continue;
      for (let k = k0; k <= k1; k += 1) {
        const idx = i + j * P + k * P2;
        if (mode === 0 || (mode === 1 && d[idx] === 0) || (mode === 2 && d[idx] !== 0)) {
          d[idx] = q.m;
          if (iso) iso[idx] = q.m;
        }
      }
    }
  }
}

function drawFloorColumn(chunk, d, i, j, F, u, v, plan, look, st, partRamps = null) {
  const grid = F.grid;
  const lab = grid.get(u, v);
  if (lab === OUT) return;
  if (F.ramps && lab >= ROOM0 && rampColumn(chunk, d, i, j, F, u, v, partRamps)) return;
  const info = gridInfo(grid);
  const z0 = F.z;
  let H = F.height;
  if (F.lowRegions) {
    for (const lr of F.lowRegions) {
      const r = lr.rect;
      if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) H = lr.height;
    }
  }
  const [k0, k1] = chunk.rangeZ(z0, z0 + H - 1);
  if (k0 > k1) return;
  const room = lab >= ROOM0 ? grid.rooms[lab - ROOM0] : null;

  // slab open over stairwells (except the near landing) and elevator shafts
  let slabOpen = false;
  if (room && room.type === "stair") {
    const s = plan.stairs[room.stair];
    slabOpen = stairSlabOpen(s, F.index, u, v);
  } else if (room && room.type === "elevator") {
    const e = plan.elevators[room.elevator];
    slabOpen = F.index > e.f0;
  } else if (room && room.type === "void") {
    // the upper half of a double-height hall
    slabOpen = true;
  }

  let ext = null;
  let side = null;
  let len = 0;
  let t = 0;
  let adjRoom = null;
  if (lab === EXT) {
    ext = extInfo(grid, u, v);
    side = ext.side;
    if (side) {
      const r = footRectAt(grid, u, v);
      if (side === "F" || side === "B") {
        len = r.x1 - r.x0 + 1;
        t = side === "F" ? u - r.x0 : r.x1 - u;
      } else {
        len = r.y1 - r.y0 + 1;
        t = side === "L" ? r.y1 - v : v - r.y0;
      }
      // room behind this facade cell
      const inward = side === "F" ? [0, 1] : side === "B" ? [0, -1] : side === "L" ? [1, 0] : [-1, 0];
      const depth = ext.layer === 0 ? 2 : 1;
      const l2 = grid.get(u + inward[0] * depth, v + inward[1] * depth);
      adjRoom = l2 >= ROOM0 ? grid.rooms[l2 - ROOM0] : null;
      if (!adjRoom && l2 === DOOR) adjRoom = { type: "door" };
    }
  }
  const door = lab === DOOR ? info.doorAt.get(u + v * grid.U) : null;
  const doorTop = door ? 2 + (door.height ?? DOOR_H) - 1 : 0;
  let wallPaint = MAT.PAINT_WHITE;
  if (lab === WALL || lab === DOOR || (ext && ext.layer === 1)) {
    const ar = lab === EXT ? adjRoom : adjacentRoom(grid, u, v);
    if (ar && ar.id !== undefined) wallPaint = info.roomPaint[ar.id];
  }
  const floorFinish = room ? info.roomFloor[room.id] : door ? floorForDoor(grid, door, info) : MAT.CONCRETE;
  const light = room && isLightCell(room, u, v);

  for (let k = k0; k <= k1; k += 1) {
    const zr = chunk.wz(k) - z0;
    let m = 0;
    if (zr < 2) {
      if (slabOpen) m = 0;
      else if (lab === EXT && ext.layer === 0) m = facadeMaterial(look, WIN.SPANDREL, F.index, zr);
      else m = zr === 0 ? MAT.CEILING : lab === WALL || lab === EXT ? MAT.CONCRETE : floorFinish;
    } else if (lab === EXT) {
      m = facadeVoxel(look, st, F, ext, len, t, zr, H, adjRoom, wallPaint);
    } else if (lab === WALL) {
      m = zr === 2 && room === null && wallPaint !== MAT.WALL_TILE_WHITE ? baseboardFor(wallPaint) : wallPaint;
    } else if (lab === DOOR) {
      // (a door raised to a street above its floor: the wall under its sill, the opening over it)
      const sill = door?.sill ?? 0;
      m = zr < 2 + sill ? st.wall : zr <= (sill ? Math.min(doorTop + sill, H - 3) : doorTop) ? 0 : wallPaint;
    } else if (room) {
      if (room.type === "shaft") m = MAT.CONCRETE_DARK;
      else if (light && zr === H - 1) m = MAT.CEILING_LIGHT;
      else m = 0;
    }
    d[i + j * P + k * P2] = m;
  }
}

/**
 * Ramp strip columns (parking garages): the floor's own ramp rising towards
 * +v to the next floor, the top of the ramp arriving from the floor below,
 * and curbs along both long edges that follow the slope (not those of
 * `partRamps`, floors whose ramps are parts, drawn in their own lattices).
 * Returns false when the column is not on a ramp.
 */
function rampColumn(chunk, d, i, j, F, u, v, partRamps = null) {
  let own = null;
  let lower = null;
  let rect = null;
  for (const r of F.ramps) {
    const q = r.rect;
    if (u < q.x0 || u > q.x1 || v < q.y0 || v > q.y1) continue;
    rect = q;
    // (a ramp that is a part of its own: the opening only)
    if (partRamps?.includes(r.f)) continue;
    if (r.f === F.index) own = r;
    else lower = r;
  }
  if (!rect) return false;
  const H = F.height;
  const [k0, k1] = chunk.rangeZ(F.z, F.z + H - 1);
  if (!own && !lower) {
    for (let k = k0; k <= k1; k += 1) d[i + j * P + k * P2] = 0;
    return true;
  }
  const t = (v - rect.y0) / Math.max(1, rect.y1 - rect.y0);
  const s = own ? 1 + Math.round(t * own.H) : -99;
  const sb = lower ? 1 + Math.round(t * lower.H) - lower.H : -99;
  const edge = u === rect.x0 || u === rect.x1;
  let curbTop = -1;
  if (edge) {
    curbTop = 8;
    if (s >= 0 && s < H) curbTop = Math.max(curbTop, s + 7);
    else if (s >= H) curbTop = H - 1;
    if (sb >= 0) curbTop = Math.max(curbTop, sb + 7);
  }
  const stripe = u === rect.x0 + 3 || u === rect.x1 - 3;
  for (let k = k0; k <= k1; k += 1) {
    const zr = chunk.wz(k) - F.z;
    let m = 0;
    if (zr === s || zr === sb) m = stripe && ((v >> 3) & 1) === 0 ? MAT.LINE_YELLOW : MAT.FLOOR_CONCRETE;
    else if (zr === s - 1 || zr === sb - 1) m = MAT.CONCRETE;
    if (edge && zr <= curbTop) m = zr === curbTop ? MAT.HAZARD_YELLOW : MAT.CONCRETE_LIGHT;
    d[i + j * P + k * P2] = m;
  }
  return true;
}

function baseboardFor(paint) {
  return paint === MAT.CONCRETE ? MAT.CONCRETE : MAT.BASEBOARD;
}

function floorForDoor(grid, door, info) {
  const a = door.a >= 0 ? door.a : door.b;
  return a >= 0 ? info.roomFloor[a] : MAT.CONCRETE;
}

function isLightCell(room, u, v) {
  // (a double-height hall is lit from the top of its void)
  if (room.type === "stair" || room.type === "elevator" || room.type === "shaft" || room.tall) return false;
  const r = room.rects[0];
  if (!r) return false;
  const w = r.x1 - r.x0 + 1;
  const h = r.y1 - r.y0 + 1;
  const sp = WIDE_LIGHTS.has(room.type) ? 24 : 28;
  const nu = Math.max(1, Math.round(w / sp));
  const nv = Math.max(1, Math.round(h / sp));
  const pu = w / nu;
  const pv = h / nv;
  const lu = (u - r.x0) % pu;
  const lv = (v - r.y0) % pv;
  const cu = pu / 2;
  const cv = pv / 2;
  return Math.abs(lu - cu) < 1.01 && Math.abs(lv - cv) < 1.01;
}

/** Facade voxel for an EXT cell at height zr. */
function facadeVoxel(look, st, F, ext, len, t, zr, H, adjRoom, wallPaint) {
  if (!ext.side) return ext.layer === 0 ? st.wall : wallPaint;
  const f = F.index < 0 ? -1 : F.index;
  if (f < 0) return ext.layer === 0 ? MAT.CONCRETE_DARK : MAT.CONCRETE;
  let cls = facadeCell(look, len, t, zr, H, F.mezzanine ? 1 : f);
  const type = adjRoom?.type;
  if (cls === WIN.GLASS || cls === WIN.STOREFRONT || cls === WIN.FRAME) {
    if (!adjRoom || NO_WINDOW.has(type) || adjRoom.noWindows) cls = WIN.WALL;
    else if (SMALL_WINDOW.has(type) && cls === WIN.GLASS && zr < look.sill + 5) cls = WIN.WALL;
    else if (cls === WIN.STOREFRONT && !(adjRoom.shop || STOREFRONT_ROOMS.has(type))) {
      cls = zr >= look.sill && zr < look.head ? WIN.GLASS : WIN.WALL;
    }
  } else if (cls === WIN.CASING && (!adjRoom || NO_WINDOW.has(type) || adjRoom.noWindows)) cls = WIN.WALL;
  // board seams / corner boards where a window was suppressed
  if (cls === WIN.WALL) cls = wallClass(look, len, t, zr);
  if (ext.layer === 0) {
    if (cls === WIN.GLASS || cls === WIN.STOREFRONT) return 0; // recess
    return facadeMaterial(look, cls, f, zr);
  }
  // inner layer
  if (cls === WIN.GLASS || cls === WIN.STOREFRONT) return st.glass;
  if (cls === WIN.FRAME) return facadeMaterial(look, cls, f, zr);
  if (cls === WIN.SILL) return MAT.TRIM_STONE;
  return wallPaint;
}

function drawFlatRoof(chunk, d, i, j, topFloor, u, v, zRoof, plan, st, env, snowy) {
  const grid = topFloor.grid;
  const lab = grid.get(u, v);
  if (lab === OUT) return;
  const [k0, k1] = chunk.rangeZ(zRoof, zRoof + 6);
  if (k0 > k1) return;
  const room = lab >= ROOM0 ? grid.rooms[lab - ROOM0] : null;
  let open = false;
  if (room?.type === "stair") open = stairSlabOpen(plan.stairs[room.stair], env.floors, u, v);
  if (room?.type === "elevator") open = true;
  const ext = lab === EXT ? extInfo(grid, u, v) : null;
  for (let k = k0; k <= k1; k += 1) {
    const zr = chunk.wz(k) - zRoof;
    let m = 0;
    if (zr < 2) {
      if (open && !ext) m = 0;
      else m = zr === 0 ? MAT.CEILING : ext ? st.wall : snowy ? MAT.SNOW : st.roof;
    } else if (ext) {
      if (zr < 6) m = st.wall;
      else if (zr === 6) m = MAT.PARAPET_CAP;
    }
    if (m || zr < 2) d[i + j * P + k * P2] = m;
  }
}

function voxelizeRoofAndAnnexes(world, env, chunk) {
  pitchedRoofOnly(world, env, chunk);
}
