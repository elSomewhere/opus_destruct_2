import { test } from "node:test";
import assert from "node:assert/strict";
import * as THREE from "three";
import { TileStreamer } from "../src/viewer/streamer.js";
import { createWorld } from "../src/engine/world/createWorld.js";
import { groundTile, tileContentRange } from "../src/engine/voxel/compose.js";

/**
 * The LOD quadtree must settle for a still camera: tiles loading (and their
 * ground ranges arriving) or parents being disposed must never flip the
 * selection back and forth (visible as LOD flicker).
 */
function simulate(world, cam, viewDistance) {
  const jobs = [];
  const pool = { onTile: null, freeSlots: () => 64, dispatchTile: (j) => (jobs.push(j), true) };
  const st = new TileStreamer({ scene: new THREE.Scene(), pool, materials: { opaque: null, transparent: null }, config: world.config });
  st.viewDistance = viewDistance;
  const entered = new Map();
  let prev = new Set();
  let lastChange = 0;
  for (let it = 0; it < 60; it += 1) {
    st.lastRangeSelect = 0;
    st.update(cam);
    let changed = false;
    for (const k of st.desired)
      if (!prev.has(k)) {
        entered.set(k, (entered.get(k) ?? 0) + 1);
        changed = true;
      }
    for (const k of prev) if (!st.desired.has(k)) changed = true;
    if (changed) lastChange = it;
    prev = new Set(st.desired);
    const batch = jobs.splice(0);
    for (const j of batch) {
      const tile = groundTile(world, j.lod, j.cx, j.cy);
      const [lo, hi] = tileContentRange(world, tile);
      st.onTile({ lod: j.lod, cx: j.cx, cy: j.cy, chunks: [], zlo: lo, zhi: hi, gzlo: tile.zMin, gzhi: tile.zMax });
    }
    if (!batch.length && it - lastChange > 3) break;
  }
  // LOD of the tile under the camera
  const px = cam.x * 8;
  const py = cam.y * 8;
  let camLod = null;
  for (const k of st.desired) {
    const [l, cx, cy] = k.split(":").map(Number);
    const size = 32 << l;
    if (px >= cx * size && px < (cx + 1) * size && py >= cy * size && py < (cy + 1) * size) camLod = l;
  }
  return { lastChange, flips: [...entered.values()].filter((n) => n > 1).length, tiles: st.desired.size, camLod };
}

test("streaming: the LOD selection settles for a still camera (no oscillation), in town and on a mountain flank", () => {
  const w = createWorld({ seed: 1337 });
  for (const cam of [new THREE.Vector3(10, 20, 45), new THREE.Vector3(-12120.8, -18062.3, 1606.7)]) {
    const r = simulate(w, cam, 1500);
    assert.ok(r.tiles > 100);
    assert.equal(r.flips, 0, `tiles re-entering the selection at ${cam.x},${cam.y}`);
    assert.ok(r.lastChange < 10, `settled after ${r.lastChange} updates`);
  }
});

test("streaming: deep underground (a research complex's tram station) the camera stands on LOD0 (collision)", () => {
  const w = createWorld({ seed: 1337 });
  const n = w.sites.nearest(0, 0, 400, 6).find((q) => q.type === "researchComplex");
  const s = w.sites.siteAt(Math.floor(n.x / w.sites.cell), Math.floor(n.y / w.sites.cell));
  s.def.structure(w, s);
  const st = s.plan.under.tram.stations[0].hall;
  const cam = new THREE.Vector3((st.x0 + st.x1) / 16, (st.y0 + st.y1) / 16, (s.plan.under.tram.z + 13) / 8);
  const r = simulate(w, cam, 800);
  assert.equal(r.camLod, 0);
  assert.equal(r.flips, 0);
});
