import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { buildChunk } from "../src/engine/voxel/compose.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { hash32 } from "../src/engine/core/hash.js";
import { YAWS, nearestYaw } from "../src/engine/core/placement.js";
import { frameOf } from "../src/engine/buildings/frame.js";
import { tierRects } from "../src/engine/buildings/archetypes.js";
import { walkabilityReport } from "../src/engine/validate/walkability.js";
import { buildingsOnRoads } from "../src/engine/validate/fit.js";
import { PART_REACH, partsIn } from "../src/engine/world/parts.js";
import { IS_SOLID } from "../src/engine/voxel/materials.js";
import { partChunk } from "../src/engine/world/partRaster.js";

/**
 * Turned buildings (ANGLED_WORLD_PLAN.md S3): a building on a lot fronting
 * a slanted street turns to face it, at the street's exact yaw, as one
 * oriented part granted from its cell's physics budget (about one part per
 * 3,600 m², one per chunk, none reaching more than 4 chunks). Its lot is the
 * planned lot cut to the block's slanted edges, clear of every street; its
 * canonical interior is rasterized through the frame's exact map (the
 * canonical-box rasterizer), its doors and furniture keep a way wide enough
 * for the viewer, who walks the world's axes, and it stands at the level of
 * the street at its door.
 */

const worlds = new Map();
function world(preset, size = null, features = null) {
  const key = `${preset}/${size}/${JSON.stringify(features)}`;
  if (!worlds.has(key)) {
    const cfg = presetConfig(preset, { size });
    if (features) cfg.world.angles = { ...cfg.world.angles, features: { ...cfg.world.angles?.features, ...features } };
    worlds.set(key, createWorld(cfg));
  }
  return worlds.get(key);
}

/** The turned buildings of the 3 x 3 cells round the origin: [{ env, lot, plan }]. */
function turned(w) {
  const out = [];
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const plan = w.cellPlan(i, j);
      for (const env of plan.buildings) if (env.turn) out.push({ env, lot: plan.lots.find((l) => l.building === env.id), plan, i, j });
    }
  return out;
}

const PRESETS = [["angledCities", null], ["angledOldHarbourTown", null], ["angledNordicTown", "fjord"]];

test("turned buildings face their slanted street at its exact yaw, in parts granted from the budget (a row of them one part)", () => {
  for (const [preset, size] of PRESETS) {
    const w = world(preset, size);
    const t = turned(w);
    let all = 0;
    for (let j = -1; j <= 1; j += 1) for (let i = -1; i <= 1; i += 1) all += w.cellPlan(i, j).buildings.length;
    // (a few in the grid cities, more in the old towns' tilted lanes; never more than 1 in 8)
    assert.ok(t.length >= 20 && t.length <= all / 8, `${preset}: ${t.length} of ${all} turned`);
    const ids = new Set();
    for (const { env, lot, plan } of t) {
      assert.ok(lot && lot.turn && lot.poly, `${env.id}: a turned lot`);
      // the slanted street's yaw: the cut the lot was trimmed to, its normal into the lot
      const f = lot.frontages.find((q) => q.slant !== undefined && q.cls && q.cls !== "alley");
      const blk = plan.net.blocks.find((b) => b.id === lot.block);
      const cut = blk.cuts.find((k) => k.id === f.slant);
      assert.equal(env.turn.yaw, nearestYaw(cut.ny, -cut.nx), `${env.id}: faces its street`);
      assert.ok(env.turn.yaw % (YAWS.length / 4) !== 0, `${env.id}: not a proper rotation`);
      // one part (its own or its row's), within reach, on a chunk its cell owns
      const part = plan.parts.find((p) => p.id === env.part);
      assert.ok(part && part.kind === "building" && part.members.includes(env.id) && part.placement.yaw === env.turn.yaw, `${env.id}: its part`);
      assert.ok(part.reach <= PART_REACH, `${env.id}: reach ${part.reach}`);
      assert.ok(part.home.cx * 32 + 31 >= plan.rect.x0 && part.home.cx * 32 + 31 < plan.rect.x1 && part.home.cy * 32 + 31 >= plan.rect.y0 && part.home.cy * 32 + 31 < plan.rect.y1, `${env.id}: home in its cell`);
      ids.add(part.id);
    }
    // every building part is a row of turned buildings, each turned in its lattice
    const members = t.filter(({ env }) => ids.has(env.part)).length;
    assert.equal(members, t.length);
    assert.ok(ids.size <= t.length);
    // the budget: one part per chunk, one per 3,600 m² of the cell, every part a turned building, a pitched road piece (S4) or a wing (S5)
    for (let j = -1; j <= 1; j += 1)
      for (let i = -1; i <= 1; i += 1) {
        const plan = w.cellPlan(i, j);
        const chunks = new Set();
        for (const p of plan.parts) {
          const k = `${p.home.cx},${p.home.cy},${p.home.cz}`;
          assert.ok(!chunks.has(k), `${plan.id}: two parts at home in ${k}`);
          chunks.add(k);
          assert.ok(p.kind === "road" || p.kind === "wing" || p.members.every((id) => plan.buildingById.get(id)?.turn && plan.buildingById.get(id).part === p.id), `${p.key}: turned buildings`);
        }
        assert.ok(plan.parts.length <= (((plan.rect.x1 - plan.rect.x0) * (plan.rect.y1 - plan.rect.y0)) / 64 / 3600) * 1.0001);
      }
  }
});

