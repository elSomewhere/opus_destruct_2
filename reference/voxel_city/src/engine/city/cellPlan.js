import { Rng, hashFloat } from "../core/hash.js";
import { SpatialGrid } from "../core/geom2d.js";
import { DISTRICTS } from "../world/registry.js";
import { planBlockLots, wholeBlockLot, microLots, freeLot } from "./lots.js";
import { ARCHETYPES, STYLES } from "../world/registry.js";
import { vx } from "../core/units.js";
import { planBuildingEnvelope, planBuildingEnvelopeAs, floorZ } from "../buildings/archetypes.js";
import { rOverlaps } from "../core/rect.js";
import { flavorOf, flavoredDistrict } from "./flavors.js";
import { planSkybridges } from "./skybridges.js";
import { portQuay } from "./industry.js";
import { roadLevelAt } from "../network/roadLevel.js";
import { landmarkUse } from "./townPlan.js";
import { Frame, nominalFront, frameOf } from "../buildings/frame.js";
import { planBuilding } from "../buildings/interior/plan.js";
import { rowLotsOf, mainFrontage } from "./lots.js";
import { insideCuts, trimToCuts } from "./blockPoly.js";
import { CIVIC, civicStyle, civicGroup } from "../buildings/civic.js";
import { SiteGrading, levelLot } from "./grading.js";
import { Placement, nearestYaw } from "../core/placement.js";
import { clipHalfPlane, fitLocalRect, localPointToWorld } from "../core/obb.js";
import { PartBudget, makePart, partId, partPriority, PART_REACH } from "../world/parts.js";
import { roadRunParts } from "../network/roadParts.js";
import { planWings, wingPlacement } from "../buildings/wings.js";
import { rampPart } from "../buildings/garageRamps.js";
import { chamferHits } from "../buildings/chamfer.js";
import { buildingBoxes } from "../buildings/interior/fixtures.js";
import { ROOM0 } from "../buildings/interior/grid.js";
import { buildingLook } from "../buildings/facade.js";

/** How far a farmstead's fields reach (voxels, before their ragged edge). */
const FARM_R = vx(240);

/** Districts whose ground stays natural between the houses (no graded block surface). */
const NATURAL_GROUND = new Set(["rural", "village"]);

/**
 * Graded surface of a block: a Coons patch stretched between the sidewalk
 * levels of its four sides (the natural ground where a side has no road),
 * so yards, parks and plazas meet every sidewalk flush. Returns a function
 * (x, y) -> ground z (voxels); side levels are sampled every 4 m.
 */
function blockSurface(world, view, block) {
  const p = block.prop;
  const W = Math.max(1, p.x1 - p.x0);
  const H = Math.max(1, p.y1 - p.y0);
  const sideLevel = (name, t) => {
    const x = name === "W" ? p.x0 - 2 : name === "E" ? p.x1 + 2 : p.x0 + t;
    const y = name === "N" ? p.y0 - 2 : name === "S" ? p.y1 + 2 : p.y0 + t;
    if (block.sides[name]?.cls) {
      const r = roadLevelAt(world, view, x, y, 24);
      if (r) return r.z + (r.sidewalk ? 1 : 0);
    }
    return world.terrain.sample(x, y).h;
  };
  const sample = (name) => {
    const len = name === "N" || name === "S" ? W : H;
    const n = Math.ceil(len / 32) + 1;
    const arr = new Float32Array(n);
    for (let i = 0; i < n; i += 1) arr[i] = sideLevel(name, Math.min(len, i * 32));
    return { arr, len };
  };
  const at = (side, t) => {
    const f = Math.max(0, Math.min(side.arr.length - 1.000001, (t / side.len) * (side.arr.length - 1)));
    const i = Math.floor(f);
    return side.arr[i] + (side.arr[Math.min(i + 1, side.arr.length - 1)] - side.arr[i]) * (f - i);
  };
  let N;
  let S;
  let Wd;
  let E;
  let c00;
  let c10;
  let c01;
  let c11;
  return (x, y) => {
    if (!N) {
      N = sample("N");
      S = sample("S");
      Wd = sample("W");
      E = sample("E");
      c00 = (at(N, 0) + at(Wd, 0)) / 2;
      c10 = (at(N, W) + at(E, 0)) / 2;
      c01 = (at(S, 0) + at(Wd, H)) / 2;
      c11 = (at(S, W) + at(E, H)) / 2;
    }
    const u = Math.max(0, Math.min(1, (x - p.x0) / W));
    const v = Math.max(0, Math.min(1, (y - p.y0) / H));
    const tx = u * W;
    const ty = v * H;
    return (
      (1 - v) * at(N, tx) + v * at(S, tx) + (1 - u) * at(Wd, ty) + u * at(E, ty) -
      ((1 - u) * (1 - v) * c00 + u * (1 - v) * c10 + (1 - u) * v * c01 + u * v * c11)
    );
  };
}

/**
 * A block whose property rect keeps clear of every street's right-of-way:
 * where a street's corridor (a disc of its half-width every metre along
 * it) reaches into the rect, the side it comes from moves in past it (the
 * cut that keeps the most area). The same block when nothing intrudes.
 */
function clearOfStreets(block, view) {
  const p0 = block.prop;
  let r = { ...p0 };
  const segs = view.near({ x0: p0.x0 - 64, y0: p0.y0 - 64, x1: p0.x1 + 64, y1: p0.y1 + 64 });
  for (let pass = 0; pass < 8; pass += 1) {
    let cut = null;
    for (const s of segs) {
      const R = s.hr;
      if (s.bbox.x1 < r.x0 || s.bbox.x0 > r.x1 || s.bbox.y1 < r.y0 || s.bbox.y0 > r.y1) continue;
      const n = Math.max(1, Math.ceil(s.len / 8));
      for (let k = 0; k <= n; k += 1) {
        const t = (k / n) * s.len;
        const px = s.ax + s.dx * t;
        const py = s.ay + s.dy * t;
        const qx = Math.max(r.x0, Math.min(r.x1, px));
        const qy = Math.max(r.y0, Math.min(r.y1, py));
        if (Math.hypot(px - qx, py - qy) >= R - 0.5) continue;
        const b = { x0: Math.max(r.x0, Math.floor(px - R)), y0: Math.max(r.y0, Math.floor(py - R)), x1: Math.min(r.x1, Math.ceil(px + R)), y1: Math.min(r.y1, Math.ceil(py + R)) };
        cut = cut ? { x0: Math.min(cut.x0, b.x0), y0: Math.min(cut.y0, b.y0), x1: Math.max(cut.x1, b.x1), y1: Math.max(cut.y1, b.y1) } : b;
      }
      if (cut) break;
    }
    if (!cut) break;
    const area = (q) => Math.max(0, q.x1 - q.x0) * Math.max(0, q.y1 - q.y0);
    const opts = [
      { ...r, x0: cut.x1 + 1 },
      { ...r, x1: cut.x0 - 1 },
      { ...r, y0: cut.y1 + 1 },
      { ...r, y1: cut.y0 - 1 },
    ];
    r = opts.reduce((a, b) => (area(b) > area(a) ? b : a));
    if (area(r) === 0) break;
  }
  if (r.x0 === p0.x0 && r.y0 === p0.y0 && r.x1 === p0.x1 && r.y1 === p0.y1) return block;
  return { ...block, prop: r };
}

/**
 * A block of the angled world (ANGLED_WORLD_PLAN.md S1): `bound`, the
 * property rect lots are planned in (then fitted one by one, fitLots), and
 * `prop`, the largest rect inside the block clear of every street, for
 * what takes a block whole (parks, squares, courtyards, a school). A
 * polygon block (blockPoly.js) is first trimmed to its slanted edges.
 */
