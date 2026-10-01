import { Rng } from "../../core/hash.js";
import { tierRects, floorZ, floorHeight } from "../archetypes.js";
import { FloorGrid, ROOM0, DOOR, doorExtraOf } from "./grid.js";
import { chamferCut } from "../chamfer.js";
import { frameOf } from "../frame.js";
import { roadLevelAt } from "../../network/roadLevel.js";
import { localPointToWorld } from "../../core/obb.js";
import { planApartmentBuilding } from "./apartments.js";
import { planHouse } from "./houses.js";
import { planOfficeBuilding } from "./offices.js";
import { planIndustrial } from "./industrial.js";
import { planGarage } from "./garage.js";
import { planSchool } from "./school.js";
import { planCabin, planChurch } from "./cabins.js";
import { CIVIC_PLANNERS } from "./civicPrograms.js";

/**
 * BuildingPlan: the full interior of one building, produced lazily the first
 * time a LOD0 chunk needs it.
 *
 *   floors     [{index, z, height, grid, kind}] sorted by z (basements < 0)
 *   stairs     U-stairs with explicit flights [{z0, H}] and floor span
 *   ramps      straight ramps [{rect, f, H}] rising (+v) from floor f to f+1
 *   links      room connections that are not doors or stairs (ramps)
 *   elevators  shafts with their floor span
 *   issues     validation problems (unreachable rooms), expected empty
 *
 * Floors with identical layout share one FloorGrid instance (typical floors),
 * which keeps tall towers cheap.
 */
export class PlanBuilder {
  constructor(env, rng) {
    this.env = env;
    this.rng = rng;
    this.floors = [];
    this.stairs = [];
    this.elevators = [];
    this.ramps = [];
    this.links = [];
    this.extras = [];
    this.issues = [];
  }

  z(f) {
    return floorZ(this.env, f);
  }

  h(f) {
    return floorHeight(this.env, f);
  }

  newGrid(f) {
    const env = this.env;
    const rects = tierRects(env, f);
    // (a chamfered corner is cut off every floor from the ground floor up)
    const cut = env.chamfer && f >= 0 ? (u, v) => chamferCut(env.chamfer, env.U, u, v) : null;
    const g = new FloorGrid(env.U, env.V, rects, cut);
    g.doorExtra = doorExtraOf(this.env);
    return g;
  }

  addFloor(index, grid, kind, extra = {}) {
    const rec = { index, z: this.z(index), height: this.h(index), grid, kind, ...extra };
    this.floors.push(rec);
    return rec;
  }

  addStair(st) {
    st.id = this.stairs.length;
    if (!st.flights) {
      st.flights = [];
      for (let f = st.f0; f < st.f1; f += 1) st.flights.push({ f, z0: this.z(f), H: this.h(f) });
    }
    this.stairs.push(st);
    return st;
  }

  addRamp(r) {
    r.id = this.ramps.length;
    this.ramps.push(r);
    return r;
  }

  addElevator(el) {
    el.id = this.elevators.length;
    this.elevators.push(el);
    return el;
  }

  build() {
    this.floors.sort((a, b) => a.z - b.z || a.index - b.index);
    const byIndex = new Map();
    for (const f of this.floors) if (!byIndex.has(f.index)) byIndex.set(f.index, f);
    // ramps a floor has to draw: its own (rising from it) and the one arriving from below
    for (const f of this.floors) {
      const rs = this.ramps.filter((r) => r.f === f.index || r.f + 1 === f.index);
      if (rs.length) f.ramps = rs;
    }
    const plan = {
      env: this.env,
      floors: this.floors,
      floorByIndex: byIndex,
      stairs: this.stairs,
      ramps: this.ramps,
      links: this.links,
      elevators: this.elevators,
      extras: this.extras,
      issues: this.issues,
    };
    validatePlan(plan);
    return plan;
  }
}

const PLANNERS = {
  walkup: planApartmentBuilding,
  midrise: planApartmentBuilding,
  tower: (ctx) => (ctx.env.program.upper === "apartments" ? planApartmentBuilding(ctx) : planOfficeBuilding(ctx)),
  office: planOfficeBuilding,
  house: planHouse,
  rowhouse: planHouse,
  warehouse: planIndustrial,
  factory: planIndustrial,
  barn: planIndustrial,
  panelSlab: planApartmentBuilding,
  panelTower: planApartmentBuilding,
  garage: planGarage,
  school: planSchool,
  // nordic: town houses are one family house or small flats (shop below)
  townhouse: (ctx) => (ctx.env.program.upper === "house" ? planHouse(ctx) : planApartmentBuilding(ctx)),
  wharfhouse: planApartmentBuilding,
  cabin: planCabin,
  church: planChurch,
  // civic buildings, venues and big shops (civicPrograms.js)
  ...CIVIC_PLANNERS,
};

export function planBuilding(world, env) {
  const rng = Rng.from(world.seed, env.id, "interior");
  const pb = new PlanBuilder(env, rng);
  const planner = PLANNERS[env.archetype];
  if (!planner) return null;
  planner({ env, rng, pb, world });
  const plan = pb.build();
  streetLevels(world, env, plan);
  return plan;
}

/** Most a street door's sill is raised above its floor (voxels): steeper, the door stays as it is. */
const SILL_MAX = 8;

/**
 * The street in front of each ground-floor door to it (a building on a
 * slope: one floor level, a street climbing along its front): where it is
 * off the building's own level (`env.groundZ`, the street's in front of the
 * lot's middle) by more than a voxel, the door records it (`d.street`, the
 * top of the sidewalk or road 6 cells out, as cellPlan streetDoorLevels
 * measures it). Where it stands higher than a walker can step up from the
 * floor, the door is raised to it (`d.sill`, voxels over the floor, steps
 * down inside, fixtures.js); where it lies lower, the steps up outside
 * start from it (entranceSteps).
 */
