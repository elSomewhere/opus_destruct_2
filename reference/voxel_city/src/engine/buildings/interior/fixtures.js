import { Rng } from "../../core/hash.js";
import { stairBoxes } from "./stairs.js";
import { OUT, ROOM0 } from "./grid.js";
import { floorZ } from "../archetypes.js";
import { furnishFloor } from "./furnish.js";
import { MAT } from "../../voxel/materials.js";
import { roofSnowCover } from "../massing.js";
import { civicDressing } from "./civicFixtures.js";

/**
 * Geometry boxes of a building (canonical u/v + absolute z), converted to
 * world boxes once and cached on the plan:
 *   building-wide: stairs, roof bulkheads, elevator overruns, roof gear,
 *                  entrance canopies / awnings, balconies
 *   per floor:     door frames + open leaves, furniture
 */

const DOOR_H = 17;

function matId(m) {
  return typeof m === "number" ? m : MAT[m] ?? MAT.CONCRETE;
}

function toWorld(frame, b) {
  if (frame.turned) {
    // a turned building (the angled world): the box stays canonical (`canon`),
    // its world bounds only cull; voxelize.stampBoxes tests every voxel's cell
    if (Math.ceil(b.x0) > Math.floor(b.x1) || Math.ceil(b.y0) > Math.floor(b.y1)) return null;
    const w = frame.rectToWorld(b);
    return { x0: w.x0, y0: w.y0, z0: b.z0, x1: w.x1, y1: w.y1, z1: b.z1, m: matId(b.m), mode: b.mode ?? 0, canon: { x0: b.x0, y0: b.y0, x1: b.x1, y1: b.y1 }, ...(b.iso ? { iso: true } : {}) };
  }
  const r = frame.rectToWorld(b);
  return { x0: r.x0, y0: r.y0, z0: b.z0, x1: r.x1, y1: r.y1, z1: b.z1, m: matId(b.m), mode: b.mode ?? 0, ...(b.iso ? { iso: true } : {}) };
}

export function buildingBoxes(world, plan, env, frame, look) {
  if (plan._boxes) return plan._boxes;
  const out = [];
  const add = (b) => {
    const q = toWorld(frame, b);
    if (q) out.push(q);
  };
  const rng = Rng.from(world.seed, env.id, "fixtures");
  const zRoof = floorZ(env, env.floors);
  const st = look.style;
  const snowyBulkheads = roofSnowCover(world, env) >= 0.5;

  for (const s of plan.stairs) {
    const house = s.open;
    const mats = {
      tread: house ? MAT.STAIR_WOOD : MAT.STAIR_CONCRETE,
      landing: house ? MAT.STAIR_WOOD : MAT.STAIR_CONCRETE,
      divider: house ? MAT.WOOD_PANEL : MAT.CONCRETE_LIGHT,
      rail: MAT.HANDRAIL_WOOD,
    };
    for (const b of stairBoxes(s, mats)) add(b);
    if (s.f1 === env.floors && env.roof.type === "flat") bulkhead(add, s, zRoof, st, snowyBulkheads);
  }
  for (const e of plan.elevators) {
    if (env.roof.type !== "flat") continue;
    const r = e.rect;
    add({ x0: r.x0 - 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1, z0: zRoof + 2, z1: zRoof + 22, m: st.wall });
    add({ x0: r.x0 - 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1, z0: zRoof + 23, z1: zRoof + 24, m: MAT.PARAPET_CAP });
  }
  if (env.roof.type === "flat") roofGear(add, plan, env, zRoof, rng, look);
  if (look.style.fireEscape && (env.archetype === "walkup" || env.archetype === "midrise") && env.floors >= 3) fireEscape(add, plan, env, rng);
  entranceDressing(add, plan, env, rng, look);
  civicDressing(add, plan, env, rng.fork("civic"));
  plan._boxes = out;
  return out;
}