test("turned buildings stand in their lots, off every street, their footprint whole", () => {
  for (const [preset, size] of PRESETS) {
    const w = world(preset, size);
    for (const { env, lot } of turned(w)) {
      const f = frameOf(env);
      // every corner of every tier and annex inside the lot polygon (the planned lot cut to the slanted edges)
      const rects = [...env.tiers.flatMap((t) => t.rects), ...env.annexes.map((a) => a.canon)];
      for (const r of rects)
        for (const [u, v] of [[r.x0, r.y0], [r.x1 + 1, r.y0], [r.x1 + 1, r.y1 + 1], [r.x0, r.y1 + 1]]) {
          const [x, y] = f.pointToWorld(u, v);
          assert.ok(inPoly(lot.poly, x, y, 0.5), `${env.id}: corner (${u}, ${v}) outside its lot`);
        }
      // the bounds hold every voxel of the footprint
      const g = tierRects(env, 0)[0];
      for (const [u, v] of [[g.x0, g.y0], [g.x1, g.y1], [g.x0, g.y1], [g.x1, g.y0]]) {
        const [x, y] = f.toWorld(u, v);
        assert.ok(x >= env.bounds.x0 && x <= env.bounds.x1 && y >= env.bounds.y0 && y <= env.bounds.y1, `${env.id}: bounds`);
      }
    }
    // no footprint on a road (the fit audit, turned footprints tested in their own axes)
    const on = buildingsOnRoads(w, { x0: -4000, y0: -4000, x1: 4000, y1: 4000 }).filter((b) => b.id && turned(w).some((t) => t.env.id === b.id));
    assert.deepEqual(on, [], `${preset}: turned buildings on roads`);
  }
});

/** Is (x, y) inside a convex polygon grown by `pad`? */
function inPoly(poly, x, y, pad = 0) {
  let sign = 0;
  for (let k = 0; k < poly.length; k += 1) {
    const [ax, ay] = poly[k];
    const [bx, by] = poly[(k + 1) % poly.length];
    const len = Math.hypot(bx - ax, by - ay) || 1;
    const c = ((bx - ax) * (y - ay) - (by - ay) * (x - ax)) / len;
    if (Math.abs(c) <= pad) continue;
    if (sign !== 0 && Math.sign(c) !== sign) return false;
    sign = Math.sign(c);
  }
  return true;
}

test("turned buildings are walkable: the viewer reaches every room from the street (wider doors and ways across the turn)", () => {
  for (const [preset, size, n] of [["angledCities", null, 6], ["angledOldHarbourTown", null, 5], ["angledNordicTown", "fjord", 5]]) {
    const w = world(preset, size);
    const t = turned(w);
    // (a fixed spread of them: every k-th)
    const step = Math.max(1, Math.floor(t.length / n));
    for (let k = 0; k < t.length && k < n * step; k += step) {
      const { env } = t[k];
      const r = walkabilityReport(w, env, { maxFloors: 2 });
      assert.ok(r.ok, `${env.id} (${env.archetype}, yaw ${env.turn.yaw}): ${r.missing.map((m) => `${m.floor}:${m.type}`).join(" ")}`);
    }
  }
});