function clearAngled(block, view) {
  let inner = block.prop;
  if (block.cuts) {
    const t = trimToCuts(block.prop, block.cuts);
    inner = t ? t.rect : { x0: block.prop.x0, y0: block.prop.y0, x1: block.prop.x0 - 1, y1: block.prop.y0 - 1 };
  }
  const cleared = inner.x1 >= inner.x0 && inner.y1 >= inner.y0 ? clearOfStreets({ prop: inner }, view).prop : inner;
  return { ...block, bound: block.prop, prop: cleared };
}

/** Lots trimmed to less than this share of their planned area drop out. */
const FIT_KEEP = 0.4;

/**
 * Lots of the angled world, planned in a block's bounding rect: each is
 * trimmed to the block's slanted edges and cleared of every street's
 * right-of-way on its own, so lots follow angled and bending streets (their
 * fronts step along a diagonal); a lot trimmed to a street fronts it there.
 * A lot left too small (or narrower than 6 m) drops out.
 */
function fitLots(lots, block, view) {
  const out = [];
  const area = (r) => (r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1);
  for (const lot of lots) {
    let rect = lot.rect;
    let frontages = lot.frontages;
    if (block.cuts) {
      const t = trimToCuts(rect, block.cuts);
      if (!t) continue;
      rect = t.rect;
      // (a side cut back by a slanted street fronts that street instead)
      for (const tr of t.trimmed) {
        if (!tr.cls || tr.cls === "alley") continue;
        frontages = [...frontages.filter((f) => f.side !== tr.side), { side: tr.side, cls: tr.cls, slant: tr.id }];
      }
    }
    rect = clearOfStreets({ prop: rect }, view).prop;
    if (rect.x1 - rect.x0 < vx(6) || rect.y1 - rect.y0 < vx(6) || area(rect) < FIT_KEEP * area(lot.rect)) continue;
    if (rect === lot.rect) {
      out.push(lot);
      continue;
    }
    const front = frontages === lot.frontages ? lot.front : (mainFrontage(rect, frontages) ?? lot.front);
    // (`whole`: the lot as planned, for a building turned to its slanted street, turnedLot)
    out.push({ ...lot, rect, frontages, front, corner: frontages.length >= 2, whole: lot.rect });
  }
  return out;
}

/** Archetypes a turned lot may carry (a part each: small enough to stay within its reach). */
const TURNABLE = new Set(["house", "rowhouse", "walkup", "townhouse", "midrise", "office"]);

/** How far past its footprint a building draws (canopies, porches, balconies, eaves): its envelope's margin. */
const DRAW_PAD = vx(2);

/**
 * A lot fronting a slanted street, turned to face it (the angled world,
 * ANGLED_WORLD_PLAN.md S3): the lot as planned (`whole`, before it was
 * trimmed to the block's slanted edges) cut to those edges holds a turned
 * rect fronting the street (core/obb.js fitLocalRect), as wide as the lot
 * was (16 m at most) and 12-26 m deep, clear of every street's
 * right-of-way. The lot record keeps the cut polygon's bounds (`rect`), the
 * cuts (`lotAt`), the slanted edge it fronts (`edge`) and the turn { yaw,
 * origin, ou, ov, U, V, fp } (its frame: buildings/frame.js lotFrameOf; fp
 * the sidewalk point in front of its middle). Every lot along one edge
 * turns in one lattice, about the block's corner (`origin`), so a row of
 * them can be one part (turnedRows). Null if nothing fits.
 */
function turnedLot(lot, block, view) {
  if (!block.cuts || !lot.whole) return null;
  const f = lot.frontages.find((q) => q.slant !== undefined && q.cls && q.cls !== "alley");
  const cut = f && block.cuts.find((k) => k.id === f.slant);
  if (!cut) return null;
  // (facing the street: the cut's normal points into the lot)
  const yaw = nearestYaw(cut.ny, -cut.nx);
  const w = lot.whole;
  let poly = [[w.x0, w.y0], [w.x1 + 1, w.y0], [w.x1 + 1, w.y1 + 1], [w.x0, w.y1 + 1]];
  for (const k of block.cuts) poly = clipHalfPlane(poly, k.nx, k.ny, k.c);
  if (poly.length < 3) return null;
  // (one lattice along the edge: every lot on it turns about the block's corner, so a row of them may share a part)
  const origin = { x: block.bound.x0, y: block.bound.y0 };
  const placement = new Placement({ origin: { ...origin, z: 0 }, yaw });
  const wide = f.side === "N" || f.side === "S" ? w.x1 - w.x0 + 1 : w.y1 - w.y0 + 1;
  const fit = fitLocalRect(placement, poly, { depth: [vx(12), vx(26)], minWidth: vx(7), maxWidth: Math.min(vx(16), wide), frontSlack: 2 });
  if (!fit || !polyClear(poly, view)) return null;
  const U = fit.x1 - fit.x0 + 1;
  const V = fit.y1 - fit.y0 + 1;
  const [fx, fy] = localPointToWorld(placement, fit.x0 + U / 2, fit.y0 - 4);
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (const [x, y] of poly) {
    x0 = Math.min(x0, x);
    y0 = Math.min(y0, y);
    x1 = Math.max(x1, x);
    y1 = Math.max(y1, y);
  }
  const rect = { x0: Math.floor(x0), y0: Math.floor(y0), x1: Math.ceil(x1) - 1, y1: Math.ceil(y1) - 1 };
  return { ...lot, rect, cuts: block.cuts, poly, front: nominalFront(yaw), edge: `${block.id}/c${cut.id}`, turn: { yaw, origin, ou: fit.x0, ov: fit.y0, U, V, fp: { x: fx, y: fy } } };
}

