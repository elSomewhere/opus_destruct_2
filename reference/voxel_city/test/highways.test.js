import { test } from "node:test";
import assert from "node:assert/strict";
import { createWorld } from "../src/engine/world/createWorld.js";
import { rampZ } from "../src/engine/network/highways.js";
import { buildChunk } from "../src/engine/voxel/compose.js";
import { MAT, IS_SOLID } from "../src/engine/voxel/materials.js";
import { P, P2 } from "../src/engine/voxel/chunk.js";
import { sampleRoadSurface, makeRoadSample, KIND } from "../src/engine/network/roadSurface.js";

/**
 * Highways one can use (network/highways.js): no spur and no dead end in
 * the air (a chance edge needs company at both of its nodes; a route's
 * last node is a terminus at grade); decks within 5%, ramps within 7%
 * landing on their arterial at its level, never over a street too low; and
 * in the voxels a car's way up a ramp onto the deck, and across a junction
 * of highways, without a barrier in the way.
 */

const worlds = new Map();
function world(mode) {
  if (!worlds.has(mode)) worlds.set(mode, createWorld({ seed: 1337, world: { mode } }));
  return worlds.get(mode);
}

/** The voxel at (x, y, z) of the world grid (chunks cached). */
function voxelReader(w) {
  const cache = new Map();
  return (x, y, z) => {
    const cx = Math.floor(x / 32);
    const cy = Math.floor(y / 32);
    const cz = Math.floor(z / 32);
    const k = `${cx},${cy},${cz}`;
    if (!cache.has(k)) cache.set(k, buildChunk(w, 0, cx, cy, cz).data);
    return cache.get(k)[x - cx * 32 + 1 + (y - cy * 32 + 1) * P + (z - cz * 32 + 1) * P2];
  };
}

/** The top solid voxel of a column within `reach` of z (the surface a car stands on), or null. */
function surface(at, x, y, z, reach = 12) {
  for (let k = z + reach; k >= z - reach; k -= 1) if (IS_SOLID[at(x, y, k)] && !IS_SOLID[at(x, y, k + 1)] && !IS_SOLID[at(x, y, k + 2)]) return k;
  return null;
}

test("highways: no spurs and no dead ends; chance edges only between urban nodes", () => {
  for (const mode of ["infiniteCity", "cities"]) {
    const hw = world(mode).highways;
    let inner = 0;
    for (let b = -3; b <= 3; b += 1)
      for (let a = -3; a <= 3; a += 1) {
        const at = hw.edgesAt(a, b);
        if (!at.length) continue;
        inner += 1;
        // (a single edge: only a route's last node, a terminus at grade)
        if (at.length === 1) assert.ok(hw.onIntercityRoute(...at[0]), `${mode}: a chance spur ends at node ${a},${b}`);
        for (const [x, p, q] of at) {
          if (hw.onIntercityRoute(x, p, q)) continue;
          const n0 = hw.latticeXY(p, q);
          const n1 = x === 0 ? hw.latticeXY(p + 1, q) : hw.latticeXY(p, q + 1);
          assert.ok(Math.min(world(mode).fields.urban(n0.x, n0.y).u, world(mode).fields.urban(n1.x, n1.y).u) >= hw.cfg.minUrbanization);
        }
      }
    assert.ok(inner > 3, `${mode}: ${inner} highway nodes`);
  }
});

test("highways: decks within 5%; ramps within 7%, landing on their arterial at its level, clear of the streets under them", () => {
  const w = world("infiniteCity");
  const hw = w.highways;
  const rs = makeRoadSample();
  const R = 8000 * 8;
  let ramps = 0;
  for (const e of hw.edgesNear({ x0: -R, y0: -R, x1: R, y1: R })) {
    for (let i = 1; i < e.pts.length; i += 1) {
      const g = Math.abs(e.pts[i].z - e.pts[i - 1].z) / Math.hypot(e.pts[i].x - e.pts[i - 1].x, e.pts[i].y - e.pts[i - 1].y);
      assert.ok(g <= 0.05 + 1e-6, `${e.id}: deck grade ${(100 * g).toFixed(3)}%`);
    }
    for (const r of hw.ramps(e)) {
      ramps += 1;
      const n = Math.ceil(Math.abs(r.sDeck - r.sGround) / 8);
      for (let k = 1; k <= n; k += 1) {
        const s0 = r.sGround + ((r.sDeck - r.sGround) * (k - 1)) / n;
        const s1 = r.sGround + ((r.sDeck - r.sGround) * k) / n;
        const g = Math.abs(rampZ(e, r, s1) - rampZ(e, r, s0)) / Math.abs(s1 - s0);
        assert.ok(g <= 0.071, `${e.id} ramp: grade ${(100 * g).toFixed(2)}%`);
      }
      // (it lands on its arterial's carriageway, at that street's level)
      const c = w.cellAt(r.x, r.y);
      sampleRoadSurface(w.roadView(c.i, c.j).near({ x0: r.x - 2, y0: r.y - 2, x1: r.x + 2, y1: r.y + 2 }), r.x + 0.5, r.y + 0.5, rs, w.seed);
      assert.equal(rs.seg?.road.id, r.arterial, `${e.id}: a ramp lands on ${rs.seg?.road.id}, not its arterial`);
      assert.ok(Math.abs(r.zGround - w.streetLevel(r.x, r.y)) <= 1);
      assert.ok(!hw.rampBlocked(e, r));
    }
  }
  assert.ok(ramps > 20, `${ramps} ramps`);
});

