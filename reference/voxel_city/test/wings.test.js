import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { makeConfig } from "../src/engine/config/defaults.js";
import { buildChunk } from "../src/engine/voxel/compose.js";
import { ChunkBuffer, P, P2 } from "../src/engine/voxel/chunk.js";
import { hash32 } from "../src/engine/core/hash.js";
import { YAWS, YAW_QUARTER, nearestYaw, yawVector, yawProduct, yawIndex } from "../src/engine/core/placement.js";
import { localPointToWorld, worldPointToLocal } from "../src/engine/core/obb.js";
import { frameOf, turnedFrame } from "../src/engine/buildings/frame.js";
import { tierRects, floorZ } from "../src/engine/buildings/archetypes.js";
import { EMBED, rasterizeWing, wingPlacement } from "../src/engine/buildings/wings.js";
import { chamferCut } from "../src/engine/buildings/chamfer.js";
import { OUT } from "../src/engine/buildings/interior/grid.js";
import { partChunk } from "../src/engine/world/partRaster.js";
import { PART_REACH, partPriority } from "../src/engine/world/parts.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../src/engine/network/roadSurface.js";
import { groundAt } from "../src/engine/validate/fit.js";
import { walkabilityReport } from "../src/engine/validate/walkability.js";
import { planDressing } from "../src/engine/city/dressing.js";
import { auditAngles } from "../src/engine/validate/angles.js";
import { IS_SOLID } from "../src/engine/voxel/materials.js";

/**
 * Wings, corner and canted bays (ANGLED_WORLD_PLAN.md S5): masses a
 * building square to the grid adds to its main one on its upper floors,
 * each an oriented part at a yaw of the table, cast into the main mass (its
 * back face inside the footprint, past the wall) and joined by priority:
 * the building owns what the two share, so the world grid draws a wing
 * only outside the footprint and its lattice holds the wall band it is cast
 * into. Granted last from the cell's budget, clear of every street's
 * carriageway, of other buildings, and high enough to walk under; off by
 * default, on in the angled presets (the budget audit shows the headroom).
 */

const worlds = new Map();
function world(preset, size = null, angles = null) {
  const key = `${preset}/${size}/${JSON.stringify(angles)}`;
  if (!worlds.has(key)) {
    const cfg = presetConfig(preset, { size });
    if (angles) cfg.world.angles = { ...cfg.world.angles, ...angles, features: { ...cfg.world.angles?.features, ...angles.features } };
    worlds.set(key, createWorld(cfg));
  }
  return worlds.get(key);
}

const PRESETS = [["angledCities", null], ["angledOldHarbourTown", null], ["angledNordicTown", "fjord"]];

/** The wings of the 3 x 3 cells round the origin: [{ w, env, plan, i, j }]. */
function wingsOf(wd) {
  const out = [];
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const plan = wd.cellPlan(i, j);
      for (const env of plan.buildings) for (const w of env.wings ?? []) out.push({ w, env, plan, i, j });
    }
  return out;
}

/** The floor of a wing a world height lies on (its roof above its top floor: -1). */
function floorAt(env, w, z) {
  if (z >= floorZ(env, w.f1 + 1)) return -1;
  let f = w.f0;
  while (f < w.f1 && floorZ(env, f + 1) <= z) f += 1;
  return f;
}

/** The main footprint's rects a wing's cell at height z meets (its roof: the floor above its top one and that one). */
function mainRects(env, w, z) {
  const f = floorAt(env, w, z);
  return f >= 0 ? tierRects(env, f) : [...tierRects(env, Math.min(w.f1 + 1, env.floors - 1)), ...tierRects(env, w.f1)];
}

/** Table yaw index of the exact direction (c, s). */
const yawOf = (c, s) => YAWS.findIndex((y) => y.c * s === y.s * c && y.c * c + y.s * s > 0);

