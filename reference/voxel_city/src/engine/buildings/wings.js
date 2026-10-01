import { vx } from "../core/units.js";
import { YAWS, Placement, nearestYaw, yawVector } from "../core/placement.js";
import { localPointToWorld, worldPointToLocal } from "../core/obb.js";
import { frameOf, turnedFrame } from "./frame.js";
import { tierRects, floorZ } from "./archetypes.js";
import { buildingLook, facadeCell, facadeMaterial } from "./facade.js";
import { roofSnowCover, snowAt } from "./massing.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../network/roadSurface.js";
import { roadLevelAt } from "../network/roadLevel.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";

/**
 * Wings, corner bays and canted bays (ANGLED_WORLD_PLAN.md S5): masses a
 * building adds to its main one, each an oriented part of its own at a yaw
 * of the table, cast into the main mass rather than merely touching it:
 * its back face lies inside the main footprint, past the wall, and the main
 * mass owns what the two share (structvox priority: a building's above its
 * wings'). So in the world grid a wing draws only what lies outside the
 * main footprint, and in its own lattice it holds that and the band of the
 * main wall it is cast into (world/partRaster.js). Upper floors only (floor
 * 1 up to the top floor, one less under a pitched roof): the street, the
 * doors and the ground stay the building's.
 *
 *   corner  a bay across the building's street corner, facing it at 43.6°
 *           or 46.4° (the 20-21-29 triple: no table yaw is 45°), over the
 *           corner of its sidewalks
 *   bay     a canted bay on the front facade over one window bay, turned
 *           12.7° or 16.3°, projecting 0.25 m at one end, 1.2 m at most at
 *           the other
 *   wing    a lot cut back by a slanted street: the upper floors fill the
 *           corner between the building and the street, facing it at its
 *           exact yaw
 *   chamfer a flat-roofed building's street corner cut off (buildings/
 *           chamfer.js: 43.6° or 46.4° to both streets, 29 k cells), every
 *           floor from the ground up: the floor plans lose the cut, a slab
 *           on the cut line carries the facade and its roof's parapet. It
 *           owns what it shares with the building (the stepped wall of the
 *           cut behind it), so its priority is above the world grid's
 *
 * A wing hangs over a sidewalk only of a street its own cell owns (whose
 * dressing keeps street trees clear of it), keeps a metre from the kerb and
 * within its lot's frontage, and never over a carriageway. A turned
 * building (S3) gets the canted bays of its front, turned by the exact
 * product of its yaw and the bay's (core/placement.js yawProduct: a triple
 * of its own, no table yaw); corner bays and wings to a slanted street are
 * the square buildings' (a turned one already faces its slanted street,
 * and its side streets are not square to it).
 *
 * A wing record (`env.wings`): { kind, key, turn { yaw, yaw2?, origin, ou, ov }, U,
 * V (its box, cells: u along its outer face, v in from it), f0, f1, z0, z1,
 * bounds (world), canon (its bounds in the building's canonical frame),
 * part }.
 */

/** Cells of the main wall a wing is cast into, and how far inside the main footprint its back corners lie at least. */
export const EMBED = 2;
const MARGIN = EMBED + 2;
/** Clearance from the kerb, the most a bay may project past a facade, and the least headroom under a wing (voxels). */
const KERB = vx(1);
const OVER = vx(1.2);
const HEADROOM = vx(2.5);
/** Roof slab over a wing's top floor, and its parapet above it (voxels). */
const ROOF_T = 2;
const PARAPET = 3;

/** Archetypes that may carry wings (flat facades of 2-10 floors, apartments and offices). */
const WINGED = new Set(["walkup", "midrise", "office", "rowhouse", "townhouse"]);