function bulkhead(add, s, zRoof, st, snowy) {
  const r = s.rect;
  const Hb = 24;
  const ring = { x0: r.x0 - 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1 };
  const z0 = zRoof + 2;
  const z1 = zRoof + Hb - 1;
  add({ ...ring, z0, z1, m: st.wall });
  // hollow it out again
  add({ ...r, z0, z1, m: 0 });
  add({ ...ring, z0: zRoof + Hb, z1: zRoof + Hb + 1, m: MAT.ROOF_MEMBRANE });
  if (snowy) add({ ...ring, z0: zRoof + Hb + 1, z1: zRoof + Hb + 1, m: MAT.SNOW });
  // door at the near end wall
  const w = 8;
  if (s.axis === "v") {
    const vWall = s.dir > 0 ? ring.y0 : ring.y1;
    const uc = Math.round((r.x0 + r.x1) / 2);
    add({ x0: uc - w / 2, y0: vWall, x1: uc + w / 2 - 1, y1: vWall, z0, z1: z0 + DOOR_H - 1, m: 0 });
    add({ x0: uc - w / 2 - 1, y0: vWall, x1: uc - w / 2 - 1, y1: vWall, z0, z1: z0 + DOOR_H, m: MAT.DOOR_FRAME });
    add({ x0: uc + w / 2, y0: vWall, x1: uc + w / 2, y1: vWall, z0, z1: z0 + DOOR_H, m: MAT.DOOR_FRAME });
  } else {
    const uWall = s.dir > 0 ? ring.x0 : ring.x1;
    const vc = Math.round((r.y0 + r.y1) / 2);
    add({ x0: uWall, y0: vc - w / 2, x1: uWall, y1: vc + w / 2 - 1, z0, z1: z0 + DOOR_H - 1, m: 0 });
  }
}