/** World point -> canonical point of a building's frame (a turned frame's canonical cells are its lattice's, offset). */
function toCanon(F, x, y) {
  const [u, v] = worldPointToLocal(F.placement, x, y);
  return [u - (F.ou ?? 0), v - (F.ov ?? 0)];
}

const inRects = (rects, u, v, m = 0) => rects.some((r) => u >= r.x0 + m && u <= r.x1 + 1 - m && v >= r.y0 + m && v <= r.y1 + 1 - m);

const hash = (c) => {
  let h = 0;
  for (let q = 0; q < c.data.length; q += 1) h = hash32(h, c.data[q], q);
  return h;
};

test("wings: corner bays at 43.6-46.4°, canted bays, wings to a slanted street: each a part at a table yaw, cast into its building", () => {
  const counts = {};
  for (const [preset, size] of PRESETS) {
    const wd = world(preset, size);
    const ids = new Set();
    for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) for (const p of wd.cellPlan(i, j).parts) ids.add(p.id);
    const all = wingsOf(wd);
    let n = 0;
    for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) n += wd.cellPlan(i, j).parts.length;
    assert.equal(ids.size, n, `${preset}: part ids unique`);
    for (const { w, env, plan } of all) {
      counts[w.kind] = (counts[w.kind] ?? 0) + 1;
      assert.ok(!env.skyDoors && ["walkup", "midrise", "office", "rowhouse", "townhouse"].includes(env.archetype), `${env.id}: a winged archetype`);
      // (a turned building's are canted bays)
      assert.ok(!env.turn || w.kind === "bay", `${env.id}: a turned building's wing is a canted bay`);
      const part = plan.parts.find((p) => p.id === w.part);
      assert.ok(part && part.kind === "wing" && part.key === `${env.id}/${w.kind}` && part.env === env.id, `${env.id}: its part`);
      assert.equal(part.placement.yaw, w.turn.yaw);
      assert.equal(part.placement.yaw2, w.turn.yaw2 ?? 0);
      assert.ok(!part.anchored, `${part.key}: structure, not ground`);
      assert.ok(part.reach <= PART_REACH, `${part.key}: reach ${part.reach}`);
      if (w.kind === "chamfer") {
        // (it owns the stepped wall of the cut behind it)
        assert.equal(part.priority, partPriority("wing", part.id));
        assert.ok(part.priority > 0);
      } else if (env.turn) {
        // (cast into a turned building's part, which owns what they share)
        const main = plan.parts.find((p) => p.id === env.part);
        assert.equal(part.priority, partPriority("wing", part.id));
        assert.ok(part.priority > 0 && part.priority < main.priority, `${part.key}: below its building`);
      } else {
        // (cast into a building the world grid holds, it yields to the world grid, priority 0: structvox docs/GRIDS.md §3)
        assert.equal(part.priority, partPriority("wing", part.id, { yieldsToGrid: true }));
        assert.ok(part.priority < 0 && Number.isInteger(part.priority) && part.priority >= -(2 ** 31), `${part.key}: yields to the world grid`);
      }
      // its yaw: to the main frame, a corner's 43.6-46.4°, a canted bay's 12.7-16.3°, a wing the slanted street's own
      const F = frameOf(env);
      const wy = yawVector(w.turn.yaw, w.turn.yaw2 ?? 0);
      const fy = yawVector(F.placement.yaw, F.placement.yaw2);
      const rel = (w.turn.yaw - F.placement.yaw + YAWS.length) % YAWS.length;
      const deg = (((Math.atan2(wy.s * fy.c - wy.c * fy.s, wy.c * fy.c + wy.s * fy.s) * 180) / Math.PI + 540) % 360) - 180;
      if (w.kind === "corner" || w.kind === "chamfer") assert.ok(Math.abs(Math.abs(deg) - 45) < 1.5, `${part.key}: ${deg.toFixed(2)}°`);
      else if (w.kind === "bay") assert.ok(Math.abs(deg) > 12 && Math.abs(deg) < 17, `${part.key}: ${deg.toFixed(2)}°`);
      else {
        assert.equal(w.kind, "wing");
        const lot = plan.lots.find((l) => l.building === env.id);
        const f = lot.frontages.find((q) => q.slant !== undefined);
        const cut = plan.net.blocks.find((b) => b.id === lot.block).cuts.find((k) => k.id === f.slant);
        assert.equal(w.turn.yaw, nearestYaw(cut.ny, -cut.nx), `${part.key}: faces its street`);
        assert.ok(rel % YAW_QUARTER !== 0);
      }
      if (w.kind === "chamfer") continue;
      // upper floors only, its roof under the building's (one floor lower under a pitched one)
      assert.ok(w.f0 === 1 && w.f1 <= env.floors - (env.roof.type === "flat" ? 1 : 2) && w.f1 >= w.f0);
      // cast in: its back face past the main wall on every floor it spans; its outer face out in the open
      const pl = wingPlacement(w);
      const canon = (u, v) => toCanon(F, ...localPointToWorld(pl, u, v));
      for (let f = w.f0; f <= w.f1; f += 1) {
        for (const [u, v] of [[0, w.V], [w.U, w.V]]) assert.ok(inRects(tierRects(env, f), ...canon(u, v), EMBED + 1), `${part.key}: back corner (${u}, ${v}) in the main mass on floor ${f}`);
        assert.ok([0, w.U / 2, w.U].some((u) => !inRects(tierRects(env, f), ...canon(u, 0), -1)), `${part.key}: its face out of the main mass`);
      }
    }
  }
  assert.ok(counts.corner >= 40 && counts.bay >= 100 && counts.wing >= 1 && counts.chamfer >= 10, JSON.stringify(counts));
});