test("a turned building is rasterized through its frame's exact map: walls square in its own axes, pure, seamless", () => {
  const w = world("angledCities");
  const b = createWorld(presetConfig("angledCities"));
  // the largest turned walkup near the origin
  const { env } = turned(w)
    .filter((t) => t.env.archetype === "walkup")
    .sort((p, q) => q.env.U * q.env.V - p.env.U * p.env.V)[0];
  const f = frameOf(env);
  const g = tierRects(env, 0)[0];
  // a storey above the ground floor: every column of the footprint holds something at mid-height, every column
  // a cell outside it nothing of this building's walls (a turned footprint, not its world box)
  const z = env.baseZ + env.storyH[0] + 12;
  const cz = Math.floor(z / 32);
  const k = z - cz * 32 + 1;
  const bb = f.rectToWorld(g);
  let wall = 0;
  let inWall = 0;
  let outside = 0;
  for (let cy = Math.floor(bb.y0 / 32); cy <= Math.floor(bb.y1 / 32); cy += 1)
    for (let cx = Math.floor(bb.x0 / 32); cx <= Math.floor(bb.x1 / 32); cx += 1) {
      const ch = buildChunk(w, 0, cx, cy, cz);
      for (let j = 1; j <= 32; j += 1)
        for (let i = 1; i <= 32; i += 1) {
          const [u, v] = f.fromWorld(ch.wx(i), ch.wy(j));
          const m = ch.data[i + j * P + k * P2];
          const onWall = u >= g.x0 && u <= g.x1 && v >= g.y0 && v <= g.y1 && (u <= g.x0 + 1 || u >= g.x1 - 1 || v <= g.y0 + 1 || v >= g.y1 - 1);
          if (onWall) {
            wall += 1;
            if (IS_SOLID[m]) inWall += 1;
          }
          // (a cell or two outside the facade: open air, balconies and canopies aside)
          const out = (u === g.x0 - 3 || u === g.x1 + 3) && v > g.y0 + 12 && v < g.y1 - 12;
          if (out && IS_SOLID[m]) outside += 1;
        }
    }
  assert.ok(wall > 200 && inWall / wall > 0.6, `the turned wall: ${inWall} of ${wall} solid`);
  assert.equal(outside, 0, "nothing beside the side walls");
  // pure: any order, any worker; seamless across chunk borders through the building
  const [x, y] = f.toWorld(Math.floor(env.U / 2), Math.floor(env.V / 2));
  const mx = Math.floor(x / 32);
  const my = Math.floor(y / 32);
  const keys = [[0, mx, my, cz], [0, mx + 1, my, cz], [0, mx, my + 1, cz], [2, mx >> 2, my >> 2, cz >> 2]];
  const hash = (c) => {
    let h = 0;
    for (let q = 0; q < c.data.length; q += 1) h = hash32(h, c.data[q], q);
    return h;
  };
  assert.deepEqual(
    keys.map(([l, a, c, d]) => hash(buildChunk(w, l, a, c, d))),
    keys
      .slice()
      .reverse()
      .map(([l, a, c, d]) => hash(buildChunk(b, l, a, c, d)))
      .reverse(),
  );
  const A = buildChunk(w, 0, mx, my, cz);
  const E = buildChunk(w, 0, mx + 1, my, cz);
  const S = buildChunk(w, 0, mx, my + 1, cz);
  let diff = 0;
  for (let kk = 1; kk <= 32; kk += 1)
    for (let q = 1; q <= 32; q += 1) {
      if (A.data[33 + q * P + kk * P2] !== E.data[1 + q * P + kk * P2]) diff += 1;
      if (A.data[q + 33 * P + kk * P2] !== S.data[q + P + kk * P2]) diff += 1;
    }
  assert.equal(diff, 0, "chunks agree across their borders");
  // the audit sees the part where the building is
  assert.ok(partsIn(w, env.bounds).some((p) => p.id === env.part));
});

