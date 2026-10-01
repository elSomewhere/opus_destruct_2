// Stage "arterials": the arterial grid (network/arterials.js) of every world of lib/worlds.mjs -
// its lines, cells and cell rects (World.cellAt, World.cellsOverlapping) - and the road classes
// (network/roadClasses.js).
import { REF, line, samples } from "../lib/rec.mjs";
import { WORLDS } from "../lib/worlds.mjs";

const { ArterialGrid } = await import(REF + "network/arterials.js");
const { World } = await import(REF + "world/World.js");
const { roadSpecs, ROAD_CLASSES, CLASS_RANK } = await import(REF + "network/roadClasses.js");
const { makeConfig } = await import(REF + "config/defaults.js");
const { presetConfig } = await import(REF + "config/presets.js");

export default function* arterials() {
  const r = samples(17);
  for (const [key, id, size] of WORLDS) {
    const cfg = makeConfig(presetConfig(id, { size }));
    const A = new ArterialGrid(cfg);
    yield line("grid", key, A.seed, A.spacing, A.jitter, A.n);
    const l0 = [];
    const l1 = [];
    for (let i = -40; i <= 40; i += 1) {
      l0.push(A.line(0, i));
      l1.push(A.line(1, i));
    }
    yield line("l0", ...l0);
    yield line("l1", ...l1);
    for (let k = 0; k < 40; k += 1) {
      const i = Math.floor((r() - 0.5) * 2e5);
      yield line("lf", i, A.line(0, i), A.line(1, i), A.lineAt(0, i), A.lineAt(1, i), A.canon(i));
    }
    for (let k = 0; k < 300; k += 1) {
      const x = Math.round((r() - 0.5) * 6e5);
      const y = Math.round((r() - 0.5) * 6e5);
      const c = A.cellAt(x, y);
      const rc = A.cellRect(c.i, c.j);
      yield line("c", x, y, c.i, c.j, rc.x0, rc.y0, rc.x1, rc.y1, A.indexAt(0, x + 0.5), A.indexAt(1, y - 0.25));
    }
    // (a World's cellAt / cellsOverlapping: worlds without an island plan, which takes a while to site)
    if (cfg.world.mode !== "island") {
      const w = new World(presetConfig(id, { size }));
      for (let k = 0; k < 20; k += 1) {
        const x0 = Math.round((r() - 0.5) * 2e5);
        const y0 = Math.round((r() - 0.5) * 2e5);
        const rect = { x0, y0, x1: x0 + Math.floor(r() * 20000), y1: y0 + Math.floor(r() * 20000) };
        const a = w.cellAt(rect.x0, rect.y1);
        yield line("co", a.i, a.j, ...w.cellsOverlapping(rect).flatMap(({ i, j }) => [i, j]));
      }
    }
    for (const [cls, s] of Object.entries(roadSpecs(cfg)))
      yield line("spec", cls, s.cls, s.lanes, s.lane, s.median, s.parking, s.shoulder, s.sidewalk, s.hc, s.hr, s.corner, s.paved);
  }
  for (const cls of [...ROAD_CLASSES, "nope"]) yield line("rank", cls, CLASS_RANK[cls]);
}
