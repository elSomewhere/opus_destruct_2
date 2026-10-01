import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { PRESETS, presetConfig, presetSeason, presetViewer } from "../src/engine/config/presets.js";
import { groundTile } from "../src/engine/voxel/compose.js";
import { MAT } from "../src/engine/voxel/materials.js";
import { walkabilityReport } from "../src/engine/validate/walkability.js";
import { seasonOf, SEASONS } from "../src/engine/world/season.js";
import { townPlan } from "../src/engine/city/townPlan.js";

/**
 * Seasons, the small Nordic island presets and the town layout: organic
 * old towns, frayed town edges, landmarks (market square, church,
 * cemetery, allotments, wharf), coast landmarks, creeks.
 */

const worlds = new Map();
function world(preset, size = null, season = null) {
  const key = `${preset}/${size}/${season}`;
  if (!worlds.has(key)) worlds.set(key, createWorld(presetConfig(preset, { size, seed: 1337, season })));
  return worlds.get(key);
}

/** Distinct cells around a point (voxels), each once. */
function cellsAround(w, x, y, r) {
  const out = new Map();
  for (let j = y - r; j <= y + r; j += 1000) for (let i = x - r; i <= x + r; i += 1000) {
    const c = w.cellAt(i, j);
    out.set(`${c.i},${c.j}`, c);
  }
  return [...out.values()];
}

function townStuff(w) {
  const s = w.fields.islandSettlements().towns[0];
  const r = Math.round(s.radius * 1.4);
  const spaces = [];
  const buildings = [];
  const roads = [];
  const blocks = [];
  for (const c of cellsAround(w, 0, 0, r)) {
    const p = w.cellPlan(c.i, c.j);
    spaces.push(...p.spaces);
    buildings.push(...p.buildings);
    roads.push(...p.net.roads);
    blocks.push(...p.net.blocks);
  }
  return { s, spaces, buildings, roads, blocks };
}

test("seasons: presets carry a season (their default), every season resolves, moods differ", () => {
  assert.equal(presetSeason("nordicIsland"), "winter");
  assert.equal(presetSeason("nordicTown"), "summer");
  assert.equal(presetSeason("oldHarbourTown"), "autumn");
  assert.equal(presetSeason("cities"), "summer");
  assert.equal(presetSeason("nordicTown", "autumn"), "autumn");
  assert.equal(presetSeason("nordicTown", "monsoon"), "summer");
  for (const p of PRESETS.all())
    for (const s of SEASONS) {
      assert.equal(presetConfig(p.id, { season: s.id }).world.season, s.id);
      assert.ok(presetViewer(p.id, s.id).atmosphere, `${p.id} ${s.id}`);
    }
  assert.notEqual(presetViewer("nordicTown", "winter").atmosphere.sunElevation, presetViewer("nordicTown", "summer").atmosphere.sunElevation);
});

/** Material histogram of LOD-2 ground tiles on a ring around the town. */
function ringGround(w, rM) {
  const counts = new Map();
  const cs = 32 << 2;
  for (let k = 0; k < 12; k += 1) {
    const a = (k / 12) * Math.PI * 2;
    const x = Math.cos(a) * rM * 8;
    const y = Math.sin(a) * rM * 8;
    if (w.fields.coastDistance(x, y) < 30) continue;
    const t = groundTile(w, 2, Math.floor(x / cs), Math.floor(y / cs));
    for (let i = 0; i < t.top.length; i += 1) if (!(t.water[i] > t.z[i])) counts.set(t.top[i], (counts.get(t.top[i]) ?? 0) + 1);
  }
  return (...ms) => ms.reduce((n, m) => n + (counts.get(m) ?? 0), 0);
}

test("seasons: snow in winter only, autumn grass and leaves, spring green, summer flowers", () => {
  const summer = ringGround(world("nordicTown", "forest", "summer"), 1000);
  const winter = ringGround(world("nordicTown", "forest", "winter"), 1000);
  const autumn = ringGround(world("nordicTown", "forest", "autumn"), 1000);
  // (the forest island's ring is bog: spring shows on the temperate countryside round the spawn city)
  const spring = ringGround(world("cities", null, "spring"), 4200);
  const snow = [MAT.SNOW, MAT.SNOW_WIND];
  assert.equal(summer(...snow), 0, "no snow in summer");
  assert.ok(winter(...snow) > 2000, `winter snow ${winter(...snow)}`);
  assert.ok(winter(...snow) > 4 * winter(MAT.GRASS, MAT.GRASS_DRY, MAT.GRASS_LAWN, MAT.MOSS), "mostly white in winter");
  assert.ok(autumn(MAT.GRASS_AUTUMN, MAT.GRASS_STRAW, MAT.HEATHER, MAT.LEAF_LITTER, MAT.MARSH_AUTUMN) > 500, "autumn ground");
  assert.equal(autumn(MAT.GRASS_SPRING), 0);
  assert.ok(spring(MAT.GRASS_SPRING) > 100, "fresh spring grass");
  // wild flowers in the summer meadows (LOD 0/1 only)
  const w = world("nordicTown", "forest", "summer");
  let flowers = 0;
  for (let k = 0; k < 40 && flowers < 5; k += 1) {
    const a = (k / 40) * Math.PI * 2;
    const x = Math.cos(a) * 900 * 8;
    const y = Math.sin(a) * 900 * 8;
    const t = groundTile(w, 1, Math.floor(x / 64), Math.floor(y / 64));
    for (const m of t.top) if (m === MAT.FLOWER_LUPIN || m === MAT.FLOWER_YELLOW || m === MAT.FLOWER_WHITE || m === MAT.FLOWER_PINK || m === MAT.HEATHER_BLOOM) flowers += 1;
  }
  assert.ok(flowers >= 5, `flowers ${flowers}`);
});