/** Table yaw index of the exact direction (c, s) (a triple's legs, signs as given): parallel and the same way, in integers. */
function yawOf(c, s) {
  return YAWS.findIndex((y) => y.c * s === y.s * c && y.c * c + y.s * s > 0);
}
/** Relative yaws: the corner bays' (+ for the front-right corner, - for the front-left), the canted bays'. */
const CORNER = { R: [yawOf(21, 20), yawOf(20, 21)], L: [yawOf(21, -20), yawOf(20, -21)] };
const CANT = [yawOf(24, 7), yawOf(24, -7), yawOf(40, 9), yawOf(40, -9)];

/** Is canonical point (u, v) at least `m` inside one rect of `rects` (continuous: a rect covers [x0, x1 + 1))? Returns the rect. */
function rectAround(rects, u, v, m) {
  for (const r of rects) if (u >= r.x0 + m && u <= r.x1 + 1 - m && v >= r.y0 + m && v <= r.y1 + 1 - m) return r;
  return null;
}

/** Canonical continuous point (u, v) of a building's frame F -> world point (a turned frame's canonical cells are its lattice's, offset). */
function canonToWorld(F, u, v) {
  return localPointToWorld(F.placement, u + (F.ou ?? 0), v + (F.ov ?? 0));
}

/** World continuous point -> canonical point of a building's frame F. */
function worldToCanon(F, x, y) {
  const [u, v] = worldPointToLocal(F.placement, x, y);
  return [u - (F.ou ?? 0), v - (F.ov ?? 0)];
}

/** Does a cell (u, v) of a building's canonical frame lie in one of `rects`? */
function inRects(rects, u, v) {
  for (const r of rects) if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return true;
  return false;
}

/** The floors a wing may span: 1 up to the top floor (one less under a pitched roof), or null. */
function floorsOf(env) {
  const f1 = env.floors - 1 - (env.roof.type === "flat" ? 0 : 1);
  return f1 >= 1 ? [1, f1] : null;
}

/**
 * The wing a building would get of `kind`, its outer face centred on
 * canonical point Q (continuous) of the main frame F, at relative yaw
 * `rel` (or world yaw `wyaw`), W cells wide: as deep as its back face needs
 * to lie MARGIN inside the main footprint on every floor it spans (at most
 * `maxD`); null when it cannot.
 */
function shape(env, F, { kind, rel = null, wyaw = null, Q, W, f0, f1, maxD = vx(6) }) {
  // (on a building square to the grid a quarter turn of a table yaw is one; on a turned one, the product of the two)
  let yaw = wyaw;
  let yaw2 = 0;
  if (yaw === null) {
    if (F.placement.q >= 0) yaw = (F.placement.yaw + rel) % YAWS.length;
    else [yaw, yaw2] = [F.placement.yaw, rel];
  }
  const Y = yawVector(yaw, yaw2);
  // (the wing's u axis in the world, and the left end of its outer face)
  const [qx, qy] = canonToWorld(F, Q[0], Q[1]);
  const placement = new Placement({ origin: { x: Math.round(qx - ((W / 2) * Y.c) / Y.r), y: Math.round(qy - ((W / 2) * Y.s) / Y.r), z: 0 }, yaw, yaw2 });
  const canon = (u, v) => worldToCanon(F, ...localPointToWorld(placement, u, v));
  let D = 0;
  for (let d = 6; d <= maxD && !D; d += 1) {
    const a = canon(0, d);
    const b = canon(W, d);
    let ok = true;
    for (let f = f0; f <= f1 && ok; f += 1) {
      const rects = tierRects(env, f);
      const r = rectAround(rects, a[0], a[1], MARGIN);
      ok = !!r && rectAround([r], b[0], b[1], MARGIN) !== null;
    }
    if (ok) D = d;
  }
  if (!D) return null;
  // (its outer face out of the main mass on every floor: its middle or an end a cell clear of it)
  for (let f = f0; f <= f1; f += 1) {
    const rects = tierRects(env, f);
    if ([0, W / 2, W].every((u) => rectAround(rects, ...canon(u, 0), -1))) return null;
  }
  const z0 = floorZ(env, f0);
  const z1 = floorZ(env, f1 + 1) + ROOF_T + PARAPET - 1;
  const bounds = placement.localBoundsToWorldAABB({ u0: 0, v0: 0, u1: W - 1, v1: D - 1, w0: z0, w1: z1 });
  let cx0 = Infinity;
  let cy0 = Infinity;
  let cx1 = -Infinity;
  let cy1 = -Infinity;
  for (const [u, v] of [[0, 0], [W, 0], [W, D], [0, D]]) {
    const [cu, cv] = canon(u, v);
    cx0 = Math.min(cx0, cu);
    cy0 = Math.min(cy0, cv);
    cx1 = Math.max(cx1, cu);
    cy1 = Math.max(cy1, cv);
  }
  return {
    kind,
    turn: { yaw: placement.yaw, ...(placement.yaw2 ? { yaw2: placement.yaw2 } : {}), origin: { x: placement.origin.x, y: placement.origin.y }, ou: 0, ov: 0 },
    U: W,
    V: D,
    f0,
    f1,
    z0,
    z1,
    bounds,
    canon: { x0: Math.floor(cx0), y0: Math.floor(cy0), x1: Math.ceil(cx1) - 1, y1: Math.ceil(cy1) - 1 },
    placement,
    outline: (u, v) => canon(u, v),
  };
}

