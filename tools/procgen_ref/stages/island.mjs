// Stage "island": the island plans (world/island.js) of the island worlds of lib/worlds.mjs
// (allWorlds) - the ellipse, highland threshold, fjords and the island's offset, the sited places
// and their settlement records, the harbour, the bounds, the pointwise functions (shape,
// highlands, fjord cuts, coast, cliffs, skerries) over the bounds, and the trunk roads (A* over
// the arterial grid; the places' base heights made first, as the terrain stage does).
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, settlementFields } from "../lib/worlds.mjs";

const { World } = await import(REF + "world/World.js");
const { makeConfig } = await import(REF + "config/defaults.js");

export default function* island() {
  const r = samples(31);
  for (const [key, overrides] of allWorlds()) {
    if (makeConfig(overrides).world.mode !== "island") continue;
    const w = new World(overrides);
    const P = w.fields.island;
    yield line("plan", key, P.R, P.theta, P.a, P.b, P.cos, P.sin, P.hdx, P.hdy, P.fjordCount, P.fjordPhase, P.ox, P.oy, P.highThreshold);
    for (const s of P.sites) yield line("site", s.kind, s.lx, s.ly, s.radius, s.importance);
    const { towns, villages } = P.settlements(w.fields);
    for (const s of [...towns, ...villages]) yield line("place", ...settlementFields(s));
    const h = P.harbour();
    yield line("harbour", ...(h ? [h.r, h.x, h.y, h.dx, h.dy] : ["-"]));
    const b = P.bounds();
    yield line("bounds", b.x0, b.y0, b.x1, b.y1);
    for (let k = 0; k < 3000; k += 1) {
      const x = b.x0 + r() * (b.x1 - b.x0);
      const y = b.y0 + r() * (b.y1 - b.y0);
      const d = (r() - 0.5) * 6000;
      const [lx, ly] = P.local(x, y);
      const c = P.coast(x, y);
      yield line("pt", x, y, lx, ly, P.shape(lx, ly), P.highRaw(lx, ly), P.highlandLocal(lx, ly), P.fjordCut(lx, ly, d), c, P.highland(x, y), P.cliff(x, y),
        P.skerry(x, y, c), P.skerry(x, y, -d / 4));
    }
    // (the A* samples the terrain at its nodes: every place's base height first, docs/CITY.md §6)
    for (const s of [...towns, ...villages]) w.terrain.settlementBase(s);
    const edges = P.trunkEdges(w);
    yield line("trunk", edges.size, ...edges);
  }
}
