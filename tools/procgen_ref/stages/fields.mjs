// Stage "fields": the macro fields (world/fields.js) of every world of lib/worlds.mjs
// (allWorlds: the presets and the extra worlds) - the settlement and village records (lattices and
// laps, or an island's places), and at sample points urbanization (u, core, settlement, parts,
// prox), proximity, mountainness, climate, the district, industry, style and fringe noises, the
// settlement warp and distances, the coast distance and the nearest places; settlementsIn /
// villagesIn over rects.
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, samplePoints, settlementFields } from "../lib/worlds.mjs";

const { World } = await import(REF + "world/World.js");

const rec = (tag, s) => (s ? line(tag, ...settlementFields(s)) : line(tag, "-"));
const ids = (list) => list.map((s) => s.id);

export default function* fields() {
  const r = samples(29);
  for (const [key, overrides] of allWorlds()) {
    const w = new World(overrides);
    const F = w.fields;
    yield line("fields", key, F.nTown, F.nVillage, F.mOff, !!F.island, F.wrap.on);
    for (const s of F.settlementsIn({ x0: -400000, y0: -400000, x1: 400000, y1: 400000 })) yield rec("town", s);
    for (const v of F.villagesIn({ x0: -200000, y0: -200000, x1: 200000, y1: 200000 })) yield rec("village", v);
    for (let j = -3; j <= 3; j += 1)
      for (let i = -3; i <= 3; i += 1) {
        yield line("cell", i, j, F.settlement(i, j)?.id, F.village(i, j)?.id);
        yield rec("lapT", F.settlement(i + 2 * F.nTown, j - F.nTown));
        yield rec("lapV", F.village(i - F.nVillage, j + 3 * F.nVillage));
      }
    for (let n = 0; n < 9; n += 1) yield line("isl", n, F.settlement(n, n === 0 ? 0 : 1000)?.id, F.village(n, 1000)?.id);
    for (const [x, y] of samplePoints(w, r)) {
      const u = F.urban(x, y);
      yield line("u", x, y, u.u, u.core, u.settlement?.id, u.parts.map(([s, wt]) => `${s.id}:${wt}`).join(","), u.prox);
      yield line("f", F.settlementProximity(x, y), F.mountainness(x, y), F.moisture(x, y), F.temperature(x, y), F.districtNoise(x, y), F.industryNoise(x, y),
        F.styleNoise(x, y), F.fringeNoise(x, y), F.settlementWarp(x, y), F.coastDistance(x, y));
      const near = F.nearestSettlements(x, y);
      const vil = F.nearestVillages(x, y);
      yield line("ns", ...ids(near), "|", ...ids(vil));
      const s0 = near[0] ?? vil[0];
      if (s0) yield line("sd", s0.id, F.settlementDistance(s0, x, y), F.settlementDistance(s0, x, y, 1.1), F.mountainsAround(x / 8, y / 8, 900));
    }
    for (let k = 0; k < 30; k += 1) {
      const x0 = Math.round((r() - 0.5) * 6e5);
      const y0 = Math.round((r() - 0.5) * 6e5);
      const rect = { x0, y0, x1: x0 + Math.floor(r() * 2e5), y1: y0 + Math.floor(r() * 2e5) };
      yield line("in", ...ids(F.settlementsIn(rect)), "|", ...ids(F.villagesIn(rect)));
    }
  }
}
