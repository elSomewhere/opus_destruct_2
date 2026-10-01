import { ChunkBuffer } from "../voxel/chunk.js";
import { LRU } from "../core/lru.js";
import { rasterizeRoadPart } from "../network/roadParts.js";
import { voxelizeBuilding } from "../buildings/interior/voxelize.js";
import { voxelizeMassing, roofSnowCover } from "../buildings/massing.js";
import { rasterizeWingPart } from "../buildings/wings.js";
import { rasterizeRampPart } from "../buildings/garageRamps.js";

/**
 * A part's content in its own lattice (ANGLED_WORLD_PLAN.md §8: structvox's
 * generate_grid): chunks addressed in the part's local cells (a chunk's
 * wx / wy / wz are the part's u / v / w), what a renderer draws with the
 * part's transform and what the world grid leaves out in parts mode
 * (`world.angles.partsMode: "separate"`, compose.js). A road piece is its
 * pitched slab (network/roadParts.js); a row of turned buildings is drawn
 * by the building voxelizers, each through its frame made square (the
 * lattice is its canonical one, offset), its w the world's z; a garage's
 * ramp its pitched slab (buildings/garageRamps.js); a wing is its box and
 * the band of its building's wall it is cast into (buildings/wings.js).
 */

const caches = new WeakMap();

/** The part's content chunk (lod, cx, cy, cz) of its own lattice, cached per world. */
export function partChunk(world, part, lod, cx, cy, cz) {
  let cache = caches.get(world);
  if (!cache) {
    cache = new LRU(512);
    caches.set(world, cache);
  }
  return cache.getOrCreate(`${part.id}:${lod}:${cx},${cy},${cz}`, () => {
    const chunk = new ChunkBuffer(lod, cx, cy, cz);
    if (part.kind === "road") rasterizeRoadPart(world, part, chunk);
    else if (part.kind === "building") rasterizeBuildingPart(world, part, chunk);
    else if (part.kind === "ramp") rasterizeRampPart(world, part, chunk);
    else if (part.kind === "wing") {
      const env = world.cellPlan(...part.cell).buildingById.get(part.env);
      rasterizeWingPart(world, env, env.wings.find((w) => w.part === part.id), chunk);
    }
    return chunk;
  });
}

const locals = new WeakMap();

/**
 * The envelope of turned building `id` in its part's lattice: the same
 * building, its frame the square one of the canonical cells offset by the
 * turn (so every canonical rect is a rect of the lattice), and a world
 * whose plan for it is its own plan with fresh box caches (they hold world
 * boxes of the turned frame).
 */
function localOf(world, part, id) {
  let byId = locals.get(part);
  if (!byId) locals.set(part, (byId = new Map()));
  let l = byId.get(id);
  if (l) return l;
  const env = world.cellPlan(...part.cell).buildingById.get(id);
  const { ou, ov } = env.turn;
  const shift = (r) => ({ ...r, x0: r.x0 + ou, y0: r.y0 + ov, x1: r.x1 + ou, y1: r.y1 + ov });
  const R = { x0: ou, y0: ov, x1: ou + env.U - 1, y1: ov + env.V - 1 };
  const annexes = env.annexes.map((a) => ({ ...a, world: shift(a.canon), canon: undefined }));
  const pad = 16;
  const bounds = { x0: R.x0 - pad, y0: R.y0 - pad, x1: R.x1 + pad, y1: R.y1 + pad };
  for (const a of annexes) {
    bounds.x0 = Math.min(bounds.x0, a.world.x0 - pad);
    bounds.y0 = Math.min(bounds.y0, a.world.y0 - pad);
    bounds.x1 = Math.max(bounds.x1, a.world.x1 + pad);
    bounds.y1 = Math.max(bounds.y1, a.world.y1 + pad);
  }
  // (its snow as in the world: the cover is taken where the building stands)
  const localEnv = { ...env, R, front: "N", turn: undefined, annexes, bounds, _snow: roofSnowCover(world, env) };
  const plan0 = world.buildingPlan(env);
  let plan = null;
  if (plan0) {
    const floors = plan0.floors.map((F) => ({ ...F, _boxes: undefined }));
    plan = { ...plan0, _boxes: undefined, floors, floorByIndex: new Map(floors.map((F) => [F.index, F])) };
  }
  const w2 = Object.create(world);
  w2.buildingPlan = () => plan;
  l = { env: localEnv, world: w2 };
  byId.set(id, l);
  return l;
}

/** A row of turned buildings (one or more, along the street): each in turn, in the lattice they share. */
function rasterizeBuildingPart(world, part, chunk) {
  for (const id of part.members) {
    const l = localOf(world, part, id);
    if (chunk.lod === 0) voxelizeBuilding(l.world, l.env, chunk);
    else voxelizeMassing(l.world, l.env, chunk);
  }
}
