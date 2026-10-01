// Stage "water": rivers (nature/rivers.js), lakes (nature/lakes.js), createWorld's harbour hook
// (the terrain graded down to a town's port lake) and the World's water predicates
// (world/createWorld.js: seaAt, isWet, seaHitsRect, seaShare, seaHitsSeg, openWaterAt, shoreNear,
// waterHitsRect) of the worlds of lib/worlds.mjs (every chart, island and lake lattice: WATER_KEYS,
// without the angled twins and the size variants that add nothing here) and two of its own, made
// by createWorld, within 20 km of the spawn: the lakes of the lattice round the spawn (and of other
// laps round a wrapping world), every town's port lake, the lakes near the region; at points drawn
// at random, round every lake (in the water,
// on its shore, beyond), along rivers (channels, banks, quays), round the harbour towns (their
// graded waterfront) and over an island (its coasts): the terrain's height, river and lake info,
// carved ground, channel gaps, water levels, shore distances and the predicates; rects and
// segments for the hit tests. Every terrain sample makes what it reads first (lib/worlds.mjs
// pureTerrain; docs/CITY.md §6).
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, pureTerrain } from "../lib/worlds.mjs";

const { createWorld } = await import(REF + "world/createWorld.js");

/** The region round the spawn (voxels: 20 km). */
const R0 = 160000;

/** The worlds of lib/worlds.mjs this stage samples (tests/city/test_water.cpp has the same). */
export const WATER_KEYS = ["cities", "infiniteCity", "wrapWorld:small", "wrapWorld:large", "island:large", "nordicIsland:medium", "nordicTown:fjord",
  "oldHarbourTown", "planetNorth", "seed7", "torusInfinite", "cube2", "islandFlat", "islandDesert", "islandTiny", "islandBeaches", "desert", "spawnFar",
  "allMountains", "cube5Wet"];

/**
 * Worlds beyond lib/worlds.mjs (tests/city/test_water.cpp has the same): an island without lakes,
 * a lake config of nulls with many big lakes.
 */
export const WATER_WORLDS = [
  ["lakesOff", '{"seed":43,"lakes":{"enabled":false},"world":{"mode":"island","island":{"radius":6000}}}'],
  ["lakesNull", '{"seed":41,"lakes":{"cell":null,"scale":null,"townProximity":null,"bigChance":0.5}}'],
];

const lakeOf = (L) => (L ? [L.id, L.x, L.y, L.cx, L.cy, L.r0, L.level, L.depth, L.big, L.port, L.reach] : ["-"]);