function roofGear(add, plan, env, zRoof, rng, look) {
  const top = env.tiers[env.tiers.length - 1].rects[0];
  const inner = { x0: top.x0 + 6, y0: top.y0 + 6, x1: top.x1 - 6, y1: top.y1 - 6 };
  if (inner.x1 - inner.x0 < 24 || inner.y1 - inner.y0 < 24) return;
  const keepOut = [...plan.stairs.map((s) => s.rect), ...plan.elevators.map((e) => e.rect)].map((r) => ({
    x0: r.x0 - 12,
    y0: r.y0 - 12,
    x1: r.x1 + 12,
    y1: r.y1 + 12,
  }));
  const placed = [];
  const free = (r) =>
    !keepOut.some((k) => r.x0 <= k.x1 && k.x0 <= r.x1 && r.y0 <= k.y1 && k.y0 <= r.y1) &&
    !placed.some((k) => r.x0 - 4 <= k.x1 && k.x0 <= r.x1 + 4 && r.y0 - 4 <= k.y1 && k.y0 <= r.y1 + 4) &&
    r.x0 >= inner.x0 &&
    r.y0 >= inner.y0 &&
    r.x1 <= inner.x1 &&
    r.y1 <= inner.y1;
  const z = zRoof + 2;
  const area = (inner.x1 - inner.x0) * (inner.y1 - inner.y0);
  const n = Math.min(14, Math.round(area / 1400));
  for (let k = 0; k < n * 3 && placed.length < n; k += 1) {
    const kind = rng.weighted([["ac", 5], ["vent", 3], ["solar", env.style === "futurist" ? 4 : 1]]);
    const w = kind === "ac" ? rng.int(8, 12) : kind === "vent" ? 3 : rng.int(12, 20);
    const h = kind === "ac" ? rng.int(6, 8) : kind === "vent" ? 3 : rng.int(8, 10);
    const u0 = rng.int(inner.x0, inner.x1 - w);
    const v0 = rng.int(inner.y0, inner.y1 - h);
    const r = { x0: u0, y0: v0, x1: u0 + w - 1, y1: v0 + h - 1 };
    if (!free(r)) continue;
    placed.push(r);
    if (kind === "ac") {
      add({ ...r, z0: z, z1: z + 6, m: MAT.AC_UNIT });
      add({ x0: r.x0 + 2, y0: r.y0 + 2, x1: r.x1 - 2, y1: r.y1 - 2, z0: z + 7, z1: z + 7, m: MAT.VENT });
    } else if (kind === "vent") {
      add({ ...r, z0: z, z1: z + 5, m: MAT.VENT });
    } else {
      add({ ...r, z0: z + 2, z1: z + 2, m: MAT.SOLAR_PANEL });
      add({ x0: r.x0, y0: r.y0, x1: r.x0, y1: r.y0, z0: z, z1: z + 1, m: MAT.METAL_BLACK });
      add({ x0: r.x1, y0: r.y1, x1: r.x1, y1: r.y1, z0: z, z1: z + 1, m: MAT.METAL_BLACK });
    }
  }
  if (look.style.waterTank) {
    const r = { x0: inner.x1 - 26, y0: inner.y1 - 26, x1: inner.x1 - 6, y1: inner.y1 - 6 };
    if (free(r)) {
      for (const [a, b] of [
        [r.x0 + 2, r.y0 + 2],
        [r.x1 - 2, r.y0 + 2],
        [r.x0 + 2, r.y1 - 2],
        [r.x1 - 2, r.y1 - 2],
      ])
        add({ x0: a, y0: b, x1: a, y1: b, z0: z, z1: z + 16, m: MAT.STEEL_BEAM });
      add({ x0: r.x0 + 2, y0: r.y0 + 2, x1: r.x1 - 2, y1: r.y1 - 2, z0: z + 16, z1: z + 16, m: MAT.STEEL_BEAM });
      // octagon-ish tank
      for (let dz = 17; dz <= 38; dz += 1) {
        add({ x0: r.x0 + 4, y0: r.y0 + 1, x1: r.x1 - 4, y1: r.y1 - 1, z0: z + dz, z1: z + dz, m: MAT.WATER_TANK_WOOD });
        add({ x0: r.x0 + 1, y0: r.y0 + 4, x1: r.x1 - 1, y1: r.y1 - 4, z0: z + dz, z1: z + dz, m: MAT.WATER_TANK_WOOD });
      }
      for (let dz = 39; dz <= 44; dz += 1) {
        const c = dz - 38;
        add({ x0: r.x0 + 1 + c * 1.4, y0: r.y0 + 1 + c * 1.4, x1: r.x1 - 1 - c * 1.4, y1: r.y1 - 1 - c * 1.4, z0: z + dz, z1: z + dz, m: MAT.ROOF_SHINGLE });
      }
    }
  }
}

/** Direction (du, dv) from a door towards the outside, for exterior doors. */
function outward(grid, d) {
  if (d.orient === "h") return grid.get(d.u0, d.v0 - 1) === OUT ? [0, -1] : [0, 1];
  return grid.get(d.u0 - 1, d.v0) === OUT ? [-1, 0] : [1, 0];
}