test("a turned building's canted bay turns by the exact product of the two yaws, cast into its front, drawn in both modes", () => {
  // (a looser budget than the presets': more turned buildings have room for a bay)
  const loose = { maxResident: 16, partArea: 1800 };
  const wd = world("angledCities", null, loose);
  const sep = world("angledCities", null, { ...loose, partsMode: "separate" });
  const bays = wingsOf(wd).filter((q) => q.env.turn);
  assert.ok(bays.length >= 3, `${bays.length} bays on turned buildings`);
  for (const { w, env, plan, i, j } of bays) {
    assert.equal(w.kind, "bay");
    // its rotation: the building's yaw times a canted bay's (12.7-16.3°), exact: a triple of its own
    const want = [yawOf(24, 7), yawOf(24, -7), yawOf(40, 9), yawOf(40, -9)].map((r) => yawProduct(env.turn.yaw, r));
    const got = yawVector(w.turn.yaw, w.turn.yaw2 ?? 0);
    assert.ok(want.some((q) => q.c === got.c && q.s === got.s && q.r === got.r), `${w.key}: ${JSON.stringify(got)}`);
    const pl = wingPlacement(w);
    assert.equal(pl.d, got.r);
    assert.deepEqual([pl.m[0], pl.m[1], pl.m[3], pl.m[4]], [got.c, 0 - got.s, got.s, got.c]);
    // on the front facade: its outer face in front of it, its back in the main mass
    assert.ok(w.canon.y0 < 0 && w.canon.y1 >= 0, `${w.key}: ${JSON.stringify(w.canon)}`);
    // drawn: in the world grid (grid mode) and in its own lattice (parts mode), the middle of its outer wall a floor up
    const u = Math.floor(w.U / 2);
    const z = floorZ(env, w.f0) + 4;
    const [x, y] = pl.toWorld(u, 1, z);
    const c = buildChunk(wd, 0, Math.floor(x / 32), Math.floor(y / 32), Math.floor(z / 32));
    assert.ok(IS_SOLID[c.data[x - Math.floor(x / 32) * 32 + 1 + (y - Math.floor(y / 32) * 32 + 1) * P + (z - Math.floor(z / 32) * 32 + 1) * P2]], `${w.key}: its wall in the world grid`);
    const part = sep.cellPlan(i, j).parts.find((p) => p.id === w.part);
    const pc = partChunk(sep, part, 0, Math.floor(u / 32), 0, Math.floor(z / 32));
    assert.ok(IS_SOLID[pc.data[u - Math.floor(u / 32) * 32 + 1 + 2 * P + (z - Math.floor(z / 32) * 32 + 1) * P2]], `${w.key}: its wall in its lattice`);
    assert.equal(part.priority, partPriority("wing", part.id));
    assert.ok(plan.parts.some((p) => p.id === env.part && p.priority > part.priority));
  }
});

