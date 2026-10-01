// Stage "season": world/season.js - every season (and a missing / unknown one) under several
// climates (none, the presets', explicit snow covers and freeze points): snow, roof snow,
// strength, tree looks of every tree kind, seasonal ground, flowers, canopy colours, patchNoise.
import { REF, line, samples } from "../lib/rec.mjs";

const { Season, patchNoise } = await import(REF + "world/season.js");
const { MAT } = await import(REF + "voxel/materials.js");

export const CLIMATES = [
  null,
  { temperature: 0.5, temperatureVar: 0.04, moisture: 0.6, moistureVar: 0.1 },
  { temperature: 0.345, temperatureVar: 0.025, moisture: 0.66, moistureVar: 0.18 },
  { temperature: 0.36, temperatureVar: 0.02 },
  { temperature: 0.3, snowCover: 0.5 },
  { snowCover: 0 },
  { snowCover: null },
  { snowCover: 1.4, freeze: 0.6 },
  { temperature: 0.2, temperatureVar: 0.1, snowCover: 0.2, freeze: null },
];
export const IDS = ["spring", "summer", "autumn", "winter", undefined, "monsoon"];
export const KINDS = ["pine", "spruce", "dwarfpine", "juniper", "jungle", "palm", "cactus", "acacia", "shrubDry", "log", "stump", "snag", "birch", "oak", "maple",
  "autumn", "street", "blossom", "rowan", "willow", "poplar", "shrub", "fern", "berry", "hazel", "aspen", "alder", "larch", "unknownKind"];
export const GROUND = ["GRASS", "GRASS_DARK", "GRASS_LAWN", "GRASS_DRY", "MARSH_GRASS", "FOREST_FLOOR", "TUNDRA", "MOSS", "SAVANNA_GRASS", "STONE", "SAND", "SNOW", "LEAF_LITTER", "GRASS_SPRING"];

export default function* season() {
  const r = samples(11);
  for (const cl of CLIMATES)
    for (const id of IDS) {
      const s = new Season({ world: { season: id, climate: cl } });
      yield line("season", id ?? "-", s.id, s.fixedSnow === undefined ? "-" : +s.fixedSnow, s.cold, s.freezeT, s.any, !!s.table);
      const ts = [];
      for (let k = -4; k <= 24; k += 1) ts.push(k / 20);
      yield line("strength", ...ts.map((t) => s.strength(t)));
      yield line("snow", ...ts.map((t) => s.snow(t)));
      yield line("roof", ...ts.map((t) => s.roofSnow(t)));
      for (const kind of KINDS)
        for (let q = 0; q < 4; q += 1) {
          const seed = Math.floor(r() * 4294967296);
          const t = r() * 1.2 - 0.1;
          const L = s.treeLook(kind, seed, t);
          yield line("tl", kind, seed, t, ...(L ? [L.leaves, L.mix, L.bare, L.snow, L.holes, L.accent] : ["-"]));
        }
    }
  // (ground, flowers and canopy depend on the season and the strength only)
  for (const id of IDS) {
    const s = new Season({ world: { season: id, climate: null } });
    for (const name of GROUND)
      for (let k = -2; k <= 14; k += 1) {
        const t = k / 20 + 0.3;
        const out = [];
        for (let q = 0; q <= 10; q += 1) out.push(s.ground(MAT[name], t, q / 10));
        out.push(s.ground(MAT[name], t, r()));
        yield line("g", name, t, ...out);
      }
    for (const name of GROUND)
      for (let k = 0; k < 120; k += 1) {
        const t = r() * 1.2 - 0.1;
        const cl = r();
        const h = r() * 0.5;
        const sp = r();
        yield line("fl", name, t, cl, h, sp, s.flower(MAT[name], t, cl, h, sp));
      }
    for (let k = 0; k < 300; k += 1) {
      const conifer = r() < 0.3;
      const tl = r() * 1.2 - 0.1;
      const kk = r();
      yield line("cn", conifer, tl, kk, s.canopy(conifer, tl, kk));
    }
  }
  for (let k = 0; k < 600; k += 1) {
    const seed = Math.floor((r() - 0.5) * 2 ** 33);
    const x = (r() - 0.5) * 2e6;
    const y = (r() - 0.5) * 2e6;
    const cell = [240, 48, 7, 1920, 0.5][k % 5];
    const salt = Math.floor(r() * 1000);
    yield line("pn", seed, x, y, cell, salt, patchNoise(seed, x, y, cell, salt), patchNoise(seed, Math.round(x), Math.round(y), cell, salt));
  }
}
