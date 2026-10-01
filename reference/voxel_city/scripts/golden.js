#!/usr/bin/env node
/**
 * Golden output of every preset and size variant (scripts/lib/golden.js):
 * the regression oracle for `world.angles.enabled = false`.
 *   node scripts/golden.js [--only key-prefix]            print the hashes
 *   node scripts/golden.js --write [--only key-prefix]    record them in test/golden/presets.json
 *   node scripts/golden.js --check [--only key-prefix]    compare with the record (exit 1 on a diff)
 *   ... --angled                                          the angled presets (test/golden/angled.json)
 * Keys are `preset` or `preset:size`; --only keeps the keys starting with it.
 */
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { goldenConfigs, goldenHashes, angledConfigs, angledHashes } from "./lib/golden.js";

const argv = process.argv.slice(2);
const angled = argv.includes("--angled");
const FILE = join(dirname(fileURLToPath(import.meta.url)), "..", "test", "golden", angled ? "angled.json" : "presets.json");
const write = argv.includes("--write");
const check = argv.includes("--check");
const only = argv.includes("--only") ? argv[argv.indexOf("--only") + 1] : null;

let record = {};
try {
  record = JSON.parse(readFileSync(FILE, "utf8"));
} catch {
  if (check) throw new Error(`no golden record at ${FILE}: run with --write first`);
}

let diffs = 0;
const t0 = performance.now();
for (const [key, id, size] of angled ? angledConfigs() : goldenConfigs()) {
  if (only && !key.startsWith(only)) continue;
  const t = performance.now();
  const h = angled ? angledHashes(id, size) : goldenHashes(id, size);
  const secs = ((performance.now() - t) / 1000).toFixed(1);
  if (check) {
    const want = record[key];
    const bad = Object.keys({ ...h, ...(want ?? {}) }).filter((k) => !want || want[k] !== h[k]);
    diffs += bad.length;
    console.log(`${bad.length ? "DIFF" : "ok  "} ${key.padEnd(22)} ${secs} s${bad.length ? `  ${bad.join(" ")}` : ""}`);
  } else {
    console.log(`${key.padEnd(22)} ${secs} s`);
    if (!write) for (const [k, v] of Object.entries(h)) console.log(`  ${k.padEnd(22)} ${v}`);
  }
  record[key] = h;
}
if (write) {
  mkdirSync(dirname(FILE), { recursive: true });
  const sorted = Object.fromEntries(Object.keys(record).sort().map((k) => [k, record[k]]));
  writeFileSync(FILE, `${JSON.stringify(sorted, null, 1)}\n`);
  console.log(`wrote ${FILE}`);
}
console.log(`(${((performance.now() - t0) / 1000).toFixed(0)} s)`);
if (check && diffs) {
  console.log(`${diffs} sample(s) differ from the golden record`);
  process.exit(1);
}
