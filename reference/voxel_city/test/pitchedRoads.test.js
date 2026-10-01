import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { buildChunk } from "../src/engine/voxel/compose.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { hash32 } from "../src/engine/core/hash.js";
import { PITCHES, YAWS } from "../src/engine/core/placement.js";
import { roadProfile, segmentLevel } from "../src/engine/network/roadLevel.js";
import { SLAB, slabFoot } from "../src/engine/network/roadParts.js";
import { partChunk } from "../src/engine/world/partRaster.js";
import { PART_REACH, partPriority } from "../src/engine/world/parts.js";
import { frameOf } from "../src/engine/buildings/frame.js";
import { tierRects } from "../src/engine/buildings/archetypes.js";
import { groundAt } from "../src/engine/validate/fit.js";
import { IS_SOLID } from "../src/engine/voxel/materials.js";
import { partVoxel, partsAt } from "../scripts/lib/parts.js";

/**
 * Pitched roads (ANGLED_WORLD_PLAN.md S4): in the angled world a steep
 * street climbs at the grades of the pitch table with level landings
 * between, and where it runs at one table grade in a direction of the yaw
 * table, its right-of-way becomes anchored parts pitched to that grade
 * (Rule C: no bonds against the anchored ground), one lattice per run, cut
 * into pieces within reach of their home chunks and granted from the
 * cell's budget. In parts mode (world.angles.partsMode "separate") the
 * world grid leaves them out, its ground stopping at their slab's foot,
 * and each is drawn in its own lattice: a plane, no 12.5 cm steps.
 */

const worlds = new Map();
function world(preset, size = null, angles = null) {
  const key = `${preset}/${size}/${JSON.stringify(angles)}`;
  if (!worlds.has(key)) worlds.set(key, createWorld(configOf(preset, size, angles)));
  return worlds.get(key);
}
function configOf(preset, size, angles) {
  const cfg = presetConfig(preset, { size });
  if (angles) cfg.world.angles = { ...cfg.world.angles, ...angles, features: { ...cfg.world.angles?.features, ...angles.features } };
  return cfg;
}

const STEEP = [["angledOldHarbourTown", null], ["angledNordicTown", "fjord"]];

/** The pitched road pieces of the 3 x 3 cells round the origin: [{ part, plan, seg }] (seg: its segment in the owner's view). */
function pieces(w) {
  const out = [];
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const plan = w.cellPlan(i, j);
      const view = w.roadView(i, j);
      for (const part of plan.parts) if (part.kind === "road") out.push({ part, plan, seg: view.segs.find((s) => s.road.id === part.road && s.idx === part.seg) });
    }
  return out;
}

/** The steepest piece near the origin (then by key). */
const steepest = (w) => pieces(w).sort((p, q) => Math.abs(q.part.grade) - Math.abs(p.part.grade) || (p.part.key < q.part.key ? -1 : 1))[0];

/** A part's local point in the world. */
function toWorld(part, u, v, w) {
  const { m, d, origin } = part.placement;
  return [origin.x + (m[0] * u + m[1] * v + m[2] * w) / d, origin.y + (m[3] * u + m[4] * v + m[5] * w) / d, origin.z + (m[6] * u + m[7] * v + m[8] * w) / d];
}

const hash = (c) => {
  let h = 0;
  for (let q = 0; q < c.data.length; q += 1) h = hash32(h, c.data[q], q);
  return h;
};

test("pitched roads: steep streets climb at the grades of the pitch table, level landings between", () => {
  const grades = PITCHES.slice(1).map((p) => p.s / p.c);
  const onTable = (g) => grades.some((q) => Math.abs(g - q) < 1e-6);
  for (const [preset, size] of STEEP) {
    const w = world(preset, size);
    const roads = new Set();
    for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) for (const s of w.roadView(i, j).segs) if (s.road.cell === `C${i}_${j}`) roads.add(s.road);
    let table = 0;
    let off = 0;
    for (const road of roads) {
      const prof = roadProfile(w, road);
      assert.ok(prof.ks && prof.ks.length === prof.kz.length, `${road.id}: a profile of knots`);
      for (let k = 1; k < prof.ks.length; k += 1) {
        const ds = prof.ks[k] - prof.ks[k - 1];
        assert.ok(ds > -1e-9, `${road.id}: knots in order`);
        if (ds <= 1e-9) continue;
        const g = Math.abs(prof.kz[k] - prof.kz[k - 1]) / ds;
        if (onTable(g)) table += ds;
        // (gentle stretches stay as fitted; only a climb too steep for the table keeps its own grade)
        else if (g >= 0.06 && g <= grades[grades.length - 1]) off += ds;
      }
    }
    assert.ok(table > 8 * 1500, `${preset}: ${(table / 8).toFixed(0)} m at table grades`);
    assert.equal(off, 0, `${preset}: ${(off / 8).toFixed(0)} m steep off the table`);
  }
  // the axis-aligned world's profiles are as they were fitted
  const base = world("oldHarbourTown");
  for (const s of base.roadView(0, 0).segs.slice(0, 40)) assert.equal(roadProfile(base, s.road).ks, undefined);
});

