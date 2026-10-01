// Stage "landcover": the natural land cover (nature/landcover.js) and the fields of open country
// (nature/farmland.js) of every world of lib/worlds.mjs, plus worlds in the seasons the presets
// leave out (spring; a cool autumn, a warm winter). At the sample points: the local climate, the
// biome, the tree line, glades, forest density, farmland masks, ponds and hedges; surfaces as the
// ground tile asks for them and with drawn slopes, urbanization, farms and heights (beaches, cliffs,
// strata, scree, snowfields, the alpine zone, ponds, forest floors, fields, glades, the biomes'
// ground); the field of the point (its strip, boundary, crops, ground and hedge in three climates)
// and fields along block edges; the planar noises; every biome's forest floor. The worlds are
// World.js's with a LandCover (createWorld's harbour hook is the stage "water"'s); every third
// sample point; every terrain sample makes what it reads first (lib/worlds.mjs pureTerrain;
// docs/CITY.md §6).
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, pureTerrain, samplePoints } from "../lib/worlds.mjs";

const { World } = await import(REF + "world/World.js");
const { LandCover, LAPSE, TREELINE, SNOWLINE } = await import(REF + "nature/landcover.js");
const { BIOMES } = await import(REF + "world/registry.js");

/** Worlds in the seasons the presets leave out (tests/city/test_landcover.cpp has the same). */
export const SEASON_WORLDS = [
  ["spring", '{"seed":17,"world":{"season":"spring"}}'],
  ["autumnCool", '{"seed":23,"world":{"season":"autumn","climate":{"temperature":0.38}}}'],
  ["winterWarm", '{"seed":29,"world":{"season":"winter","climate":{"temperature":0.68,"moisture":0.5}}}'],
];

/** LandCover.surface's [top, sub, waterDepth, bump, field] (undefined: 0, false - as its callers read them). */
const surf = (s) => [s[0], s[1], s[2] ?? 0, s[3] ?? 0, s[4] ?? false];

/** A field's record and its ground in three climates. */
function field(FM, f, x, y) {
  const g = (t) => FM.ground(f, t, x, y);
  return [f.key, f.alongX, f.a, f.b, f.edge, f.edgeKey, FM.crop(f.key, 0.3), FM.crop(f.key, 0.5), FM.crop(f.key, 0.7), g(0.3), g(0.5), g(0.7), FM.hedge(f, 0.3),
    FM.hedge(f, 0.5)];
}

export default function* landcover() {
  const r = samples(61);
  yield line("const", LAPSE, TREELINE, SNOWLINE);
  const worlds = [...allWorlds(), ...SEASON_WORLDS.map(([key, json]) => [key, JSON.parse(json)])];
  let first = true;
  for (const [key, overrides] of worlds) {
    const w = pureTerrain(new World(overrides));
    const LC = new LandCover(w);
    const FM = LC.farmland;
    yield line("lc", key, LC.torus, FM.season, FM.seed);
    const sea = w.config.world.seaLevel * 8;
    const pts = samplePoints(w, r).filter((_, i) => i % 3 === 0);
    for (const [x, y] of pts) {
      const ts = w.terrain.sample(x, y);
      const hM = ts.h * 0.125;
      const c = LC.climate(x, y, hM);
      const b = LC.biomeAt(x, y, hM);
      yield line("p", x, y, ts.h, ts.u, c.t, c.m, b.id, LC.treeLine(c.t), LC.clearing(x, y), LC.forestDensity(x, y, ts.u), LC.isFarmland(x, y, ts.u),
        LC.farmMask(x, y, 0, b), LC.farmMask(x, y, 0.03, b), LC.farmMask(x, y, 0.12, b), LC.farmMask(x, y, 0.19, b));
      // surfaces: at the ground's height, then drawn ones (high and low ground, slopes, towns, farms), then a shore
      const slope1 = r() * r() * 1.2;
      const s1 = LC.surface(x, y, Math.round(ts.h), slope1, ts.u);
      const h2 = Math.round(ts.h + (r() - 0.3) * 24000);
      const slope2 = r() * 2.8;
      const u2 = r() * 0.25;
      const farm2 = r() < 0.25;
      const s2 = LC.surface(x, y, h2, slope2, u2, farm2);
      const h3 = Math.round(sea + r() * 14 - 4);
      const slope3 = r();
      const u3 = r() * 0.2;
      const s3 = LC.surface(x, y, h3, slope3, u3);
      yield line("s", slope1, ...surf(s1), h2, slope2, u2, farm2, ...surf(s2), h3, slope3, u3, ...surf(s3));
      const u4 = r() * 0.25;
      const pslope = r() * 0.15;
      const pu = r() * 0.12;
      const hu = r() * 0.25;
      const ht = 0.2 + r() * 0.6;
      const hfarm = r() < 0.5;
      const sv = 20 + r() * 2000;
      const ox = (r() - 0.5) * 20;
      const oy = (r() - 0.5) * 20;
      const oct = Math.floor(r() * 4);
      const sM = 50 + r() * 2000;
      const oct2 = 1 + Math.floor(r() * 3);
      yield line("q", LC.forestDensity(x, y, u4, b, hM), LC.poolDepth(x, y, b, pslope, pu), LC.hedgeAt(x, y, hu, b, ht, hfarm), LC.fieldNoise(LC.nPatch, x, y, sv, ox, oy, oct),
        LC.fieldFbmM(LC.nForest, x, y, sM, oct2));
      yield line("f", ...field(FM, FM.fieldAt(x, y), x, y));
    }
    // fields along farm block edges (the block's own edges, the walls and hedges both blocks share)
    for (let k = 0; k < 300; k += 1) {
      const bi = Math.floor((r() - 0.5) * 200);
      const x = bi * 2400 + Math.floor(r() * 24) - 12;
      const y = Math.floor((r() - 0.5) * 480000);
      const along = r() < 0.5;
      const [px, py] = along ? [x, y] : [y, x];
      yield line("e", px, py, ...field(FM, FM.fieldAt(px, py), px, py));
    }
    if (first) {
      // every biome's forest floor
      first = false;
      for (const bio of BIOMES.all())
        for (let k = 0; k < 40; k += 1) {
          const t = r() * 0.8;
          const h = r();
          const p2 = r() * 2 - 1;
          const patch = r() * 2 - 1;
          yield line("ff", bio.id, t, h, p2, patch, LC.forestFloor(bio, t, h, p2, patch));
        }
    }
  }
}
