// The worlds the stages of the world base sample (tests/city/worlds.hpp is the C++ twin): every
// golden preset and size variant of the reference (scripts/lib/golden.js GOLDEN_PRESETS), plus two
// angled presets. [key, preset id, size id | null].
import { REF } from "./rec.mjs";

const { presetConfig } = await import(REF + "config/presets.js");

export const WORLDS = [
  ["cities", "cities", null],
  ["infiniteCity", "infiniteCity", null],
  ["wrapWorld:small", "wrapWorld", "small"],
  ["wrapWorld:medium", "wrapWorld", "medium"],
  ["wrapWorld:large", "wrapWorld", "large"],
  ["island:small", "island", "small"],
  ["island:medium", "island", "medium"],
  ["island:large", "island", "large"],
  ["nordicIsland:small", "nordicIsland", "small"],
  ["nordicIsland:medium", "nordicIsland", "medium"],
  ["nordicIsland:large", "nordicIsland", "large"],
  ["nordicTown:skerry", "nordicTown", "skerry"],
  ["nordicTown:fjord", "nordicTown", "fjord"],
  ["nordicTown:forest", "nordicTown", "forest"],
  ["oldHarbourTown", "oldHarbourTown", null],
  ["whiteSeaTown", "whiteSeaTown", null],
  ["planetEquator", "planetEquator", null],
  ["planetNorth", "planetNorth", null],
  ["angledCities", "angledCities", null],
  ["angledOldHarbourTown", "angledOldHarbourTown", null],
];

/**
 * Worlds beyond the presets, as config overrides (JSON, parsed alike by both sides): other seeds,
 * a torus without latitude, other cube faces, islands without highlands or fjords, tiny and
 * desert islands (the main town's fallback site), deserts, a wet planet face, mountains
 * everywhere, spawn-mountain ranges no offset can meet (the offset's fallbacks).
 */
export const EXTRA_WORLDS = [
  ["seed7", '{"seed":7}'],
  ["torusInfinite", '{"seed":99,"world":{"mode":"infiniteCity","chart":"torus","size":30000,"latitude":false}}'],
  ["cube2", '{"seed":2024,"world":{"chart":"cube","planet":{"radius":120000,"face":2}}}'],
  ["islandFlat", '{"seed":5,"world":{"mode":"island","island":{"radius":3000,"highlands":0,"fjords":0,"skerries":0,"population":60000,"towns":2,"villages":3,"hamlets":3}}}'],
  ["islandDesert", '{"seed":11,"world":{"mode":"island","island":{"radius":12000,"highlands":0.9,"fjords":1,"cliffs":0.9,"elongation":3,"roughness":1,"townRise":20},"climate":{"temperature":0.7,"moisture":0.2}}}'],
  ["desert", '{"seed":3,"world":{"climate":{"temperature":0.75,"temperatureVar":0.05,"moisture":0.15}}}'],
  ["islandTiny", '{"seed":13,"world":{"mode":"island","island":{"radius":1500,"population":500,"peak":300}}}'],
  ["islandCrowded", '{"seed":21,"world":{"mode":"island","island":{"radius":900,"population":30000,"highlands":0.95}}}'],
  ["spawnNear", '{"seed":8,"terrain":{"spawnMountains":[1,2]}}'],
  ["spawnFar", '{"seed":9,"terrain":{"spawnMountains":[300,400]}}'],
  ["allMountains", '{"seed":4,"terrain":{"mountainBelt":[0,0.01]}}'],
  ["cube5Wet", '{"seed":-12345,"world":{"chart":"cube","planet":{"radius":240000,"face":5},"climate":{"moisture":0.9}}}'],
  ["islandBeaches", '{"seed":31,"world":{"mode":"island","season":"winter","island":{"radius":5000,"fjords":0.3,"skerries":1,"cliffs":0}}}'],
];

/** Every world of the world base's stages as [key, config overrides]: WORLDS (presetConfig), then EXTRA_WORLDS. */
export function allWorlds() {
  return [...WORLDS.map(([key, id, size]) => [key, presetConfig(id, { size })]), ...EXTRA_WORLDS.map(([key, json]) => [key, JSON.parse(json)])];
}

/** The golden sample points (metres): the spawn town's centre, its streets, the outskirts, open country, far away. */
export const GOLDEN_POINTS = [
  [0, 0],
  [180, -140],
  [1400, 900],
  [-3200, 2600],
  [7000, -5200],
];