test("a row of turned buildings along a slanted edge is one part: one lattice, every member drawn in it", () => {
  let rows = 0;
  for (const [preset, size] of PRESETS) {
    const w = world(preset, size);
    for (let j = -1; j <= 1; j += 1)
      for (let i = -1; i <= 1; i += 1) {
        const plan = w.cellPlan(i, j);
        // (at most 1 building in 8 of the cell turned)
        const turnedN = plan.buildings.filter((b) => b.turn).length;
        assert.ok(turnedN * 8 <= plan.buildings.length, `${plan.id}: ${turnedN} of ${plan.buildings.length} turned`);
        for (const part of plan.parts) {
          if (part.kind !== "building" || part.members.length < 2) continue;
          rows += 1;
          const envs = part.members.map((id) => plan.buildingById.get(id));
          const lots = envs.map((env) => plan.lots.find((l) => l.building === env.id));
          // one slanted edge, one lattice: the members' frames are the part's placement, offset
          assert.ok(lots.every((l) => l.edge === lots[0].edge && l.block === lots[0].block), `${part.key}: one edge`);
          for (const env of envs) {
            assert.equal(env.turn.yaw, part.placement.yaw);
            assert.deepEqual(env.turn.origin, { x: part.placement.origin.x, y: part.placement.origin.y });
            // its footprint inside the part's extent
            const e = part.extent;
            assert.ok(env.turn.ou >= e.u0 && env.turn.ou + env.U - 1 <= e.u1 && env.turn.ov >= e.v0 && env.turn.ov + env.V - 1 <= e.v1 && env.bottomZ >= e.w0 && env.topZ <= e.w1, `${env.id}: in its part`);
          }
          // along the street, neighbours close together
          for (let k = 1; k < envs.length; k += 1) assert.ok(envs[k].turn.ou >= envs[k - 1].turn.ou && envs[k].turn.ou - (envs[k - 1].turn.ou + envs[k - 1].U) <= 48);
          // its lattice holds every member's walls: a storey up, the middle of each footprint's front wall is solid
          const sep = world(preset, size, { partsMode: "separate" });
          const sp = sep.cellPlan(i, j).parts.find((q) => q.id === part.id);
          for (const env of envs) {
            const u = env.turn.ou + Math.floor(env.U / 2);
            const v = env.turn.ov;
            const z = env.baseZ + env.storyH[0] + 12;
            const c = partChunk(sep, sp, 0, Math.floor(u / 32), Math.floor(v / 32), Math.floor(z / 32));
            const m = c.data[u - Math.floor(u / 32) * 32 + 1 + (v - Math.floor(v / 32) * 32 + 1) * P + (z - Math.floor(z / 32) * 32 + 1) * P2];
            assert.ok(IS_SOLID[m] || [0, 1].some((dv) => IS_SOLID[partChunk(sep, sp, 0, Math.floor(u / 32), Math.floor((v + dv + 1) / 32), Math.floor(z / 32)).data[u - Math.floor(u / 32) * 32 + 1 + (v + dv + 1 - Math.floor((v + dv + 1) / 32) * 32 + 1) * P + (z - Math.floor(z / 32) * 32 + 1) * P2]]), `${env.id}: its front wall in the row's lattice`);
          }
        }
      }
  }
  assert.ok(rows >= 10, `${rows} rows`);
});

test("turned buildings are the angled world's only, and switch off with features.buildings", () => {
  const off = world("angledOldHarbourTown", null, { buildings: false });
  let n = 0;
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const plan = off.cellPlan(i, j);
      n += plan.buildings.length;
      assert.ok(plan.buildings.every((b) => !b.turn) && plan.parts.every((p) => p.kind !== "building") && plan.lots.every((l) => !l.turn));
    }
  assert.ok(n > 500);
  const base = world("oldHarbourTown");
  assert.ok(base.cellPlan(0, 0).buildings.every((b) => !b.turn) && base.cellPlan(0, 0).parts.length === 0);
});
