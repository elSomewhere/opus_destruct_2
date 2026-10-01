/**
 * World config from script arguments:
 *   --preset id   a world preset (config/presets.js: cities, infiniteCity,
 *                 wrapWorld, island, nordicIsland, planetEquator, planetNorth)
 *   --variant v   the preset's size variant (small / medium / large)
 *   --seed n      world seed (default 1337)
 *   --season s    spring / summer / autumn / winter (default: the preset's)
 *   --parts 1     parts mode (world.angles.partsMode "separate"): the world
 *                 grid leaves the angled world's parts out, the renderers draw
 *                 each in its own lattice (scripts/lib/parts.js)
 *   --wings 1|0   the angled world's wings, corner and canted bays
 *                 (world.angles.features.wings, S5) on or off
 *   --angles 1|0  world.angles.enabled (a preset of the grid world with
 *                 the angled world's features, as --features sets them)
 *   --features f=1,g=0   world.angles.features (roads, vegetation,
 *                 buildings, ramps, wings): e.g. --preset cities --angles 1
 *                 --features roads=0,buildings=0,vegetation=0,ramps=1 draws
 *                 the grid city with its garages' ramps pitched
 */
import { presetConfig } from "../../src/engine/config/presets.js";

export function worldConfigFromArgs(args, extra = {}) {
  const seed = Number(args.seed ?? 1337);
  const base = args.preset ? presetConfig(args.preset, { size: args.variant ?? null, seed, season: args.season ?? null }) : { seed, ...(args.season ? { world: { season: args.season } } : {}) };
  const cfg = { ...base, ...extra, world: { ...(base.world ?? {}), ...(extra.world ?? {}) } };
  if (Number(args.parts ?? 0)) cfg.world.angles = { ...(cfg.world.angles ?? {}), partsMode: "separate" };
  if (args.wings !== undefined) cfg.world.angles = { ...(cfg.world.angles ?? {}), features: { ...(cfg.world.angles?.features ?? {}), wings: Number(args.wings) === 1 } };
  if (args.angles !== undefined) cfg.world.angles = { ...(cfg.world.angles ?? {}), enabled: Number(args.angles) === 1 };
  if (args.features) {
    const f = Object.fromEntries(String(args.features).split(",").map((kv) => kv.split("=")).map(([k, v]) => [k, Number(v) === 1]));
    cfg.world.angles = { ...(cfg.world.angles ?? {}), features: { ...(cfg.world.angles?.features ?? {}), ...f } };
  }
  return cfg;
}
