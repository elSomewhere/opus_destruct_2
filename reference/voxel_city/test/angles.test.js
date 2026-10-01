import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { makeConfig } from "../src/engine/config/defaults.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { Placement } from "../src/engine/core/placement.js";
import { PartBudget, makePart, partId, homeChunk, PART_REACH } from "../src/engine/world/parts.js";
import { ArterialGrid } from "../src/engine/network/arterials.js";
import { auditAngles, LOAD_RADIUS } from "../src/engine/validate/angles.js";

/**
 * The physics budget of the angled world (ANGLED_WORLD_PLAN.md §3, §5.2):
 * about one oriented part per 3,600 m² (6-8 resident within structvox's
 * 96 m load radius), granted in squares of `partCluster` times that holding
 * as many (a run of road pieces shares one), never more than `maxResident`
 * in any 96 m disc whichever cells they are of, at most one at home per chunk, none
 * reaching more than 4 chunks from its home, no two overlapping unless a
 * priority owns it.
 */

const R = LOAD_RADIUS * 8;
/** Mean parts within a 96 m disc when every budget square holds one: 28,953 m² / 3,600 m². */
const MEAN_CAP = (Math.PI * LOAD_RADIUS * LOAD_RADIUS) / 3600;

/** Resident parts round the disc centres of a rect: { mean, max }. */
function resident(homes, rect, step = 128) {
  let sum = 0;
  let n = 0;
  let max = 0;
  for (let y = rect.y0; y <= rect.y1; y += step)
    for (let x = rect.x0; x <= rect.x1; x += step) {
      let c = 0;
      for (const [hx, hy] of homes) if ((hx - x) ** 2 + (hy - y) ** 2 <= R * R) c += 1;
      sum += c;
      n += 1;
      max = Math.max(max, c);
    }
  return { mean: sum / n, max };
}

test("part budget: a flood of candidates keeps every cap: its squares' quotas, one part per chunk, the resident cap in every disc", () => {
  const config = makeConfig({ world: { angles: { enabled: true } } });
  const rect = { x0: -2480, y0: 1200, x1: 2544, y1: 5840 };
  const flood = (order) => {
    const budget = new PartBudget(config, rect);
    const homes = [];
    const chunks = new Map();
    for (const [x, y] of order) {
      const home = homeChunk(x, y, 480, rect);
      const k = budget.grant(x, y, home);
      if (k < 0) continue;
      assert.equal(k, homes.length);
      // (resident where its home chunk is, as the audit counts it)
      homes.push([home.cx * 32 + 16, home.cy * 32 + 16]);
      const key = `${home.cx},${home.cy},${home.cz}`;
      chunks.set(key, (chunks.get(key) ?? 0) + 1);
    }
    return { budget, homes, chunks };
  };
  // every chunk of the cell asks, in raster order
  const raster = [];
  for (let y = rect.y0 + 16; y < rect.y1; y += 32) for (let x = rect.x0 + 16; x < rect.x1; x += 32) raster.push([x, y]);
  const a = flood(raster);
  const { partCluster: cluster, maxResident, maxPartsPerChunk } = config.world.angles;
  assert.equal(a.budget.cluster, cluster);
  // (never more than one part per 3,600 m² of the cell, nor more in a square than its quota)
  assert.ok(a.budget.limit <= ((rect.x1 - rect.x0) * (rect.y1 - rect.y0)) / 64 / 3600);
  for (const n of a.chunks.values()) assert.ok(n <= maxPartsPerChunk);
  // clustered: every candidate as close to the cell's centre as it can be
  const cx = (rect.x0 + rect.x1) / 2;
  const cy = (rect.y0 + rect.y1) / 2;
  const clustered = raster.slice().sort((p, q) => Math.hypot(p[0] - cx, p[1] - cy) - Math.hypot(q[0] - cx, q[1] - cy));
  const b = flood(clustered);
  for (const f of [a, b]) {
    for (const n of f.budget.used) assert.ok(n <= cluster);
    // no disc (every 4 m) holds more than the resident cap, however the candidates bunch
    const res = resident(f.homes, rect, 32);
    assert.ok(res.max <= maxResident, `max ${res.max} > ${maxResident}`);
    // and the cap is what binds a flood: its densest disc is full
    assert.ok(res.max >= maxResident - 1, `max ${res.max}`);
  }
  // the density over the whole cell never exceeds one part per 3,600 m²
  const res = resident(a.homes, { x0: rect.x0 + R, y0: rect.y0 + R, x1: rect.x1 - R, y1: rect.y1 - R });
  assert.ok(res.mean <= MEAN_CAP + 0.5, `mean ${res.mean}`);
  // angles off: nothing is granted
  assert.equal(new PartBudget(makeConfig(), rect).grant(0, 2000, homeChunk(0, 2000, 0, rect)), -1);
});

