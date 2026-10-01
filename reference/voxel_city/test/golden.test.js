import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { goldenConfigs, goldenHashes } from "../scripts/lib/golden.js";
import { makeConfig } from "../src/engine/config/defaults.js";
import { presetConfig } from "../src/engine/config/presets.js";

/**
 * The regression oracle of the angled world (ANGLED_WORLD_PLAN.md §5.1):
 * with `world.angles.enabled = false` every preset of the axis-aligned
 * world generates bit for bit what it did before angles existed. The
 * record (test/golden/presets.json) was written by scripts/golden.js from
 * the engine as it was then: cell plans, ground tiles and chunks at LOD 0,
 * 2, 5 and 8 round five points, and the LOD 0 interiors of four buildings,
 * for every preset and size variant.
 */
const record = JSON.parse(readFileSync(new URL("./golden/presets.json", import.meta.url), "utf8"));

for (const [key, id, size] of goldenConfigs()) {
  test(`golden: ${key} is bit-identical to the axis-aligned world`, () => {
    assert.equal(makeConfig(presetConfig(id, { size })).world.angles.enabled, false);
    const want = record[key];
    assert.ok(want, `no golden record for ${key}`);
    const got = goldenHashes(id, size);
    const diff = Object.keys({ ...want, ...got }).filter((k) => want[k] !== got[k]);
    assert.deepEqual(diff, [], `${key}: samples differ`);
  });
}