/** Chamfer legs (the 20-21-29 triple, along the front first) and the depth (cells) of its slab: its facade and the stepped wall of the cut behind it. */
const CH_LEGS = [[20, 21], [21, 20]];
const CH_T = 4;
/** Most floors of a chamfered building (the corner blocks of a street grid: their plans are checked with the cut). */
const CH_FLOORS = 8;

/**
 * The chamfer of a flat-roofed building's front corner `side` ("L", "R"),
 * legs `legs` times k (buildings/chamfer.js): the slab on the cut line (u
 * along it, v in from it, its w the world's z from the ground floor to the
 * roof's parapet), or null where some floor's footprint does not hold the
 * corner with room to spare.
 */
function chamferShape(env, F, side, legs, k) {
  const a = legs[0] * k;
  const b = legs[1] * k;
  const L = 29 * k;
  const spare = vx(3);
  for (let f = 0; f < env.floors; f += 1) {
    const ok = tierRects(env, f).some((r) => r.y0 === 0 && r.y1 >= b + spare && (side === "L" ? r.x0 === 0 && r.x1 >= a + spare : r.x1 === env.U - 1 && r.x0 <= env.U - 1 - a - spare));
    if (!ok) return null;
  }
  // (u from the side street's end of the cut to the front's on the left, from the front's to the side street's on the right: v points in)
  const [ou, ov, du, dv] = side === "L" ? [0, b, a, -b] : [env.U - a, 0, a, b];
  const [ox, oy] = canonToWorld(F, ou, ov);
  const [dx, dy] = F.dirToWorld(du, dv);
  const yaw = yawOf(dx, dy);
  const placement = new Placement({ origin: { x: ox, y: oy, z: 0 }, yaw });
  const z0 = floorZ(env, 0);
  const z1 = floorZ(env, env.floors) + ROOF_T + 4;
  const bounds = placement.localBoundsToWorldAABB({ u0: 0, v0: 0, u1: L - 1, v1: CH_T - 1, w0: z0, w1: z1 });
  const canon = (u, v) => worldToCanon(F, ...localPointToWorld(placement, u, v));
  const cs = [canon(0, 0), canon(L, 0), canon(L, CH_T), canon(0, CH_T)];
  return {
    kind: "chamfer",
    chamfer: { side, a, b },
    turn: { yaw, origin: { x: ox, y: oy }, ou: 0, ov: 0 },
    U: L,
    V: CH_T,
    f0: 0,
    f1: env.floors - 1,
    z0,
    z1,
    bounds,
    canon: { x0: Math.floor(Math.min(...cs.map((c) => c[0]))), y0: Math.floor(Math.min(...cs.map((c) => c[1]))), x1: Math.ceil(Math.max(...cs.map((c) => c[0]))) - 1, y1: Math.ceil(Math.max(...cs.map((c) => c[1]))) - 1 },
    placement,
    outline: (u, v) => canon(u, v),
  };
}

