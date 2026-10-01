import test from "node:test";
import assert from "node:assert/strict";
import { RENDER_LAB_VIEWS } from "../src/viewer/renderLab.js";
import { PRESETS } from "../src/engine/config/presets.js";
import { LOOKS } from "../src/viewer/looks.js";
import { MESHERS } from "../src/engine/voxel/meshers.js";

test("render lab has eight reproducible, distinct viewpoints", () => {
  assert.equal(RENDER_LAB_VIEWS.length, 8);
  assert.equal(new Set(RENDER_LAB_VIEWS.map((v) => v.id)).size, RENDER_LAB_VIEWS.length);
  for (const entry of RENDER_LAB_VIEWS) {
    assert.ok(PRESETS.has(entry.preset), entry.preset);
    assert.equal(entry.seed, 1337);
    assert.ok(["walk", "fly", "orbit"].includes(entry.view.mode));
    for (const key of ["x", "y", "yaw", "pitch"]) assert.ok(Number.isFinite(entry.view[key]), `${entry.id}.${key}`);
  }
});

test("classic remains the default and comparison options are registered", () => {
  assert.deepEqual(MESHERS, ["greedy", "marching-cubes", "dual-contour"]);
  assert.equal(LOOKS.classic.toneMapping, "none");
  for (const id of ["bleak-overcast", "rain-at-dusk", "hard-noon", "fog", "wet-night", "winter-grey"]) assert.ok(LOOKS[id], id);
});

test("render lab covers the baseline subjects", () => {
  assert.deepEqual(RENDER_LAB_VIEWS.map((v) => v.id), ["dense-street", "pitched-street", "angled-junction", "turned-building", "hillside", "highway-ramp", "skyline", "interior"]);
});
