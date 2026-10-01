import { frameOf } from "../buildings/frame.js";
import { tierRects } from "../buildings/archetypes.js";
import { SpatialGrid } from "../core/geom2d.js";
import { vx } from "../core/units.js";

/**
 * Site grading: how planned ground meets the terrain and the streets.
 *
 * An urban block's ground is one smooth surface stretched between the levels
 * of the sidewalks round it (cellPlan.blockSurface; the natural ground in a
 * village or on a farm), so gardens, yards, courtyards, parks and plazas
 * meet every street and every neighbour flush.
 *
 * A building stands on a level pad at the level of its entrance
 * (`env.groundZ`: the sidewalk in front of it, the courtyard at a slab's
 * middle): its footprint and an apron of APRON round it, plus the forecourt
 * of a civic building. Away from a pad the ground eases back to the block
 * surface over a band that widens with the height difference (EASE: at
 * most ~1 : 1.5), whatever it is (a garden, a courtyard, a park, the
 * leftover ground of the block), so on a hillside a house sits on a terrace
 * with its garden sloping away and neighbours meet without cliffs. Where
 * pads crowd each other their pulls are averaged; where a slope has no
 * room, the rest is a retaining wall (compose.js caps it).
 *
 * Lots that need one level surface (work yards, car parks, petrol
 * forecourts, schoolyards, wharves) stay a single terrace at the entrance
 * level and act as one big pad for the ground round them.
 *
 * `SiteGrading.at` is the one answer for graded ground in a cell: the ground
 * tiles (compose.js) and everything placed on it (yard trees, hedges,
 * fences, sheds, park furniture; dressing.js) ask here.
 */

export const APRON = vx(1.5);
const MIN_BAND = vx(1.5);
const MAX_BAND = vx(16);
const EASE = 2.3;

/** Lots kept as one level terrace. */
export function levelLot(lot, env) {
  if (!env) return false;
  if (env.yard || env.parking || env.archetype === "school" || env.archetype === "wharfhouse") return true;
  return env.annexes.some((a) => a.kind === "canopy");
}

/**
 * World rects of a building's pad (its footprint, annexes and a civic
 * forecourt), or the lot for a level lot. A turned building's pads are its
 * canonical rects ({ turned: frame, r }), measured in its own axes.
 */
function padRects(env, lot) {
  if (levelLot(lot, env)) return [lot.rect];
  const frame = frameOf(env);
  if (frame.turned) {
    const pads = tierRects(env, 0).map((r) => ({ turned: frame, r }));
    for (const a of env.annexes) if (a.kind !== "pylon") pads.push({ turned: frame, r: a.canon });
    if (env.civic && env.setF > 0) {
      const fp = env.tiers[0].rects[0];
      pads.push({ turned: frame, r: { x0: fp.x0, y0: fp.y0 - env.setF, x1: fp.x1, y1: fp.y0 } });
    }
    return pads;
  }
  const rects = tierRects(env, 0).map((r) => frame.rectToWorld(r));
  for (const a of env.annexes) if (a.kind !== "pylon") rects.push(a.world);
  // a civic building's forecourt (portico steps, porch, banners) is level with its door
  if (env.civic && env.setF > 0) {
    const fp = env.tiers[0].rects[0];
    rects.push(frame.rectToWorld({ x0: fp.x0, y0: fp.y0 - env.setF, x1: fp.x1, y1: fp.y0 }));
  }
  return rects;
}

function rectDistance(r, x, y) {
  if (r.turned) return r.turned.distance(r.r, x, y);
  const dx = Math.max(r.x0 - x, 0, x - r.x1);
  const dy = Math.max(r.y0 - y, 0, y - r.y1);
  return dx === 0 ? dy : dy === 0 ? dx : Math.hypot(dx, dy);
}

/** The pads of a cell's buildings and the level lots, looked up by position. */
export class SiteGrading {
  /** lots: the cell plan's lots; buildingById: its envelopes. */
  constructor(lots, buildingById) {
    this.grid = new SpatialGrid(128);
    this.levels = new Set();
    const reach = APRON + MAX_BAND;
    for (const lot of lots) {
      if (!lot.building || lot.underground || lot.underHighway) continue;
      const env = buildingById.get(lot.building);
      if (!env) continue;
      if (levelLot(lot, env)) this.levels.add(lot);
      const rects = padRects(env, lot);
      const bb = { x0: Infinity, y0: Infinity, x1: -Infinity, y1: -Infinity };
      for (const q of rects) {
        const r = q.turned ? q.turned.rectToWorld(q.r) : q;
        bb.x0 = Math.min(bb.x0, r.x0 - reach);
        bb.y0 = Math.min(bb.y0, r.y0 - reach);
        bb.x1 = Math.max(bb.x1, r.x1 + reach);
        bb.y1 = Math.max(bb.y1, r.y1 + reach);
      }
      this.grid.insert({ level: lot.groundZ, rects, apron: levelLot(lot, env) ? 0 : APRON, block: lot.block }, bb);
    }
  }

  /**
   * Graded ground (voxels, the top of the ground) at world column (x, y) of
   * block `blockId` (only its own buildings pull; a street lies between it
   * and the others): `base` is the block surface there (or the natural
   * ground), `lot` the lot the column is on, if any.
   */
  at(x, y, base, blockId, lot = null) {
    if (lot && this.levels.has(lot)) return lot.groundZ;
    const pads = this.grid.queryPoint(x, y);
    if (!pads.length) return Math.round(base);
    let sw = 0;
    let sd = 0;
    for (const p of pads) {
      if (p.block !== blockId) continue;
      let d = Infinity;
      for (const r of p.rects) d = Math.min(d, rectDistance(r, x, y));
      d -= p.apron;
      if (d <= 0) return p.level;
      const dz = p.level - base;
      const band = Math.max(MIN_BAND, Math.min(MAX_BAND, Math.abs(dz) * EASE));
      if (d >= band) continue;
      const t = d / band;
      const w = 1 - t * t * (3 - 2 * t);
      sw += w;
      sd += w * dz;
    }
    return Math.round(base + sd / Math.max(1, sw));
  }
}