function streetLevels(world, env, plan) {
  const F = plan.floorByIndex.get(0);
  if (!F || !world.roadView || !world.cellAt) return;
  const frame = frameOf(env);
  for (const d of F.grid.doors) {
    if (d.b !== -1 || d.kind === "balcony" || d.kind === "window") continue;
    // (out from the wall the door is in, 6 cells)
    let u = (d.u0 + d.u1 + 1) / 2;
    let v = (d.v0 + d.v1 + 1) / 2;
    if (d.orient === "h") v = d.v0 <= 1 ? d.v0 - 6 : d.v1 + 7;
    else u = d.u0 <= 1 ? d.u0 - 6 : d.u1 + 7;
    const [x, y] = frame.pointToWorld ? frame.pointToWorld(u, v) : localPointToWorld(frame.placement, u, v);
    const c = world.cellAt(x, y);
    const r = roadLevelAt(world, world.roadView(c.i, c.j), x, y, 24);
    if (!r) continue;
    const L = Math.round(r.z) + (r.sidewalk ? 1 : 0);
    if (Math.abs(L - env.groundZ) <= 1) continue;
    d.street = L;
    // (feet on the street L + 1, on the floor F.z + 2)
    const rise = L - F.z - 1;
    // (under the ceiling a walker's 1.75 m over the sill: 14 voxels, and the slab and a lintel)
    if (rise > 2 && rise <= Math.min(SILL_MAX, F.height - 18)) d.sill = rise;
  }
}

const UNREACHABLE_OK = new Set(["shaft", "elevator", "mechanicalShaft", "void"]);

/**
 * Flood fill through rooms and doors, starting from exterior doors on the
 * ground floor; stairwells connect vertically. Every room that people should
 * reach must be reached.
 */
export function validatePlan(plan) {
  const seen = new Set(); // `${gridFloorIndex}:${roomId}`
  const queue = [];
  const key = (fi, rid) => `${fi}:${rid}`;
  const floorsByIdx = new Map(plan.floors.map((f) => [f.index, f]));
  for (const f of plan.floors) {
    for (const d of f.grid.doors) {
      if (d.b === -1 && d.kind !== "window" && d.kind !== "balcony" && f.index === 0) {
        const k = key(f.index, d.a);
        if (!seen.has(k)) {
          seen.add(k);
          queue.push([f.index, d.a]);
        }
      }
    }
  }
  // extra links (ramps) in both directions
  const linkAdj = new Map();
  for (const [a, b] of plan.links ?? []) {
    for (const [p, q] of [
      [a, b],
      [b, a],
    ]) {
      const k = key(p[0], p[1]);
      if (!linkAdj.has(k)) linkAdj.set(k, []);
      linkAdj.get(k).push(q);
    }
  }
  // adjacency per grid (cached)
  const adjCache = new Map();
  const adjOf = (grid) => {
    let a = adjCache.get(grid);
    if (!a) {
      a = grid.doorGraph();
      // elevator doors are closed
      for (const d of grid.doors) {
        if (d.kind === "elevator") {
          a.get(d.a)?.delete(d.b);
          a.get(d.b)?.delete(d.a);
        }
      }
      adjCache.set(grid, a);
    }
    return a;
  };
  while (queue.length) {
    const [fi, rid] = queue.pop();
    const f = floorsByIdx.get(fi);
    const room = f.grid.rooms[rid];
    for (const nb of adjOf(f.grid).get(rid) ?? []) {
      if (nb < 0) continue;
      const k = key(fi, nb);
      if (!seen.has(k)) {
        seen.add(k);
        queue.push([fi, nb]);
      }
    }
    if (room.type === "stair") {
      const st = plan.stairs[room.stair];
      for (const g of [fi - 1, fi + 1]) {
        if (g < st.f0 || g > st.f1) continue;
        const gf = floorsByIdx.get(g);
        if (!gf) continue;
        const other = gf.grid.rooms.find((r) => r.type === "stair" && r.stair === room.stair);
        if (!other) continue;
        const k = key(g, other.id);
        if (!seen.has(k)) {
          seen.add(k);
          queue.push([g, other.id]);
        }
      }
    }
    for (const [g, r2] of linkAdj.get(key(fi, rid)) ?? []) {
      const k = key(g, r2);
      if (!seen.has(k)) {
        seen.add(k);
        queue.push([g, r2]);
      }
    }
    // mezzanines: rooms flagged `linkUp` connect to the matching room above
    if (room.linkTo) {
      const k = key(room.linkTo.floor, room.linkTo.room);
      if (!seen.has(k)) {
        seen.add(k);
        queue.push([room.linkTo.floor, room.linkTo.room]);
      }
    }
  }
  for (const f of plan.floors) {
    for (const r of f.grid.rooms) {
      if (UNREACHABLE_OK.has(r.type)) continue;
      if (!seen.has(key(f.index, r.id))) {
        plan.issues.push({ floor: f.index, room: r.id, type: r.type, msg: "unreachable" });
      }
    }
  }
  return plan.issues;
}

/** Room label at canonical cell for floor record (null when not a room). */
export function roomAtCell(floor, u, v) {
  const l = floor.grid.get(u, v);
  if (l >= ROOM0) return floor.grid.rooms[l - ROOM0];
  return l === DOOR ? "door" : null;
}