function entranceDressing(add, plan, env, rng, look) {
  const g = plan.floorByIndex.get(0);
  if (!g) return;
  const grid = g.grid;
  const z0 = g.z;
  const awningMats = [MAT.AWNING_RED, MAT.AWNING_GREEN, MAT.AWNING_BLUE, MAT.AWNING_BLACK];
  for (const d of grid.doors) {
    if (d.b !== -1 || d.kind === "balcony") continue;
    const [du, dv] = outward(grid, d);
    const top = z0 + 2 + (d.height ?? DOOR_H) + 2;
    const ext = d.kind === "shopfront" ? 10 : d.kind === "rollup" ? 0 : 8;
    if (!ext) continue;
    const pad = d.kind === "shopfront" ? 8 : 3;
    let r;
    if (d.orient === "h") {
      const vOut = dv < 0 ? d.v0 - 1 : d.v1 + 1;
      r = { x0: d.u0 - pad, x1: d.u1 + pad, y0: dv < 0 ? vOut - ext + 1 : vOut, y1: dv < 0 ? vOut : vOut + ext - 1 };
    } else {
      const uOut = du < 0 ? d.u0 - 1 : d.u1 + 1;
      r = { y0: d.v0 - pad, y1: d.v1 + pad, x0: du < 0 ? uOut - ext + 1 : uOut, x1: du < 0 ? uOut : uOut + ext - 1 };
    }
    if (d.kind === "shopfront" && rng.chance(look.style.awning)) {
      const m = rng.pick(awningMats);
      add({ ...r, z0: top, z1: top, m });
      // sloped edge: drop one voxel at the outer rim
      const rim =
        d.orient === "h"
          ? { ...r, y0: dv < 0 ? r.y0 : r.y1, y1: dv < 0 ? r.y0 : r.y1 }
          : { ...r, x0: du < 0 ? r.x0 : r.x1, x1: du < 0 ? r.x0 : r.x1 };
      add({ ...rim, z0: top - 2, z1: top - 1, m: MAT.AWNING_STRIPE });
    } else if (d.kind === "entrance" && env.archetype !== "house" && !env.porch && !env.steeple) {
      add({ ...r, z0: top, z1: top + 1, m: look.style.frame });
    }
  }
}

/** Per-floor boxes: door frames/leaves + furniture. Cached on the floor record. */
export function floorBoxes(world, plan, F, env, frame, look) {
  if (F._boxes) return F._boxes;
  const out = [];
  const add = (b) => {
    const q = toWorld(frame, b);
    if (q) out.push(q);
  };
  const grid = F.grid;
  const z0 = F.z;
  const rng = Rng.from(world.seed, env.id, "floor", F.index);
  for (const d of grid.doors) {
    doorBoxes(add, grid, d, z0, rng, look);
    if (d.kind === "balcony" && F.index >= 1) balconyBoxes(add, grid, d, F, look);
  }
  if (F.index === 0) entranceSteps(add, grid, F, env, look.style.trim);
  // (furniture: isolated voxels to a physics host, ANGLED_WORLD_PLAN.md §4.6)
  for (const b of furnishFloor(world, plan, F, env, rng)) add({ ...b, iso: true });
  F._boxes = out;
  return out;
}

const LEAF = {
  wood: MAT.DOOR_WOOD,
  glass: MAT.DOOR_GLASS,
  metal: MAT.DOOR_METAL,
  white: MAT.DOOR_WHITE,
};

