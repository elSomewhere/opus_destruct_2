import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { presetConfig } from "../src/engine/config/presets.js";
import { buildChunk, groundTile } from "../src/engine/voxel/compose.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { hash32 } from "../src/engine/core/hash.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../src/engine/network/roadSurface.js";
import { groundSteps, roadJumps, buildingsOnRoads } from "../src/engine/validate/fit.js";
import { insideCuts } from "../src/engine/city/blockPoly.js";
import { roadPieces, PART_REACH } from "../src/engine/world/parts.js";
import { edgeInfo } from "../src/engine/city/cellNetwork.js";

/**
 * Angled and curved streets (ANGLED_WORLD_PLAN.md S1): diagonal boulevards
 * over the arterial grid, old-town lanes at natural angles, country roads
 * that wander, while every invariant of the axis-aligned streets holds:
 * level across, graded along, continuous through junctions, no street
 * through a lot or a building, no new steps in the ground, chunks that
 * agree across their borders, and road pieces that fit structvox's reach.
 */

const worlds = new Map();
function world(preset, size = null) {
  const key = `${preset}/${size}`;
  if (!worlds.has(key)) worlds.set(key, createWorld(presetConfig(preset, { size })));
  return worlds.get(key);
}

/** Distance from a point to a convex polygon ([[x, y], ...] in order), 0 inside. */
function polyDistance(poly, x, y) {
  let inside = true;
  let sign = 0;
  let best = Infinity;
  for (let k = 0; k < poly.length; k += 1) {
    const [ax, ay] = poly[k];
    const [bx, by] = poly[(k + 1) % poly.length];
    const c = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    if (c !== 0) {
      if (sign !== 0 && Math.sign(c) !== sign) inside = false;
      sign = Math.sign(c);
    }
    const L = (bx - ax) ** 2 + (by - ay) ** 2 || 1;
    const t = Math.max(0, Math.min(1, ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / L));
    best = Math.min(best, Math.hypot(ax + (bx - ax) * t - x, ay + (by - ay) * t - y));
  }
  return inside ? 0 : best;
}

const slanted = (r) => r.pts.length === 2 && r.pts[0].x !== r.pts[1].x && r.pts[0].y !== r.pts[1].y;

/** Roads of the cells round the origin. */
function roadsNear(w, n = 1) {
  const out = [];
  for (let j = -n; j <= n; j += 1) for (let i = -n; i <= n; i += 1) out.push(...w.cellNet(i, j).roads);
  return out;
}

test("angled streets: diagonal boulevards, tilted old-town lanes and wandering country roads", () => {
  const city = roadsNear(world("angledCities"));
  assert.ok(city.filter((r) => r.diagonal).length >= 4, "diagonal boulevards through the spawn town");
  assert.ok(city.filter((r) => !r.diagonal && slanted(r)).length >= 20, "tilted lanes in its organic quarters");
  // exact yaw: every diagonal runs along the 3-4-5 triple
  for (const r of city.filter((q) => q.diagonal)) {
    const [a, b] = r.pts;
    assert.ok(Math.abs(Math.abs((b.y - a.y) / (b.x - a.x)) - 0.75) < 1e-9, `${r.id} slope`);
  }
  assert.ok(roadsNear(world("angledOldHarbourTown")).filter(slanted).length >= 10, "the old harbour town's lanes tilt");
  // (the axis-aligned world has none of it)
  assert.equal(roadsNear(world("cities")).filter((r) => r.diagonal || slanted(r)).length, 0);
  // country roads out in the country wander further than they did
  const wob = (w) => {
    let m = 0;
    let n = 0;
    for (let a = 8; a < 14; a += 1)
      for (let b = -4; b < 2; b += 1)
        for (const axis of [0, 1]) {
          const e = edgeInfo(w, axis, a, b);
          if (e.cls !== "rural") continue;
          n += 1;
          m = Math.max(m, e.wob);
        }
    assert.ok(n > 20, `${n} country roads`);
    return m;
  };
  assert.ok(wob(world("angledCities")) > 1.4 * wob(world("cities")), "country roads bend more");
});

