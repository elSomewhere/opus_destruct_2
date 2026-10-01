import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { BIOMES } from "../src/engine/nature/biomes.js";
import { TREE_KINDS, treeBounds } from "../src/engine/nature/trees.js";
import { TREE_SEARCH, UNDER_SEARCH } from "../src/engine/nature/forest.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { vx } from "../src/engine/core/units.js";

test("biomes: climate zones cover most registered biomes within 120 km", () => {
  const w = createWorld({ seed: 1337 });
  const seen = new Set();
  const step = 8 * 2000;
  for (let y = -60000 * 8; y <= 60000 * 8; y += step)
    for (let x = -60000 * 8; x <= 60000 * 8; x += step) {
      const h = w.terrain.sample(x, y).h;
      seen.add(w.landCover.biomeAt(x, y, h / 8).id);
    }
  assert.ok(seen.size >= BIOMES.all().length - 2, `biomes seen: ${[...seen].join(", ")}`);
});

test("forests: open countryside far from any road still grows trees", () => {
  const w = createWorld({ seed: 1337 });
  let checked = 0;
  for (let gy = -40000; gy <= 40000 && checked < 3; gy += 3700)
    for (let gx = -40000; gx <= 40000 && checked < 3; gx += 3700) {
      const x = gx * 8;
      const y = gy * 8;
      const ts = w.terrain.sample(x, y);
      if (ts.u > 0.01 || ts.h < 24 || ts.mountain > 0.2) continue;
      const b = w.landCover.biomeAt(x, y, ts.h / 8);
      if (b.forest < 0.9 || w.landCover.forestDensity(x, y, ts.u, b) < 0.6) continue;
      // (lakes and rivers stay open water)
      if (w.isWet(x, y, 200)) continue;
      const trees = w.forest.treesIn({ x0: x - 200, y0: y - 200, x1: x + 200, y1: y + 200 });
      assert.ok(trees.length > 10, `forest at ${x},${y} (${b.id}) has ${trees.length} trees`);
      checked += 1;
    }
  assert.ok(checked > 0, "found forest samples");
});

test("rural cells stay natural land with a few farmsteads", () => {
  const w = createWorld({ seed: 1337 });
  let rural = 0;
  let farms = 0;
  for (let j = -8; j <= 8; j += 4)
    for (let i = -8; i <= 8; i += 4) {
      const plan = w.cellPlan(i, j);
      const ruralSubs = plan.net.subcells.filter((s) => s.district === "rural");
      if (!ruralSubs.length) continue;
      rural += 1;
      for (const sp of plan.spaces) assert.notEqual(sp.district, "rural", `rural block planned as open space in ${plan.id}`);
      farms += plan.lots.filter((l) => l.farmstead).length;
    }
  assert.ok(rural > 0, "sampled rural cells");
  assert.ok(farms > 0, "farmsteads exist");
});

test("highways: neighbouring settlements are linked by intercity routes", () => {
  const w = createWorld({ seed: 1337 });
  const hw = w.highways;
  const f = w.fields;
  let pairs = 0;
  for (let j = -2; j <= 2; j += 1)
    for (let i = -2; i <= 2; i += 1) {
      const s = f.settlement(i, j);
      const t = f.settlement(i + 1, j);
      if (!s || !t) continue;
      pairs += 1;
      // every lattice edge of the east-west leg exists (on one of the two L rows)
      const a0 = Math.round(s.x / hw.spacing);
      const a1 = Math.round(t.x / hw.spacing);
      const rows = [Math.round(s.y / hw.spacing), Math.round(t.y / hw.spacing)];
      const row = rows.find((b) => hw.routeHas(s, t, 0, Math.min(a0, a1), b)) ?? rows[0];
      for (let a = Math.min(a0, a1); a < Math.max(a0, a1); a += 1) assert.ok(hw.edgeExists(0, a, row), `edge ${a},${row} of ${s.id}-${t.id}`);
    }
  assert.ok(pairs > 0);
});