/**
 * Material of a chamfer's slab cell (u along it, v in from its face) at
 * world height z (`s` the LOD's cell size): the building's facade on its
 * outer cells, floor by floor as long as the cut (the ground floor's
 * shopfront too), wall behind; over the top floor the roof slab and its
 * parapet, as the flat roof draws them on the exterior wall. `null`: nothing.
 */
function chamferMaterial(env, w, look, u, v, z, s) {
  const thick = Math.max(2, s);
  const zRoof = floorZ(env, env.floors);
  if (z >= zRoof) {
    const zr = z - zRoof;
    if (zr < ROOF_T) return look.style.wall;
    if (zr < ROOF_T + 4) return look.style.wall;
    return zr === ROOF_T + 4 ? MAT.PARAPET_CAP : null;
  }
  let f = 0;
  while (f < env.floors - 1 && floorZ(env, f + 1) <= z) f += 1;
  const zr = z - floorZ(env, f);
  if (v < thick) return facadeMaterial(look, facadeCell(look, w.U, u, zr, env.storyH[f], f), f, zr);
  return zr < 2 ? MAT.CONCRETE : look.style.wall;
}

/**
 * Is what a wing shows outside the main mass clear (its outer edges
 * sampled every 4 cells, and a kerb's clearance out from them)? Never on a
 * carriageway or a kerb; over a sidewalk or a plaza only of a street its
 * cell `cellId` owns; `inLot(x, y)` where it stands on no street; and
 * HEADROOM above the ground under it (the building's ground level or the
 * sidewalk's, whichever is higher: a steep street rises along a facade).
 */
function clearOutside(world, view, env, F, w, cellId, inLot) {
  const rs = makeRoadSample();
  const pts = [];
  for (let u = 0; u <= w.U; u += 4) pts.push([Math.min(u, w.U), 0, 0, -1]);
  for (let v = 0; v <= w.V; v += 4) pts.push([0, Math.min(v, w.V), -1, 0], [w.U, Math.min(v, w.V), 1, 0]);
  for (const [u, v, du, dv] of pts) {
    const [cu, cv] = w.outline(u, v);
    if (rectAround(tierRects(env, w.f0), cu, cv, 0)) continue;
    const [gx, gy] = localPointToWorld(w.placement, u, v);
    const r = roadLevelAt(world, view, gx, gy, 16);
    if (w.z0 - Math.max(env.groundZ, r ? Math.ceil(r.z) + 1 : -Infinity) < HEADROOM) return false;
    for (const k of [0, KERB]) {
      const [x, y] = localPointToWorld(w.placement, u + du * k, v + dv * k);
      sampleRoadSurface(view.near({ x0: x - 2, y0: y - 2, x1: x + 2, y1: y + 2 }), x, y, rs, world.seed);
      if (rs.kind === KIND.CARRIAGE || rs.kind === KIND.CURB || rs.kind === KIND.MEDIAN || rs.kind === KIND.SHOULDER) return false;
      if (rs.kind !== KIND.NONE) {
        if (rs.seg.road.cell !== cellId) return false;
      } else if (k === 0 && !inLot(x, y)) return false;
    }
  }
  return true;
}

/**
 * The wings a building may carry, in the order a cell grants them (a
 * corner bay, a wing to a slanted street, a canted bay), two at most:
 * geometry only, checked clear of the streets and in its lot (the cell
 * checks other buildings and grants the parts). `lot` the building's lot,
 * `block` its block (cuts), `view` the cell's road view, `rng` the
 * building's own stream.
 */