test("seasons: trees change with the season (bare in winter, autumn colours, spring green)", () => {
  const looks = (season) => {
    const s = seasonOf({ world: { season } });
    const out = { bare: 0, autumn: 0, spring: 0, n: 0 };
    for (let k = 0; k < 200; k += 1) {
      const look = s.treeLook(k % 2 ? "birch" : "oak", k * 7919, 0.36);
      out.n += 1;
      if (!look) continue;
      if (look.bare) out.bare += 1;
      if (look.leaves?.some((m) => m === MAT.LEAVES_YELLOW || m === MAT.LEAVES_RED || m === MAT.LEAVES_AUTUMN)) out.autumn += 1;
      if (look.leaves?.includes(MAT.LEAVES_SPRING)) out.spring += 1;
    }
    return out;
  };
  assert.ok(looks("winter").bare > 190);
  assert.equal(looks("summer").bare, 0);
  assert.ok(looks("autumn").autumn > 100);
  assert.ok(looks("spring").spring > 60);
  // conifers keep their needles, warm lands hardly change
  assert.equal(seasonOf({ world: { season: "winter" } }).treeLook("spruce", 1, 0.36)?.bare ?? false, false);
  assert.equal(seasonOf({ world: { season: "autumn" } }).treeLook("oak", 3, 0.8), null);
  // forest trees carry their look
  const w = world("nordicTown", "forest", "winter");
  const trees = w.forest.treesIn({ x0: 4000, y0: 4000, x1: 5600, y1: 5600 });
  assert.ok(trees.length > 10 && trees.some((t) => t.look?.snow > 0), "snow on the forest");
});

test("small islands: 4-6 km, one small town at the origin, every variant", () => {
  const variants = [...PRESETS.get("nordicTown").sizes.map((z) => ["nordicTown", z.id]), ["oldHarbourTown", null], ["whiteSeaTown", null]];
  for (const [id, size] of variants) {
    const w = world(id, size);
    const isl = w.fields.island;
    assert.ok(isl.R <= 3000, `${id}/${size} radius ${isl.R}`);
    const { towns, villages } = w.fields.islandSettlements();
    assert.equal(towns.length, 1, `${id}/${size}: one town`);
    assert.ok(towns[0].radius / 8 < 800, `${id}/${size}: a small town (${(towns[0].radius / 8).toFixed(0)} m)`);
    assert.ok(w.fields.coastDistance(0, 0) > 0, "the town is on land");
    for (const v of villages) assert.ok(w.fields.coastDistance(v.x, v.y) > 0, "hamlets on land");
    assert.equal(w.highways, null);
    assert.equal(w.subway, null);
  }
});

test("town plan: a market square, a church, a cemetery with a chapel, allotments and a wharf", () => {
  const w = world("nordicTown", "fjord");
  const { s, spaces, buildings } = townStuff(w);
  const kinds = new Set(spaces.map((q) => q.kind));
  for (const k of ["square", "cemetery", "allotments"]) assert.ok(kinds.has(k), `${k} in ${[...kinds]}`);
  const arch = new Set(buildings.map((b) => b.archetype));
  for (const a of ["church", "wharfhouse", "townhouse"]) assert.ok(arch.has(a), `${a} in ${[...arch]}`);
  const cemetery = spaces.find((q) => q.kind === "cemetery");
  assert.ok(cemetery.chapel, "a chapel in the cemetery");
  // the plan's landmarks sit on land and most found a block
  const anchors = townPlan(w, s).anchors;
  assert.ok(anchors.length >= 8);
  assert.ok(anchors.filter((a) => a.block).length >= anchors.length - 3, JSON.stringify(anchors.map((a) => [a.kind, a.block])));
});