/**
 * Sample points (voxels) of a World, drawn from r (samples()): the golden points, 1200 within
 * 40 km of the spawn, 300 within 400 km (other climates: deserts, canyons), 8 round every town
 * (within 20 km) and village (within 10 km) of the spawn, 800 over an island's bounds (its coasts),
 * and 50 off the voxel grid.
 */
export function samplePoints(w, r) {
  const pts = GOLDEN_POINTS.map(([mx, my]) => [Math.round(mx * 8), Math.round(my * 8)]);
  for (let k = 0; k < 1200; k += 1) {
    const x = Math.round((r() - 0.5) * 640000);
    const y = Math.round((r() - 0.5) * 640000);
    pts.push([x, y]);
  }
  for (let k = 0; k < 300; k += 1) {
    const x = Math.round((r() - 0.5) * 6400000);
    const y = Math.round((r() - 0.5) * 6400000);
    pts.push([x, y]);
  }
  const F = w.fields;
  const near = [...F.settlementsIn({ x0: -160000, y0: -160000, x1: 160000, y1: 160000 }), ...F.villagesIn({ x0: -80000, y0: -80000, x1: 80000, y1: 80000 })];
  for (const s of near)
    for (let k = 0; k < 8; k += 1) {
      const a = r() * 2 * Math.PI;
      const d = r() * 3 * s.radius;
      pts.push([Math.round(s.x + Math.cos(a) * d), Math.round(s.y + Math.sin(a) * d)]);
    }
  if (F.island) {
    const b = F.island.bounds();
    for (let k = 0; k < 800; k += 1) {
      const x = Math.round((b.x0 + r() * (b.x1 - b.x0)) * 8);
      const y = Math.round((b.y0 + r() * (b.y1 - b.y0)) * 8);
      pts.push([x, y]);
    }
  }
  for (let k = 0; k < 50; k += 1) {
    const x = (r() - 0.5) * 100000;
    const y = (r() - 0.5) * 100000;
    pts.push([x, y]);
  }
  return pts;
}

/**
 * Makes the base height of every town and village a terrain sample at one of the points (voxels)
 * can read - nearestSettlements and nearestVillages, the settlements of urban()'s parts - in a fixed
 * order, before anything samples there. The reference's Terrain reuses one context, and a sample
 * that makes a base height on first use reads what that nested call left in it (its coast and
 * ruggedness; on an island the waterfront grading too): with the base heights made first, its
 * samples are what they are in any order, and what the port's are (docs/CITY.md §6). Port lakes
 * (lakes.portLakeOf) need the same once a stage reaches them.
 */
export function warmBasesAt(w, pts) {
  for (const [x, y] of pts) {
    for (const s of w.fields.nearestSettlements(x, y)) w.terrain.settlementBase(s);
    for (const v of w.fields.nearestVillages(x, y)) w.terrain.settlementBase(v);
  }
}

/** The same for every town and village whose disk may reach a rect (voxels): settlementsIn, villagesIn. */
export function warmBasesIn(w, rect) {
  for (const s of w.fields.settlementsIn(rect)) w.terrain.settlementBase(s);
  for (const v of w.fields.villagesIn(rect)) w.terrain.settlementBase(v);
}

/**
 * The worlds of the city stages (cell networks, streets, town plans): allWorlds(), then the other
 * angled presets - the angled infinite city (diagonal boulevards through the grid) and the angled
 * island towns (crooked old-town lanes, diagonals through a small town). [key, config overrides].
 * (tests/city/city_records.hpp city_worlds is the C++ twin.)
 */
export const CITY_EXTRA_WORLDS = [
  ["angledInfiniteCity", "angledInfiniteCity", null],
  ["angledNordicTown:skerry", "angledNordicTown", "skerry"],
  ["angledNordicTown:fjord", "angledNordicTown", "fjord"],
  ["angledNordicTown:forest", "angledNordicTown", "forest"],
];
export function cityWorlds() {
  return [...allWorlds(), ...CITY_EXTRA_WORLDS.map(([key, id, size]) => [key, presetConfig(id, { size })])];
}

/**
 * createWorld.js's island sea tests (seaAt, seaHitsRect, seaShare, seaHitsSeg), verbatim, installed
 * on a World of World.js: the cell network asks them. The city stages plan on such a World and
 * nothing else of createWorld - no lakes, highways or harbour grading of the terrain - which is
 * the port's World as it stands (world/createWorld.cpp: the sea tests). Returns the world.
 */