test("a chamfer cuts its building's street corner off every floor, a slab on the cut line carrying the facade, the plans whole", () => {
  const wd = world("angledCities");
  const sep = world("angledCities", null, { partsMode: "separate" });
  const all = wingsOf(wd).filter((q) => q.w.kind === "chamfer");
  assert.ok(all.length >= 10, `${all.length} chamfers`);
  for (const { w, env, i, j } of all.slice(0, 8)) {
    const c = w.chamfer;
    assert.deepEqual(env.chamfer, c);
    assert.ok(!env.turn && env.roof.type === "flat" && env.floors <= 8);
    // the 20-21-29 triple: its legs 20 and 21 times k, its facade 29 k cells long
    const k = c.a / 20 === Math.floor(c.a / 20) && c.b === (c.a / 20) * 21 ? c.a / 20 : c.a / 21;
    assert.ok([c.a, c.b].sort().join() === [20 * k, 21 * k].sort().join() && w.U === 29 * k, JSON.stringify(c));
    // every floor from the ground up: the cut's cells outside, the rest of the corner inside, rooms off the cut
    const plan = wd.buildingPlan(env);
    assert.equal(plan.issues.length, 0);
    for (const F of plan.floors) {
      const g = F.grid;
      for (let v = 0; v < c.b + 4; v += 1)
        for (let x = 0; x < c.a + 4; x += 1) {
          const u = c.side === "L" ? x : env.U - 1 - x;
          const cut = chamferCut(c, env.U, u, v);
          if (F.index >= 0 && cut) assert.equal(g.get(u, v), OUT, `${env.id}: floor ${F.index} (${u}, ${v}) cut`);
          if (!cut) assert.notEqual(g.get(u, v), OUT, `${env.id}: floor ${F.index} (${u}, ${v}) kept`);
        }
    }
    // (walkable as it was: nothing the viewer reaches in the building without its chamfer is out of reach with it)
    const plain = world("angledCities", null, { features: { wings: false } }).cellPlan(i, j).buildingById.get(env.id);
    const miss = (r) => new Set(r.missing.map((m) => `${m.floor}:${m.type}`));
    const before = miss(walkabilityReport(world("angledCities", null, { features: { wings: false } }), plain, { maxFloors: 2 }));
    const after = [...miss(walkabilityReport(wd, env, { maxFloors: 2 }))].filter((m) => !before.has(m));
    assert.deepEqual(after, [], `${env.id}: out of reach with its chamfer`);
    // the slab: its outer face on the cut line, its cells behind it solid in the world grid (grid mode) and in its lattice
    const pl = wingPlacement(w);
    const Fr = frameOf(env);
    const z = floorZ(env, 1) + 5;
    let checked = 0;
    for (let s2 = 2; s2 < w.U - 2; s2 += 7) {
      // (the world voxel holding the slab's cell (s2, 0)'s centre: in front of the line it would be cut)
      const [x, y] = pl.toWorld(s2, 0, z);
      const [u, v] = Fr.fromWorld(x, y);
      assert.ok(!chamferCut(c, env.U, u, v), `${env.id}: slab cell (${s2}, 0) behind the line`);
      const ch = buildChunk(wd, 0, Math.floor(x / 32), Math.floor(y / 32), Math.floor(z / 32));
      assert.ok(IS_SOLID[ch.data[x - Math.floor(x / 32) * 32 + 1 + (y - Math.floor(y / 32) * 32 + 1) * P + (z - Math.floor(z / 32) * 32 + 1) * P2]], `${env.id}: its facade in the world grid`);
      // one cell out in front of it: open air (outside the building and the slab)
      const [ox, oy] = pl.toWorld(s2, -2, z);
      const oc = buildChunk(wd, 0, Math.floor(ox / 32), Math.floor(oy / 32), Math.floor(z / 32));
      assert.equal(oc.data[ox - Math.floor(ox / 32) * 32 + 1 + (oy - Math.floor(oy / 32) * 32 + 1) * P + (z - Math.floor(z / 32) * 32 + 1) * P2], 0, `${env.id}: air in front of its facade`);
      checked += 1;
    }
    assert.ok(checked >= 5);
    // parts mode: the world grid keeps the stepped wall of the cut; the slab, in its lattice, owns it
    const part = sep.cellPlan(i, j).parts.find((p) => p.id === w.part);
    const pc = partChunk(sep, part, 0, 0, 0, Math.floor(z / 32));
    assert.ok(IS_SOLID[pc.data[10 + 1 + P + (z - Math.floor(z / 32) * 32 + 1) * P2]], `${env.id}: its lattice`);
    assert.ok(part.priority > 0);
  }
});

