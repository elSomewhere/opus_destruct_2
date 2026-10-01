import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { groundSteps, roadJumps, buildingsOnRoads, plantFit } from "../src/engine/validate/fit.js";

/**
 * Terrain, roads, lots, buildings and plants fit together (validate/fit.js):
 * on a hillside lots are graded, not stacked terraces with cliffs between
 * them; streets are continuous through their junctions; no street runs
 * through a building; trees, plants and yard furniture stand on the ground
 * as the tiles shape it (cut and fill beside roads, graded town ground,
 * river banks).
 */

const worlds = new Map();
function world(preset, size = null) {
  const key = `${preset}/${size}`;
  if (!worlds.has(key)) worlds.set(key, createWorld(presetConfig(preset, { seed: 1337, size })));
  return worlds.get(key);
}

/** A square (world voxels) of `m` metres round the steepest built-up ground of the preset's main town. */
function hillside(w, m) {
  const s = w.fields.islandSettlements().towns[0];
  let best = null;
  for (let a = 0; a < 24; a += 1)
    for (const f of [0.2, 0.4, 0.6, 0.8]) {
      const x = Math.round(s.x + Math.cos((a / 24) * Math.PI * 2) * s.radius * f);
      const y = Math.round(s.y + Math.sin((a / 24) * Math.PI * 2) * s.radius * f);
      if (w.fields.urban(x, y).u < 0.3) continue;
      const h = (dx, dy) => w.terrain.sample(x + dx, y + dy).h;
      const g = Math.hypot(h(320, 0) - h(-320, 0), h(0, 320) - h(0, -320)) / 640;
      if (!best || g > best.g) best = { g, x, y };
    }
  const half = (m * 8) / 2;
  return { g: best.g, rect: { x0: best.x - half, y0: best.y - half, x1: best.x + half, y1: best.y + half } };
}

test("hillside towns: lots are graded, no cliffs between gardens, yards and streets", () => {
  const w = world("whiteSeaTown");
  const { g, rect } = hillside(w, 240);
  assert.ok(g > 0.15, `a steep town (${g.toFixed(2)})`);
  const { pairs } = groundSteps(w, rect, { lod: 1, thr: 4 });
  const max = (p) => pairs.get(p)?.max ?? 0;
  // (flat lots on their front sidewalk's level stood up to 17 m over their neighbours here;
  // what remains is the odd terrace between the pads of two narrow neighbours, ~3 m)
  assert.ok(max("lot-lot") <= 32, `lot-lot step ${max("lot-lot")} voxels`);
  assert.ok(max("lot-road") <= 16, `lot-road step ${max("lot-road")} voxels`);
  assert.ok(max("road-space") <= 16 && max("lot-space") <= 24, `space steps ${max("road-space")} / ${max("lot-space")}`);
});

test("streets are continuous through their junctions (no jumps in level)", () => {
  for (const [preset, size] of [["whiteSeaTown", null], ["nordicTown", "fjord"]]) {
    const w = world(preset, size);
    const r = roadJumps(w, 0, 0);
    // (a street climbs at most ~0.75 voxel per voxel between two close junctions; a jump is a step)
    assert.ok(r.jump <= 1.5, `${preset}: ${r.jump.toFixed(2)} voxels on ${r.road} at ${(r.along / 8).toFixed(1)} m`);
  }
});

test("no street runs through a building (block lots keep off every street's right-of-way)", () => {
  for (const [preset, size] of [["whiteSeaTown", null], ["nordicTown", "fjord"]]) {
    const w = world(preset, size);
    const s = w.fields.islandSettlements().towns[0];
    const r = s.radius;
    const hits = buildingsOnRoads(w, { x0: s.x - r, y0: s.y - r, x1: s.x + r, y1: s.y + r });
    // (a kerb sliver touching a wall at most)
    assert.ok(hits.every((h) => h.n <= 2), `${preset}: ${JSON.stringify(hits.filter((h) => h.n > 2).slice(0, 4))}`);
  }
});

test("trees, plants and yard trees stand on the ground as the tiles shape it", () => {
  const w = world("whiteSeaTown");
  const { rect } = hillside(w, 200);
  const r = plantFit(w, rect);
  assert.ok(r.checked > 50, `${r.checked} plants`);
  assert.deepEqual(r.off.slice(0, 4), [], `${r.off.length} plants off the ground`);
});
