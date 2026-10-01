// Stage "terrain": the terrain (terrain/terrain.js and the landform stack of terrain/landforms.js)
// of every world of lib/worlds.mjs - Terrain.sample()'s whole record at the sample points (hints,
// stream channels, roughness, ruggedness, coast), samples with a precomputed urban sample and raw
// ones, heights, the landform stack run directly (natural() and the context it leaves: the hints
// and its lazy coast type, climate and desertness), and the settlements' base heights. Samples run
// in order on one Terrain: its one context carries what nested calls leave (a settlement's base
// height made inside a sample), as the reference's does.
import { REF, line, samples } from "../lib/rec.mjs";
import { WORLDS, samplePoints } from "../lib/worlds.mjs";

const { World } = await import(REF + "world/World.js");
const { presetConfig } = await import(REF + "config/presets.js");

const streamOf = (s) => (s ? [s.d, s.half, s.extra, s.wet] : "-");

export default function* terrain() {
  const r = samples(37);
  for (const [key, id, size] of WORLDS) {
    const w = new World(presetConfig(id, { size }));
    const T = w.terrain;
    yield line("terrain", key, T.seaLevel, T.forms.map((f) => f.lf.id).join(","), T.ctx.torusR);
    const pts = samplePoints(w, r);
    for (const [x, y] of pts) {
      const s = T.sample(x, y);
      yield line("s", x, y, s.h, s.natural, s.u, s.core, s.settlement?.id, s.grade, s.mountain, s.canyon, s.ravine, s.outcrop, streamOf(s.stream), s.rough,
        s.rugged, s.coast);
    }
    for (let k = 0; k < 200; k += 1) {
      const [x, y] = pts[Math.floor(r() * pts.length)];
      const a = T.sample(x, y, w.fields.urban(x, y));
      const b = T.sample(x + 3, y - 5, null, true);
      yield line("s2", x, y, a.h, a.rugged, a.coast, b.h, b.natural, b.rugged, b.coast, T.height(x - 7, y + 2));
    }
    for (let k = 0; k < 300; k += 1) {
      const [x, y] = pts[Math.floor(r() * pts.length)];
      const u = r() * 0.4;
      const prox = r() < 0.5 ? null : r();
      const [fx, fy, fz, fw] = w.chart.toField(x * 0.125, y * 0.125);
      const h = T.natural(x, y, u, fx, fy, fz, prox, fw);
      const c = T.ctx;
      yield line("nat", x, y, u, prox ?? "-", h, c.lowland, c.mountain, c.ridge, c.canyon, c.ravine, c.outcrop, c.valley, c.rough, c.channel, c._rugged,
        streamOf(c.stream), c.prox, c.coast, c._cliff, c._clim ? [c._clim.t, c._clim.m] : "-", c._desert);
    }
    const near = [...w.fields.settlementsIn({ x0: -160000, y0: -160000, x1: 160000, y1: 160000 }), ...w.fields.villagesIn({ x0: -80000, y0: -80000, x1: 80000, y1: 80000 })];
    for (const s of near) yield line("base", s.id, T.settlementBase(s));
  }
}
