#!/usr/bin/env node
// Conformance records of the city generator's port (docs/CITY.md §Conformance): the reference
// (reference/voxel_city, pinned) run stage by stage, each stage's output as text lines. The C++
// port writes the same lines (tests/city, SVX_CITY_RECORDS=DIR); a stage conforms when the two
// files are identical. Numbers print as JavaScript prints them (String(x): the shortest
// round-trip form), so equal text means equal bits.
//
//   node tools/procgen_ref/dump.mjs STAGE [ARGS...] > out.txt
//   node tools/procgen_ref/dump.mjs --list
//
// Stages live in tools/procgen_ref/stages/<name>.mjs: `export default function (args) -> lines`
// (an array of strings, or a generator of them).
import { readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const stages = readdirSync(join(here, "stages")).filter((f) => f.endsWith(".mjs")).map((f) => f.slice(0, -4)).sort();
if (!argv.length || argv[0] === "--list") {
  console.log(stages.join("\n"));
  process.exit(argv.length ? 0 : 2);
}
const [stage, ...args] = argv;
if (!stages.includes(stage)) {
  console.error(`no stage "${stage}" (${stages.join(", ")})`);
  process.exit(2);
}
const mod = await import(pathToFileURL(join(here, "stages", `${stage}.mjs`)).href);
const out = process.stdout;
let buf = "";
for (const line of mod.default(args)) {
  buf += line + "\n";
  if (buf.length > 1 << 20) {
    out.write(buf);
    buf = "";
  }
}
out.write(buf);