test("angled streets: every street in its cell; blocks never overlap; every lot lies in its block, off every street's right-of-way", () => {
  for (const [preset, size] of [["angledCities", null], ["angledOldHarbourTown", null], ["angledNordicTown", "fjord"]]) {
    const w = world(preset, size);
    let lots = 0;
    for (let j = -1; j <= 1; j += 1)
      for (let i = -1; i <= 1; i += 1) {
        const plan = w.cellPlan(i, j);
        // (a cell's own streets stay in it: the ownership rule every cell relies on)
        const cr = w.arterials.cellRect(i, j);
        for (const r of plan.net.roads) if (!r.arterialEdge) for (const p of r.pts) assert.ok(p.x >= cr.x0 - 1 && p.x <= cr.x1 + 1 && p.y >= cr.y0 - 1 && p.y <= cr.y1 + 1, `${preset}: ${r.id} leaves its cell`);
        const view = w.roadView(i, j);
        const blocks = new Map(plan.net.blocks.map((b) => [b.id, b]));
        const inside = (b, x, y) => x >= b.prop.x0 && x <= b.prop.x1 && y >= b.prop.y0 && y <= b.prop.y1 && (!b.cuts || insideCuts(b.cuts, x + 0.5, y + 0.5));
        for (let y = plan.rect.y0; y < plan.rect.y1; y += 48)
          for (let x = plan.rect.x0; x < plan.rect.x1; x += 48) assert.ok(plan.net.blocks.filter((b) => inside(b, x, y)).length <= 1, `${preset}: blocks overlap at ${x},${y}`);
        for (const l of plan.lots) {
          const b = blocks.get(l.block);
          if (!b) continue;
          lots += 1;
          const r = l.rect;
          if (l.poly) {
            // a turned lot (S3): its polygon, the planned lot cut to the block's slanted edges
            for (const [x, y] of l.poly) assert.ok(x >= b.prop.x0 && x <= b.prop.x1 + 1 && y >= b.prop.y0 && y <= b.prop.y1 + 1 && (!b.cuts || b.cuts.every((k) => k.nx * x + k.ny * y >= k.c - 1e-6)), `${preset}: turned lot ${l.id} leaves its block`);
            for (const s of view.near({ x0: r.x0 - 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1 }))
              for (let t = 0; t <= s.len; t += 4) assert.ok(polyDistance(l.poly, s.ax + s.dx * t, s.ay + s.dy * t) >= s.hr - 1.5, `${preset}: turned lot ${l.id} on ${s.road.id}`);
            continue;
          }
          for (const [x, y] of [[r.x0, r.y0], [r.x1, r.y0], [r.x0, r.y1], [r.x1, r.y1]]) assert.ok(inside(b, x, y), `${preset}: lot ${l.id} leaves its block`);
          for (const s of view.near({ x0: r.x0 - 1, y0: r.y0 - 1, x1: r.x1 + 1, y1: r.y1 + 1 })) {
            for (let t = 0; t <= s.len; t += 4) {
              const px = s.ax + s.dx * t;
              const py = s.ay + s.dy * t;
              const qx = Math.max(r.x0, Math.min(r.x1 + 1, px));
              const qy = Math.max(r.y0, Math.min(r.y1 + 1, py));
              assert.ok(Math.hypot(px - qx, py - qy) >= s.hr - 1.5, `${preset}: lot ${l.id} on ${s.road.id}`);
            }
          }
        }
      }
    assert.ok(lots > 200, `${preset}: ${lots} lots`);
  }
});

