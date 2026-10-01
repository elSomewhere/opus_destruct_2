// Stage "townworld": the town plans and landmark uses of stage "townplan" (city/townPlan.js) on the
// worlds of lib/worlds.mjs (cityWorlds) as createWorld makes them - with its lakes, its highways
// (a highway's corridor keeps a civic landmark off a block) and the terrain's harbour grading -
// where "townplan" plans on a World of World.js. Every terrain sample makes what it reads first
// (lib/worlds.mjs pureTerrain; docs/CITY.md §6).
import { REF, samples } from "../lib/rec.mjs";
import { cityWorlds, pureTerrain } from "../lib/worlds.mjs";
import { townsOf } from "./townplan.mjs";

const { createWorld } = await import(REF + "world/createWorld.js");

export default function* townworld() {
  const r = samples(71);
  for (const [key, overrides] of cityWorlds()) yield* townsOf(pureTerrain(createWorld(overrides)), key, r, false);
}