test("pitched road pieces: exact table yaw and pitch, lying on the street's level, one lattice per run, granted within budget and reach", () => {
  for (const [preset, size] of STEEP) {
    const w = world(preset, size);
    const ps = pieces(w);
    assert.ok(ps.length >= 20, `${preset}: ${ps.length} pieces`);
    const ids = new Set();
    const runs = new Map();
    let metres = 0;
    for (const { part, plan, seg } of ps) {
      const pl = part.placement;
      assert.ok(seg, `${part.key}: its segment`);
      // the street's exact direction, climbing at a table grade
      const Y = YAWS[pl.yaw];
      assert.ok(Math.abs(Y.c / Y.r - seg.dx) < 1e-9 && Math.abs(Y.s / Y.r - seg.dy) < 1e-9, `${part.key}: yaw ${pl.yaw} along its street`);
      const k = Math.abs(pl.pitch);
      assert.ok(k >= 1 && k < PITCHES.length, `${part.key}: pitch ${pl.pitch}`);
      assert.equal(part.grade, (Math.sign(pl.pitch) * PITCHES[k].s) / PITCHES[k].c);
      // anchored (Rule C), within reach, at home in its cell, a unique id and a road's priority (below every building's)
      assert.ok(part.anchored && pl.anchored, `${part.key}: anchored`);
      assert.ok(part.reach <= PART_REACH, `${part.key}: reach ${part.reach}`);
      const hx = part.home.cx * 32 + 31;
      const hy = part.home.cy * 32 + 31;
      assert.ok(hx >= plan.rect.x0 && hx < plan.rect.x1 && hy >= plan.rect.y0 && hy < plan.rect.y1, `${part.key}: home in its cell`);
      assert.ok(!ids.has(part.id), `${part.key}: id ${part.id} unique`);
      ids.add(part.id);
      assert.equal(part.priority, partPriority("road", part.id));
      assert.ok(part.priority < partPriority("building", 1));
      // its surface on the street's level all along (the centre line, at the slab's top)
      const W = part.extent.v1 + 1;
      for (let u = part.extent.u0; u <= part.extent.u1; u += 4) {
        const [x, y, z] = toWorld(part, u + 0.5, W / 2, SLAB);
        const lvl = segmentLevel(w, seg, (x - seg.ax) * seg.dx + (y - seg.ay) * seg.dy) + 1;
        assert.ok(Math.abs(z - lvl) <= 0.75, `${part.key}: surface ${z.toFixed(2)} at level ${lvl.toFixed(2)}`);
      }
      metres += (part.extent.u1 - part.extent.u0 + 1) / 8;
      const run = part.key.slice(0, part.key.lastIndexOf("."));
      runs.set(run, [...(runs.get(run) ?? []), part]);
    }
    assert.ok(metres > 500, `${preset}: ${metres.toFixed(0)} m of pitched street`);
    // a run's pieces share its lattice, end to end (a piece the budget refused leaves a gap, never an overlap)
    let shared = 0;
    for (const list of runs.values()) {
      list.sort((p, q) => p.extent.u0 - q.extent.u0);
      for (let k = 1; k < list.length; k += 1) {
        const [p, q] = [list[k - 1], list[k]];
        assert.deepEqual(q.placement.origin, p.placement.origin);
        assert.ok(q.placement.yaw === p.placement.yaw && q.placement.pitch === p.placement.pitch);
        assert.ok(q.extent.u0 > p.extent.u1, `${q.key}: after ${p.key}`);
        if (q.extent.u0 === p.extent.u1 + 1) shared += 1;
      }
    }
    assert.ok(shared > 0, `${preset}: runs of pieces end to end`);
  }
});

