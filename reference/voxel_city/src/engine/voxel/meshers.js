import { meshChunk } from "./mesher.js";
import { meshDualContour, meshMarchingCubes } from "./smoothMeshers.js";

export const MESHERS = ["greedy", "marching-cubes", "dual-contour"];

/** Worker-safe mesher selector. Transparent voxels retain the proven greedy pass. */
export function meshChunkWith(data, opts = {}) {
  const id = MESHERS.includes(opts.mesher) ? opts.mesher : "greedy";
  const classic = meshChunk(data, opts);
  // Coarse tiles keep greedy skirts: they close LOD boundaries without
  // requiring a second apron or Transvoxel transition samples.
  if (id === "greedy" || opts.skirt) return classic;
  const opaque = id === "marching-cubes" ? meshMarchingCubes(data) : meshDualContour(data);
  return { opaque, transparent: classic.transparent };
}