test("wings keep clear: never over a carriageway or a kerb, over sidewalks of their own cell's streets only, walkable under, no tree or balcony in them", () => {
  const rs = makeRoadSample();
  for (const [preset, size] of PRESETS) {
    const wd = world(preset, size);
    const byCell = new Map();
    for (const q of wingsOf(wd)) byCell.set(`${q.i},${q.j}`, [...(byCell.get(`${q.i},${q.j}`) ?? []), q]);
    for (const [key, list] of byCell) {
      const [i, j] = key.split(",").map(Number);
      const plan = wd.cellPlan(i, j);
      const view = wd.roadView(i, j);
      const trees = planDressing(wd, i, j).trees;
      for (const { w, env } of list) {
        // (a chamfer stands on its building's ground, test below)
        if (w.kind === "chamfer") continue;
        const F = frameOf(env);
        const wf = turnedFrame(w.turn, w.U, w.V);
        let open = 0;
        for (let y = w.bounds.y0; y <= w.bounds.y1; y += 2)
          for (let x = w.bounds.x0; x <= w.bounds.x1; x += 2) {
            const [u, v] = wf.fromWorld(x, y);
            if (u < 0 || u >= w.U || v < 0 || v >= w.V) continue;
            const [mu, mv] = F.fromWorld(x, y);
            if (inRects(tierRects(env, w.f0), mu + 0.5, mv + 0.5)) continue;
            open += 1;
            sampleRoadSurface(view.near({ x0: x - 2, y0: y - 2, x1: x + 2, y1: y + 2 }), x + 0.5, y + 0.5, rs, wd.seed);
            assert.ok(![KIND.CARRIAGE, KIND.CURB, KIND.MEDIAN, KIND.SHOULDER].includes(rs.kind), `${env.id}/${w.kind}: over a carriageway at (${x}, ${y})`);
            if (rs.kind !== KIND.NONE) assert.equal(rs.seg.road.cell, plan.id, `${env.id}/${w.kind}: over another cell's street`);
            const g = groundAt(wd, x, y);
            assert.ok(w.z0 - g.z >= 18, `${env.id}/${w.kind}: ${w.z0 - g.z} voxels over the ground at (${x}, ${y})`);
            // no other building under it
            for (const o of plan.buildingsIn({ x0: x, y0: y, x1: x, y1: y })) {
              if (o === env) continue;
              const [ou, ov] = frameOf(o).fromWorld(x, y);
              assert.ok(!inRects(tierRects(o, 0), ou + 0.5, ov + 0.5), `${env.id}/${w.kind}: over ${o.id}`);
            }
          }
        assert.ok(open > 0, `${env.id}/${w.kind}: shows outside its building`);
        // street trees keep clear of it
        for (const t of trees) assert.ok(!(t.bb.x0 <= w.bounds.x1 && w.bounds.x0 <= t.bb.x1 && t.bb.y0 <= w.bounds.y1 && w.bounds.y0 <= t.bb.y1 && t.bb.z1 >= w.z0 && t.bb.z0 <= w.z1), `${env.id}/${w.kind}: a tree in it`);
        // no balcony where it is cast into the facade
        const bp = wd.buildingPlan(env);
        for (const f of bp?.floors ?? [])
          for (const d of f.grid.doors) {
            if (d.kind !== "balcony") continue;
            const c = w.canon;
            assert.ok(!(d.u1 >= c.x0 - 8 && d.u0 <= c.x1 + 8 && d.v1 >= c.y0 - 12 && d.v0 <= c.y1 + 12), `${env.id}/${w.kind}: a balcony at ${d.u0}..${d.u1}`);
          }
      }
    }
  }
});

