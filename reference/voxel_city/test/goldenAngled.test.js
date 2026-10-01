import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { angledConfigs, angledHashes } from "../scripts/lib/golden.js";
import { makeConfig } from "../src/engine/config/defaults.js";
import { presetConfig } from "../src/engine/config/presets.js";

/**
 * The record of the angled presets (test/golden/angled.json, written by
 * scripts/golden.js --angled): their samples in both parts modes, their
 * parts and the content of a few in their own lattices. Unlike the
 * axis-aligned oracle it may move, but only on purpose: a change that means
 * to alter the angled world records it again, and says which samples moved.
 */
const record = JSON.parse(readFileSync(new URL("./golden/angled.json", import.meta.url), "utf8"));

for (const [key, id, size] of angledConfigs()) {
  test(`angled golden: ${key} generates what was recorded`, () => {
    assert.equal(makeConfig(presetConfig(id, { size })).world.angles.enabled, true);
    const want = record[key];
    assert.ok(want, `no angled record for ${key}`);
    const got = angledHashes(id, size);
    const diff = Object.keys({ ...want, ...got }).filter((k) => want[k] !== got[k]);
    assert.deepEqual(diff, [], `${key}: samples differ (node scripts/golden.js --angled --check)`);
  });
}