function doorBoxes(add, grid, d, z0, rng, look) {
  const H = d.height ?? DOOR_H;
  // (a door raised to its street: frame and leaf over its sill, plan.js streetLevels)
  const zb = z0 + 2 + (d.sill ?? 0);
  const exterior = d.b === -1;
  const frameM = exterior ? look.style.frame : MAT.DOOR_FRAME;
  if (d.kind === "elevator") {
    add({ x0: d.u0, y0: d.v0, x1: d.u1, y1: d.v1, z0: zb, z1: zb + H - 1, m: MAT.ELEVATOR_DOOR });
    return;
  }
  // frame posts + lintel
  if (d.kind !== "opening" || exterior) {
    if (d.orient === "h") {
      add({ x0: d.u0 - 1, y0: d.v0, x1: d.u0 - 1, y1: d.v1, z0: zb, z1: zb + H, m: frameM });
      add({ x0: d.u1 + 1, y0: d.v0, x1: d.u1 + 1, y1: d.v1, z0: zb, z1: zb + H, m: frameM });
      add({ x0: d.u0 - 1, y0: d.v0, x1: d.u1 + 1, y1: d.v1, z0: zb + H, z1: zb + H, m: frameM });
    } else {
      add({ x0: d.u0, y0: d.v0 - 1, x1: d.u1, y1: d.v0 - 1, z0: zb, z1: zb + H, m: frameM });
      add({ x0: d.u0, y0: d.v1 + 1, x1: d.u1, y1: d.v1 + 1, z0: zb, z1: zb + H, m: frameM });
      add({ x0: d.u0, y0: d.v0 - 1, x1: d.u1, y1: d.v1 + 1, z0: zb + H, z1: zb + H, m: frameM });
    }
  }
  if (d.leaf === "none" || d.kind === "opening" || d.sill) return;
  if (d.leaf === "rollup") {
    add({ x0: d.u0, y0: d.v0, x1: d.u1, y1: d.v1, z0: zb + H - 2, z1: zb + H - 1, m: d.color ?? MAT.ROLLUP_DOOR });
    return;
  }
  let leafM = LEAF[d.leaf] ?? MAT.DOOR_WOOD;
  if (d.kind === "entry") leafM = rng.pick([MAT.DOOR_WOOD, MAT.DOOR_WHITE, MAT.DOOR_RED, MAT.DOOR_GREEN, MAT.DOOR_WOOD]);
  // open leaf resting flat against the wall beside the opening
  const r = grid.doorLeafRect(d);
  if (r) add({ ...r, z0: zb, z1: zb + H - 2, m: leafM });
}

/**
 * Steps outside exterior doors whose floor sits above the ground outside
 * (stoops, raised ground floors, back doors to the garden).
 */
export function entranceSteps(add, grid, F, env, rail = MAT.FRAME_WHITE) {
  const floorTop = F.z + 1;
  for (const d of grid.doors) {
    if (d.b !== -1 || d.kind === "balcony" || d.kind === "rollup") continue;
    const [du, dv] = outward(grid, d);
    // a door raised to a street above its floor: steps down inside instead
    if (d.sill) {
      insideSteps(add, d, [-du, -dv], F.z, d.sill);
      continue;
    }
    // (from the street in front of this door where it is off the building's level: plan.js streetLevels)
    const ground = d.street ?? env.groundZ;
    const D = floorTop - ground;
    if (D < 2) continue;
    if (env.porch && d.kind === "entrance" && dv < 0 && porch(add, grid, d, floorTop, env, rail)) continue;
    const steps = D - 1;
    for (let k = 1; k <= steps; k += 1) {
      const top = floorTop - k;
      const n0 = 2 * (k - 1);
      const n1 = 2 * k - 1;
      let r;
      if (d.orient === "h") {
        const vFace = dv < 0 ? d.v0 - 1 : d.v1 + 1;
        r = { x0: d.u0 - 1, x1: d.u1 + 1, y0: dv < 0 ? vFace - n1 : vFace + n0, y1: dv < 0 ? vFace - n0 : vFace + n1 };
      } else {
        const uFace = du < 0 ? d.u0 - 1 : d.u1 + 1;
        r = { y0: d.v0 - 1, y1: d.v1 + 1, x0: du < 0 ? uFace - n1 : uFace + n0, x1: du < 0 ? uFace - n0 : uFace + n1 };
      }
      add({ ...r, z0: ground + 1, z1: top, m: MAT.LIMESTONE });
    }
    // landing in front of the door at floor level
    let land;
    if (d.orient === "h") {
      const vFace = dv < 0 ? d.v0 - 1 : d.v1 + 1;
      land = { x0: d.u0 - 1, x1: d.u1 + 1, y0: vFace, y1: vFace };
    } else {
      const uFace = du < 0 ? d.u0 - 1 : d.u1 + 1;
      land = { y0: d.v0 - 1, y1: d.v1 + 1, x0: uFace, x1: uFace };
    }
    void land;
  }
}

/**
 * Steps down inside a door raised by `sill` voxels over its floor (at
 * z0): a landing at the sill's level, then one voxel lower every two cells
 * into the room ([du, dv] into it), down to the floor.
 */