/** Carriageway columns of the LOD 0 tiles round a point (as in roads.test.js). */
function roadColumns(w, px, py, n = 4) {
  const out = [];
  const rs = makeRoadSample();
  const c0x = Math.floor(px / 32);
  const c0y = Math.floor(py / 32);
  for (let cy = c0y - n; cy <= c0y + n; cy += 1)
    for (let cx = c0x - n; cx <= c0x + n; cx += 1) {
      const tile = groundTile(w, 0, cx, cy);
      for (let j = 1; j < P - 1; j += 1)
        for (let i = 1; i < P - 1; i += 1) {
          const x = cx * 32 - 1 + i;
          const y = cy * 32 - 1 + j;
          const c = w.cellAt(x, y);
          sampleRoadSurface(w.roadView(c.i, c.j).near({ x0: x - 1, y0: y - 1, x1: x + 1, y1: y + 1 }), x + 0.5, y + 0.5, rs, w.seed);
          if (rs.kind !== KIND.CARRIAGE || tile.deck?.[i + j * P]) continue;
          out.push({ z: tile.z[i + j * P], seg: rs.seg, along: Math.round(rs.along), x, y });
        }
    }
  return out;
}

test("angled streets: level across the carriageway, graded along it, a diagonal and a hillside included", () => {
  const w = world("angledCities");
  const d = roadsNear(w).find((r) => r.diagonal);
  const h = world("angledOldHarbourTown");
  for (const [ww, px, py] of [[w, (d.pts[0].x + d.pts[1].x) / 2, (d.pts[0].y + d.pts[1].y) / 2], [h, 0, 0], [h, 1200, -800]]) {
    const cols = roadColumns(ww, Math.round(px), Math.round(py));
    assert.ok(cols.length > 500, "road columns");
    const byCut = new Map();
    for (const c of cols) {
      const k = `${c.seg.road.id}|${c.seg.idx}|${c.along}`;
      const e = byCut.get(k) ?? { min: Infinity, max: -Infinity, n: 0 };
      e.min = Math.min(e.min, c.z);
      e.max = Math.max(e.max, c.z);
      e.n += 1;
      byCut.set(k, e);
    }
    for (const e of byCut.values()) if (e.n >= 8) assert.ok(e.max - e.min <= 1, `cross-slope ${e.max - e.min} voxels`);
    const at = new Map(cols.map((c) => [`${c.x},${c.y}`, c]));
    let worst = 0;
    for (const c of cols)
      for (const [dx, dy] of [[8, 0], [0, 8]]) {
        const o = at.get(`${c.x + dx},${c.y + dy}`);
        if (o && o.seg.road === c.seg.road) worst = Math.max(worst, Math.abs(o.z - c.z) / 8);
      }
    assert.ok(worst <= 0.21, `grade ${worst.toFixed(2)}`);
  }
});

test("angled streets: continuous through their junctions, clear of every building", () => {
  for (const [preset, size] of [["angledCities", null], ["angledOldHarbourTown", null], ["angledNordicTown", "fjord"]]) {
    const w = world(preset, size);
    for (let j = -1; j <= 1; j += 1)
      for (let i = -1; i <= 1; i += 1) {
        const r = roadJumps(w, i, j);
        assert.ok(r.jump <= 1.5, `${preset} C${i}_${j}: ${r.jump.toFixed(2)} voxels on ${r.road} at ${(r.along / 8).toFixed(1)} m`);
      }
    const hits = buildingsOnRoads(w, { x0: -2400, y0: -2400, x1: 2400, y1: 2400 });
    assert.ok(hits.every((q) => q.n <= 2), `${preset}: ${JSON.stringify(hits.filter((q) => q.n > 2).slice(0, 4))}`);
  }
});