test("a wing draws only outside its building's footprint in the world grid, and holds the wall it is cast into in its own lattice", () => {
  const wd = world("angledCities");
  // (a corner bay, a canted bay and a wing to a slanted street, each on a building square to the grid)
  const picks = ["corner", "bay", "wing"].map((kind) => ({ ...wingsOf(wd).find((q) => q.w.kind === kind && !q.env.turn), wdd: wd }));
  for (const { w, env, plan, wdd } of picks) {
    const F = frameOf(env);
    const wf = turnedFrame(w.turn, w.U, w.V);
    // the world grid: every cell it writes lies in its box, off the main footprint of its floor
    let face = 0;
    for (let cz = Math.floor(w.z0 / 32); cz <= Math.floor(w.z1 / 32); cz += 1)
      for (let cy = Math.floor(w.bounds.y0 / 32); cy <= Math.floor(w.bounds.y1 / 32); cy += 1)
        for (let cx = Math.floor(w.bounds.x0 / 32); cx <= Math.floor(w.bounds.x1 / 32); cx += 1) {
          const c = new ChunkBuffer(0, cx, cy, cz);
          c.data.fill(255);
          rasterizeWing(wdd, env, w, c);
          for (let k = 0; k < P; k += 1)
            for (let j = 0; j < P; j += 1)
              for (let i = 0; i < P; i += 1) {
                const m = c.data[i + j * P + k * P2];
                if (m === 255) continue;
                const [x, y, z] = [c.wx(i), c.wy(j), c.wz(k)];
                const [u, v] = wf.fromWorld(x, y);
                assert.ok(u >= 0 && u < w.U && v >= 0 && v < w.V && z >= w.z0 && z <= w.z1, `${env.id}/${w.kind}: outside its box`);
                const [mu, mv] = F.fromWorld(x, y);
                assert.ok(!mainRects(env, w, z).some((r) => mu >= r.x0 && mu <= r.x1 && mv >= r.y0 && mv <= r.y1), `${env.id}/${w.kind}: in the main footprint at (${x}, ${y}, ${z})`);
                if (v < 2 && IS_SOLID[m] && floorAt(env, w, z) >= 0) face += 1;
              }
        }
    assert.ok(face > 100, `${env.id}/${w.kind}: its outer face (${face})`);
    // its lattice: the same outside the footprint, the wall band it is cast into, nothing deeper in
    const part = plan.parts.find((p) => p.id === w.part);
    const pl = wingPlacement(w);
    let cast = 0;
    for (let cz = Math.floor(w.z0 / 32); cz <= Math.floor(w.z1 / 32); cz += 1)
      for (let cy = 0; cy <= Math.floor((w.V - 1) / 32); cy += 1)
        for (let cx = 0; cx <= Math.floor((w.U - 1) / 32); cx += 1) {
          const c = partChunk(wdd, part, 0, cx, cy, cz);
          for (let k = 1; k <= 32; k += 1)
            for (let j = 1; j <= 32; j += 1)
              for (let i = 1; i <= 32; i += 1) {
                if (!c.data[i + j * P + k * P2]) continue;
                const [u, v, z] = [c.wx(i), c.wy(j), c.wz(k)];
                assert.ok(u >= 0 && u < w.U && v >= 0 && v < w.V && z >= w.z0 && z <= w.z1, `${part.key}: outside its extent`);
                const [mu, mv] = toCanon(F, ...localPointToWorld(pl, u + 0.5, v + 0.5));
                const rects = mainRects(env, w, z);
                if (!inRects(rects, mu, mv)) continue;
                assert.ok(!inRects(rects, mu, mv, EMBED), `${part.key}: deeper in the main mass than its wall`);
                cast += 1;
              }
        }
    assert.ok(cast > 20, `${part.key}: cast into the main wall (${cast} cells)`);
  }
});