test("parts mode: the world grid leaves a pitched piece out, its ground meets the slab, its surface is one plane", () => {
  const grid = world("angledOldHarbourTown");
  const sep = world("angledOldHarbourTown", null, { partsMode: "separate" });
  const { part } = steepest(sep);
  assert.ok(Math.abs(part.grade) >= 0.1, `grade ${part.grade}`);
  const W = part.extent.v1 + 1;
  const v = Math.floor(W / 2);
  const stepped = new Set();
  const tops = new Set();
  let n = 0;
  for (let u = part.extent.u0 + 2; u <= part.extent.u1 - 2; u += 1) {
    // the centre line's world column
    const [x, y] = toWorld(part, u + 0.5, v + 0.5, SLAB);
    const X = Math.floor(x);
    const Y = Math.floor(y);
    const g = groundAt(grid, X, Y);
    const s = groundAt(sep, X, Y);
    // parts mode: the ground stops at the slab's foot, under the street the grid mode draws stepped
    assert.equal(s.z, Math.floor(slabFoot(part, X + 0.5, Y + 0.5)));
    assert.ok(s.z < g.z, `(${X}, ${Y}): ground ${s.z} under the road ${g.z}`);
    // and the slab sits on it: no air between
    assert.ok(partsAt(sep, [part], X + 0.5, Y + 0.5, s.z + 1.25) > 0, `(${X}, ${Y}): slab on the ground`);
    stepped.add(g.z);
    // the column's top in the part's own lattice
    let top = -1;
    for (let w = part.extent.w1; w >= part.extent.w0 && top < 0; w -= 1) if (partVoxel(sep, part, u, v, w)) top = w;
    tops.add(top);
    n += 1;
  }
  assert.ok(n > 64);
  // the world grid's street climbs in steps; the part's is one plane
  assert.ok(stepped.size >= (n / 8) * Math.abs(part.grade) * 8 * 0.8, `${stepped.size} steps over ${n} voxels`);
  assert.deepEqual([...tops], [SLAB - 1]);
});

test("parts mode: chunks and parts are pure (any order, any worker) and agree across chunk borders", () => {
  const cfg = () => configOf("angledOldHarbourTown", null, { partsMode: "separate" });
  const a = createWorld(cfg());
  const b = createWorld(cfg());
  const { part } = steepest(a);
  const W = part.extent.v1 + 1;
  const [x, y, z] = toWorld(part, (part.extent.u0 + part.extent.u1) / 2, W / 2, SLAB);
  const cx = Math.floor(x / 32);
  const cy = Math.floor(y / 32);
  const cz = Math.floor(z / 32);
  const keys = [[0, cx, cy, cz], [0, cx + 1, cy, cz], [0, cx, cy + 1, cz], [0, cx, cy, cz - 1], [2, cx >> 2, cy >> 2, cz >> 2]];
  assert.deepEqual(
    keys.map(([l, i, j, k]) => hash(buildChunk(a, l, i, j, k))),
    keys
      .slice()
      .reverse()
      .map(([l, i, j, k]) => hash(buildChunk(b, l, i, j, k)))
      .reverse(),
  );
  const A = buildChunk(a, 0, cx, cy, cz);
  const E = buildChunk(a, 0, cx + 1, cy, cz);
  const S = buildChunk(a, 0, cx, cy + 1, cz);
  let diff = 0;
  for (let k = 1; k <= 32; k += 1)
    for (let q = 1; q <= 32; q += 1) {
      if (A.data[33 + q * P + k * P2] !== E.data[1 + q * P + k * P2]) diff += 1;
      if (A.data[q + 33 * P + k * P2] !== S.data[q + P + k * P2]) diff += 1;
    }
  assert.equal(diff, 0, "chunks agree across their borders");
  // the part's own chunks: the same from another world, in another order, and across their borders
  const pb = b.cellPlan(...part.cell).parts.find((p) => p.id === part.id);
  assert.ok(pb && pb.key === part.key);
  const e = part.extent;
  const own = [];
  for (let k = Math.floor(e.w0 / 32); k <= Math.floor(e.w1 / 32); k += 1)
    for (let j = Math.floor(e.v0 / 32); j <= Math.floor(e.v1 / 32); j += 1) for (let i = Math.floor(e.u0 / 32); i <= Math.floor(e.u1 / 32); i += 1) own.push([i, j, k]);
  assert.ok(own.length >= 2);
  const ha = own.map(([i, j, k]) => hash(partChunk(a, part, 0, i, j, k)));
  const hb = own
    .slice()
    .reverse()
    .map(([i, j, k]) => hash(partChunk(b, pb, 0, i, j, k)))
    .reverse();
  assert.deepEqual(ha, hb);
  let seams = 0;
  let solid = 0;
  for (const [i, j, k] of own) {
    const c = partChunk(a, part, 0, i, j, k);
    for (let q = 0; q < c.data.length; q += 1) if (c.data[q]) solid += 1;
    if (i + 1 > Math.floor(e.u1 / 32)) continue;
    const d = partChunk(a, part, 0, i + 1, j, k);
    for (let kk = 1; kk <= 32; kk += 1) for (let q = 1; q <= 32; q += 1) if (c.data[33 + q * P + kk * P2] !== d.data[1 + q * P + kk * P2]) seams += 1;
  }
  assert.ok(solid > 1000);
  assert.equal(seams, 0, "the part's chunks agree across their borders");
});

