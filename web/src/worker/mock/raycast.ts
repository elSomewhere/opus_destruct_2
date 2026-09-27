/**
 * Voxel ray traversal (Amanatides & Woo 1987) against the block grid.
 */
import type { RaycastHit, Vec3 } from '../../engine/protocol.ts';
import { blockMaterial } from './blocks.ts';
import { AIR, type VoxelWorld } from './world.ts';

/** Solid voxel hit in grid terms (used by the engine for carving and detaching). */
export interface VoxelHit {
  /** Grid coordinates of the hit voxel. */
  voxel: Vec3;
  block: number;
  /** Distance along the ray in metres. */
  distance: number;
  /** Outward normal of the entered face (zero when the origin starts inside a solid). */
  normal: Vec3;
}

export function traceVoxels(world: VoxelWorld, origin: Vec3, dir: Vec3, maxDist: number): VoxelHit | null {
  const len = Math.hypot(dir[0], dir[1], dir[2]);
  if (!(len > 0) || !(maxDist > 0)) return null;
  const d = [dir[0] / len, dir[1] / len, dir[2] / len];
  const o = world.toGrid(origin);
  const h = world.h;
  const maxT = maxDist / h; // grid units (d is unit length)

  // Clip to the grid box. One layer below z = 0 is included: world.get reports the
  // bedrock under the grid there, so downward rays stop on it.
  const lo = [0, 0, -1];
  const hi = [world.nx, world.ny, world.nz];
  let tEnter = 0;
  let tExit = maxT;
  let enterAxis = -1;
  for (let a = 0; a < 3; a++) {
    const da = d[a]!;
    const oa = o[a]!;
    if (Math.abs(da) < 1e-12) {
      if (oa < lo[a]! || oa > hi[a]!) return null;
      continue;
    }
    let t0 = (lo[a]! - oa) / da;
    let t1 = (hi[a]! - oa) / da;
    if (t0 > t1) [t0, t1] = [t1, t0];
    if (t0 > tEnter) {
      tEnter = t0;
      enterAxis = a;
    }
    if (t1 < tExit) tExit = t1;
    if (tEnter > tExit) return null;
  }

  const p = [o[0] + d[0]! * tEnter, o[1] + d[1]! * tEnter, o[2] + d[2]! * tEnter];
  const vox = [0, 0, 0];
  const step = [0, 0, 0];
  const tMax = [Infinity, Infinity, Infinity];
  const tDelta = [Infinity, Infinity, Infinity];
  for (let a = 0; a < 3; a++) {
    const da = d[a]!;
    // Entering through a face lands exactly on a boundary; nudge into the entered cell.
    const pa = a === enterAxis ? p[a]! + Math.sign(da) * 1e-9 : p[a]!;
    vox[a] = Math.min(Math.max(Math.floor(pa), lo[a]!), hi[a]! - 1);
    step[a] = da > 0 ? 1 : da < 0 ? -1 : 0;
    if (step[a] !== 0) {
      const boundary = vox[a]! + (step[a]! > 0 ? 1 : 0);
      tMax[a] = tEnter + (boundary - p[a]!) / da;
      tDelta[a] = Math.abs(1 / da);
    }
  }

  const normal: Vec3 = [0, 0, 0];
  if (enterAxis >= 0) {
    normal[enterAxis] = -step[enterAxis]!;
  } else {
    // Origin inside the grid: if it starts in a solid, report the face facing the ray.
    let a = 0;
    if (Math.abs(d[1]!) > Math.abs(d[a]!)) a = 1;
    if (Math.abs(d[2]!) > Math.abs(d[a]!)) a = 2;
    normal[a] = -step[a]!;
  }
  let t = tEnter;
  for (let guard = 0; guard < 1 << 16; guard++) {
    const block = world.get(vox[0]!, vox[1]!, vox[2]!);
    if (block !== AIR) {
      return { voxel: [vox[0]!, vox[1]!, vox[2]!], block, distance: t * h, normal };
    }
    let a = 0;
    if (tMax[1]! < tMax[a]!) a = 1;
    if (tMax[2]! < tMax[a]!) a = 2;
    t = tMax[a]!;
    if (t > tExit) return null;
    vox[a] = vox[a]! + step[a]!;
    tMax[a] = tMax[a]! + tDelta[a]!;
    normal[0] = 0;
    normal[1] = 0;
    normal[2] = 0;
    normal[a] = -step[a]!;
  }
  return null;
}

/** Protocol-level raycast. */
export function raycast(world: VoxelWorld, origin: Vec3, dir: Vec3, maxDist: number): RaycastHit | null {
  const hit = traceVoxels(world, origin, dir, maxDist);
  if (!hit) return null;
  const len = Math.hypot(dir[0], dir[1], dir[2]);
  const k = hit.distance / len;
  return {
    pos: [origin[0] + dir[0] * k, origin[1] + dir[1] * k, origin[2] + dir[2] * k],
    normal: hit.normal,
    distance: hit.distance,
    material: blockMaterial(hit.block),
  };
}
