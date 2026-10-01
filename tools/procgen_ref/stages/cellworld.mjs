// Stage "cellworld": the cell networks (city/cellNetwork.js planCellNetwork) of the worlds of
// lib/worlds.mjs (cityWorlds) as createWorld makes them - with its lakes (a sub-cell on a big
// lake's shore is a port: portNear), its highways and the terrain's harbour grading - where the
// stage "cellnet" plans on a World of World.js. The same cells as cellnet's (cellsOf), drawn from
// samples of their own. Every terrain sample makes what it reads first (lib/worlds.mjs
// pureTerrain; docs/CITY.md §6), so the records are what they are in any order.
import { REF, line, samples } from "../lib/rec.mjs";
import { cityWorlds, pureTerrain } from "../lib/worlds.mjs";
import { netLines } from "../lib/city.mjs";
import { cellsOf } from "./cellnet.mjs";

const { createWorld } = await import(REF + "world/createWorld.js");
const { planCellNetwork } = await import(REF + "city/cellNetwork.js");

export default function* cellworld() {
  const r = samples(59);
  for (const [key, overrides] of cityWorlds()) {
    const w = pureTerrain(createWorld(overrides));
    const cells = cellsOf(w, r);
    yield line("world", key, cells.length);
    for (const [i, j] of cells) yield* netLines(planCellNetwork(w, i, j));
  }
}