export default function* water() {
  const r = samples(53);
  const worlds = [...allWorlds().filter(([key]) => WATER_KEYS.includes(key)), ...WATER_WORLDS.map(([key, json]) => [key, JSON.parse(json)])];
  for (const [key, overrides] of worlds) {
    const w = pureTerrain(createWorld(overrides));
    const { rivers: RV, lakes: LK, terrain: T, fields: F } = w;
    yield line("water", key, RV.cfg.enabled, RV.cfg.maxHalfWidth, LK.cfg.enabled, LK.cell, LK.n);
    // the lattice round the spawn, and other laps round a wrapping world
    for (let b = -5; b <= 5; b += 1) for (let a = -5; a <= 5; a += 1) yield line("lake", a, b, ...lakeOf(LK.lake(a, b)));
    if (LK.n)
      for (let k = 0; k < 30; k += 1) {
        const a = Math.floor((r() - 0.5) * 6 * LK.n);
        const b = Math.floor((r() - 0.5) * 6 * LK.n);
        yield line("lap", a, b, ...lakeOf(LK.lake(a, b)));
      }
    // every town's port lake, in a fixed order; the lakes near the region
    const region = { x0: -R0, y0: -R0, x1: R0, y1: R0 };
    const towns = F.settlementsIn(region);
    for (const s of towns) yield line("port", s.id, LK.portLakeOf(s)?.id);
    const lakes = LK.near(region);
    yield line("near", ...lakes.map((L) => L.id));
    for (let k = 0; k < 40; k += 1) {
      const x0 = Math.round((r() - 0.5) * 2 * R0);
      const y0 = Math.round((r() - 0.5) * 2 * R0);
      const rect = { x0, y0, x1: x0 + Math.floor(r() * 60000), y1: y0 + Math.floor(r() * 60000) };
      yield line("nr", ...LK.near(rect).map((L) => L.id));
    }
    // the points: at random, round the lakes, round the harbour towns, along rivers, over an island
    const pts = [];
    for (let k = 0; k < 300; k += 1) {
      const x = Math.round((r() - 0.5) * 2 * R0);
      const y = Math.round((r() - 0.5) * 2 * R0);
      pts.push([x, y]);
    }
    for (const L of lakes) {
      for (let k = 0; k < 8; k += 1) {
        const a = r() * 2 * Math.PI;
        const d = (0.1 + r() * 1.9) * L.r0;
        pts.push([Math.round(L.x + Math.cos(a) * d), Math.round(L.y + Math.sin(a) * d)]);
      }
      for (let k = 0; k < 4; k += 1) {
        const x = Math.round(L.x + (r() - 0.5) * 3 * L.r0);
        const y = Math.round(L.y + (r() - 0.5) * 3 * L.r0);
        yield line("sk", L.id, x, y, LK.shoreK(L, x, y), LK.shoreK(L, x, y, true));
      }
    }
    for (const s of towns) {
      const L = LK.portLakeOf(s);
      if (!L) continue;
      for (let k = 0; k < 30; k += 1) {
        const t = r() * 1.3;
        const j = (r() - 0.5) * 1.6 * s.radius;
        const dx = L.x - s.x;
        const dy = L.y - s.y;
        const d = Math.hypot(dx, dy) || 1;
        pts.push([Math.round(s.x + dx * t - (dy / d) * j), Math.round(s.y + dy * t + (dx / d) * j)]);
      }
    }
    for (let k = 0; k < 24; k += 1) {
      let x = Math.round((r() - 0.5) * 2 * R0);
      const y = Math.round((r() - 0.5) * 2 * R0);
      let found = 0;
      for (let s = 0; s < 400 && found < 12; s += 1, x += 40)
        if (RV.channelGap(x, y) < 40) {
          pts.push([x, y]);
          found += 1;
        }
    }
    if (F.island) {
      const bb = F.island.bounds();
      for (let k = 0; k < 300; k += 1) {
        const x = Math.round((bb.x0 + r() * (bb.x1 - bb.x0)) * 8);
        const y = Math.round((bb.y0 + r() * (bb.y1 - bb.y0)) * 8);
        pts.push([x, y]);
      }
    }
    for (let i = 0; i < pts.length; i += 1) {
      const [x, y] = pts[i];
      const ts = T.sample(x, y);
      const h = Math.round(ts.h);
      // (at() returns a shared object: read before the next call)
      const ri = RV.at(x, y);
      const riv = ri ? [ri.d, ri.half, ri.water, ri.bed, ri.urban, ri.bank, RV.groundAt(ri, h), RV.groundAt(ri, ri.water - 30), RV.groundAt(ri, ri.water + 40)] : ["-"];
      const gap = RV.channelGap(x, y);
      const lk = LK.at(x, y);
      const lkv = lk ? [lk.level, lk.bed, lk.k, lk.lake.id, LK.groundAt(lk, h), LK.groundAt(lk, lk.level - 30), LK.groundAt(lk, lk.level + 40)] : ["-"];
      yield line("pt", x, y, ts.h, ts.natural, ...riv, gap, ...lkv, i % 10 === 0 ? RV.waterLevel(x, y) : "-");
      const sh = w.shoreNear(x, y, 4000);
      const ls = LK.shoreNear(x, y, 2400);
      yield line("w", w.seaAt(x, y), w.seaAt(x, y, 30), w.isWet(x, y), w.isWet(x, y, 0), w.isWet(x, y, 15), w.openWaterAt(x, y),
        ...(sh ? [sh.level, sh.dist, sh.nx, sh.ny, sh.lake?.id] : ["-"]), ...(ls ? [ls.lake.id, ls.dist, ls.nx, ls.ny] : ["-"]));
    }
    // rects and segments round the points
    for (let k = 0; k < 160; k += 1) {
      const [px, py] = pts[Math.floor(r() * pts.length)];
      const x0 = Math.round(px - r() * 600);
      const y0 = Math.round(py - r() * 600);
      const rect = { x0, y0, x1: x0 + Math.round(r() * 800), y1: y0 + Math.round(r() * 800) };
      const m = Math.floor(r() * 12);
      yield line("rect", rect.x0, rect.y0, rect.x1, rect.y1, m, w.seaHitsRect(rect, m), w.seaHitsRect(rect), w.seaShare(rect), RV.hitsRect(rect, m), RV.hitsRect(rect),
        LK.hitsRect(rect, m), LK.hitsRect(rect), w.waterHitsRect(rect, m), w.waterHitsRect(rect));
      const [qx, qy] = pts[Math.floor(r() * pts.length)];
      yield line("seg", px, py, qx, qy, w.seaHitsSeg(px, py, qx, qy, m), w.seaHitsSeg(px, py, qx, qy));
    }
  }
}
