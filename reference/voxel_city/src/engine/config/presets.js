import { Registry } from "../world/registry.js";
import { SEASONS, SEASON_ATMOSPHERE } from "../world/season.js";

/**
 * World presets: named, loadable starting points (config overrides plus a
 * viewer mood). A preset is data: register one with
 *
 *   PRESETS.register({ id, label, description, sizes?, defaultSize?,
 *                      defaultSeason?,
 *                      config(size) -> config overrides,
 *                      viewer? -> { timeOfDay, atmosphere, seasons? } })
 *
 * `sizes` (optional) lists named variants ({ id, label, ... }) handed to
 * `config(size)`; `presetConfig(id, { size, seed, season })` resolves a
 * preset into the overrides `createWorld` / `makeConfig` take. Every preset
 * can be shown in every season (world/season.js; `defaultSeason`, else
 * summer); `presetViewer(id, season)` gives the viewer mood for it.
 *
 * Atmospheres (viewer/ViewerEngine.js) tint sky, fog and light:
 *   { sky, fogDensity (x default), sun (intensity x), ambient (x),
 *     sunElevation (max sine of the sun's altitude: a low northern sun),
 *     desaturate (0..1) }
 * layered: the season's default (SEASON_ATMOSPHERE), the preset's
 * `atmosphere`, then the preset's `seasons[season]`.
 */
export const PRESETS = new Registry("preset");

PRESETS.register({
  id: "cities",
  label: "Cities in countryside",
  description: "An endless plane of towns, villages, farms, mountains and biomes.",
  config: () => ({ world: { mode: "cities" } }),
});

PRESETS.register({
  id: "infiniteCity",
  label: "Infinite city",
  description: "Urban land everywhere.",
  config: () => ({ world: { mode: "infiniteCity" } }),
});

/** Torus sizes: the world repeats every `size` meters east-west and north-south. */
const WRAP_SIZES = [
  { id: "small", label: "48 km", size: 48000 },
  { id: "medium", label: "96 km", size: 96000 },
  { id: "large", label: "192 km", size: 192000 },
];

PRESETS.register({
  id: "wrapWorld",
  label: "Wrapping world (flat planet)",
  description: "A finite world whose edges join: walk east (or north) long enough and you are back where you started. Climate runs from an equator to a pole and back.",
  sizes: WRAP_SIZES,
  defaultSize: "medium",
  config: (size) => wrapConfig(size.size),
});

/**
 * Terrain and climate scales follow the world size, so a small world still
 * has a full set of biomes, a mountain range or two and several towns.
 */
export function wrapConfig(size) {
  const k = Math.min(1, size / 200000);
  return {
    world: { mode: "cities", chart: "torus", size, latitude: true, climateScale: Math.round(8000 + 24000 * k) },
    terrain: {
      mountainBeltScale: Math.round(Math.max(30000, size * 0.9)),
      mountainMassifScale: Math.round(Math.max(18000, size * 0.35)),
      mountainScale: Math.round(12000 + 15000 * k),
      spawnMountains: [Math.min(10, size / 12000), Math.min(35, size / 4000)],
    },
  };
}

/** Island sizes: coast radius, main-town population, smaller places. */
const ISLAND_SIZES = [
  { id: "small", label: "Small (≈ 9 km)", radius: 4500, population: 9000, towns: 0, villages: 1, hamlets: 2, peak: 620 },
  { id: "medium", label: "Medium (≈ 16 km)", radius: 8000, population: 20000, towns: 1, villages: 2, hamlets: 2, peak: 950 },
  { id: "large", label: "Large (≈ 26 km)", radius: 13000, population: 32000, towns: 1, villages: 3, hamlets: 4, peak: 1250 },
];

const ISLAND_KEYS = ["radius", "population", "towns", "villages", "hamlets", "peak", "elongation", "roughness", "highlands", "fjords", "skerries", "cliffs", "shelf", "townRise", "cabins"];

function islandSize(size) {
  const out = {};
  for (const k of ISLAND_KEYS) if (size[k] !== undefined) out[k] = size[k];
  return out;
}