export function planWings(world, env, lot, block, view, cellId, rng) {
  if (!WINGED.has(env.archetype) || !lot) return [];
  const fl = floorsOf(env);
  if (!fl) return [];
  const [f0, f1] = fl;
  const F = frameOf(env);
  const look = buildingLook(env, world.seed);
  // (where a wing may stand off every street: the lot, its slanted corner included)
  const whole = lot.whole ?? lot.rect;
  const cuts = block?.cuts ?? [];
  // (a voxel past a slanted property line is the street's sidewalk, whose owner is checked)
  const inLot = (x, y) => x >= whole.x0 && x <= whole.x1 + 1 && y >= whole.y0 && y <= whole.y1 + 1 && cuts.every((k) => k.nx * x + k.ny * y >= k.c - 1);
  // (a turned building fronts its slanted street)
  const streetSides = env.turn ? new Set(["F"]) : new Set(lot.frontages.filter((f) => f.cls && f.cls !== "alley").map((f) => F.canonSide(f.side)));
  const out = [];
  const tryAdd = (w) => {
    if (!w || out.length >= 2 || !clearOutside(world, view, env, F, w, cellId, inLot) || out.some((o) => overlaps(o.bounds, w.bounds))) return false;
    out.push(w);
    return true;
  };
  // a corner bay where the front meets a side street, the footprint square to both
  const corners = [];
  if (streetSides.has("F") && streetSides.has("L")) corners.push("L");
  if (streetSides.has("F") && streetSides.has("R")) corners.push("R");
  if (corners.length && rng.chance(0.55)) {
    const side = rng.pick(corners);
    // a flat-roofed corner block square to the grid (8 floors at most) cuts the corner off instead half of the time, where a chamfer fits
    let cut = false;
    if (!env.turn && env.roof.type === "flat" && env.floors <= CH_FLOORS && rng.chance(0.5)) {
      const legs = rng.pick(CH_LEGS);
      for (let k = Math.min(3, Math.floor(Math.min(env.U / 3, env.V / 2) / 21)); k >= 2 && !cut; k -= 1) cut = tryAdd(chamferShape(env, F, side, legs, k));
    }
    if (!cut) {
      const rel = rng.pick(CORNER[side]);
      const Y = YAWS[rel];
      // (the corner point, out along the bay's normal 0-2 cells)
      const C = side === "L" ? [0, 0] : [env.U, 0];
      const p = rng.int(0, 2);
      const W = 2 * rng.int(vx(1.25), vx(1.6));
      tryAdd(shape(env, F, { kind: "corner", rel, Q: [C[0] + (p * Y.s) / Y.r, C[1] - (p * Y.c) / Y.r], W, f0, f1 }));
    }
  }
  // a wing to the slanted street a lot was cut back by
  const sl = lot.frontages.find((f) => f.slant !== undefined && f.cls && f.cls !== "alley");
  const cut = sl && cuts.find((k) => k.id === sl.slant);
  if (cut && !env.turn && rng.chance(0.75)) {
    const wyaw = nearestYaw(cut.ny, -cut.nx);
    const Y = YAWS[wyaw];
    // the footprint corner nearest the street, on the street's line a little inside the lot
    let apex = null;
    for (const r of tierRects(env, f0))
      for (const [u, v] of [[r.x0, r.y0], [r.x1 + 1, r.y0], [r.x1 + 1, r.y1 + 1], [r.x0, r.y1 + 1]]) {
        const [x, y] = canonToWorld(F, u, v);
        const d = cut.nx * x + cut.ny * y - cut.c;
        if (!apex || d < apex.d) apex = { x, y, d };
      }
    // (its outer face square to the street: the table yaw is the street's own)
    const square = Math.abs(Y.c / Y.r - cut.ny) < 1e-6 && Math.abs(Y.s / Y.r + cut.nx) < 1e-6;
    if (square && apex && apex.d >= 0) {
      // (its outer face on the line c + 2, or through the corner where that stands closer,
      // centred on the corner or slid along the street into the wedge between it and a facade)
      const g = Math.max(0, apex.d - 2);
      const [fx, fy] = [apex.x - cut.nx * g, apex.y - cut.ny * g];
      search: for (let W = vx(10); W >= vx(4); W -= vx(1))
        for (const t of [0, W / 2, -W / 2]) {
          const [qu, qv] = worldToCanon(F, fx + (t * Y.c) / Y.r, fy + (t * Y.s) / Y.r);
          if (tryAdd(shape(env, F, { kind: "wing", wyaw, Q: [qu, qv], W, f0, f1, maxD: vx(14) }))) break search;
        }
    }
  }
  // a canted bay over one window bay of the front
  if (streetSides.has("F") && rng.chance(0.3)) {
    const bay = look.bay;
    const nb = Math.floor(env.U / bay);
    if (nb >= 4) {
      const margin = Math.floor((env.U - nb * bay) / 2);
      const k = rng.int(1, nb - 2);
      const rel = rng.pick(CANT);
      const Y = YAWS[rel];
      const W = bay + 4;
      const sn = Math.abs(Y.s / Y.r);
      const p0 = 2 + (W / 2) * sn;
      if (p0 + (W / 2) * sn <= OVER) tryAdd(shape(env, F, { kind: "bay", rel, Q: [margin + (k + 0.5) * bay + (p0 * Y.s) / Y.r, -(p0 * Y.c) / Y.r], W, f0, f1 }));
    }
  }
  for (const w of out) {
    delete w.outline;
    delete w.placement;
  }
  return out;
}