test("angled streets: no new steps in the ground of the steep towns", () => {
  for (const [base, preset, size] of [["oldHarbourTown", "angledOldHarbourTown", null], ["nordicTown", "angledNordicTown", "fjord"]]) {
    const rect = { x0: -2000, y0: -2000, x1: 2000, y1: 2000 };
    const count = (w) => {
      const { pairs } = groundSteps(w, rect, { lod: 1, thr: 4 });
      let n = 0;
      let max = 0;
      for (const e of pairs.values()) {
        n += e.n;
        max = Math.max(max, e.max);
      }
      return { n, max };
    };
    const a = count(world(base, size));
    const b = count(world(preset, size));
    assert.ok(b.n <= a.n, `${preset}: ${b.n} steps, the axis-aligned town ${a.n}`);
    // (the tilted lanes lay a town's blocks out differently, so its worst
    // step is another lot's: the terrace wall of a big pad on the slope, in
    // either town; within half a metre of the axis-aligned town's worst)
    assert.ok(b.max <= a.max + 4, `${preset}: steps up to ${b.max} voxels, the axis-aligned town ${a.max}`);
  }
});

test("angled streets: chunks along a diagonal are pure (any order, any worker) and agree across borders", () => {
  const a = createWorld(presetConfig("angledCities"));
  const b = createWorld(presetConfig("angledCities"));
  const d = roadsNear(a).find((r) => r.diagonal);
  const mx = Math.floor((d.pts[0].x + d.pts[1].x) / 2 / 32);
  const my = Math.floor((d.pts[0].y + d.pts[1].y) / 2 / 32);
  const cz = Math.floor(groundTile(a, 0, mx, my).z[16 + 16 * P] / 32);
  const keys = [[0, mx, my, cz], [0, mx + 1, my, cz], [0, mx, my + 1, cz], [2, mx >> 2, my >> 2, cz >> 2]];
  const hash = (c) => {
    let h = 0;
    for (let i = 0; i < c.data.length; i += 1) h = hash32(h, c.data[i], i);
    return h;
  };
  const ha = keys.map(([l, x, y, z]) => hash(buildChunk(a, l, x, y, z)));
  const hb = keys.slice().reverse().map(([l, x, y, z]) => hash(buildChunk(b, l, x, y, z))).reverse();
  assert.deepEqual(ha, hb);
  const A = buildChunk(a, 0, mx, my, cz);
  const B = buildChunk(a, 0, mx + 1, my, cz);
  let diff = 0;
  for (let k = 1; k <= 32; k += 1) for (let j = 1; j <= 32; j += 1) if (A.data[33 + j * P + k * P2] !== B.data[1 + j * P + k * P2]) diff += 1;
  assert.equal(diff, 0);
});

test("road pieces: straight, within reach of their home chunk, tiling every road", () => {
  const w = world("angledCities");
  let n = 0;
  let far = 0;
  for (let j = -1; j <= 1; j += 1)
    for (let i = -1; i <= 1; i += 1) {
      const rect = w.arterials.cellRect(i, j);
      for (const road of w.cellNet(i, j).roads) {
        const pieces = roadPieces(road, rect);
        let L = 0;
        for (let k = 1; k < road.pts.length; k += 1) L += Math.hypot(road.pts[k].x - road.pts[k - 1].x, road.pts[k].y - road.pts[k - 1].y);
        assert.ok(Math.abs(pieces[0].s0) < 1e-9 && Math.abs(pieces[pieces.length - 1].s1 - L) < 1e-6, road.id);
        for (let k = 1; k < pieces.length; k += 1) assert.ok(Math.abs(pieces[k].s0 - pieces[k - 1].s1) < 1e-6, road.id);
        for (const p of pieces) {
          if (p.reach > PART_REACH) far += 1;
          // (a chunk is its cell's when the cell holds its last voxel)
          const own = { x: p.home.cx * 32 + 31, y: p.home.cy * 32 + 31 };
          assert.ok(own.x >= rect.x0 && own.x < rect.x1 && own.y >= rect.y0 && own.y < rect.y1, `${p.key}: home in its cell`);
          assert.ok(p.s1 - p.s0 >= 64 - 1e-6 || pieces.filter((q) => q.seg === p.seg).length === 1, `${p.key}: no piece under 8 m`);
        }
        n += pieces.length;
      }
    }
  assert.ok(n > 500);
  assert.ok(far < 0.01 * n, `${far} of ${n} pieces reach too far`);
});