test("parts mode: the world grid leaves wings to their lattices; chunks through a wing are pure and agree across their borders", () => {
  const grid = world("angledCities");
  const sep = world("angledCities", null, { partsMode: "separate" });
  const { w, env } = wingsOf(grid).find((q) => q.w.kind === "corner");
  const wf = turnedFrame(w.turn, w.U, w.V);
  const F = frameOf(env);
  // a storey up, the cells of the wing's face outside the building: solid in grid mode, air in parts mode
  const z = floorZ(env, w.f0) + 12;
  let solid = 0;
  let left = 0;
  const at = (wd, x, y) => {
    const c = buildChunk(wd, 0, x >> 5, y >> 5, z >> 5);
    return c.data[x - (x >> 5) * 32 + 1 + (y - (y >> 5) * 32 + 1) * P + (z - (z >> 5) * 32 + 1) * P2];
  };
  for (let y = w.bounds.y0; y <= w.bounds.y1; y += 1)
    for (let x = w.bounds.x0; x <= w.bounds.x1; x += 1) {
      const [u, v] = wf.fromWorld(x, y);
      if (u < 2 || u >= w.U - 2 || v < 0 || v > 1) continue;
      const [mu, mv] = F.fromWorld(x, y);
      if (inRects(tierRects(env, w.f0), mu - 2, mv - 2) || inRects(tierRects(env, w.f0), mu + 3, mv + 3)) continue;
      if (IS_SOLID[at(grid, x, y)]) solid += 1;
      if (at(sep, x, y)) left += 1;
    }
  assert.ok(solid > 10, `the face in grid mode (${solid})`);
  assert.equal(left, 0, "parts mode leaves the wing to its lattice");
  // pure and seamless through the wing
  const b = createWorld(presetConfig("angledCities"));
  const cx = (w.bounds.x0 + w.bounds.x1) >> 6;
  const cy = (w.bounds.y0 + w.bounds.y1) >> 6;
  const cz = z >> 5;
  const keys = [[0, cx, cy, cz], [0, cx + 1, cy, cz], [0, cx, cy + 1, cz], [2, cx >> 2, cy >> 2, cz >> 2]];
  assert.deepEqual(
    keys.map(([l, i, j, k]) => hash(buildChunk(grid, l, i, j, k))),
    keys
      .slice()
      .reverse()
      .map(([l, i, j, k]) => hash(buildChunk(b, l, i, j, k)))
      .reverse(),
  );
  const A = buildChunk(grid, 0, cx, cy, cz);
  const E = buildChunk(grid, 0, cx + 1, cy, cz);
  const S = buildChunk(grid, 0, cx, cy + 1, cz);
  let diff = 0;
  for (let k = 1; k <= 32; k += 1)
    for (let q = 1; q <= 32; q += 1) {
      if (A.data[33 + q * P + k * P2] !== E.data[1 + q * P + k * P2]) diff += 1;
      if (A.data[q + 33 * P + k * P2] !== S.data[q + P + k * P2]) diff += 1;
    }
  assert.equal(diff, 0, "chunks agree across their borders");
});