test("old towns: irregular cobbled blocks, lanes, pitched roofs; the town frays at its edge", () => {
  const w = world("nordicTown", "fjord");
  const { s, roads, blocks, buildings } = townStuff(w);
  const old = blocks.filter((b) => b.district === "oldtown");
  assert.ok(old.length > 20, `${old.length} old-town blocks`);
  const sides = old.map((b) => Math.min(b.prop.x1 - b.prop.x0, b.prop.y1 - b.prop.y0));
  const mean = sides.reduce((a, b) => a + b, 0) / sides.length;
  const sd = Math.sqrt(sides.reduce((a, b) => a + (b - mean) ** 2, 0) / sides.length);
  assert.ok(sd / mean > 0.2, `varied blocks (cv ${(sd / mean).toFixed(2)})`);
  assert.ok(roads.some((r) => r.cls === "lane"), "narrow lanes");
  assert.ok(roads.filter((r) => r.paving === "cobble").length > 10, "cobbled streets");
  const blockish = buildings.filter((b) => (b.archetype === "walkup" || b.archetype === "midrise") && (b.district === "oldtown" || b.district === "mixed"));
  assert.ok(blockish.filter((b) => b.roof.type !== "flat").length > blockish.length * 0.6, "pitched roofs on the stone blocks");
  // the edge: a ring round the town is partly built, partly open land
  const R = s.radius;
  let inBlock = 0;
  let n = 0;
  for (let a = 0; a < 180; a += 1)
    for (const f of [0.85, 0.95, 1.05]) {
      const x = Math.cos((a / 180) * Math.PI * 2) * R * f;
      const y = Math.sin((a / 180) * Math.PI * 2) * R * f;
      if (w.fields.coastDistance(x, y) < 30) continue;
      n += 1;
      if (blocks.some((b) => x >= b.prop.x0 && x <= b.prop.x1 && y >= b.prop.y0 && y <= b.prop.y1 && b.district !== "rural")) inBlock += 1;
    }
  assert.ok(n > 100 && inBlock > n * 0.1 && inBlock < n * 0.85, `edge ring ${inBlock}/${n} built`);
});

test("wharf warehouses and cemetery chapels have valid, walkable interiors", () => {
  const w = world("nordicTown", "fjord");
  const { buildings } = townStuff(w);
  const wharf = buildings.filter((b) => b.archetype === "wharfhouse").slice(0, 3);
  const chapel = buildings.filter((b) => b.archetype === "church" && b.lot.endsWith("/l0")).slice(0, 1);
  assert.ok(wharf.length >= 2 && chapel.length === 1);
  for (const env of [...wharf, ...chapel]) {
    assert.deepEqual(w.buildingPlan(env).issues, [], env.id);
    const r = walkabilityReport(w, env, {});
    assert.ok(r.ok, `${env.id} missing ${JSON.stringify(r.missing.slice(0, 4))}`);
  }
});

test("island landmarks: a lighthouse on a headland, boathouses at the water, cairns on the fells", () => {
  for (const [id, size] of [["nordicTown", "fjord"], ["oldHarbourTown", null]]) {
    const w = world(id, size);
    const items = w.landmarks.all();
    const light = items.filter((it) => it.kind === "lighthouse");
    assert.equal(light.length, 1, `${id}: one lighthouse`);
    const lc = { x: (light[0].foot.x0 + light[0].foot.x1) / 2, y: (light[0].foot.y0 + light[0].foot.y1) / 2 };
    assert.ok(w.fields.coastDistance(lc.x, lc.y) > 0, "the lighthouse stands on land");
    const boats = items.filter((it) => it.kind === "boathouse");
    assert.ok(boats.length >= 3, `${id}: ${boats.length} boathouses`);
    for (const b of boats) {
      // one end on dry ground, the other at the water's edge
      const near = [
        [b.foot.x0, b.foot.y0],
        [b.foot.x1, b.foot.y0],
        [b.foot.x0, b.foot.y1],
        [b.foot.x1, b.foot.y1],
      ].map(([x, y]) => w.terrain.sample(x, y).h / 8);
      assert.ok(Math.min(...near) < 3.5 && Math.max(...near) > 0.2, `${b.foot.x0},${b.foot.y0}: ${near.map((h) => h.toFixed(1))}`);
    }
    if (w.fields.island.cfg.peak > 300) assert.ok(items.some((it) => it.kind === "cairn"), `${id}: cairns`);
  }
});

test("creeks and ravines never cut below the sea; outcrops break through the woods", () => {
  const w = world("nordicTown", "skerry");
  let streams = 0;
  let rock = 0;
  for (let y = -12000; y <= 12000; y += 36)
    for (let x = -12000; x <= 12000; x += 172) {
      if (w.fields.coastDistance(x, y) < 40) continue;
      const ts = w.terrain.sample(x, y);
      if (ts.stream) {
        streams += 1;
        assert.ok(ts.h / 8 > 1, `stream at ${(x / 8).toFixed(0)},${(y / 8).toFixed(0)} ${(ts.h / 8).toFixed(2)} m`);
      }
      if (ts.outcrop > 0.5) rock += 1;
    }
  assert.ok(streams > 5, `${streams} creek samples`);
  assert.ok(rock > 20, `${rock} outcrop samples`);
});
