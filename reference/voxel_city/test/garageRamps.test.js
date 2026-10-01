import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { buildChunk } from "../src/engine/voxel/compose.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { PITCHES } from "../src/engine/core/placement.js";
import { frameOf } from "../src/engine/buildings/frame.js";
import { floorZ } from "../src/engine/buildings/archetypes.js";
import { partChunk } from "../src/engine/world/partRaster.js";
import { PART_REACH, partPriority } from "../src/engine/world/parts.js";
import { MAT } from "../src/engine/voxel/materials.js";

/**
 * A parking garage's ramps as pitched parts (buildings/garageRamps.js): in
 * the angled world a garage plans its ramps at the gentlest table grade
 * that fits, so each rises exactly a storey over its run, and a ramp the
 * budget grants is one smooth slab from deck to deck, not anchored,
 * yielding to the decks; in parts mode the world grid leaves its strip
 * open. (The grid city with only the ramps of the angled world: its one
 * garage near the origin; the angled presets' lots have none near it.)
 */

function world(partsMode = "grid") {
  const cfg = presetConfig("cities");
  cfg.world.angles = { ...cfg.world.angles, enabled: true, partsMode, features: { roads: false, buildings: false, vegetation: false, wings: false, ramps: true } };
  return createWorld(cfg);
}

const at = (c, x, y, z) => c.data[x - c.cx * 32 + 1 + (y - c.cy * 32 + 1) * P + (z - c.cz * 32 + 1) * P2];
const chunkAt = (w, x, y, z) => buildChunk(w, 0, Math.floor(x / 32), Math.floor(y / 32), Math.floor(z / 32));

test("garage ramps: each a pitched slab part at a table grade, rising exactly a storey, deck to deck", () => {
  const w = world();
  const plan = w.cellPlan(0, 0);
  const env = plan.buildings.find((b) => b.archetype === "garage");
  assert.ok(env && env.pitchedRamps, "a garage");
  const bp = w.buildingPlan(env);
  assert.ok(bp.ramps.length >= 2);
  assert.deepEqual(env.rampParts, bp.ramps.map((r) => r.f));
  const F = frameOf(env);
  for (const r of bp.ramps) {
    // the gentlest table grade that fits: its run H c / s
    const T = PITCHES[r.pitch];
    assert.equal(r.rect.y1 - r.rect.y0 + 1, Math.round((r.H * T.c) / T.s));
    assert.ok(T.s / T.c >= 0.12 && T.s / T.c <= 0.17, `grade ${T.s / T.c}`);
    const part = plan.parts.find((p) => p.key === `${env.id}/ramp${r.f}`);
    assert.ok(part && part.kind === "ramp" && !part.anchored && part.reach <= PART_REACH);
    assert.equal(part.placement.pitch, r.pitch);
    // (between decks the world grid holds: it yields to the world grid)
    assert.equal(part.priority, partPriority("ramp", part.id, { yieldsToGrid: true }));
    assert.ok(part.priority < 0);
    // its surface (local w -1) climbs from the lower deck's floor to the upper one's, one cell per step at most
    const W = part.ramp.W;
    const v = Math.floor(W / 2);
    let last = null;
    for (let u = 0; u < part.ramp.Lu; u += 1) {
      const c = partChunk(w, part, 0, Math.floor(u / 32), Math.floor(v / 32), -1);
      const m = c.data[u - Math.floor(u / 32) * 32 + 1 + (v - Math.floor(v / 32) * 32 + 1) * P + (-1 + 32 + 1) * P2];
      assert.ok(m === MAT.FLOOR_CONCRETE || m === MAT.LINE_YELLOW, `ramp ${r.f}: its surface at u ${u}`);
      const z = part.placement.toWorld(u, v, -1)[2];
      if (last !== null) assert.ok(z - last >= 0 && z - last <= 1);
      last = z;
    }
    const z0 = part.placement.toWorld(0, v, -1)[2];
    assert.equal(z0, floorZ(env, r.f) + 1, "from the lower deck's floor");
    assert.ok(Math.abs(last - (floorZ(env, r.f + 1) + 1)) <= 1, `to the upper deck's: ${last} vs ${floorZ(env, r.f + 1) + 1}`);
    // its kerbs along both edges, over the surface
    const kc = partChunk(w, part, 0, 0, 0, 0);
    assert.ok([MAT.CONCRETE_LIGHT, MAT.HAZARD_YELLOW].includes(kc.data[10 + 1 + (0 + 1) * P + (3 + 1) * P2]));
    // up the ramp: the garage's canonical +v, in the world
    const [dx, dy] = F.dirToWorld(0, 1);
    const [ax, ay] = part.placement.toWorld(0, v, 0);
    const [bx, by] = part.placement.toWorld(60, v, 0);
    assert.ok((bx - ax) * dx + (by - ay) * dy > 50);
  }
});

test("garage ramps in parts mode: the world grid leaves their strips open (the decks around them kept); grid mode draws them stepped", () => {
  const grid = world();
  const sep = world("separate");
  const env = sep.cellPlan(0, 0).buildings.find((b) => b.archetype === "garage");
  const r = sep.buildingPlan(env).ramps[0];
  const F = frameOf(env);
  // the middle of the ramp's strip, a third of the way up, and the deck beside it
  const u = Math.floor((r.rect.x0 + r.rect.x1) / 2);
  const vv = r.rect.y0 + Math.floor((r.rect.y1 - r.rect.y0) / 3);
  const [x, y] = F.toWorld(u, vv);
  const z0 = floorZ(env, r.f);
  let sepSolid = 0;
  let gridSolid = 0;
  for (let z = z0; z < z0 + r.H; z += 1) {
    if (at(chunkAt(sep, x, y, z), x, y, z)) sepSolid += 1;
    if (at(chunkAt(grid, x, y, z), x, y, z)) gridSolid += 1;
  }
  assert.equal(sepSolid, 0, "parts mode: the strip open");
  assert.ok(gridSolid >= 2, "grid mode: the stepped ramp");
  // the deck beside the strip (an aisle's width off it) keeps its floor
  const side = r.rect.x0 > env.U / 2 ? r.rect.x0 - 20 : r.rect.x1 + 20;
  const [sx, sy] = F.toWorld(side, vv);
  assert.ok(at(chunkAt(sep, sx, sy, z0), sx, sy, z0) || at(chunkAt(sep, sx, sy, z0 + 1), sx, sy, z0 + 1), "the deck's floor");
});
