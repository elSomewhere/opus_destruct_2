// Stage "biomes": the BIOMES registry (nature/biomes.js) entry by entry, classifyBiome and
// desertness over the climate square, and every biome's ground on sample contexts.
import { REF, line, samples } from "../lib/rec.mjs";

const { BIOMES, classifyBiome, desertness } = await import(REF + "nature/biomes.js");

export default function* biomes() {
  const r = samples(23);
  for (const b of BIOMES.all())
    yield line("biome", b.id, b.label.replaceAll(" ", "_"), b.color, b.climate, b.forest, b.meadow, b.farmland, b.trees.map(([k, w]) => `${k}:${w}`).join(","), b.floor, b.pools ?? 0, b.dunes ?? 0);
  for (let i = -5; i <= 55; i += 1) {
    const t = i / 50;
    const ids = [];
    const des = [];
    for (let j = -5; j <= 55; j += 1) {
      const m = j / 50;
      ids.push(classifyBiome(t, m).id);
      des.push(desertness(t, m));
    }
    yield line("cl", t, ...ids);
    yield line("de", t, ...des);
  }
  for (let k = 0; k < 2000; k += 1) {
    const t = r() * 1.3 - 0.15;
    const m = r() * 1.3 - 0.15;
    yield line("cr", t, m, classifyBiome(t, m).id, desertness(t, m));
  }
  for (const b of BIOMES.all())
    for (let k = 0; k < 300; k += 1) {
      const patch = (r() - 0.5) * 2.4;
      const p2 = (r() - 0.5) * 2.4;
      const h = r();
      const x = Math.floor((r() - 0.5) * 1e5);
      const y = Math.floor((r() - 0.5) * 1e5);
      yield line("gr", b.id, patch, p2, h, ...b.ground({ patch, x, y, h, p2 }));
    }
}
