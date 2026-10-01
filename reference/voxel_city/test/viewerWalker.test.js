import { test } from "node:test";
import assert from "node:assert/strict";
import * as THREE from "three";
import { CameraRig } from "../src/viewer/controls.js";
import { TileStreamer } from "../src/viewer/streamer.js";

/**
 * The viewer's walker (viewer/controls.js) and the streamer's collision
 * (viewer/streamer.js), without a browser: a fall never passes through a
 * one-voxel slab, a walker put inside a wall steps out, a walker below
 * every ground known is reported lost, and below a tile's lowest chunk
 * the ground is solid.
 */

const VOXEL = 0.125;
globalThis.window ??= { addEventListener() {}, removeEventListener() {} };
globalThis.document ??= { addEventListener() {}, removeEventListener() {}, exitPointerLock() {}, pointerLockElement: null };
const dom = { addEventListener() {}, removeEventListener() {}, requestPointerLock() {} };

function rig(solidAt, extra = {}) {
  const cam = new THREE.PerspectiveCamera(60, 1, 0.05, 1000);
  cam.up.set(0, 0, 1);
  const r = new CameraRig(cam, dom, { solidAt, ...extra });
  r.setMode("walk");
  return r;
}

test("viewer walker: a long fall lands on a one-voxel slab, never through it", () => {
  // (a slab one voxel thick at z = 0, air above and below: the world known everywhere)
  const r = rig((x, y, z) => z === 0);
  r.teleport(0.5, 0.5, 60);
  for (let f = 0; f < 600; f += 1) r.update(0.05);
  assert.ok(r.onGround, "on the slab");
  assert.equal(r.pos.z, VOXEL, "feet on the slab's top");
});

test("viewer walker: put inside a wall it steps out; it waits while its ground is unknown", () => {
  // a wall 4 voxels thick along x = 0..3 (all heights), a floor at z < 0
  const r = rig((x, y, z) => z < 0 || (x >= 0 && x <= 3));
  r.teleport(0.25, 0.5, 0);
  assert.ok(r.blocked(r.pos.x, r.pos.y, r.pos.z), "starts inside the wall");
  for (let f = 0; f < 5; f += 1) r.update(0.05);
  assert.equal(r.overlap(r.pos.x, r.pos.y, r.pos.z), false, "out of the wall");
  // unknown ground: the walker waits where it is
  const w = rig(() => null);
  w.teleport(1, 1, 10);
  w.keys.add("KeyW");
  w.update(0.05);
  assert.equal(w.status, "waiting");
  assert.equal(w.pos.z, 10);
});

test("viewer walker: below every ground known there it is reported lost", () => {
  let lost = 0;
  const r = rig(() => false, { floorAt: () => 20, onLost: () => (lost += 1) });
  r.teleport(0, 0, 5);
  r.update(0.05);
  assert.ok(lost > 0);
});

test("viewer streamer: below a tile's lowest chunk the ground is solid, above its content air", () => {
  const pool = { freeSlots: () => 0, dispatchTile: () => false, onTile: null };
  const materials = { opaque: new THREE.MeshBasicMaterial(), transparent: new THREE.MeshBasicMaterial({ transparent: true }), tinted: () => new THREE.MeshBasicMaterial() };
  const s = new TileStreamer({ scene: new THREE.Scene(), pool, materials, config: { streaming: { maxLod: 3 } } });
  const key = s.key(0, 0, 0);
  s.tiles.set(key, { key, lod: 0, cx: 0, cy: 0, state: "queued" });
  // a tile whose content is chunks 2 and 3: chunk 2 solid up to its middle, chunk 3 empty
  const solid2 = new Uint32Array(1024);
  for (let n = 0; n < 16 * 1024; n += 1) solid2[n >>> 5] |= 1 << (n & 31);
  s.onTile({ lod: 0, cx: 0, cy: 0, cz0: 2, cz1: 3, zlo: 64, zhi: 127, gzlo: 64, gzhi: 80, chunks: [{ cz: 2, solid: solid2, climb: new Uint32Array(1024) }], parts: [] });
  assert.equal(s.solidAt(5, 5, 10), true, "below the lowest chunk: solid");
  assert.equal(s.solidAt(5, 5, 64 + 3), true, "the ground in chunk 2");
  assert.equal(s.solidAt(5, 5, 64 + 20), false, "air above it");
  assert.equal(s.solidAt(5, 5, 200), false, "above the content: air");
  assert.equal(s.floorAt(5 * VOXEL, 5 * VOXEL), 64 * VOXEL);
  assert.equal(s.solidAt(40, 5, 70), null, "a tile not loaded: unknown");
});

test("viewer queries: the inspector names what is at a voxel; the angled world lists a place for each kind of part", async () => {
  const { createWorld } = await import("../src/engine/world/createWorld.js");
  const { presetConfig } = await import("../src/engine/config/presets.js");
  const { inspect, pois } = await import("../src/engine/stream/queries.js");
  const { frameOf } = await import("../src/engine/buildings/frame.js");
  const w = createWorld(presetConfig("angledCities"));
  // a turned building's wall, one storey up
  const env = w.cellPlan(0, 0).buildings.find((e) => frameOf(e).turned);
  const [x, y] = frameOf(env).toWorld(Math.floor(env.U / 2), 0);
  const r = inspect(w, x, y, env.groundZ + 12);
  assert.equal(r.building?.id, env.id);
  assert.ok(r.district && r.cell);
  // (a voxel of a part: its kind, its local cell, its material)
  const part = w.cellPlan(0, 0).parts.find((p) => p.kind === "wing");
  const b = part.aabb;
  let found = null;
  for (let z = b.z0; z <= b.z1 && !found; z += 2)
    for (let yy = b.y0; yy <= b.y1 && !found; yy += 2)
      for (let xx = b.x0; xx <= b.x1 && !found; xx += 2) {
        const q = inspect(w, xx, yy, z, part.id).parts.find((p) => p.id === part.id && p.material);
        if (q) found = q;
      }
  assert.ok(found, "a voxel of the wing found");
  assert.equal(found.kind, "wing");
  const places = pois(w, 0, 0).filter((p) => p.group === "Angled world");
  assert.ok(places.some((p) => p.label.startsWith("Turned building")) && places.some((p) => p.label.startsWith("Corner bay")), places.map((p) => p.label).join(", "));
});