const overlaps = (a, b) => a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;

/** The placement of a wing's lattice (its w the world's z). */
export function wingPlacement(w) {
  return new Placement({ origin: { x: w.turn.origin.x, y: w.turn.origin.y, z: 0 }, yaw: w.turn.yaw, yaw2: w.turn.yaw2 ?? 0 });
}

const frames = new WeakMap();
/** A wing's frame: its box's cells (u, v) in the world (buildings/frame.js TurnedFrame). */
function wingFrame(w) {
  let f = frames.get(w);
  if (!f) {
    f = turnedFrame(w.turn, w.U, w.V);
    frames.set(w, f);
  }
  return f;
}

/**
 * Material of a wing's cell at local (u, v), world height z, on floor f
 * (zr above its slab) or on its roof: its outer faces a facade of the
 * building's look (the front its width long, the sides its depth), inside
 * a floor slab under open rooms. `null` above its roof.
 */
function wingMaterial(env, w, look, u, v, z, s, snowy) {
  const thick = Math.max(2, s);
  const zRoof = floorZ(env, w.f1 + 1);
  const wall = u < thick || u >= w.U - thick || v < thick;
  if (z >= zRoof) {
    const zz = z - zRoof;
    if (zz < ROOF_T) return wall ? look.style.wall : snowy && zz > ROOF_T - 1 - s ? MAT.SNOW : look.style.roof;
    if (!wall || zz >= ROOF_T + PARAPET) return null;
    return zz === ROOF_T + PARAPET - 1 ? MAT.PARAPET_CAP : look.style.wall;
  }
  let f = w.f0;
  while (f < w.f1 && floorZ(env, f + 1) <= z) f += 1;
  const zr = z - floorZ(env, f);
  if (wall) {
    const [len, t] = v < thick ? [w.U, u] : u < thick ? [w.V, w.V - 1 - v] : [w.V, v];
    return facadeMaterial(look, facadeCell(look, len, t, zr, env.storyH[f], f), f, zr);
  }
  if (zr === 0) return MAT.CONCRETE;
  if (zr === 1) return env.program?.upper === "office" ? MAT.FLOOR_CARPET_GRAY : MAT.FLOOR_OAK;
  return 0;
}

/**
 * A wing in the world grid (the building source, grid mode): its cells
 * outside the main footprint of their floor (what the main mass does not
 * own), every LOD.
 */
