// Shared inputs of the building shell stages (massing, wings, garageramps, skybridges, grading; the
// C++ twin is tests/city/shell_records.hpp): the worlds a shell is drawn in (every season, explicit
// snow covers), scripted envelopes of every archetype, the chunks round a box and their digests.
import { REF, line } from "./rec.mjs";
import { scriptedLot } from "./buildings.mjs";

const { World } = await import(REF + "world/World.js");
const { ARCHETYPES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const { Rng } = await import(REF + "core/hash.js");

/**
 * The worlds of the shell stages (config overrides as JSON, parsed alike by both sides): summer,
 * winter, a cold spring and autumn (snow on roofs from the season), an explicit snow cover in
 * summer (patches) and one above 1 (every roof white).
 */
export const SHELL_WORLDS = [
  '{"seed":1}',
  '{"seed":2,"world":{"season":"winter"}}',
  '{"seed":3,"world":{"season":"spring","climate":{"temperature":0.2}}}',
  '{"seed":4,"world":{"season":"autumn","climate":{"temperature":0.1,"temperatureVar":0.02}}}',
  '{"seed":5,"world":{"climate":{"snowCover":0.6}}}',
  '{"seed":6,"world":{"season":"winter","climate":{"snowCover":1.4}}}',
  '{"seed":7,"world":{"season":"winter","climate":{"temperature":0.05}}}',
];

const worlds = new Map();
/** World k of SHELL_WORLDS (a World of World.js, made once). */
export function shellWorld(k) {
  let w = worlds.get(k);
  if (!w) worlds.set(k, (w = new World(JSON.parse(SHELL_WORLDS[k]))));
  return w;
}

// a flavor's church dome materials (and a list with a name the palette lacks)
const DOMES = [["GOLD", "DOME_GREEN", "DOME_BLUE"], ["DOME_SHINGLE", "DOME_GREEN"], ["NOPE"]];

/**
 * A scripted envelope of archetype `a` (an archetype record) in style `style` (null: the district's
 * pick, planBuildingEnvelope) on a scripted lot of a size it fits (lib/buildings.mjs scriptedLot,
 * plain or turned), in district d, in world w: { lot, env } (env null when none fits). Draws from r.
 */
export function shellEnvelope(r, a, style, d, w, k) {
  let U = 0;
  let V = 0;
  for (let t = 0; t < 40; t += 1) {
    U = 40 + Math.floor(r() * 560);
    V = 40 + Math.floor(r() * 560);
    if (a.fits(U, V)) break;
  }
  const lot = scriptedLot(r, U, V, k, d.id);
  const extra = { u: r(), core: r() * 1.2, groundZ: Math.floor(r() * 3000) - 200, config: w.config };
  if (r() < 0.2) extra.chapel = true;
  if (r() < 0.5) extra.dome = DOMES[Math.floor(r() * DOMES.length)];
  if (r() < 0.4) extra.pitchedCivic = 0.6;
  const rng = new Rng(Math.floor(r() * 4294967296));
  const env = style === null ? A.planBuildingEnvelope(lot, d, rng, extra) : A.planBuildingEnvelopeAs(lot, a.id, style, d, rng, extra);
  return { lot, env };
}

/** Every archetype, in registration order. */
export const archetypeList = () => ARCHETYPES.all();

/** A chunk's content: FNV-1a over its voxels, and the count of the solid ones. */
export function chunkDigest(c) {
  let h = 2166136261;
  let n = 0;
  const d = c.data;
  for (let i = 0; i < d.length; i += 1) {
    h = Math.imul(h ^ d[i], 16777619);
    if (d[i]) n += 1;
  }
  return [h >>> 0, n];
}

/**
 * The chunks (LOD 0 world chunk coordinates [lod, cx, cy, cz] for each lod of `lods`) holding
 * the given points (voxels [x, y, z]), each once per LOD in the points' order.
 */
export function chunksAt(lods, pts) {
  const out = [];
  for (const lod of lods) {
    const E = 32 << lod;
    const seen = new Set();
    for (const [x, y, z] of pts) {
      const c = [lod, Math.floor(x / E), Math.floor(y / E), Math.floor(z / E)];
      const key = c.join(",");
      if (seen.has(key)) continue;
      seen.add(key);
      out.push(c);
    }
  }
  return out;
}

export { line };