/**
 * Island mode keeps the infrastructure of a small place: no elevated
 * highways or subway, two-lane main streets instead of avenues.
 */
const ISLAND_BASE = {
  highways: { enabled: false },
  subway: { enabled: false },
  lakes: { bigChance: 0 },
  city: { ruralRoadChance: 0.18, mainRoad: "collector" },
};

/**
 * Small islands (4-6 km): a lattice fine enough for one small town, its
 * streets and a few lanes into the country, tarns instead of lakes.
 */
function smallIsland(size, extra = {}) {
  return {
    ...ISLAND_BASE,
    city: { ...ISLAND_BASE.city, arterialSpacing: 500, ruralRoadChance: 0.12, ...(extra.city ?? {}) },
    lakes: { bigChance: 0, cell: 900, chance: 0.5, scale: 0.28, townProximity: 0.55 },
    rivers: { enabled: false },
    world: {
      mode: "island",
      villageRadius: [150, 380],
      island: { ...islandSize(size), ...(extra.island ?? {}) },
      climate: extra.climate,
    },
  };
}

PRESETS.register({
  id: "island",
  label: "Island",
  description: "One temperate island in an endless sea: a harbour town, villages, forests, highlands, beaches and cliffs.",
  sizes: ISLAND_SIZES,
  defaultSize: "medium",
  config: (size) => ({
    ...ISLAND_BASE,
    world: { mode: "island", island: { ...islandSize(size), cliffs: 0.3, flavor: "harbourTown" }, climate: { temperature: 0.5, temperatureVar: 0.04, moisture: 0.6, moistureVar: 0.1 } },
  }),
});

/**
 * A bleak northern island (Norway / Karelia / the White Sea): a harbour town
 * with an old wooden centre, panel-block estates and a little industry,
 * fishing hamlets, cabins in the pine forest, bare fells over fjords, snow
 * on the ground, frozen lakes and a low sun behind an overcast sky.
 */
/** Northern light through the year: a low winter sun behind an overcast sky, long pale summer days. */
const NORDIC_SEASONS = {
  winter: { sky: 0xa3adb5, fogDensity: 1.9, sun: 0.45, ambient: 0.95, sunElevation: 0.2, desaturate: 0.35 },
  spring: { sky: 0xadbecc, fogDensity: 1.5, sun: 0.8, ambient: 1, sunElevation: 0.5, desaturate: 0.14 },
  summer: { sky: 0xa6bdd0, fogDensity: 1.25, sun: 0.95, ambient: 1, sunElevation: 0.72, desaturate: 0.06 },
  autumn: { sky: 0xa2acb3, fogDensity: 1.75, sun: 0.62, ambient: 0.95, sunElevation: 0.34, desaturate: 0.24 },
};

PRESETS.register({
  id: "nordicIsland",
  label: "Nordic island",
  description: "A bleak Scandinavian / north-Russian island: old wooden town centre, post-Soviet housing estates, a small port, cabins in pine and spruce forests, fjords and fells. Winter by default.",
  sizes: ISLAND_SIZES,
  defaultSize: "medium",
  defaultSeason: "winter",
  config: (size) => ({
    ...ISLAND_BASE,
    world: {
      mode: "island",
      island: { ...islandSize(size), highlands: 0.5, fjords: 0.8, skerries: 0.7, cliffs: 0.45, elongation: 1.7, flavor: "nordicBleak", cabins: 0.8 },
      climate: { temperature: 0.345, temperatureVar: 0.025, moisture: 0.66, moistureVar: 0.18 },
    },
    city: { ...ISLAND_BASE.city, ruralRoadChance: 0.14 },
  }),
  viewer: { timeOfDay: 11.5, seasons: NORDIC_SEASONS },
});

/**
 * Small Nordic islands (4-5 km): one small old town and a hamlet or two,
 * dense spruce and pine forest, meadows, creeks and tarns, rock outcrops
 * and bare skerries. Three kinds of island.
 */