export function rasterizeWing(world, env, w, chunk) {
  const b = w.bounds;
  const box = chunk.worldBox;
  if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1 || w.z1 < box.z0 || w.z0 > box.z1) return;
  if (w.kind === "chamfer") {
    rasterizeChamfer(world, env, w, chunk);
    return;
  }
  const wf = wingFrame(w);
  const F = frameOf(env);
  const look = buildingLook(env, world.seed);
  const snow = roofSnowCover(world, env);
  const [i0, i1] = chunk.rangeX(Math.max(b.x0, box.x0), Math.min(b.x1, box.x1));
  const [j0, j1] = chunk.rangeY(Math.max(b.y0, box.y0), Math.min(b.y1, box.y1));
  const [k0, k1] = chunk.rangeZ(w.z0, w.z1);
  const zRoof = floorZ(env, w.f1 + 1);
  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j);
    for (let i = i0; i <= i1; i += 1) {
      const x = chunk.wx(i);
      const [u, v] = wf.fromWorld(x, y);
      if (u < 0 || u >= w.U || v < 0 || v >= w.V) continue;
      const [mu, mv] = F.fromWorld(x, y);
      const snowy = snow && snowAt(world.seed, snow, x, y);
      for (let k = k0; k <= k1; k += 1) {
        const z = chunk.wz(k);
        // (the main mass owns its footprint, floor by floor; its top floor's under the wing's roof)
        let f = w.f0;
        while (f < w.f1 && floorZ(env, f + 1) <= z) f += 1;
        if (inRects(tierRects(env, z >= zRoof ? Math.min(w.f1 + 1, env.floors - 1) : f), mu, mv) || (z >= zRoof && inRects(tierRects(env, w.f1), mu, mv))) continue;
        const m = wingMaterial(env, w, look, u, v, z, chunk.s, snowy);
        if (m !== null) chunk.data[i + j * P + k * P2] = m;
      }
    }
  }
}

/**
 * A wing's content in its own lattice (world/partRaster.js, parts mode):
 * the same cells, and where the main footprint holds its cell's centre,
 * the band of the main wall it is cast into (EMBED cells in from the
 * footprint's edge) solid, deeper in nothing (the main mass owns it).
 */
export function rasterizeWingPart(world, env, w, chunk) {
  if (w.kind === "chamfer") {
    rasterizeChamferPart(world, env, w, chunk);
    return;
  }
  const F = frameOf(env);
  const pl = wingPlacement(w);
  const look = buildingLook(env, world.seed);
  const snow = roofSnowCover(world, env);
  const [i0, i1] = chunk.rangeX(-chunk.s + 1, w.U - 1 + chunk.s - 1);
  const [j0, j1] = chunk.rangeY(-chunk.s + 1, w.V - 1 + chunk.s - 1);
  const [k0, k1] = chunk.rangeZ(w.z0, w.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const zRoof = floorZ(env, w.f1 + 1);
  // (a coarse cell is the wing's where any of its corners or its centre shows: a bay survives every LOD)
  const s = chunk.s;
  const probes = s === 1 ? [[0, 0]] : [[0, 0], [-chunk.half, -chunk.half], [s - 1 - chunk.half, -chunk.half], [-chunk.half, s - 1 - chunk.half], [s - 1 - chunk.half, s - 1 - chunk.half]];
  for (let j = j0; j <= j1; j += 1) {
    for (let i = i0; i <= i1; i += 1) {
      let u = -1;
      let v = -1;
      for (const [du, dv] of probes) {
        const pu = chunk.wx(i) + du;
        const pv = chunk.wy(j) + dv;
        if (pu < 0 || pu >= w.U || pv < 0 || pv >= w.V) continue;
        const [px, py] = localPointToWorld(pl, pu + 0.5, pv + 0.5);
        const [qu, qv] = worldToCanon(F, px, py);
        if (s > 1 && rectAround(tierRects(env, w.f0), qu, qv, 0)) continue;
        u = pu;
        v = pv;
        break;
      }
      if (u < 0) continue;
      const [x, y] = localPointToWorld(pl, u + 0.5, v + 0.5);
      const [mu, mv] = worldToCanon(F, x, y);
      const snowy = snow && snowAt(world.seed, snow, Math.floor(x), Math.floor(y));
      for (let k = k0; k <= k1; k += 1) {
        const z = chunk.wz(k);
        let f = w.f0;
        while (f < w.f1 && floorZ(env, f + 1) <= z) f += 1;
        const rects = tierRects(env, z >= zRoof ? Math.min(w.f1 + 1, env.floors - 1) : f);
        let m;
        if (rectAround(rects, mu, mv, 0)) m = rectAround(rects, mu, mv, EMBED) ? null : look.style.wall;
        else m = wingMaterial(env, w, look, u, v, z, chunk.s, snowy);
        if (m) chunk.data[i + j * P + k * P2] = m;
      }
    }
  }
}

/**
 * A chamfer in the world grid (grid mode, after its building): every cell
 * of its slab, over the stepped wall of the cut (it owns what they share);
 * at a coarse LOD a slab as deep as two of its cells, so the facade covers
 * the cut's edge however the coarse columns fall.
 */
function rasterizeChamfer(world, env, w, chunk) {
  const b = w.bounds;
  const box = chunk.worldBox;
  const wf = wingFrame(w);
  const look = buildingLook(env, world.seed);
  const s = chunk.s;
  const depth = Math.max(w.V, 2 * s);
  const [i0, i1] = chunk.rangeX(Math.max(b.x0 - 2 * s, box.x0), Math.min(b.x1 + 2 * s, box.x1));
  const [j0, j1] = chunk.rangeY(Math.max(b.y0 - 2 * s, box.y0), Math.min(b.y1 + 2 * s, box.y1));
  const [k0, k1] = chunk.rangeZ(w.z0, w.z1);
  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j);
    for (let i = i0; i <= i1; i += 1) {
      const [u, v] = wf.fromWorld(chunk.wx(i), y);
      if (u < 0 || u >= w.U || v < 0 || v >= depth) continue;
      for (let k = k0; k <= k1; k += 1) {
        const m = chamferMaterial(env, w, look, u, v, chunk.wz(k), s);
        if (m !== null) chunk.data[i + j * P + k * P2] = m;
      }
    }
  }
}