test("parts mode: a turned building leaves the world grid and stands square in its own lattice, where the grid mode draws it", () => {
  const grid = world("angledCities");
  const sep = world("angledCities", null, { partsMode: "separate" });
  // the largest turned walkup near the origin, a storey up (at the sill, under the windows)
  let best = null;
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) for (const env of sep.cellPlan(i, j).buildings) if (env.turn && env.archetype === "walkup" && (!best || env.U * env.V > best.U * best.V)) best = env;
  const env = best;
  assert.ok(env && env.part, "a turned walkup");
  const part = sep.cellPlan(...[-1, 0, 1].flatMap((j) => [-1, 0, 1].map((i) => [i, j])).find(([i, j]) => sep.cellPlan(i, j).buildingById.has(env.id))).parts.find((p) => p.id === env.part);
  assert.ok(part && part.kind === "building" && part.members.includes(env.id));
  const f = frameOf(env);
  const g = tierRects(env, 0)[0];
  const z = env.baseZ + env.storyH[0] + 4;
  const { ou, ov } = env.turn;
  // in its lattice: the front and side walls, square in its axes, where the grid mode draws them in the world
  let wall = 0;
  let inPart = 0;
  let inGrid = 0;
  const at = new Map();
  const worldVoxel = (w, X, Y, Z) => {
    const k = `${X >> 5},${Y >> 5},${Z >> 5}`;
    if (!at.has(k)) at.set(k, buildChunk(w, 0, X >> 5, Y >> 5, Z >> 5));
    const c = at.get(k);
    return c.data[X - (X >> 5) * 32 + 1 + (Y - (Y >> 5) * 32 + 1) * P + (Z - (Z >> 5) * 32 + 1) * P2];
  };
  for (let u = g.x0; u <= g.x1; u += 1)
    for (const v of [g.y0, g.y1]) {
      wall += 1;
      if (IS_SOLID[partVoxel(sep, part, u + ou, v + ov, z)]) inPart += 1;
      const [x, y] = f.toWorld(u, v);
      if (IS_SOLID[worldVoxel(grid, Math.floor(x), Math.floor(y), z)]) inGrid += 1;
    }
  assert.ok(wall > 40 && inPart / wall > 0.6, `its walls: ${inPart} of ${wall} solid in its lattice`);
  assert.ok(Math.abs(inPart - inGrid) / wall < 0.2, `the lattice and the grid mode agree: ${inPart} vs ${inGrid} of ${wall}`);
  // in the world grid of parts mode: nothing of it inside its footprint at that height
  at.clear();
  let left = 0;
  let inside = 0;
  const bb = f.rectToWorld(g);
  for (let Y = bb.y0; Y <= bb.y1; Y += 1)
    for (let X = bb.x0; X <= bb.x1; X += 1) {
      const [u, v] = f.fromWorld(X, Y);
      if (u < g.x0 + 2 || u > g.x1 - 2 || v < g.y0 + 2 || v > g.y1 - 2) continue;
      inside += 1;
      if (IS_SOLID[worldVoxel(sep, X, Y, z)]) left += 1;
    }
  assert.ok(inside > 400);
  assert.equal(left, 0, "the world grid leaves the building to its part");
});

test("pitched road pieces are the angled world's only, and switch off with features.ramps", () => {
  const off = world("angledOldHarbourTown", null, { features: { ramps: false } });
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const plan = off.cellPlan(i, j);
      assert.ok(plan.parts.every((p) => p.kind !== "road") && plan.pitched.size === 0, `C${i}_${j}: no pitched pieces`);
    }
  for (const s of off.roadView(0, 0).segs.slice(0, 40)) assert.equal(roadProfile(off, s.road).ks, undefined);
  const base = world("oldHarbourTown");
  const plan = base.cellPlan(0, 0);
  assert.ok(plan.parts.length === 0 && plan.pitched.size === 0);
});