export function withSeaTests(world) {
  const island = world.fields.island;
  world.seaAt = (x, y, marginM = 0) => (island ? island.coast(x / 8, y / 8) < marginM : false);
  world.seaHitsRect = (r, marginM = 6) => {
    if (!island) return false;
    const step = 96;
    for (let y = r.y0; y <= r.y1 + step - 1; y += step)
      for (let x = r.x0; x <= r.x1 + step - 1; x += step) if (island.coast(Math.min(x, r.x1) / 8, Math.min(y, r.y1) / 8) < marginM + 8.5) return true;
    return false;
  };
  world.seaShare = (r) => {
    if (!island) return 0;
    let n = 0;
    for (let j = 0; j < 5; j += 1)
      for (let i = 0; i < 5; i += 1) if (island.coast((r.x0 + ((r.x1 - r.x0) * (i + 0.5)) / 5) / 8, (r.y0 + ((r.y1 - r.y0) * (j + 0.5)) / 5) / 8) < 0) n += 1;
    return n / 25;
  };
  world.seaHitsSeg = (ax, ay, bx, by, marginM = 4) => {
    if (!island) return false;
    const n = Math.max(2, Math.ceil(Math.hypot(bx - ax, by - ay) / 240));
    for (let k = 0; k <= n; k += 1) if (island.coast((ax + ((bx - ax) * k) / n) / 8, (ay + ((by - ay) * k) / n) / 8) < marginM) return true;
    return false;
  };
  return world;
}

/** Makes the base height of every place of an island first (its trunk roads' A* samples the terrain all over it); nothing elsewhere. */
export function warmIsland(w) {
  if (!w.fields.island) return;
  const { towns, villages } = w.fields.islandSettlements();
  for (const s of [...towns, ...villages]) w.terrain.settlementBase(s);
}

/**
 * The same warm-up for every terrain sample a World makes, whoever makes it - a stage, or a plan
 * inside a stage's query (a lake's rim, a river's water level, the land cover's forest density):
 * each sample first makes, in a fixed order, what it may read that would otherwise be made by a
 * call nested in it (in the reference's shared terrain context): the base height of every town and
 * village it can see (nearestSettlements, nearestVillages: urban()'s parts), then - on a world with
 * createWorld's harbour grading hook, where the sample will call it (not raw, near a town) - the
 * port lakes the hook reads (lakes.portLakeOf, whose lakes sample the terrain themselves: the hook
 * is run once first, its result dropped). A terrain call nested in a sample all the same throws, so
 * a stage that passes made none. The reference's samples are then pure functions of (world,
 * arguments), as the port's are, in any order (docs/CITY.md §6). Call it once, right after making
 * the world, before anything samples.
 */
export function pureTerrain(w) {
  const T = w.terrain;
  const F = w.fields;
  const { sample, natural } = Object.getPrototypeOf(T);
  let busy = false; // a sample is running (its own landform stack, grading, harbour hook)
  let own = false; // the next natural() is that sample's own
  T.natural = function (...a) {
    if (busy && !own) throw new Error("pureTerrain: a landform stack run nested in a terrain sample");
    own = false;
    return natural.apply(this, a);
  };
  T.sample = function (x, y, urban = null, raw = false) {
    if (busy) throw new Error("pureTerrain: a terrain sample nested in a terrain sample");
    for (const s of F.nearestSettlements(x, y)) T.settlementBase(s);
    for (const v of F.nearestVillages(x, y)) T.settlementBase(v);
    if (!raw && T.portGrade && (urban ?? F.urban(x, y)).prox > 0) T.portGrade(x, y, 0);
    busy = true;
    own = true;
    try {
      return sample.call(this, x, y, urban, raw);
    } finally {
      busy = false;
      own = false;
    }
  };
  return w;
}

/** A settlement record as a line: every field JS gives it ("-" where JS leaves it undefined or null). */
export function settlementFields(s) {
  return [s.id, s.i, s.j, s.village ?? false, s.hamlet ?? false, s.x, s.y, s.radius, s.importance, s.style, s.peak ?? 0, s.cx ?? "-", s.cy ?? "-", s.t, s.m, s.flavor ?? "-", s.island ?? false];
}
