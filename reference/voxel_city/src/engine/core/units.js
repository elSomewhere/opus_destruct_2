/**
 * Physical units. The whole engine plans in integer voxel coordinates where
 * one voxel is 12.5 cm; every dimension in configs and catalogs is written in
 * meters and converted through `vx()` so the voxel size stays a single knob.
 */
export const VOXEL_SIZE = 0.125;
export const VOXELS_PER_METER = 1 / VOXEL_SIZE;

/** meters -> voxels (rounded) */
export const vx = (meters) => Math.round(meters * VOXELS_PER_METER);

/** voxels -> meters */
export const vm = (voxels) => voxels * VOXEL_SIZE;

/** Chunk edge length in voxels at every LOD. */
export const CHUNK = 32;
/** Padded chunk edge (1-voxel apron on every side for seamless meshing). */
export const PCHUNK = CHUNK + 2;