/** Distance (voxels) from a point to a convex polygon ([[x, y], ...] in order), 0 inside. */
function polyDistance(poly, x, y) {
  let inside = true;
  let best = Infinity;
  let sign = 0;
  for (let k = 0; k < poly.length; k += 1) {
    const [ax, ay] = poly[k];
    const [bx, by] = poly[(k + 1) % poly.length];
    const ex = bx - ax;
    const ey = by - ay;
    const c = ex * (y - ay) - ey * (x - ax);
    if (c !== 0) {
      if (sign !== 0 && Math.sign(c) !== sign) inside = false;
      sign = Math.sign(c);
    }
    const t = Math.max(0, Math.min(1, ((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey || 1)));
    const dx = ax + ex * t - x;
    const dy = ay + ey * t - y;
    best = Math.min(best, Math.sqrt(dx * dx + dy * dy));
  }
  return inside ? 0 : best;
}

/** Is a turned lot's polygon clear of every street's right-of-way (its corridor sampled every metre)? */
function polyClear(poly, view) {
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (const [x, y] of poly) {
    x0 = Math.min(x0, x);
    y0 = Math.min(y0, y);
    x1 = Math.max(x1, x);
    y1 = Math.max(y1, y);
  }
  for (const s of view.near({ x0: x0 - 64, y0: y0 - 64, x1: x1 + 64, y1: y1 + 64 })) {
    const n = Math.max(1, Math.ceil(s.len / 8));
    for (let k = 0; k <= n; k += 1) {
      const t = (k / n) * s.len;
      if (polyDistance(poly, s.ax + s.dx * t, s.ay + s.dy * t) < s.hr - 0.5) return false;
    }
  }
  return true;
}

/**
 * The street levels (voxels, the first above the sidewalk or the road) in
 * front of a building's street doors, as its interior plan places them (a
 * plan is a pure function of the envelope; this one is thrown away):
 * { entrance: at its entrance (else its first street door), all: at every
 * one }; doors with no street near are left out.
 */
function streetDoorLevels(world, view, env) {
  const plan = planBuilding(world, env);
  const doors = plan?.floorByIndex.get(0)?.grid.doors.filter((q) => q.b === -1 && q.kind !== "balcony") ?? [];
  const f = frameOf(env);
  const level = (d) => {
    // (out from the wall the door is in, 6 cells)
    let u = (d.u0 + d.u1 + 1) / 2;
    let v = (d.v0 + d.v1 + 1) / 2;
    if (d.orient === "h") v = d.v0 <= 1 ? d.v0 - 6 : d.v1 + 7;
    else u = d.u0 <= 1 ? d.u0 - 6 : d.u1 + 7;
    const [x, y] = f.pointToWorld(u, v);
    const r = roadLevelAt(world, view, x, y, 24);
    return r ? Math.round(r.z) + (r.sidewalk ? 1 : 0) : null;
  };
  const main = doors.find((d) => d.kind === "entrance") ?? doors[0];
  return { entrance: main ? level(main) : null, all: doors.map(level).filter((z) => z !== null) };
}

/** Most the sidewalk along a turned building's front may stray from its ground floor's level (voxels). */
const FRONT_SLACK = 4;

/** Does the sidewalk along a building's front stay within FRONT_SLACK of its ground level (sampled 6 cells out, 5 times)? */
function levelFront(world, view, env) {
  const f = frameOf(env);
  for (let k = 0; k <= 4; k += 1) {
    const [x, y] = f.pointToWorld((k / 4) * env.U, -6);
    const r = roadLevelAt(world, view, x, y, 24);
    if (r && Math.abs(Math.round(r.z) + (r.sidewalk ? 1 : 0) - env.groundZ) > FRONT_SLACK) return false;
  }
  return true;
}

/**
 * The oriented part of a row of turned buildings along one slanted edge of
 * a block (world/parts.js; one building or more, `members` their envelope
 * ids along the street): the lattice they share (the edge's: its w the
 * world's z), its extent everything they draw (footprints and annexes,
 * grown by DRAW_PAD, from the lowest basement to the highest roof), index 0
 * until the budget grants it one.
 */
function buildingPart(cell, envs) {
  const t = envs[0].turn;
  const placement = new Placement({ origin: { x: t.origin.x, y: t.origin.y, z: 0 }, yaw: t.yaw });
  const e = { u0: Infinity, v0: Infinity, w0: Infinity, u1: -Infinity, v1: -Infinity, w1: -Infinity };
  const grow = (u0, v0, u1, v1) => {
    e.u0 = Math.min(e.u0, u0 - DRAW_PAD);
    e.v0 = Math.min(e.v0, v0 - DRAW_PAD);
    e.u1 = Math.max(e.u1, u1 + DRAW_PAD);
    e.v1 = Math.max(e.v1, v1 + DRAW_PAD);
  };
  for (const env of envs) {
    const { ou, ov } = env.turn;
    grow(ou, ov, ou + env.U - 1, ov + env.V - 1);
    for (const a of env.annexes) grow(ou + a.canon.x0, ov + a.canon.y0, ou + a.canon.x1, ov + a.canon.y1);
    e.w0 = Math.min(e.w0, env.bottomZ);
    e.w1 = Math.max(e.w1, env.topZ);
  }
  const part = makePart({ cell, index: 0, key: envs[0].id, kind: "building", placement, extent: e });
  part.members = envs.map((env) => env.id);
  return part;
}

/** Most two neighbours of a row of turned buildings may stand apart along their edge (voxels): further, they are two rows. */
const ROW_GAP = vx(6);

/**
 * Rows of turned-building candidates ({ env, edge }): the candidates along
 * each slanted edge (in the order the edges first come), by their place
 * along it, cut where two stand more than ROW_GAP apart or where the row's
 * part would reach past PART_REACH. [[candidate, ...], ...]
 */
function turnedRows(cell, cands) {
  const byEdge = new Map();
  for (const c of cands) {
    if (!byEdge.has(c.edge)) byEdge.set(c.edge, []);
    byEdge.get(c.edge).push(c);
  }
  const rows = [];
  for (const list of byEdge.values()) {
    list.sort((a, b) => a.env.turn.ou - b.env.turn.ou || (a.env.id < b.env.id ? -1 : a.env.id > b.env.id ? 1 : 0));
    let row = [];
    for (const c of list) {
      const prev = row[row.length - 1];
      if (prev && (c.env.turn.ou - (prev.env.turn.ou + prev.env.U) > ROW_GAP || buildingPart(cell, [...row, c].map((q) => q.env)).reach > PART_REACH)) {
        rows.push(row);
        row = [];
      }
      row.push(c);
    }
    if (row.length) rows.push(row);
  }
  return rows;
}

/** Cells round a stair or an inner door a chamfer's cut keeps clear of, and round a door to the outside (its balcony, its steps, its awning). */
const CUT_CLEAR = 3;
const CUT_CLEAR_OUT = 12;
/** Rooms a chamfer may take a corner of (the rest keep clear of it: baths, kitchens, stores, corridors, stairs, shafts). */
const CUT_ROOMS = new Set(["living", "bedroom", "studio", "office", "open", "shop", "hall", "lobby", "reception", "backroom", "meeting", "classroom"]);

/**
 * Does a chamfer (buildings/wings.js, chamfer.js) leave its building whole:
 * the floor plans of the building with it complete (every room reached from
 * the street, the ground floor's door to the street kept), no door or stair
 * near the cut, and nothing the building draws outside its rooms (stairs,
 * bulkheads, shop signs) in front of it? What the floors draw inside their
 * rooms keeps to the rooms' cells, which the cut takes away; what they draw
 * outside hangs on their doors (balconies, steps). A plan is a pure
 * function of the envelope: this one is thrown away, the world's is the same.
 */
function chamferFits(world, env, w) {
  const e2 = { ...env, chamfer: w.chamfer, wings: [...(env.wings ?? []), w] };
  const plan = planBuilding(world, e2);
  if (!plan || plan.issues.length) return false;
  const c = w.chamfer;
  const hits = (r, pad = 0) => chamferHits(c, env.U, { x0: r.x0 - pad, y0: r.y0 - pad, x1: r.x1 + pad, y1: r.y1 + pad });
  for (const F of plan.floors) {
    if (F.index < 0) continue;
    if (F.index === 0 && !F.grid.doors.some((d) => d.b === -1 && d.kind !== "balcony")) return false;
    for (const d of F.grid.doors) if (hits({ x0: d.u0, y0: d.v0, x1: d.u1, y1: d.v1 }, d.b === -1 ? CUT_CLEAR_OUT : CUT_CLEAR)) return false;
    for (const room of F.grid.rooms) {
      if (!room.rects.some((r) => hits(r, CUT_CLEAR))) continue;
      // (only a large room gives up its corner, and most of it stays: the walker gets round the cut)
      if (!CUT_ROOMS.has(room.type)) return false;
      let kept = 0;
      for (const r of room.rects) for (let v = r.y0; v <= r.y1; v += 1) for (let u = r.x0; u <= r.x1; u += 1) if (F.grid.get(u, v) === ROOM0 + room.id) kept += 1;
      if (kept < 0.7 * F.grid.area(room)) return false;
    }
  }
  const frame = frameOf(e2);
  const z0 = floorZ(env, 0);
  for (const q of buildingBoxes(world, plan, e2, frame, buildingLook(e2, world.seed))) if (q.z1 >= z0 && hits(q.canon ?? frame.rectFromWorld(q))) return false;
  return true;
}

/**
 * Graded surface of a polygon block (the angled world): the sidewalk
 * levels along each edge's property line (sampled every 4 m, the natural
 * ground where an edge has no road), blended by inverse square distance,
 * so the ground meets every sidewalk, the slanted ones too.
 */
function polySurface(world, view, block) {
  let edges = null;
  const build = () => {
    const poly = block.poly;
    const c = { x: 0, y: 0 };
    for (const p of poly) {
      c.x += p.x / poly.length;
      c.y += p.y / poly.length;
    }
    edges = [];
    for (let k = 0; k < poly.length; k += 1) {
      const p = poly[k];
      const q = poly[(k + 1) % poly.length];
      const L = Math.hypot(q.x - p.x, q.y - p.y);
      if (L < 1) continue;
      const ux = (q.x - p.x) / L;
      const uy = (q.y - p.y) / L;
      // inward normal, the property line hr inside the centre line
      let nx = -uy;
      let ny = ux;
      if (nx * (c.x - p.x) + ny * (c.y - p.y) < 0) {
        nx = -nx;
        ny = -ny;
      }
      const hr = p.side?.hr ?? 0;
      const ax = p.x + nx * hr;
      const ay = p.y + ny * hr;
      const n = Math.ceil(L / 32) + 1;
      const lv = new Float32Array(n);
      for (let s = 0; s < n; s += 1) {
        const t = Math.min(L, s * 32);
        // (just outside the property line: on the sidewalk)
        const x = ax + ux * t - nx * 2;
        const y = ay + uy * t - ny * 2;
        const r = p.side?.cls ? roadLevelAt(world, view, x, y, 24) : null;
        lv[s] = r ? r.z + (r.sidewalk ? 1 : 0) : world.terrain.sample(x, y).h;
      }
      edges.push({ ax, ay, ux, uy, L, lv });
    }
  };
  return (x, y) => {
    if (!edges) build();
    let sw = 0;
    let sz = 0;
    for (const e of edges) {
      const t = Math.max(0, Math.min(e.L, (x - e.ax) * e.ux + (y - e.ay) * e.uy));
      const dx = x - (e.ax + e.ux * t);
      const dy = y - (e.ay + e.uy * t);
      const f = Math.min(e.lv.length - 1.000001, t / 32);
      const i = Math.floor(f);
      const z = e.lv[i] + (e.lv[Math.min(i + 1, e.lv.length - 1)] - e.lv[i]) * (f - i);
      const w = 1 / (dx * dx + dy * dy + 16);
      sw += w;
      sz += w * z;
    }
    return sz / sw;
  };
}

/**
 * Stage 2 of an arterial cell: block programs, lots, building envelopes and
 * open spaces. Depends on the cell's own network plus macro fields, the
 * terrain and the highway corridors (world-level, pure).
 */
/**
 * Cabins in the woods and along the shores (island worlds): a few
 * freestanding plots per country block, in the forest or within sight of
 * the sea or a lake, on ground level enough for a hut, facing the water
 * when there is some.
 */
function cabinLots(world, block, taken, density) {
  const p = block.prop;
  const rng = Rng.from(world.seed, block.id, "cabins");
  const area = ((p.x1 - p.x0) * (p.y1 - p.y0)) / 64;
  const tries = Math.round((area / 380000) * 7 * density);
  const out = [];
  const size = vx(18);
  const gap = vx(70);
  const clash = (r) => [...taken, ...out].some((l) => l.rect.x0 <= r.x1 + gap && r.x0 <= l.rect.x1 + gap && l.rect.y0 <= r.y1 + gap && r.y0 <= l.rect.y1 + gap);
  const isl = world.fields.island;
  for (let k = 0; k < tries; k += 1) {
    const x0 = Math.round(rng.float(p.x0 + vx(12), p.x1 - vx(12) - size));
    const y0 = Math.round(rng.float(p.y0 + vx(12), p.y1 - vx(12) - size));
    if (x0 < p.x0 || y0 < p.y0) continue;
    const rect = { x0, y0, x1: x0 + size - 1, y1: y0 + size - 1 };
    if (clash(rect)) continue;
    const cx = x0 + size / 2;
    const cy = y0 + size / 2;
    const ur = world.fields.urban(cx, cy);
    if (ur.u > 0.08) continue;
    // level enough: the corners within 2.5 m of each other
    let lo = Infinity;
    let hi = -Infinity;
    for (const [qx, qy] of [[x0, y0], [rect.x1, y0], [x0, rect.y1], [rect.x1, rect.y1]]) {
      const h = world.terrain.sample(qx, qy).h;
      lo = Math.min(lo, h);
      hi = Math.max(hi, h);
    }
    if (hi - lo > vx(2.5) || lo < vx(1.5)) continue;
    // in the forest, or with a view of the water; the door faces the sea
    const coast = isl ? isl.coast(cx / 8, cy / 8) : Infinity;
    const shore = coast > 25 && coast < 260;
    const woods = world.landCover.forestDensity(cx, cy, ur.u) > 0.32;
    if (!shore && !woods) continue;
    let front = rng.pick(["N", "S", "W", "E"]);
    if (shore) {
      const gx = isl.coast(cx / 8 + 8, cy / 8) - coast;
      const gy = isl.coast(cx / 8, cy / 8 + 8) - coast;
      front = Math.abs(gx) > Math.abs(gy) ? (gx > 0 ? "W" : "E") : gy > 0 ? "N" : "S";
    }
    out.push({ ...freeLot(block, rect, front, rng, { cabin: true }), id: `${block.id}/c${out.length}` });
  }
  return out;
}

/** A cabin (the "cabin" archetype when registered, else a small wooden house). */
function planCabin(lotRec, district, flavor, rng, extra) {
  if (ARCHETYPES.has("cabin")) return planBuildingEnvelopeAs(lotRec, "cabin", "cabin", district, rng, extra);
  return planBuildingEnvelopeAs(lotRec, "house", "siding", district, rng, extra);
}

export function planCell(world, i, j) {
  const net = world.cellNet(i, j);
  const { config, terrain, fields } = world;
  const seed = config.seed;
  const cabinDensity = config.world.mode === "island" ? config.world.island.cabins ?? 0 : 0;
  // the angled world's streets (ANGLED_WORLD_PLAN.md S1): blocks may be polygons, lots are fitted
  const angled = !!(config.world.angles?.enabled && config.world.angles.features?.roads !== false);
  const lots = [];
  const buildings = [];
  const spaces = [];
  // oriented parts of the cell (world/parts.js): none while angles are off
  const parts = [];
  // turned buildings (S3): lots fronting slanted streets, granted from the
  // cell's part budget in a fixed order after every block is planned
  const turning = angled && config.world.angles.features?.buildings !== false;
  // pitched road pieces (S4): the cell's own streets where they run at a table grade
  const ramping = !!(config.world.angles?.enabled && config.world.angles.features?.ramps !== false);
  // wings, corner and canted bays (S5): off unless asked for
  const winging = angled && config.world.angles.features?.wings === true;
  const budget = turning || ramping || winging ? new PartBudget(config, net.rect, { lattice: world.arterials }) : null;
  const cell = { i, j, ci: world.arterials.canon(i), cj: world.arterials.canon(j), rect: net.rect };
  const deferred = [];
  const lotGrid = new SpatialGrid(256);
  const buildingGrid = new SpatialGrid(256);
  const spaceGrid = new SpatialGrid(256);
  const corridors = world.highways ? world.highways.corridorsNear(net.rect) : [];
  const view = world.roadView(i, j);
  const blockGrid = new SpatialGrid(256);
  // farm country: the rural blocks round the farmsteads are fields (nature/farmland.js)
  const farmGrid = new SpatialGrid(256);

  for (const block0 of net.blocks) {
    // lots and open spaces keep off every street's right-of-way (a wobbly
    // old-town street, a street of the next cell crossing the block)
    // (the angled world: lots are fitted one by one, fitLots)
    const block = angled ? clearAngled(block0, view) : clearOfStreets(block0, view);
    const district = DISTRICTS.get(block.district);
    const rng = Rng.from(seed, block.id, "use");
    const p = block.prop;
    if (p.x1 - p.x0 < 16 || p.y1 - p.y0 < 16) {
      // (an angled block too small for anything still has graded ground)
      if (angled && !NATURAL_GROUND.has(block.district) && block0.prop.x1 > block0.prop.x0 && block0.prop.y1 > block0.prop.y0) {
        block0.levelAt ??= block0.poly ? polySurface(world, view, block0) : blockSurface(world, view, block0);
        blockGrid.insert(block0, block0.prop);
      }
      continue;
    }
    const bcx = (p.x0 + p.x1) / 2;
    const bcy = (p.y0 + p.y1) / 2;
    const ur = fields.urban(bcx, bcy);
    const flavor0 = flavorOf(ur.settlement);
    const fd0 = flavoredDistrict(district, flavor0);
    let use = rng.weighted(fd0.blockUse);
    const natural = NATURAL_GROUND.has(block.district);
    // a town's landmarks (market square, main church, cemetery, parks,
    // schools, allotments...) take the block they land on (city/townPlan.js)
    const landmark = natural ? null : landmarkUse(world, block);
    if (landmark) use = landmark;
    // urban blocks get a graded surface meeting their sidewalks
    // (the graded surface spans the whole block, between its sidewalks)
    const levelAt = natural ? null : (block0.levelAt ??= block0.poly ? polySurface(world, view, block0) : blockSurface(world, view, block0));
    if (levelAt) blockGrid.insert(block0, block0.prop);
    const blockZ = levelAt ? Math.round(levelAt(bcx, bcy)) : Math.round(terrain.sample(bcx, bcy, ur).h) + 1;
    // lots sit flush with their sidewalk; farms and village plots on their own ground
    const lotGround = (lot) => {
      const fp = frontagePoint(lot);
      if (lot.farmstead || lot.cabin) return Math.round(terrain.sample((lot.rect.x0 + lot.rect.x1) / 2, (lot.rect.y0 + lot.rect.y1) / 2).h) + 1;
      const r = roadLevelAt(world, view, fp.x, fp.y, 24);
      if (r) return Math.round(r.z) + (r.sidewalk ? 1 : 0);
      return levelAt ? Math.round(levelAt(fp.x, fp.y)) : Math.round(terrain.sample(fp.x, fp.y).h) + 1;
    };
    const rural = district.lots.mode === "rural";
    // city blocks crossed by a river become riverside parks with quays
    // (a village keeps its houses: its lots simply keep clear of the river)
    if (!rural && district.lots.mode !== "village" && world.rivers && world.rivers.hitsRect(p, 6)) use = "riverside";

    if ((use === "school" || use === "church" || use === "cemetery" || use === "allotments") && world.seaHitsRect(p, 2)) use = "lots";
    // a sports ground, a car park or a square is a block of its own size, not a field
    const big = Math.max(p.x1 - p.x0, p.y1 - p.y0);
    if ((use === "sports" || use === "square" || use === "plaza") && big > vx(120)) use = district.lots.mode === "micro" ? "micro" : "lots";
    if (use === "parking" && big > vx(100) && !district.port) use = district.lots.mode === "micro" ? "micro" : "lots";
    if (use === "cemetery") {
      // a walled graveyard: rows of graves on either side of a main path from
      // the gate on the street to a small chapel at the far end
      const space = { id: `${block.id}/o`, kind: "cemetery", block: block.id, district: block.district, rect: p, groundZ: blockZ, levelAt, underHighway: corridors.some((c) => c.hitsRect(p)), front: mainFront(block) };
      spaces.push(space);
      spaceGrid.insert(space, p);
      const lf = new Frame(p, space.front);
      if (lf.U >= vx(30) && lf.V >= vx(44) && !space.underHighway && ARCHETYPES.has("church")) {
        const cw = Math.min(vx(20), lf.U - vx(8));
        const u0 = Math.round((lf.U - cw) / 2);
        const rect = lf.rectToWorld({ x0: u0, y0: lf.V - vx(32), x1: u0 + cw - 1, y1: lf.V - vx(3) });
        const lot = { ...freeLot(block, rect, space.front, rng, { chapel: true }), id: `${block.id}/l0` };
        const groundZ = levelAt ? Math.round(levelAt((rect.x0 + rect.x1) / 2, (rect.y0 + rect.y1) / 2)) : blockZ;
        const lotRec = { ...lot, groundZ, u: ur.u, core: ur.core, building: null, levelAt, underHighway: false };
        const env = planBuildingEnvelopeAs(lotRec, "church", churchStyle(flavor0, lot.id, seed), district, Rng.from(seed, lot.id, "building"), { u: ur.u, core: ur.core, groundZ, config, chapel: true, dome: flavor0.churchDome });
        if (env) {
          env.flavor = flavor0.id;
          lotRec.building = env.id;
          lots.push(lotRec);
          lotGrid.insert(lotRec, rect);
          buildings.push(env);
          buildingGrid.insert(env, env.bounds);
          space.chapel = rect;
        }
      }
      continue;
    }
    if (use === "wharf") {
      // a row of narrow gabled warehouses (Bryggen), their gables to the
      // street along the harbour; the block's other sides are ordinary houses
      const isl = fields.island;
      const hb = isl ? isl.harbour() : null;
      const toSea = { N: [0, -1], S: [0, 1], W: [-1, 0], E: [1, 0] };
      const rows = angled ? fitLots(rowLotsOf({ ...block, prop: block.bound }, rng, [7, 11]), block, view) : rowLotsOf(block, rng, [7, 11]);
      const planned = rows.filter((lot) => !world.seaHitsRect(lot.rect, 2) && !(world.waterHitsRect && world.waterHitsRect(lot.rect, 3)));
      for (const lot of planned) {
        const [dx, dy] = toSea[lot.front];
        const wx = hb ? hb.x * 8 - bcx : 0;
        const wy = hb ? hb.y * 8 - bcy : 0;
        lot.wharf = !hb || (dx * wx + dy * wy) / (Math.hypot(wx, wy) || 1) > 0.35;
      }
      for (const lot of planned) {
        const groundZ = lotGround(lot);
        const lotRec = { ...lot, groundZ, u: ur.u, core: ur.core, building: null, levelAt, underHighway: corridors.some((c) => c.hitsRect(lot.rect)) };
        lots.push(lotRec);
        lotGrid.insert(lotRec, lot.rect);
        if (lotRec.underHighway) continue;
        const brng = Rng.from(seed, lot.id, "building");
        const extra = { u: ur.u, core: ur.core, groundZ, config };
        const env = (lot.wharf && ARCHETYPES.has("wharfhouse") ? planBuildingEnvelopeAs(lotRec, "wharfhouse", STYLES.has("nordicWood") ? "nordicWood" : "siding", fd0, brng, extra) : null) ?? planBuildingEnvelope(lotRec, fd0, brng, extra);
        if (!env) continue;
        env.flavor = flavor0.id;
        lotRec.building = env.id;
        buildings.push(env);
        buildingGrid.insert(env, env.bounds);
      }
      continue;
    }
    if (use === "church") {
      // a wooden church in its churchyard: the whole block (else plain lots)
      const lot = wholeBlockLot(block, rng);
      const groundZ = lotGround(lot);
      const lotRec = { ...lot, groundZ, u: ur.u, core: ur.core, building: null, levelAt, churchyard: true, underHighway: corridors.some((c) => c.hitsRect(lot.rect)) };
      const brng = Rng.from(seed, lot.id, "building");
      const env = ARCHETYPES.has("church") && !lotRec.underHighway ? planBuildingEnvelopeAs(lotRec, "church", churchStyle(flavor0, lot.id, seed), district, brng, { u: ur.u, core: ur.core, groundZ, config, dome: flavor0.churchDome }) : null;
      if (env) {
        env.flavor = flavorOf(ur.settlement).id;
        lotRec.building = env.id;
        lots.push(lotRec);
        lotGrid.insert(lotRec, lot.rect);
        buildings.push(env);
        buildingGrid.insert(env, env.bounds);
        continue;
      }
      use = "lots";
    }
    // a civic building (a museum, a hospital, a supermarket...): its own lot
    // on the block's main street, the rest of the block plain lots
    let civic = null;
    if (use.startsWith("civic:")) {
      civic = use.slice(6);
      use = district.lots.mode === "micro" ? "micro" : "lots";
    }
    // an island harbour's waterfront blocks are quays (a yard or a car park on the quay)
    // (only where the water really reaches into the block; a corner of sea leaves the lots)
    if (!civic && district.port && use === "lots" && world.seaHitsRect(p, 2) && world.seaShare(p) >= 0.12) use = rng.chance(0.3) ? "containerYard" : "quay";
    if (use === "school") {
      // a civic block: one lot, a school with its yard (else plain lots)
      const lot = wholeBlockLot(block, rng);
      const groundZ = lotGround(lot);
      const lotRec = { ...lot, groundZ, u: ur.u, core: ur.core, building: null, levelAt, underHighway: corridors.some((c) => c.hitsRect(lot.rect)) };
      const flavor = flavorOf(ur.settlement);
      const fd = flavoredDistrict(district, flavor);
      const brng = Rng.from(seed, lot.id, "building");
      const style = fd.styles.length ? brng.weighted(fd.styles) : "brick";
      const env = lotRec.underHighway ? null : planBuildingEnvelopeAs(lotRec, "school", style, fd, brng, { u: ur.u, core: ur.core, groundZ, config });
      if (env) {
        env.flavor = flavor.id;
        lotRec.building = env.id;
        lots.push(lotRec);
        lotGrid.insert(lotRec, lot.rect);
        buildings.push(env);
        buildingGrid.insert(env, env.bounds);
        continue;
      }
      use = "lots";
    }

    let civicRect = null;
    if (civic) {
      const placed = civicLot(world, block, civic, { seed, corridors, lotGround, ur, district, config, levelAt });
      if (placed) {
        const { lotRec, env } = placed;
        lots.push(lotRec);
        lotGrid.insert(lotRec, lotRec.rect);
        buildings.push(env);
        buildingGrid.insert(env, env.bounds);
        const m = vx(1.5);
        civicRect = { x0: lotRec.rect.x0 - m, y0: lotRec.rect.y0 - m, x1: lotRec.rect.x1 + m, y1: lotRec.rect.y1 + m };
      }
    }

    if (use === "micro") {
      // superblock: a shared courtyard with freestanding slabs and towers on it
      const space = { id: `${block.id}/o`, kind: "courtyard", block: block.id, district: block.district, rect: p, groundZ: blockZ, levelAt, underHighway: corridors.some((c) => c.hitsRect(p)) };
      spaces.push(space);
      spaceGrid.insert(space, p);
      const flavor = flavorOf(ur.settlement);
      const fd = flavoredDistrict(district, flavor);
      microLots(block, rng).forEach((lot, k) => {
        lot.id = `${block.id}/l${k}`;
        if (corridors.some((c) => c.hitsRect(lot.rect)) || (world.waterHitsRect && world.waterHitsRect(lot.rect, 6))) return;
        if (civicRect && rOverlaps(civicRect, lot.rect)) return;
        const lz = levelAt ? Math.round(levelAt((lot.rect.x0 + lot.rect.x1) / 2, (lot.rect.y0 + lot.rect.y1) / 2)) : blockZ;
        const lotRec = { ...lot, groundZ: lz, u: ur.u, core: ur.core, building: null, levelAt, underHighway: false };
        const brng = Rng.from(seed, lot.id, "building");
        const style = fd.styles.length ? brng.weighted(fd.styles) : "panel";
        const env = planBuildingEnvelopeAs(lotRec, lot.arch, style, fd, brng, { u: ur.u, core: ur.core, groundZ: lz, config });
        if (!env) return;
        env.flavor = flavor.id;
        lotRec.building = env.id;
        lots.push(lotRec);
        lotGrid.insert(lotRec, lot.rect);
        buildings.push(env);
        buildingGrid.insert(env, env.bounds);
      });
      continue;
    }

    if (use === "lots") {
      const village = district.lots.mode === "village";
      const planned = angled ? fitLots(planBlockLots({ ...block, prop: block.bound }, district, rng), block, view) : planBlockLots(block, district, rng);
      if (rural && cabinDensity > 0) planned.push(...cabinLots(world, angled ? { ...block, prop: block.bound } : block, planned, cabinDensity));
      // out on the town's rim houses stand further apart: lots drop out
      // where the town thins (gardens, meadow and scrub between them)
      const outskirts = !rural && !village && config.world.mode !== "infiniteCity" && (block.u ?? ur.u) < 0.42 && (district.lots.mode === "suburban" || district.lots.mode === "perimeter");
      const blockLots = planned.filter((lot) => {
        if (civicRect && rOverlaps(civicRect, lot.rect)) return false;
        if ((rural || village) && world.waterHitsRect && world.waterHitsRect(lot.rect, 8)) return false;
        // (island towns: nothing is built out over the sea)
        if (world.seaHitsRect && world.seaHitsRect(lot.rect, 2)) return false;
        // village houses thin out towards the edge of the village
        if (village) {
          const vu = fields.urban((lot.rect.x0 + lot.rect.x1) / 2, (lot.rect.y0 + lot.rect.y1) / 2).u;
          if (vu < 0.05 || hashFloat(seed, lot.id, 77) > 0.35 + vu * 2.6) return false;
        }
        if (outskirts) {
          const lx = (lot.rect.x0 + lot.rect.x1) / 2;
          const ly = (lot.rect.y0 + lot.rect.y1) / 2;
          const lu = fields.urban(lx, ly).u + 0.08 * fields.fringeNoise(lx, ly) + 0.14 * (hashFloat(seed, lot.id, 78) - 0.5);
          if (lu < 0.19) return false;
        }
        return true;
      });
      const planEnv = (lotRec) => {
        const brng = Rng.from(seed, lotRec.id, "building");
        const flavor = flavorOf(ur.settlement);
        const extra = { u: ur.u, core: ur.core, groundZ: lotRec.groundZ, config };
        const env =
          lotRec.farmRole === "barn"
            ? planBuildingEnvelopeAs(lotRec, "barn", "barn", district, brng, extra)
            : lotRec.cabin
              ? planCabin(lotRec, district, flavor, brng, extra)
              : planBuildingEnvelope(lotRec, flavoredDistrict(district, flavor), brng, extra);
        if (env) env.flavor = flavor.id;
        // (the angled world's garages ramp at a table grade, their ramps pitched parts where the budget has room)
        if (env && ramping && env.archetype === "garage") env.pitchedRamps = true;
        return env;
      };
      const commit = (lotRec, env) => {
        lots.push(lotRec);
        lotGrid.insert(lotRec, lotRec.rect);
        if (env) {
          lotRec.building = env.id;
          buildings.push(env);
          buildingGrid.insert(env, env.bounds);
        }
      };
      const planLot = (lot) => {
        const blocked = corridors.some((c) => c.hitsRect(lot.rect));
        const lotRec = { ...lot, groundZ: lotGround(lot), u: ur.u, core: ur.core, building: null, levelAt, underHighway: blocked };
        commit(lotRec, blocked ? null : planEnv(lotRec));
      };
      // a building turned to its slanted street, if it fits and is small
      // enough: a candidate, granted a part (alone or with its neighbours
      // along the street) once every block is planned
      const turnedCandidate = (lot) => {
        const t = turnedLot(lot, block, view);
        if (!t || corridors.some((c) => c.hitsRect(t.rect)) || (civicRect && rOverlaps(civicRect, t.rect))) return null;
        if ((world.seaHitsRect && world.seaHitsRect(t.rect, 2)) || (world.waterHitsRect && world.waterHitsRect(t.rect, 3))) return null;
        const lotRec = { ...t, groundZ: lotGround(t), u: ur.u, core: ur.core, building: null, levelAt, underHighway: false };
        let env = planEnv(lotRec);
        if (!env || !env.turn || !TURNABLE.has(env.archetype) || levelLot(lotRec, env) || env.skyDoors) return null;
        // the ground floor at the sidewalk's level in front of its street door
        // (a slanted street may climb along the front: the lot's middle is
        // not where one walks in; the door is where the plan puts it)
        const doors = streetDoorLevels(world, view, env);
        const doorZ = doors.entrance ?? lotRec.groundZ;
        if (doorZ !== lotRec.groundZ) {
          lotRec.groundZ = doorZ;
          env = planEnv(lotRec);
          if (!env || !env.turn || !TURNABLE.has(env.archetype) || levelLot(lotRec, env)) return null;
        }
        // (a street climbing steeply along the front keeps its buildings square
        // to the grid; so does one where a shop door would be a step too high)
        if (!levelFront(world, view, env) || doors.all.some((z) => z < doorZ - 1 || z > doorZ + 3)) return null;
        if (buildingPart(cell, [env]).reach > PART_REACH) return null;
        return { lotRec, env, edge: t.edge };
      };
      for (const lot of blockLots) {
        if (turning && lot.whole && lot.frontages.some((f) => f.slant !== undefined)) deferred.push({ lot, planLot, commit, turnedCandidate });
        else planLot(lot);
      }
    } else {
      const space = {
        id: `${block.id}/o`,
        kind: use,
        block: block.id,
        district: block.district,
        rect: p,
        groundZ: blockZ,
        levelAt,
        underHighway: corridors.some((c) => c.hitsRect(p)),
        front: mainFront(block),
      };
      // harbour yards: a straight quay where the lake reaches in
      if (district.port) space.quay = portQuay(world, p);
      // a quay apron needs a quay line; without one it is a plain car park
      if (space.kind === "quay" && !space.quay) space.kind = "parking";
      spaces.push(space);
      spaceGrid.insert(space, p);
    }
  }

  // the turned buildings (S3): every candidate, then the rows of them along
  // each slanted edge, one part a row (one lattice), granted in order while
  // the cell turns at most 1 building in 8 (ANGLED_WORLD_PLAN.md §3.2); a row
  // the budget refuses asks again building by building. Then every lot in the
  // order it was planned: turned where granted, else as axis-aligned as any other
  if (deferred.length) {
    for (const d of deferred) d.cand = d.turnedCandidate(d.lot);
    // (the cell ends with at least the buildings it has and the turned ones: t <= (n + t) / 8)
    let room = Math.floor(buildings.length / 7);
    const grant = (row) => {
      const part = buildingPart(cell, row.map((c) => c.env));
      const k = budget.grant(part.base.x, part.base.y, part.home);
      if (k < 0) return false;
      part.id = partId(cell.ci, cell.cj, k);
      // (a priority of its own: whatever two parts' boxes share, one owns)
      part.priority = part.placement.priority = partPriority("building", part.id);
      parts.push(part);
      for (const c of row) {
        c.env.part = part.id;
        c.granted = true;
      }
      room -= row.length;
      return true;
    };
    // (the longest rows first: the most turned buildings for the parts the budget has)
    const rows = turnedRows(cell, deferred.map((d) => d.cand).filter(Boolean));
    for (const row0 of rows.map((r, k) => [r, k]).sort((a, b) => b[0].length - a[0].length || a[1] - b[1]).map(([r]) => r)) {
      if (room <= 0) break;
      const row = row0.slice(0, room);
      if (!grant(row) && row.length > 1) for (const c of row) if (room > 0) grant([c]);
    }
    for (const d of deferred) {
      if (d.cand?.granted) d.commit(d.cand.lotRec, d.cand.env);
      else d.planLot(d.lot);
    }
  }

  // then the pitched road pieces, steepest first (each run of a street at one
  // table grade, one lattice; a piece the budget refuses stays in the world
  // grid, stepped as any other road)
  const pitched = new Map();
  if (ramping) {
    const own = new Map();
    for (const s of view.segs) if (s.road.cell === net.id) own.set(s.road, [...(own.get(s.road) ?? []), s]);
    const cands = [];
    for (const [road, segs] of own) cands.push(...roadRunParts(world, road, segs, cell));
    cands.sort((a, b) => Math.abs(b.grade) - Math.abs(a.grade) || (a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
    for (const c of cands) {
      if (c.reach > PART_REACH) continue;
      const k = budget.grant(c.base.x, c.base.y, c.home);
      if (k < 0) continue;
      c.id = partId(cell.ci, cell.cj, k);
      c.priority = c.placement.priority = partPriority("road", c.id);
      parts.push(c);
      pitched.set(c.road, [...(pitched.get(c.road) ?? []), c]);
    }
  }

  // then a garage's ramps (each a pitched slab between two decks, the plan's
  // table grade; a ramp the budget refuses stays stepped in the world grid)
  if (ramping)
    for (const env of buildings) {
      if (!env.pitchedRamps) continue;
      for (const r of planBuilding(world, env)?.ramps ?? []) {
        if (!r.pitch) continue;
        const part = rampPart(cell, env, r);
        if (part.reach > PART_REACH) continue;
        const k = budget.grant(part.base.x, part.base.y, part.home);
        if (k < 0) continue;
        part.id = partId(cell.ci, cell.cj, k);
        // (cast between the decks the world grid holds, which own its ends)
        part.priority = part.placement.priority = partPriority("ramp", part.id, { yieldsToGrid: true });
        parts.push(part);
        (env.rampParts ??= []).push(r.f);
      }
    }

  // special sites anchored in this cell contribute their surface buildings
  const sites = world.sites ? world.sites.sitesInCell(i, j) : [];
  for (const site of sites) {
    if (!site.def.surface) continue;
    for (const { lot, env } of site.def.surface(world, site)) {
      lot.building = env.id;
      lots.push({ ...lot, site: site.id });
      buildings.push(env);
      buildingGrid.insert(env, env.bounds);
    }
  }

  // farm country: the fields round each farmstead, out to FARM_R (a ragged
  // edge), within its rural block
  for (const lot of lots) {
    if (!lot.farmstead || !lot.building) continue;
    const blk = net.blocks.find((b) => b.id === lot.block);
    if (!blk) continue;
    const r = blk.rect ?? blk.prop;
    const q = { x0: Math.max(r.x0, lot.rect.x0 - FARM_R), y0: Math.max(r.y0, lot.rect.y0 - FARM_R), x1: Math.min(r.x1, lot.rect.x1 + FARM_R), y1: Math.min(r.y1, lot.rect.y1 + FARM_R) };
    farmGrid.insert({ lot: lot.rect, clip: q }, q);
  }
  const farmNoise = (x, y) => world.landCover.fieldNoise(world.landCover.nFarm, x, y, 480, 13.1, -7.9);

  // skybridges between facing office buildings (sets env.skyDoors)
  const skybridges = planSkybridges(world, net, buildings, corridors);

  // wings, corner and canted bays (S5): parts on buildings square to the
  // grid and canted bays on turned ones, granted last from what the budget
  // has left, clear of every other building, annex, wing and skybridge of the cell
  const wings = [];
  let wingGrid = null;
  if (winging) {
    const lotOf = new Map();
    for (const l of lots) if (l.building) lotOf.set(l.building, l);
    const blockOf = new Map(net.blocks.map((b) => [b.id, b]));
    for (const env of buildings) {
      const lot = lotOf.get(env.id);
      if (!lot || env.skyDoors) continue;
      for (const w of planWings(world, env, lot, blockOf.get(lot.block), view, net.id, Rng.from(seed, env.id, "wings"))) {
        const b = w.bounds;
        const hit = (r) => r.x0 <= b.x1 + 2 && b.x0 - 2 <= r.x1 && r.y0 <= b.y1 + 2 && b.y0 - 2 <= r.y1;
        if (buildingGrid.query(b).some((o) => o !== env && (hit(o.R) || o.annexes.some((a) => hit(a.world))))) continue;
        if (env.annexes.some((a) => hit(a.world)) || (wingGrid && wingGrid.query(b).some((q) => hit(q.w.bounds))) || skybridges.some((q) => hit(q.bb))) continue;
        const part = makePart({ cell, index: 0, key: `${env.id}/${w.kind}`, kind: "wing", placement: wingPlacement(w), extent: { u0: 0, v0: 0, w0: w.z0, u1: w.U - 1, v1: w.V - 1, w1: w.z1 } });
        if (part.reach > PART_REACH) continue;
        // (a chamfer changes its building's floor plans: they must still work, nothing left in front of the cut)
        if (w.kind === "chamfer" && (!budget.fits(part.base.x, part.base.y, part.home) || !chamferFits(world, env, w))) continue;
        const k = budget.grant(part.base.x, part.base.y, part.home);
        if (k < 0) continue;
        part.id = partId(cell.ci, cell.cj, k);
        // (cast into a building square to the grid, which owns the band it shares, a wing yields to the world
        // grid; a chamfer owns the stepped wall of its cut, a turned building's wing is cast into a part)
        part.priority = part.placement.priority = partPriority("wing", part.id, { yieldsToGrid: !env.turn && w.kind !== "chamfer" });
        part.env = env.id;
        if (w.kind === "chamfer") env.chamfer = w.chamfer;
        w.key = part.key;
        w.part = part.id;
        (env.wings ??= []).push(w);
        parts.push(part);
        wings.push(w);
        (wingGrid ??= new SpatialGrid(256)).insert({ env, w }, b);
      }
    }
  }
  const buildingById = new Map(buildings.map((b) => [b.id, b]));
  // building pads and level lots: the graded ground round them (city/grading.js)
  const grading = new SiteGrading(lots, buildingById);

  return {
    id: net.id,
    sites,
    skybridges,
    i,
    j,
    rect: net.rect,
    net,
    lots,
    buildings,
    spaces,
    parts,
    // (road id -> its pitched pieces: the ground leaves them out in parts mode, compose.js)
    pitched,
    // (the wings of its buildings, S5: the dressing keeps its street trees clear of them)
    ...(wings.length ? { wings } : {}),
    lotGrid,
    buildingGrid,
    spaceGrid,
    buildingById,
    grading,
    lotAt(x, y) {
      const hits = lotGrid.queryPoint(x, y);
      // (a turned lot of the angled world: on its side of the block's slanted edges)
      return hits.find((l) => !l.cuts || insideCuts(l.cuts, x + 0.5, y + 0.5)) ?? null;
    },
    spaceAt(x, y) {
      const hits = spaceGrid.queryPoint(x, y);
      return hits.length ? hits[0] : null;
    },
    /** Is a point in farm country (a rural block: fields round the farmsteads)? */
    farmAt(x, y) {
      const hits = farmGrid.queryPoint(x, y);
      if (!hits.length) return false;
      const reach = FARM_R * (0.7 + 0.3 * farmNoise(x, y));
      for (const f of hits) {
        const r = f.lot;
        const dx = Math.max(r.x0 - x, 0, x - r.x1);
        const dy = Math.max(r.y0 - y, 0, y - r.y1);
        if (Math.hypot(dx, dy) < reach) return true;
      }
      return false;
    },
    /** Urban block (with a graded surface `levelAt`) containing a point, or null. */
    blockAt(x, y) {
      const hits = blockGrid.queryPoint(x, y);
      // (a polygon block of the angled world: on its side of every slanted edge)
      return hits.find((b) => !b.cuts || insideCuts(b.cuts, x + 0.5, y + 0.5)) ?? null;
    },
    buildingsIn(rect, out = []) {
      buildingGrid.query(rect, out);
      // (a building whose wing reaches the rect, past its bounds)
      if (wingGrid) for (const { env } of wingGrid.query(rect)) if (!out.includes(env)) out.push(env);
      return out;
    },
  };
}

/**
 * The lot of civic building `id` on a block: its `lot` size (m) on the
 * block's main street (another street side where it does not fit), at a
 * corner or in the middle, or the whole block where the rest would be a
 * sliver. Returns { lotRec, env } or null (no fit, water, a highway).
 */
function civicLot(world, block, id, { seed, corridors, lotGround, ur, district, config, levelAt }) {
  const spec = CIVIC[id];
  const p = block.prop;
  const rng = Rng.from(seed, block.id, "civic");
  const main = mainFront(block);
  const fronts = [main, ...["S", "N", "W", "E"].filter((f) => f !== main && block.sides[f]?.cls)];
  const flavor = flavorOf(ur.settlement);
  const fd = flavoredDistrict(district, flavor);
  const group = civicGroup(flavor.id);
  const old = district.id === "oldtown" || district.id === "oldcore";
  for (const front of fronts) {
    const lf = new Frame(p, front);
    let lu = Math.min(lf.U, vx(spec.lot[0]));
    let lv = Math.min(lf.V, vx(spec.lot[1]));
    if (lf.U - lu < vx(14)) lu = lf.U;
    if (lf.V - lv < vx(14)) lv = lf.V;
    if (!ARCHETYPES.get(id).fits(lu, lv)) continue;
    const slack = lf.U - lu;
    const starts = slack === 0 ? [0] : rng.shuffle([0, slack, Math.round(slack / 2)]);
    const rect = starts.map((u0) => lf.rectToWorld({ x0: u0, y0: 0, x1: u0 + lu - 1, y1: lv - 1 })).find((r) => !corridors.some((c) => c.hitsRect(r)) && !(world.seaHitsRect && world.seaHitsRect(r, 2)) && !(world.waterHitsRect && world.waterHitsRect(r, 4)));
    if (!rect) continue;
    const lot = { ...freeLot(block, rect, front, rng, { civic: id }), id: `${block.id}/c` };
    const groundZ = lotGround(lot);
    const lotRec = { ...lot, groundZ, u: ur.u, core: ur.core, building: null, levelAt, underHighway: false };
    const brng = Rng.from(seed, lot.id, "building");
    // pitched roofs on the civic buildings of northern and old towns
    const pitchedCivic = group === "nordic" ? 0.65 : group === "soviet" ? 0 : old ? 0.35 : 0.08;
    const env = planBuildingEnvelopeAs(lotRec, id, civicStyle(id, flavor.id, brng), fd, brng, { u: ur.u, core: ur.core, groundZ, config, pitchedCivic });
    if (!env) continue;
    env.flavor = flavor.id;
    lotRec.building = env.id;
    return { lotRec, env };
  }
  return null;
}

/**
 * Style of a church: the flavor's (the wooden churches of the north), else
 * a rendered or a brick church.
 */
function churchStyle(flavor, id, seed) {
  if (flavor.churchStyle && STYLES.has(flavor.churchStyle)) return flavor.churchStyle;
  return hashFloat(seed, id.length, id.charCodeAt(id.length - 1), 0xc7) < 0.55 ? "plaster" : "brick";
}

/** The block side on the most important street (a gate or a main entrance goes there). */
function mainFront(block) {
  const rank = { arterial: 5, collector: 4, local: 3, village: 3, pedestrian: 2, rural: 1, lane: 1, alley: 0 };
  let best = "S";
  let bs = -1;
  for (const side of ["S", "N", "W", "E"]) {
    const c = block.sides[side]?.cls;
    if (!c) continue;
    const sc = rank[c] ?? 0;
    if (sc > bs) {
      bs = sc;
      best = side;
    }
  }
  return best;
}

function frontagePoint(lot) {
  if (lot.turn) return lot.turn.fp;
  const r = lot.rect;
  const cx = (r.x0 + r.x1) / 2;
  const cy = (r.y0 + r.y1) / 2;
  switch (lot.front) {
    case "N":
      return { x: cx, y: r.y0 - 4 };
    case "S":
      return { x: cx, y: r.y1 + 4 };
    case "W":
      return { x: r.x0 - 4, y: cy };
    default:
      return { x: r.x1 + 4, y: cy };
  }
}

export function rectHitsAny(rect, rects) {
  return rects.some((r) => rOverlaps(r, rect));
}