test("wings are granted last: turned buildings and pitched roads keep their parts, the budget its caps, the buildings their walks", () => {
  for (const [preset, size] of PRESETS) {
    const on = world(preset, size);
    const off = world(preset, size, { features: { wings: false } });
    for (let j = -1; j <= 1; j += 1)
      for (let i = -1; i <= 1; i += 1) {
        const a = on.cellPlan(i, j).parts.filter((p) => p.kind !== "wing").map((p) => `${p.id}:${p.key}`);
        const b = off.cellPlan(i, j).parts.map((p) => `${p.id}:${p.key}`);
        assert.deepEqual(a, b, `${preset} C${i}_${j}: the same turned buildings and road pieces`);
        assert.ok(off.cellPlan(i, j).buildings.every((e) => !e.wings) && !off.cellPlan(i, j).wings);
      }
  }
  // the budget with wings on: within its caps (the headroom the audit shows)
  const wd = world("angledCities");
  const a = auditAngles(wd, { x0: -4800, y0: -4800, x1: 4800, y1: 4800 });
  const ang = wd.config.world.angles;
  assert.ok(a.perChunk.max <= ang.maxPartsPerChunk && a.reach.max <= PART_REACH && a.overlap.unowned === 0);
  assert.ok(a.resident.max <= ang.maxResident && a.resident.mean < 6, `resident ${a.resident.mean.toFixed(2)} / ${a.resident.max}`);
  // buildings with wings stay walkable (no balcony door into a wing, the rest as it was)
  const some = wingsOf(wd)
    .filter((q) => q.w.kind === "corner")
    .slice(0, 4);
  for (const { env } of some) {
    const r = walkabilityReport(wd, env, { maxFloors: 3 });
    assert.ok(r.ok, `${env.id}: ${r.missing.map((m) => `${m.floor}:${m.type}`).join(" ")}`);
  }
});

test("pitched roads, parts and wings take their angles from the exact tables (no sin, cos, atan2 or hypot on the generation path)", () => {
  for (const f of ["src/engine/buildings/wings.js", "src/engine/network/roadParts.js", "src/engine/world/partRaster.js", "src/engine/world/parts.js"]) {
    const src = readFileSync(new URL(`../${f}`, import.meta.url), "utf8");
    assert.ok(!/Math\.(sin|cos|tan|asin|acos|atan2?|hypot)\b/.test(src), `${f}: a transcendental`);
  }
  // the bays' yaws are the table's triples: 20-21-29 at a corner, 7-24-25 and 9-40-41 on a facade
  const deg = (k) => (Math.atan2(YAWS[k].s, YAWS[k].c) * 180) / Math.PI;
  const rels = new Set();
  const gcd = (a, b) => (b ? gcd(b, a % b) : Math.abs(a));
  for (const { w, env } of wingsOf(world("angledCities"))) {
    if (w.kind === "wing") continue;
    // (the bay's rotation over its building's, exact: W times the conjugate of B, reduced)
    const W = yawVector(w.turn.yaw, w.turn.yaw2 ?? 0);
    const B = yawVector(frameOf(env).placement.yaw, frameOf(env).placement.yaw2);
    const c = W.c * B.c + W.s * B.s;
    const sn = W.s * B.c - W.c * B.s;
    const g = gcd(gcd(c, sn), W.r * B.r);
    rels.add(yawIndex({ c: c / g + 0, s: sn / g + 0, r: (W.r * B.r) / g }));
  }
  assert.ok(rels.size >= 4);
  for (const k of rels) assert.ok([29, 25, 41].includes(YAWS[k].r), `yaw ${k} (${deg(k).toFixed(2)}°, r ${YAWS[k].r})`);
});

test("wings are off by default and the angled presets' only", () => {
  assert.equal(makeConfig().world.angles.features.wings, false);
  assert.equal(makeConfig(presetConfig("angledCities")).world.angles.features.wings, true);
  const base = world("cities");
  for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) assert.ok(base.cellPlan(i, j).buildings.every((e) => !e.wings) && base.cellPlan(i, j).parts.length === 0);
});
