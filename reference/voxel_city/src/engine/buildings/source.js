import { voxelizeMassing } from "./massing.js";
import { rasterizeWing } from "./wings.js";

/**
 * Feature source: buildings. LOD0 chunks get full interiors once a building
 * plan exists (buildings/interior); coarser LODs use the massing voxelizer.
 * In parts mode (world.angles.partsMode "separate") a turned building, a
 * part of its own (`env.part`), is left out: it is drawn in its lattice.
 * A building's wings (S5, buildings/wings.js) follow it, in the world grid
 * unless parts mode leaves them to their lattices.
 */
const own = (world) => (world.config.world.angles?.partsMode === "separate" ? (env) => !env.part : () => true);
export const buildingSource = {
  id: "buildings",
  order: 10,
  maxLod: 6,

  zRange(world, rect) {
    let lo = Infinity;
    let hi = -Infinity;
    const keep = own(world);
    for (const env of world.envelopesIn(rect)) {
      if (!keep(env)) continue;
      if (env.bottomZ < lo) lo = env.bottomZ;
      if (env.topZ > hi) hi = env.topZ;
    }
    return lo === Infinity ? null : [lo, hi];
  },

  rasterize(world, chunk) {
    const box = chunk.worldBox;
    const envs = world.envelopesIn(box);
    const keep = own(world);
    const separate = world.config.world.angles?.partsMode === "separate";
    for (const env of envs) {
      if (!keep(env)) continue;
      if (chunk.lod === 0 && world.voxelizeBuilding) world.voxelizeBuilding(env, chunk);
      else voxelizeMassing(world, env, chunk);
      if (env.wings && !separate) for (const w of env.wings) rasterizeWing(world, env, w, chunk);
    }
  },
};