test("part budget: cells flooded on their own keep the resident cap in every disc across their borders and corners", () => {
  const config = makeConfig({ seed: 7, world: { angles: { enabled: true } } });
  const lattice = new ArterialGrid(config);
  // the 3 x 3 cells round a lattice corner, each flooded by every chunk it owns, in raster order and from its far corner
  const homes = [];
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const rect = lattice.cellRect(i, j);
      const budget = new PartBudget(config, rect, { lattice });
      const order = [];
      for (let cy = Math.ceil((rect.y0 - 31) / 32); cy * 32 + 31 < rect.y1; cy += 1) for (let cx = Math.ceil((rect.x0 - 31) / 32); cx * 32 + 31 < rect.x1; cx += 1) order.push([cx, cy]);
      if ((i + j) & 1) order.reverse();
      for (const [cx, cy] of order) {
        const home = { cx, cy, cz: 15 };
        if (budget.grant(cx * 32 + 16, cy * 32 + 16, home) >= 0) homes.push([cx * 32 + 16, cy * 32 + 16]);
      }
    }
  // every disc (centres every 2 m over the corner's neighbourhood)
  const c = lattice.cellRect(0, 0);
  const res = resident(homes, { x0: c.x0 - 2400, y0: c.y0 - 2400, x1: c.x0 + 2400, y1: c.y0 + 2400 }, 16);
  const max = config.world.angles.maxResident;
  assert.ok(res.max <= max, `max ${res.max} > ${max}`);
  // (and the cap binds: some disc is full)
  assert.ok(res.max >= max - 1, `max ${res.max}`);
  assert.ok(homes.length > 100, `${homes.length} granted`);
});

test("parts: stable u32 ids, homes in the cell's own chunks, reach from the home chunk", () => {
  const ids = new Set();
  for (const [ci, cj] of [[0, 0], [1, 0], [0, 1], [-1, -1], [1000, -700], [-2045, 5]])
    for (let k = 0; k < 256; k += 17) {
      const id = partId(ci, cj, k);
      assert.ok(Number.isInteger(id) && id > 0 && id < 2 ** 32);
      ids.add(id);
    }
  assert.equal(ids.size, 6 * 16);
  // (ids repeat only 2,048 cells apart: over a thousand kilometres)
  assert.equal(partId(-1, -1, 7), partId(2047, 2047, 7));
  const rect = { x0: 100, y0: -40, x1: 4000, y1: 3000 };
  // a point in a chunk whose centre lies outside the cell: its home moves in
  const h = homeChunk(101, -39, 70, rect);
  assert.ok(h.cx * 32 + 31 >= rect.x0 && h.cy * 32 + 31 >= rect.y0 && h.cz === 2);
  const cell = { i: 0, j: 0, ci: 0, cj: 0, rect };
  const p = makePart({ cell, index: 3, key: "t", kind: "building", placement: new Placement({ origin: { x: 800, y: 900, z: 64 }, yaw: 9 }), extent: { u0: 0, v0: 0, w0: 0, u1: 199, v1: 99, w1: 40 } });
  assert.equal(p.id, partId(0, 0, 3));
  assert.ok(p.reach <= PART_REACH, `reach ${p.reach}`);
  assert.equal(p.home.cz, 2);
});

test("angles budget: the densest downtown (the angled infinite city) stays within the physics budget", () => {
  const world = createWorld(presetConfig("angledInfiniteCity"));
  const a = auditAngles(world, { x0: -4800, y0: -4800, x1: 4800, y1: 4800 });
  assert.ok(a.perChunk.max <= world.config.world.angles.maxPartsPerChunk, `per chunk ${a.perChunk.max}`);
  assert.ok(a.resident.mean <= MEAN_CAP, `resident mean ${a.resident.mean}`);
  // (within the resident cap in every disc, across cell borders too)
  assert.ok(a.resident.max <= world.config.world.angles.maxResident, `resident max ${a.resident.max}`);
  assert.ok(a.reach.max <= PART_REACH, `reach ${a.reach.max}`);
  assert.equal(a.overlap.unowned, 0);
});

test("angles budget: the thickest forest (the angled forest town) stays within the physics budget", () => {
  const world = createWorld(presetConfig("angledNordicTown", { size: "forest" }));
  const a = auditAngles(world, { x0: 14400, y0: -16800, x1: 20800, y1: -10400 });
  assert.ok(a.perChunk.max <= world.config.world.angles.maxPartsPerChunk, `per chunk ${a.perChunk.max}`);
  assert.ok(a.resident.mean <= MEAN_CAP, `resident mean ${a.resident.mean}`);
  // (within the resident cap in every disc, across cell borders too)
  assert.ok(a.resident.max <= world.config.world.angles.maxResident, `resident max ${a.resident.max}`);
  assert.ok(a.reach.max <= PART_REACH, `reach ${a.reach.max}`);
  assert.equal(a.overlap.unowned, 0);
});
