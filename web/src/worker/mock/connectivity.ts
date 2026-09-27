/**
 * Support check after edits: which solid voxels next to a carve are no longer connected
 * (6-connectivity) to anchored ground?
 *
 * Each seed runs a best-first flood fill that always expands the lowest voxel first, so a
 * supported piece usually reaches the ground by walking straight down (O(height)), while an
 * unsupported piece is explored completely and returned as an island. Searches share their
 * results: touching a voxel already proven supported ends a search early.
 *
 * This is the mock's stand-in for the engine's hierarchical connectivity (plan §B7).
 */
import { isAnchored } from './blocks.ts';
import { AIR, type VoxelWorld } from './world.ts';

export interface Island {
  /** Packed grid indices (VoxelWorld.pack). */
  voxels: Int32Array;
  /** Block id of each voxel, same order. */
  blocks: Uint8Array;
}

export interface IslandSearchResult {
  islands: Island[];
  /** Voxels visited by all searches (a work counter for stats). */
  visited: number;
}

/**
 * @param seeds packed indices of voxels to check (air or duplicate seeds are fine)
 * @param maxVisit per-search cap; a larger component is assumed supported
 */
export function findIslands(world: VoxelWorld, seeds: Iterable<number>, maxVisit: number): IslandSearchResult {
  const owner = new Map<number, number>(); // voxel -> search id
  const supported = new Set<number>();
  const islands: Island[] = [];
  const { nx, ny, nz } = world;
  const plane = nx * ny;
  let searchId = 0;
  let visited = 0;
  // Bucket queue keyed by z: pop order = lowest z first.
  const buckets: number[][] = [];
  for (let z = 0; z < nz; z++) buckets.push([]);

  for (const seed of seeds) {
    if (owner.has(seed)) continue;
    const sx = seed % nx;
    const sy = Math.floor(seed / nx) % ny;
    const sz = Math.floor(seed / plane);
    if (world.get(sx, sy, sz) === AIR) continue;

    const id = ++searchId;
    const members: number[] = [seed];
    owner.set(seed, id);
    buckets[sz]!.push(seed);
    let minZ = sz;
    let pending = 1;
    let isSupported = false;

    search: while (pending > 0) {
      while (buckets[minZ]!.length === 0) minZ++;
      const i = buckets[minZ]!.pop()!;
      pending--;
      const x = i % nx;
      const y = Math.floor(i / nx) % ny;
      const z = Math.floor(i / plane);
      if (z === 0 || isAnchored(world.get(x, y, z))) {
        isSupported = true;
        break;
      }
      for (let k = 0; k < 6; k++) {
        let jx = x;
        let jy = y;
        let jz = z;
        if (k === 0) jz--;
        else if (k === 1) jx--;
        else if (k === 2) jx++;
        else if (k === 3) jy--;
        else if (k === 4) jy++;
        else jz++;
        if (jx < 0 || jy < 0 || jz < 0 || jx >= nx || jy >= ny || jz >= nz) continue;
        if (world.get(jx, jy, jz) === AIR) continue;
        const j = jx + nx * (jy + ny * jz);
        const o = owner.get(j);
        if (o === undefined) {
          owner.set(j, id);
          members.push(j);
          buckets[jz]!.push(j);
          if (jz < minZ) minZ = jz;
          pending++;
        } else if (o !== id) {
          // Earlier searches either proved support or captured a closed island; a closed
          // island cannot be reached from outside, so this component is supported.
          isSupported = supported.has(o) || isSupported;
          if (isSupported) break search;
        }
      }
      if (members.length > maxVisit) {
        isSupported = true;
        break;
      }
    }
    // Drain leftovers so the buckets are empty for the next seed.
    if (pending > 0) for (let z = minZ; z < nz; z++) buckets[z]!.length = 0;
    visited += members.length;

    if (isSupported) {
      supported.add(id);
    } else {
      const voxels = Int32Array.from(members);
      const blocks = new Uint8Array(voxels.length);
      for (let n = 0; n < voxels.length; n++) {
        const v = voxels[n]!;
        blocks[n] = world.get(v % nx, Math.floor(v / nx) % ny, Math.floor(v / plane));
      }
      islands.push({ voxels, blocks });
    }
  }
  return { islands, visited };
}
