import { ARCHETYPES } from "../world/registry.js";
import { planBuildingEnvelopeAs } from "./archetypes.js";
import { Frame } from "./frame.js";
import { Rng } from "../core/hash.js";
import { rOverlaps } from "../core/rect.js";

/**
 * Stage sample buildings of an archetype on real, graded lots of a world,
 * in place of the buildings the lots already carry. For tests and debug
 * renders of archetypes that no district places yet: the staged envelopes
 * keep their lot's building id, and `world.envelopesIn` serves them instead
 * of the originals, so chunks, plans and the walker see them like any other
 * building. The lot keeps its original ground and yard dressing.
 *
 * opts:
 *   n         number of buildings to stage (default 3)
 *   district  district record for the envelope ({id, floors, ...});
 *             default: the lot's district id with 2-3 floors
 *   from      lot archetypes to replace (default: any)
 *   radius    cell search radius around (0, 0)
 *   lotRect   (lot, frame) => canonical sub-rect to build on (e.g. a
 *             cabin-sized plot at the front of a large lot), or null
 */
export function stageArchetype(world, archetypeId, styleId, opts = {}) {
  const { n = 3, district = null, from = null, radius = 3, lotRect = null } = opts;
  const arch = ARCHETYPES.get(archetypeId);
  const staged = stagedMap(world);
  const out = [];
  const cells = [];
  for (let r = 0; r <= radius; r += 1)
    for (let j = -r; j <= r; j += 1) for (let i = -r; i <= r; i += 1) if (Math.max(Math.abs(i), Math.abs(j)) === r) cells.push([i, j]);
  for (const [i, j] of cells) {
    if (out.length >= n) return out;
    const plan = world.cellPlan(i, j);
    for (const lot of plan.lots) {
      if (out.length >= n) break;
      const orig = lot.building ? plan.buildingById.get(lot.building) : null;
      if (!orig || staged.has(orig.id) || (from && !from.includes(orig.archetype))) continue;
      let rec = lot;
      if (lotRect) {
        const lf = new Frame(lot.rect, lot.front);
        const sub = lotRect(lot, lf);
        if (!sub) continue;
        rec = { ...lot, rect: lf.rectToWorld(sub) };
      }
      const f = new Frame(rec.rect, rec.front);
      if (!arch.fits(f.U, f.V)) continue;
      const d = district ?? { id: lot.district, floors: [2, 3], archetypes: [], styles: [] };
      const rng = Rng.from(world.seed, lot.id, "stage", archetypeId);
      const env = planBuildingEnvelopeAs(rec, archetypeId, styleId, d, rng, { u: lot.u, core: lot.core, groundZ: lot.groundZ, config: world.config });
      if (!env) continue;
      // same id as the building it replaces (finalizeEnvelope: `${lot.id}/B`)
      env.id = orig.id;
      env.lot = lot.id;
      world.buildingPlans?.map?.delete(env.id);
      staged.set(env.id, env);
      out.push(env);
    }
  }
  return out;
}

function stagedMap(world) {
  if (world._staged) return world._staged;
  const staged = new Map();
  const base = world.envelopesIn.bind(world);
  world.envelopesIn = (rect) => {
    const res = base(rect).filter((e) => !staged.has(e.id));
    for (const e of staged.values()) if (rOverlaps(e.bounds, rect)) res.push(e);
    return res;
  };
  world._staged = staged;
  return staged;
}