/** A chamfer's slab in its own lattice (parts mode): a coarse cell is its where its centre or a corner lies in the slab. */
function rasterizeChamferPart(world, env, w, chunk) {
  const look = buildingLook(env, world.seed);
  const s = chunk.s;
  const [i0, i1] = chunk.rangeX(-s + 1, w.U - 1 + s - 1);
  const [j0, j1] = chunk.rangeY(-s + 1, w.V - 1 + s - 1);
  const [k0, k1] = chunk.rangeZ(w.z0, w.z1);
  if (i0 > i1 || j0 > j1 || k0 > k1) return;
  const probes = s === 1 ? [[0, 0]] : [[0, 0], [-chunk.half, -chunk.half], [s - 1 - chunk.half, -chunk.half], [-chunk.half, s - 1 - chunk.half], [s - 1 - chunk.half, s - 1 - chunk.half]];
  for (let j = j0; j <= j1; j += 1)
    for (let i = i0; i <= i1; i += 1) {
      let u = -1;
      let v = -1;
      for (const [du, dv] of probes) {
        const pu = chunk.wx(i) + du;
        const pv = chunk.wy(j) + dv;
        if (pu < 0 || pu >= w.U || pv < 0 || pv >= w.V) continue;
        u = pu;
        v = pv;
        break;
      }
      if (u < 0) continue;
      for (let k = k0; k <= k1; k += 1) {
        const m = chamferMaterial(env, w, look, u, v, chunk.wz(k), s);
        if (m) chunk.data[i + j * P + k * P2] = m;
      }
    }
}

/** Does a canonical rect of a building (a balcony's reach) come within `pad` of one of its wings? */
export function nearWing(env, r, pad = 16) {
  if (!env.wings) return false;
  return env.wings.some((w) => w.canon.x0 - pad <= r.x1 && w.canon.x1 + pad >= r.x0 && w.canon.y0 - pad <= r.y1 && w.canon.y1 + pad >= r.y0);
}