function insideSteps(add, d, [du, dv], z0, sill) {
  for (let k = 0; k < sill; k += 1) {
    const n0 = 2 * k;
    const n1 = 2 * k + 1;
    let r;
    if (d.orient === "h") {
      const vFace = dv > 0 ? d.v1 + 1 : d.v0 - 1;
      r = { x0: d.u0, x1: d.u1, y0: dv > 0 ? vFace + n0 : vFace - n1, y1: dv > 0 ? vFace + n1 : vFace - n0 };
    } else {
      const uFace = du > 0 ? d.u1 + 1 : d.u0 - 1;
      r = { y0: d.v0, y1: d.v1, x0: du > 0 ? uFace + n0 : uFace - n1, x1: du > 0 ? uFace + n1 : uFace - n0 };
    }
    add({ ...r, z0: z0 + 2, z1: z0 + 1 + sill - k, m: MAT.LIMESTONE });
  }
}

export { ROOM0 };

/**
 * Wooden porch in front of a front door (cabins): a deck at floor level
 * across part of the facade, a painted railing (top rail on balusters) round
 * its open sides except at the steps, and wooden steps down to the ground in
 * front of the door.
 */
function porch(add, grid, d, floorTop, env, railM) {
  const ground = env.groundZ;
  const depth = 12;
  const uc = Math.round((d.u0 + d.u1) / 2);
  const half = Math.round(d.width / 2) + 10;
  const x0 = Math.max(0, uc - half);
  const x1 = Math.min(grid.U - 1, uc + half);
  const y1 = d.v0 - 1;
  const y0 = y1 - depth + 1;
  const wood = MAT.WOOD_WEATHERED;
  add({ x0, x1, y0, y1, z0: ground + 1, z1: floorTop, m: wood });
  const zr = floorTop + 7;
  const rail = (r) => {
    add({ ...r, z0: zr, z1: zr, m: railM, mode: 1 });
    for (let u = r.x0; u <= r.x1; u += 2) for (let v = r.y0; v <= r.y1; v += 2) add({ x0: u, x1: u, y0: v, y1: v, z0: floorTop + 1, z1: zr - 1, m: railM, mode: 1 });
  };
  rail({ x0, x1: x0, y0, y1: y1 - 1 });
  rail({ x0: x1, x1, y0, y1: y1 - 1 });
  rail({ x0, x1: Math.max(x0, uc - 8), y0, y1: y0 });
  rail({ x0: Math.min(x1, uc + 8), x1, y0, y1: y0 });
  // steps: one voxel down per two voxels out, centred on the door
  for (let k = 1; k < floorTop - ground; k += 1) {
    const v1 = y0 - 1 - 2 * (k - 1);
    add({ x0: uc - 7, x1: uc + 7, y0: v1 - 1, y1: v1, z0: ground + 1, z1: floorTop - k, m: wood });
  }
  return true;
}