test("rivers: lots stay out of the channel and roads cross on bridges over water", async () => {
  const { pois } = await import("../src/engine/stream/queries.js");
  const { groundTile, buildChunk } = await import("../src/engine/voxel/compose.js");
  const { MAT } = await import("../src/engine/voxel/materials.js");
  const { P, P2 } = await import("../src/engine/voxel/chunk.js");
  const w = createWorld({ seed: 1337 });
  const br = pois(w, 0, 0).find((p) => p.label === "River bridge");
  assert.ok(br, "a river bridge near the spawn city");
  const c = w.cellAt(br.x, br.y);
  for (let j = c.j - 1; j <= c.j + 1; j += 1)
    for (let i = c.i - 1; i <= c.i + 1; i += 1)
      for (const lot of w.cellPlan(i, j).lots) assert.ok(!w.rivers.hitsRect(lot.rect, 0), `lot ${lot.id} in the river`);
  const cx = Math.floor(br.x / 32);
  const cy = Math.floor(br.y / 32);
  const tile = groundTile(w, 0, cx, cy);
  const idx = br.x - (cx * 32 - 1) + (br.y - (cy * 32 - 1)) * P;
  assert.ok(tile.deck && tile.deck[idx], "the road column is a bridge deck");
  const at = (z) => {
    const ch = buildChunk(w, 0, cx, cy, Math.floor(z / 32), tile);
    return ch.data[idx + (z - Math.floor(z / 32) * 32 + 1) * P2];
  };
  assert.ok(at(tile.z[idx]) !== 0, "deck surface is solid");
  assert.equal(at(tile.water[idx]), MAT.WATER, "water under the deck");
  assert.equal(at(tile.z[idx] - 4), 0, "air between deck and water");
});

test("terrain: mountain flanks are furrowed by gullies and couloirs down the fall line", () => {
  const w = createWorld({ seed: 1337 });
  // a steep, high flank (searched for)
  let best = null;
  for (let gy = -30000; gy <= -12000; gy += 1000)
    for (let gx = -24000; gx <= -6000; gx += 1000) {
      const x = gx * 8;
      const y = gy * 8;
      const ts = w.terrain.sample(x, y);
      if (ts.mountain < 0.7) continue;
      const g = Math.hypot(w.terrain.sample(x + 800, y).h - w.terrain.sample(x - 800, y).h, w.terrain.sample(x, y + 800).h - w.terrain.sample(x, y - 800).h) / 1600;
      if (!best || g > best.g) best = { x, y, g };
    }
  assert.ok(best && best.g > 0.4, "a steep flank");
  // across the slope: heights over 600 m, less a 160 m moving average, keep a few metres of grooves and ribs
  const gx = w.terrain.sample(best.x + 80, best.y).h - w.terrain.sample(best.x - 80, best.y).h;
  const gy = w.terrain.sample(best.x, best.y + 80).h - w.terrain.sample(best.x, best.y - 80).h;
  const L = Math.hypot(gx, gy) || 1;
  const [nx, ny] = [-gy / L, gx / L];
  const hs = [];
  for (let k = -150; k <= 150; k += 1) hs.push(w.terrain.sample(best.x + nx * k * 16, best.y + ny * k * 16).h / 8);
  let sq = 0;
  let n = 0;
  for (let i = 40; i < hs.length - 40; i += 1) {
    let m = 0;
    for (let q = -40; q <= 40; q += 1) m += hs[i + q];
    sq += (hs[i] - m / 81) ** 2;
    n += 1;
  }
  const sd = Math.sqrt(sq / n);
  assert.ok(sd > 4, `furrows ${sd.toFixed(1)} m`);
});

test("plants reach no further from their roots than the lattices are searched: no chunk misses the tip of one", () => {
  // every tree kind at its tallest and broadest (crowded or open-grown), many seeds
  let most = 0;
  for (const [kind, spec] of Object.entries(TREE_KINDS)) {
    for (let seed = 1; seed <= 120; seed += 1) {
      const t = { x: 0, y: 0, z: 0, h: Math.round(vx(spec.h[1]) * 1.1), r: vx(spec.r[1]) * 1.25, kind, seed: seed * 2654435761, open: seed % 2 === 0 };
      const bb = treeBounds(t);
      const reach = Math.max(-bb.x0, bb.x1, -bb.y0, bb.y1);
      most = Math.max(most, reach);
      assert.ok(reach <= TREE_SEARCH, `${kind} seed ${seed}: reaches ${reach} of ${TREE_SEARCH}`);
    }
  }
  assert.ok(most > 60, `the farthest reach ${most}`);
  // the understory as a wood grows it (shrubs, hazels, saplings, logs)
  const w = createWorld(presetConfig("nordicTown", { size: "forest" }));
  let n = 0;
  for (let y = -12000; y < -6000; y += 1500)
    for (let x = -12000; x < -6000; x += 1500)
      for (const t of w.forest.understoryIn({ x0: x, y0: y, x1: x + 300, y1: y + 300 })) {
        const reach = Math.max(t.x - t.bb.x0, t.bb.x1 - t.x, t.y - t.bb.y0, t.bb.y1 - t.y);
        assert.ok(reach <= UNDER_SEARCH, `${t.kind}: reaches ${reach} of ${UNDER_SEARCH}`);
        n += 1;
      }
  assert.ok(n > 500, `${n} understory plants`);
});
