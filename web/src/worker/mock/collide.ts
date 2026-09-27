/**
 * Axis-separated AABB sweep against the voxel grid, with optional step-up.
 *
 * Each axis move scans every voxel layer the leading face would enter, so there is no
 * tunnelling at any speed. Voxels the box already overlaps are ignored: a box that ends
 * up inside geometry (e.g. after a remesh) can always move out, never further in.
 */
import type { Vec3 } from '../../engine/protocol.ts';
import { AIR, type VoxelWorld } from './world.ts';

/** Tolerance for "exactly on a voxel boundary", in voxels. */
const EPS = 1e-6;
/** Gap kept between the box and blocking faces, in voxels. */
const SKIN = 1e-3;
/** Longest move handled per call, in voxels (bounds the work of a bogus request). */
const MAX_MOVE = 256;

export interface CollideOutcome {
  move: Vec3;
  onGround: boolean;
}

interface Box {
  min: number[];
  max: number[];
}

function layerBlocked(world: VoxelWorld, box: Box, axis: number, layer: number): boolean {
  const b = (axis + 1) % 3;
  const c = (axis + 2) % 3;
  const b0 = Math.floor(box.min[b]! + EPS);
  const b1 = Math.ceil(box.max[b]! - EPS) - 1;
  const c0 = Math.floor(box.min[c]! + EPS);
  const c1 = Math.ceil(box.max[c]! - EPS) - 1;
  const p = [0, 0, 0];
  p[axis] = layer;
  for (let j = c0; j <= c1; j++) {
    p[c] = j;
    for (let i = b0; i <= b1; i++) {
      p[b] = i;
      if (world.get(p[0]!, p[1]!, p[2]!) !== AIR) return true;
    }
  }
  return false;
}

/** Moves `box` along `axis` by up to `delta` voxels; returns the distance actually moved. */
function sweepAxis(world: VoxelWorld, box: Box, axis: number, delta: number): number {
  if (delta === 0) return 0;
  let allowed = delta;
  if (delta > 0) {
    const first = Math.ceil(box.max[axis]! - EPS);
    const last = Math.ceil(box.max[axis]! + delta) - 1;
    for (let layer = first; layer <= last; layer++) {
      if (layerBlocked(world, box, axis, layer)) {
        allowed = Math.max(0, Math.min(delta, layer - box.max[axis]! - SKIN));
        break;
      }
    }
  } else {
    const first = Math.floor(box.min[axis]! + EPS) - 1;
    const last = Math.floor(box.min[axis]! + delta);
    for (let layer = first; layer >= last; layer--) {
      if (layerBlocked(world, box, axis, layer)) {
        allowed = Math.min(0, Math.max(delta, layer + 1 - box.min[axis]! + SKIN));
        break;
      }
    }
  }
  box.min[axis] = box.min[axis]! + allowed;
  box.max[axis] = box.max[axis]! + allowed;
  return allowed;
}

function clampMove(x: number): number {
  return Math.max(-MAX_MOVE, Math.min(MAX_MOVE, Number.isFinite(x) ? x : 0));
}

/** True when a solid lies within a hair below the box. */
function grounded(world: VoxelWorld, box: Box): boolean {
  const probe: Box = { min: box.min.slice(), max: box.max.slice() };
  return sweepAxis(world, probe, 2, -0.05) > -0.05 + 1e-9;
}

/**
 * Sweeps the world-space box [min,max] by `move` (metres): z first, then x, then y, each
 * clamped at the first blocking voxel layer. `onGround` is true when the box ends up
 * resting on (or landed on) a solid. Stepping up ledges is the caller's job (it issues
 * further sweeps: up, across, down), so this stays exactly the docs/API.md contract.
 */
export function collideAabb(world: VoxelWorld, min: Vec3, max: Vec3, move: Vec3): CollideOutcome {
  const h = world.h;
  const g0 = world.toGrid(min);
  const g1 = world.toGrid(max);
  const start: Box = { min: [g0[0], g0[1], g0[2]], max: [g1[0], g1[1], g1[2]] };
  const m = [clampMove(move[0] / h), clampMove(move[1] / h), clampMove(move[2] / h)];

  const box: Box = { min: start.min.slice(), max: start.max.slice() };
  const dz = sweepAxis(world, box, 2, m[2]!);
  const landed = m[2]! < 0 && dz > m[2]! + 1e-9;
  sweepAxis(world, box, 0, m[0]!);
  sweepAxis(world, box, 1, m[1]!);

  const onGround = landed || grounded(world, box);
  return {
    move: [(box.min[0]! - start.min[0]!) * h, (box.min[1]! - start.min[1]!) * h, (box.min[2]! - start.min[2]!) * h],
    onGround,
  };
}