const TOWN_ISLANDS = [
  {
    id: "skerry",
    label: "Skerry town (≈ 4 km)",
    radius: 1900,
    elongation: 1.35,
    roughness: 0.8,
    population: 3200,
    towns: 0,
    villages: 0,
    hamlets: 1,
    peak: 120,
    highlands: 0.18,
    fjords: 0,
    skerries: 1,
    cliffs: 0.3,
    shelf: 900,
    townRise: 10,
    cabins: 0.9,
    flavor: "nordicHarbour",
  },
  {
    id: "fjord",
    label: "Fjord town (≈ 5 km)",
    radius: 2500,
    elongation: 1.5,
    roughness: 0.6,
    population: 4600,
    towns: 0,
    villages: 0,
    hamlets: 2,
    peak: 470,
    highlands: 0.48,
    fjords: 0.45,
    skerries: 0.5,
    cliffs: 0.5,
    shelf: 1100,
    townRise: 34,
    cabins: 0.8,
    flavor: "nordicHarbour",
  },
  {
    id: "forest",
    label: "Forest town (≈ 5 km)",
    radius: 2400,
    elongation: 1.2,
    roughness: 0.5,
    population: 4200,
    towns: 0,
    villages: 1,
    hamlets: 1,
    peak: 200,
    highlands: 0.2,
    fjords: 0.1,
    skerries: 0.35,
    cliffs: 0.2,
    shelf: 1000,
    townRise: 8,
    cabins: 1,
    flavor: "nordicBleak",
    moisture: 0.62,
  },
];

PRESETS.register({
  id: "nordicTown",
  label: "Nordic small-town island",
  description: "A 4-5 km island with one small northern town: an old wooden centre on cobbled lanes, a church and its cemetery, a harbour, allotments, a few bleak blocks by the works; dense spruce forest, meadows, creeks, tarns and skerries. Summer by default.",
  sizes: TOWN_ISLANDS,
  defaultSize: "fjord",
  defaultSeason: "summer",
  config: (size) =>
    smallIsland(size, {
      island: { flavor: size.flavor },
      climate: { temperature: 0.36, temperatureVar: 0.02, moisture: size.moisture ?? 0.66, moistureVar: 0.14 },
    }),
  viewer: { timeOfDay: 14, seasons: NORDIC_SEASONS },
});

/**
 * An old Hanseatic harbour town (after Bergen): a steep town round a
 * harbour bay under the fells, a wharf of gabled warehouses, cobbled lanes
 * climbing the hillside, stone churches, a market square; autumn rain.
 */
PRESETS.register({
  id: "oldHarbourTown",
  label: "Old harbour town (Bergen-like)",
  description: "A 6 km fjord island with an old Hanseatic harbour town climbing the hillside: wharf warehouses, cobbled lanes, wooden houses, churches and a market square; fells all round. Autumn by default.",
  defaultSeason: "autumn",
  config: () =>
    smallIsland(
      { radius: 3000, elongation: 1.45, roughness: 0.65, population: 7500, towns: 0, villages: 0, hamlets: 2, peak: 560, highlands: 0.55, fjords: 0.55, skerries: 0.45, cliffs: 0.55, shelf: 1200, townRise: 48, cabins: 0.6 },
      {
        island: { flavor: "nordicHarbour" },
        climate: { temperature: 0.38, temperatureVar: 0.02, moisture: 0.63, moistureVar: 0.1 },
      },
    ),
  viewer: {
    timeOfDay: 13,
    seasons: { ...NORDIC_SEASONS, autumn: { sky: 0x9ea9b0, fogDensity: 1.9, sun: 0.55, ambient: 0.95, sunElevation: 0.38, desaturate: 0.26 } },
  },
});

/**
 * A White Sea / Karelian town: flat forest and bog country, a wooden town
 * with a log church, Soviet panel blocks and a sawmill by the water.
 */