test("highways: in the voxels, a ramp takes a car from its arterial onto the deck, and a junction of highways has no barrier across it", () => {
  const w = world("infiniteCity");
  const hw = w.highways;
  const at = voxelReader(w);
  const R = 4000 * 8;
  const edges = hw.edgesNear({ x0: -R, y0: -R, x1: R, y1: R });
  // a ramp: its centre line from the landing to the deck end climbs a voxel at a time, every column a surface
  const e = edges.find((q) => hw.ramps(q).length);
  const r = hw.ramps(e)[0];
  const mid = r.side * hw.rampMid;
  const point = (s, off) => {
    let k = 0;
    while (k < e.lengths.length - 2 && e.lengths[k + 1] < s) k += 1;
    const a = e.pts[k];
    const b = e.pts[k + 1];
    const t = (s - e.lengths[k]) / (e.lengths[k + 1] - e.lengths[k]);
    const len = e.lengths[k + 1] - e.lengths[k];
    const tx = (b.x - a.x) / len;
    const ty = (b.y - a.y) / len;
    return [Math.floor(a.x + (b.x - a.x) * t - ty * off), Math.floor(a.y + (b.y - a.y) * t + tx * off)];
  };
  let prev = null;
  const steps = Math.ceil(Math.abs(r.sDeck - r.sGround) / 4);
  for (let k = 0; k <= steps; k += 1) {
    const s = r.sGround + ((r.sDeck - r.sGround) * k) / steps;
    const [x, y] = point(s, mid);
    const z = surface(at, x, y, Math.round(rampZ(e, r, s)), 6);
    assert.ok(z !== null, `no ramp surface at ${x},${y} (arc ${s.toFixed(0)})`);
    if (prev !== null) assert.ok(Math.abs(z - prev) <= 1, `a step of ${z - prev} voxels on the ramp at ${x},${y}`);
    prev = z;
  }
  // from the ramp's deck end, across into the deck's outer lane: level, no barrier over the surface
  const sMerge = r.sDeck + Math.sign(r.sGround - r.sDeck) * 8;
  let level = null;
  for (let off = Math.abs(mid); off >= hw.hw - 20; off -= 1) {
    const [x, y] = point(sMerge, r.side * off);
    const z = surface(at, x, y, prev, 6);
    assert.ok(z !== null, `no surface at the merge (offset ${off})`);
    if (level !== null) assert.ok(Math.abs(z - level) <= 1, `a step of ${z - level} at the merge (offset ${off})`);
    level = z;
    assert.notEqual(at(x, y, z + 1), MAT.HW_BARRIER, `a barrier at the merge (offset ${off})`);
  }
  // a junction of three or four highways: one level, no barrier on its surface
  const j = edges.flatMap((q) => q.junctions).find((q) => q.degree >= 3);
  assert.ok(j, "a junction of highways");
  for (let dy = -40; dy <= 40; dy += 8)
    for (let dx = -40; dx <= 40; dx += 8) {
      const z = surface(at, Math.round(j.x + dx), Math.round(j.y + dy), Math.round(j.z), 4);
      if (z === null) continue;
      assert.ok(Math.abs(z - Math.round(j.z)) <= 1, `junction surface at ${z}, its level ${j.z}`);
      assert.notEqual(at(Math.round(j.x + dx), Math.round(j.y + dy), z + 1), MAT.HW_BARRIER, "a barrier across the junction");
    }
});