/** Balcony slab + railing outside a balcony door. */
function balconyBoxes(add, grid, d, F, look) {
  const [du, dv] = outward(grid, d);
  const depth = 10;
  const half = Math.round(d.width / 2) + 8;
  const z = F.z;
  const railM = look.type === "curtain" || look.style.id === "concrete" || look.style.id === "futurist" ? MAT.GLASS : MAT.RAILING;
  const slabM = look.style.id === "brick" || look.style.id === "deco" ? MAT.TRIM_STONE : MAT.CONCRETE_LIGHT;
  let slab;
  let rails;
  if (d.orient === "h") {
    const uc = Math.round((d.u0 + d.u1) / 2);
    const vFace = dv < 0 ? d.v0 - 1 : d.v1 + 1;
    const v0 = dv < 0 ? vFace - depth + 1 : vFace;
    const v1 = dv < 0 ? vFace : vFace + depth - 1;
    slab = { x0: uc - half, x1: uc + half, y0: v0, y1: v1 };
    const vOut = dv < 0 ? v0 : v1;
    rails = [
      { x0: uc - half, x1: uc + half, y0: vOut, y1: vOut },
      { x0: uc - half, x1: uc - half, y0: v0, y1: v1 },
      { x0: uc + half, x1: uc + half, y0: v0, y1: v1 },
    ];
  } else {
    const vc = Math.round((d.v0 + d.v1) / 2);
    const uFace = du < 0 ? d.u0 - 1 : d.u1 + 1;
    const u0 = du < 0 ? uFace - depth + 1 : uFace;
    const u1 = du < 0 ? uFace : uFace + depth - 1;
    slab = { y0: vc - half, y1: vc + half, x0: u0, x1: u1 };
    const uOut = du < 0 ? u0 : u1;
    rails = [
      { y0: vc - half, y1: vc + half, x0: uOut, x1: uOut },
      { y0: vc - half, y1: vc - half, x0: u0, x1: u1 },
      { y0: vc + half, y1: vc + half, x0: u0, x1: u1 },
    ];
  }
  add({ ...slab, z0: z, z1: z + 1, m: slabM, mode: 1 });
  for (const r of rails) add({ ...r, z0: z + 2, z1: z + 9, m: railM, mode: 1 });
  for (const r of rails.slice(0, 1)) add({ ...r, z0: z + 10, z1: z + 10, m: MAT.RAILING, mode: 1 });
}

/** Zig-zag steel fire escape on the front facade of older walk-ups. */
function fireEscape(add, plan, env, rng) {
  const g0 = plan.floorByIndex.get(0)?.grid;
  const ent = g0?.doors.find((d) => d.b === -1 && d.kind === "entrance");
  const W = 24;
  const D = 10;
  // pick a bay away from the entrance
  const U = env.U;
  let u0 = rng.chance(0.5) ? Math.round(U * 0.12) : Math.round(U * 0.66);
  if (ent && Math.abs(u0 + W / 2 - (ent.u0 + ent.u1) / 2) < W) u0 = u0 < U / 2 ? Math.round(U * 0.66) : Math.round(U * 0.12);
  u0 = Math.max(2, Math.min(U - W - 2, u0));
  const m = MAT.RAILING;
  for (let f = 1; f < env.floors; f += 1) {
    const zf = floorZ(env, f) + 1; // walking level of the platform (top voxel)
    const plat = { x0: u0, x1: u0 + W - 1, y0: -D, y1: -1 };
    add({ ...plat, z0: zf, z1: zf, m: MAT.STEEL_BEAM, mode: 1 });
    add({ x0: u0, x1: u0 + W - 1, y0: -D, y1: -D, z0: zf + 1, z1: zf + 8, m, mode: 1 });
    add({ x0: u0, x1: u0, y0: -D, y1: -1, z0: zf + 1, z1: zf + 8, m, mode: 1 });
    add({ x0: u0 + W - 1, x1: u0 + W - 1, y0: -D, y1: -1, z0: zf + 1, z1: zf + 8, m, mode: 1 });
    if (f + 1 < env.floors) {
      const H = env.storyH[f];
      const up = (f & 1) === 1;
      for (let k = 1; k < H; k += 1) {
        const a = Math.round(((W - 3) * k) / H);
        const uu = up ? u0 + 1 + a : u0 + W - 2 - a;
        add({ x0: uu, x1: uu, y0: -D + 1, y1: -D + 4, z0: zf + k, z1: zf + k, m: MAT.STEEL_BEAM, mode: 1 });
      }
    }
  }
  // drop ladder under the first platform
  const z1 = floorZ(env, 1) + 1;
  for (let z = z1 - 18; z < z1; z += 3) add({ x0: u0 + 2, x1: u0 + 5, y0: -D + 1, y1: -D + 1, z0: z, z1: z, m: MAT.STEEL_BEAM, mode: 1 });
}