PRESETS.register({
  id: "whiteSeaTown",
  label: "White Sea town (north Russian)",
  description: "A flat 5 km island of spruce forest, bogs and tarns with a Karelian wooden town, Soviet panel blocks, garages, a sawmill and a small port. Autumn by default.",
  defaultSeason: "autumn",
  config: () =>
    smallIsland(
      { radius: 2500, elongation: 1.3, roughness: 0.45, population: 5200, towns: 0, villages: 1, hamlets: 1, peak: 90, highlands: 0.1, fjords: 0, skerries: 0.4, cliffs: 0.12, shelf: 1300, townRise: 6, cabins: 0.7 },
      {
        island: { flavor: "nordicBleak" },
        climate: { temperature: 0.33, temperatureVar: 0.02, moisture: 0.7, moistureVar: 0.14 },
      },
    ),
  viewer: { timeOfDay: 12.5, seasons: NORDIC_SEASONS },
});

PRESETS.register({
  id: "planetEquator",
  label: "Planet face (equator)",
  description: "One face of a cube-sphere planet on the equator.",
  config: () => ({ world: { mode: "cities", chart: "cube", planet: { radius: 240000, face: 0 } } }),
});

PRESETS.register({
  id: "planetNorth",
  label: "Planet face (north pole)",
  description: "The north-pole face of a cube-sphere planet.",
  config: () => ({ world: { mode: "cities", chart: "cube", planet: { radius: 240000, face: 4 } } }),
});

/**
 * Angled variants (ANGLED_WORLD_PLAN.md): the same worlds with
 * `world.angles` on, as presets of their own. The presets above keep the
 * axis-aligned world, bit for bit (test/golden.test.js). Their buildings
 * carry wings, corner and canted bays (S5, off by default): the budget
 * audit shows the headroom (scripts/audit-angles.js, test/wings.test.js).
 */
function angled(id, description) {
  const base = PRESETS.get(id);
  PRESETS.register({
    ...base,
    id: `angled${id[0].toUpperCase()}${id.slice(1)}`,
    label: `${base.label}, angled`,
    description,
    config: (size) => {
      const cfg = base.config(size);
      const angles = cfg.world?.angles ?? {};
      return { ...cfg, world: { ...(cfg.world ?? {}), angles: { ...angles, enabled: true, features: { wings: true, ...(angles.features ?? {}) } } } };
    },
  });
}

angled("cities", "Cities in countryside with angled streets and buildings: diagonal boulevards, curving country roads, a building in eight turned to its street.");
angled("infiniteCity", "The infinite city, angled: diagonal boulevards through the grid, turned buildings along them.");
angled("nordicTown", "A small northern town island, angled: crooked old-town lanes, winding roads through a more natural forest.");
angled("oldHarbourTown", "The old harbour town, angled: steep streets climbing the hillside on smooth ramps.");

/** Size variant of a preset by id (or its default). */
export function presetSize(preset, sizeId) {
  const p = typeof preset === "string" ? PRESETS.get(preset) : preset;
  if (!p.sizes) return null;
  return p.sizes.find((s) => s.id === sizeId) ?? p.sizes.find((s) => s.id === p.defaultSize) ?? p.sizes[0];
}

/** Season of a preset: the given one if valid, else its default (else summer). */
export function presetSeason(preset, season = null) {
  const p = typeof preset === "string" ? PRESETS.get(preset) : preset;
  if (season && SEASONS.some((s) => s.id === season)) return season;
  return p.defaultSeason ?? "summer";
}

/** Config overrides of a preset (+ size variant, seed and season). */
export function presetConfig(id, { size = null, seed = null, season = null } = {}) {
  const p = PRESETS.get(id);
  const cfg = p.config(presetSize(p, size));
  if (seed !== null && seed !== undefined) cfg.seed = seed;
  cfg.world = { ...(cfg.world ?? {}), season: presetSeason(p, season) };
  return cfg;
}

/** Viewer mood of a preset in a season: { timeOfDay, atmosphere }. */
export function presetViewer(id, season = null) {
  const p = PRESETS.get(id);
  const s = presetSeason(p, season);
  const v = p.viewer ?? {};
  const atmosphere = { ...SEASON_ATMOSPHERE[s], ...(v.atmosphere ?? {}), ...(v.seasons?.[s] ?? {}) };
  return { timeOfDay: v.timeOfDay ?? 13, atmosphere };
}
