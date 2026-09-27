/**
 * Voxel damage of character models: rays against posed models (bullets), carving voxels out
 * (wounds, gore), and severing parts that lost their connection to the joint they hang from
 * (limbs shot off, gibs).
 *
 * Posed models are given by their skin matrices (WorldPose.writeSkin: 16 floats per bone,
 * column-major, rigid, mapping rest model-space points to world). Rays are transformed into
 * each part's rest space with the inverse of its bone's matrix and walked through the part's
 * cells (3D DDA). Carving and severing work in rest space, on the model's own parts: damage a
 * character's clone of a shared model (VoxelModel.clone), never the shared one.
 */
import type { V3 } from '../math/vec.ts';
import { shrinkPart, type VoxelModel, type VoxelPart } from './model.ts';

export interface CharacterHit {
  /** Index into model.parts. */
  part: number;
  /** The part's bone. */
  bone: number;
  /** Lattice coordinates of the voxel hit. */
  cell: [number, number, number];
  /** World hit point (on the voxel face). */
  point: V3;
  /** World face normal. */
  normal: V3;
  /** Hit point in rest model space. */
  restPoint: V3;
  /** Distance along the ray. */
  distance: number;
  /** Palette slot of the voxel hit. */
  slot: number;
}

export interface RemovedVoxel {
  bone: number;
  /** Centre of the removed voxel in rest model space. */
  rest: V3;
  slot: number;
  shade: number;
}

// scratch for raycastModel (no allocation per part)
const ro: V3 = [0, 0, 0];
const rd: V3 = [0, 0, 0];

/**
 * Nearest voxel hit along the ray (unit `dir`) over all parts of the posed model, or null.
 * Each part is culled by its bounding sphere, then by a slab test in its rest space, then walked
 * cell by cell.
 */
export function raycastModel(model: VoxelModel, skin: Float32Array, origin: Readonly<V3>, dir: Readonly<V3>, maxDist: number): CharacterHit | null {
  const s = model.voxelSize;
  const sk = model.skeleton;
  let best = maxDist;
  let hitPart = -1;
  let hx = 0, hy = 0, hz = 0; // cell (local indices)
  let hAxis = 0, hSign = 0;
  for (let pi = 0; pi < model.parts.length; pi++) {
    const p = model.parts[pi]!;
    if (p.count === 0) continue;
    const o = p.bone * 16;
    if (o + 16 > skin.length || p.bone >= sk.count) continue;
    const [nx, ny, nz] = p.dims;
    // rest-space box
    const bx0 = p.origin[0] * s, by0 = p.origin[1] * s, bz0 = p.origin[2] * s;
    const bx1 = bx0 + nx * s, by1 = by0 + ny * s, bz1 = bz0 + nz * s;
    // bounding sphere in world space
    const cx = (bx0 + bx1) / 2, cy = (by0 + by1) / 2, cz = (bz0 + bz1) / 2;
    const wx = skin[o]! * cx + skin[o + 4]! * cy + skin[o + 8]! * cz + skin[o + 12]!;
    const wy = skin[o + 1]! * cx + skin[o + 5]! * cy + skin[o + 9]! * cz + skin[o + 13]!;
    const wz = skin[o + 2]! * cx + skin[o + 6]! * cy + skin[o + 10]! * cz + skin[o + 14]!;
    const r = 0.5 * Math.sqrt((bx1 - bx0) ** 2 + (by1 - by0) ** 2 + (bz1 - bz0) ** 2);
    const ex = wx - origin[0], ey = wy - origin[1], ez = wz - origin[2];
    const tc = ex * dir[0] + ey * dir[1] + ez * dir[2];
    if (tc < -r || tc - r > best) continue;
    const perp2 = ex * ex + ey * ey + ez * ez - tc * tc;
    if (perp2 > r * r) continue;
    // ray into rest space: x_rest = R^T (x_world - T)
    const tx = origin[0] - skin[o + 12]!, ty = origin[1] - skin[o + 13]!, tz = origin[2] - skin[o + 14]!;
    ro[0] = skin[o]! * tx + skin[o + 1]! * ty + skin[o + 2]! * tz;
    ro[1] = skin[o + 4]! * tx + skin[o + 5]! * ty + skin[o + 6]! * tz;
    ro[2] = skin[o + 8]! * tx + skin[o + 9]! * ty + skin[o + 10]! * tz;
    rd[0] = skin[o]! * dir[0] + skin[o + 1]! * dir[1] + skin[o + 2]! * dir[2];
    rd[1] = skin[o + 4]! * dir[0] + skin[o + 5]! * dir[1] + skin[o + 6]! * dir[2];
    rd[2] = skin[o + 8]! * dir[0] + skin[o + 9]! * dir[1] + skin[o + 10]! * dir[2];
    // slab test
    let t0 = 0, t1 = best;
    let entryAxis = -1;
    const lo = [bx0, by0, bz0], hi = [bx1, by1, bz1];
    let miss = false;
    for (let a = 0; a < 3; a++) {
      const d = rd[a]!;
      const oa = ro[a]!;
      if (Math.abs(d) < 1e-12) {
        if (oa < lo[a]! || oa > hi[a]!) {
          miss = true;
          break;
        }
        continue;
      }
      let ta = (lo[a]! - oa) / d, tb = (hi[a]! - oa) / d;
      if (ta > tb) {
        const x = ta;
        ta = tb;
        tb = x;
      }
      if (ta > t0) {
        t0 = ta;
        entryAxis = a;
      }
      if (tb < t1) t1 = tb;
      if (t0 > t1) {
        miss = true;
        break;
      }
    }
    if (miss) continue;
    // DDA in local cell units
    const px = (ro[0] + rd[0] * t0) / s - p.origin[0];
    const py = (ro[1] + rd[1] * t0) / s - p.origin[1];
    const pz = (ro[2] + rd[2] * t0) / s - p.origin[2];
    let ix = Math.min(nx - 1, Math.max(0, Math.floor(px)));
    let iy = Math.min(ny - 1, Math.max(0, Math.floor(py)));
    let iz = Math.min(nz - 1, Math.max(0, Math.floor(pz)));
    const sx = rd[0] > 0 ? 1 : -1, sy = rd[1] > 0 ? 1 : -1, sz = rd[2] > 0 ? 1 : -1;
    const dtx = rd[0] !== 0 ? s / Math.abs(rd[0]) : Infinity;
    const dty = rd[1] !== 0 ? s / Math.abs(rd[1]) : Infinity;
    const dtz = rd[2] !== 0 ? s / Math.abs(rd[2]) : Infinity;
    // parameter of the next cell boundary on each axis
    let ntx = rd[0] !== 0 ? t0 + ((sx > 0 ? ix + 1 - px : px - ix) * s) / Math.abs(rd[0]) : Infinity;
    let nty = rd[1] !== 0 ? t0 + ((sy > 0 ? iy + 1 - py : py - iy) * s) / Math.abs(rd[1]) : Infinity;
    let ntz = rd[2] !== 0 ? t0 + ((sz > 0 ? iz + 1 - pz : pz - iz) * s) / Math.abs(rd[2]) : Infinity;
    let t = t0;
    let axis = entryAxis;
    const cells = p.cells;
    for (;;) {
      if (cells[ix + nx * (iy + ny * iz)] !== 0) {
        // (t <= t1 <= best: nearer than any earlier part's hit)
        best = t;
        hitPart = pi;
        hx = ix;
        hy = iy;
        hz = iz;
        if (axis < 0) {
          // the ray starts inside a solid cell: report the face it looks at
          const ax = Math.abs(rd[0]), ay = Math.abs(rd[1]), az = Math.abs(rd[2]);
          axis = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
        }
        hAxis = axis;
        hSign = -(axis === 0 ? sx : axis === 1 ? sy : sz);
        break;
      }
      if (ntx < nty && ntx < ntz) {
        t = ntx;
        ix += sx;
        ntx += dtx;
        axis = 0;
        if (ix < 0 || ix >= nx) break;
      } else if (nty < ntz) {
        t = nty;
        iy += sy;
        nty += dty;
        axis = 1;
        if (iy < 0 || iy >= ny) break;
      } else {
        t = ntz;
        iz += sz;
        ntz += dtz;
        axis = 2;
        if (iz < 0 || iz >= nz) break;
      }
      if (t > t1) break;
    }
  }
  if (hitPart < 0) return null;
  const p = model.parts[hitPart]!;
  const o = p.bone * 16;
  // recompute the rest-space ray of the winning part
  const tx = origin[0] - skin[o + 12]!, ty = origin[1] - skin[o + 13]!, tz = origin[2] - skin[o + 14]!;
  const orx = skin[o]! * tx + skin[o + 1]! * ty + skin[o + 2]! * tz;
  const ory = skin[o + 4]! * tx + skin[o + 5]! * ty + skin[o + 6]! * tz;
  const orz = skin[o + 8]! * tx + skin[o + 9]! * ty + skin[o + 10]! * tz;
  const drx = skin[o]! * dir[0] + skin[o + 1]! * dir[1] + skin[o + 2]! * dir[2];
  const dry = skin[o + 4]! * dir[0] + skin[o + 5]! * dir[1] + skin[o + 6]! * dir[2];
  const drz = skin[o + 8]! * dir[0] + skin[o + 9]! * dir[1] + skin[o + 10]! * dir[2];
  // world normal = R (the rest face normal: axis hAxis, sign hSign)
  const col = hAxis * 4;
  const normal: V3 = [skin[o + col]! * hSign, skin[o + col + 1]! * hSign, skin[o + col + 2]! * hSign];
  return {
    part: hitPart,
    bone: p.bone,
    cell: [p.origin[0] + hx, p.origin[1] + hy, p.origin[2] + hz],
    point: [origin[0] + dir[0] * best, origin[1] + dir[1] * best, origin[2] + dir[2] * best],
    normal,
    restPoint: [orx + drx * best, ory + dry * best, orz + drz * best],
    distance: best,
    slot: p.cells[hx + p.dims[0] * (hy + p.dims[1] * hz)]! - 1,
  };
}

/** Squared distance from point c to the rest-space box of a part. */
function boxDist2(p: VoxelPart, s: number, c: Readonly<V3>): number {
  let d2 = 0;
  for (let a = 0; a < 3; a++) {
    const lo = p.origin[a]! * s;
    const hi = (p.origin[a]! + p.dims[a]!) * s;
    const v = c[a]!;
    const e = v < lo ? lo - v : v > hi ? v - hi : 0;
    d2 += e * e;
  }
  return d2;
}

/**
 * Removes every solid cell within `radius` (m) of `centerRest` (rest model space) from the
 * given parts (default: every part in range: joint balls are copied into several parts, so a
 * wound at a joint opens all of them). Bumps each changed part's `version` and updates its
 * `count`; the removed voxels are appended to `out` (a voxel shared by several parts is
 * reported once). Deterministic: parts in order, cells z-major.
 */
export function carveModel(model: VoxelModel, centerRest: Readonly<V3>, radius: number, parts?: readonly number[], out: RemovedVoxel[] = []): RemovedVoxel[] {
  const s = model.voxelSize;
  const r2 = radius * radius;
  const list = parts ?? model.parts.map((_, i) => i);
  const seen = new Set<number>();
  const i0 = Math.floor((centerRest[0] - radius) / s), i1 = Math.floor((centerRest[0] + radius) / s);
  const j0 = Math.floor((centerRest[1] - radius) / s), j1 = Math.floor((centerRest[1] + radius) / s);
  const k0 = Math.floor((centerRest[2] - radius) / s), k1 = Math.floor((centerRest[2] + radius) / s);
  for (const pi of list) {
    const p = model.parts[pi];
    if (!p || p.count === 0 || boxDist2(p, s, centerRest) > r2) continue;
    const [nx, ny, nz] = p.dims;
    const [ox, oy, oz] = p.origin;
    let removed = 0;
    for (let k = Math.max(k0, oz); k <= Math.min(k1, oz + nz - 1); k++) {
      const dz = (k + 0.5) * s - centerRest[2];
      for (let j = Math.max(j0, oy); j <= Math.min(j1, oy + ny - 1); j++) {
        const dy = (j + 0.5) * s - centerRest[1];
        for (let i = Math.max(i0, ox); i <= Math.min(i1, ox + nx - 1); i++) {
          const dx = (i + 0.5) * s - centerRest[0];
          if (dx * dx + dy * dy + dz * dz > r2) continue;
          const idx = i - ox + nx * (j - oy + ny * (k - oz));
          const c = p.cells[idx]!;
          if (c === 0) continue;
          p.cells[idx] = 0;
          removed++;
          // lattice key (exact for |i|, |j|, |k| < 2^15)
          const key = ((i + 32768) * 65536 + (j + 32768)) * 65536 + (k + 32768);
          if (!seen.has(key)) {
            seen.add(key);
            out.push({ bone: p.bone, rest: [(i + 0.5) * s, (j + 0.5) * s, (k + 0.5) * s], slot: c - 1, shade: p.shade[idx]! });
          }
        }
      }
    }
    if (removed > 0) {
      p.count -= removed;
      p.version++;
    }
  }
  return out;
}

/** Remaining fraction of a part's voxels (count / initialCount), 0..1. */
export function partIntegrity(part: VoxelPart): number {
  return part.initialCount > 0 ? Math.max(0, Math.min(1, part.count / part.initialCount)) : 0;
}

/** A new part of `src`'s box holding only the listed cells (shrunk, version 0). */
function pieceOf(src: VoxelPart, cells: readonly number[]): VoxelPart {
  const n = src.cells.length;
  const full: VoxelPart = {
    bone: src.bone,
    origin: [...src.origin],
    dims: [...src.dims],
    cells: new Uint8Array(n),
    shade: new Uint8Array(n),
    count: cells.length,
    initialCount: cells.length,
    version: 0,
  };
  for (const idx of cells) {
    full.cells[idx] = src.cells[idx]!;
    full.shade[idx] = src.shade[idx]!;
  }
  const out = shrinkPart(full);
  out.initialCount = out.count;
  out.version = 0;
  return out;
}

/**
 * Severing: the cells of part `part` that are no longer 6-connected to its anchor (the cells
 * within `anchorRadius` of its bone's rest head, where it hangs from its parent) are split off,
 * removed from the part and returned as new parts (same bone, shrunk to their bounds,
 * version 0), one per connected component, largest first. With no anchor cell left the whole
 * part comes off as one piece (the limb is cut at the joint). The root and the bones directly
 * under it (the pelvis: the body itself) never sever. Returns [] if nothing came off.
 */
export function severDisconnected(model: VoxelModel, part: number, anchorRadius: number): VoxelPart[] {
  const p = model.parts[part];
  if (!p || p.count === 0) return [];
  const sk = model.skeleton;
  const par = sk.parents[p.bone]!;
  if (par < 0 || sk.parents[par]! < 0) return [];
  const s = model.voxelSize;
  const [nx, ny, nz] = p.dims;
  const [ox, oy, oz] = p.origin;
  const head = sk.restHead[p.bone]!;
  const ar2 = anchorRadius * anchorRadius;
  const n = p.cells.length;
  const mark = new Uint8Array(n); // 1 = reached from the anchor
  const queue = new Int32Array(n);
  let qh = 0, qt = 0;
  for (let k = 0; k < nz; k++)
    for (let j = 0; j < ny; j++)
      for (let i = 0; i < nx; i++) {
        const idx = i + nx * (j + ny * k);
        if (p.cells[idx] === 0) continue;
        const dx = (ox + i + 0.5) * s - head[0], dy = (oy + j + 0.5) * s - head[1], dz = (oz + k + 0.5) * s - head[2];
        if (dx * dx + dy * dy + dz * dz <= ar2) {
          mark[idx] = 1;
          queue[qt++] = idx;
        }
      }
  if (qt === 0) {
    // cut at the joint: everything comes off
    const all: number[] = [];
    for (let idx = 0; idx < n; idx++) if (p.cells[idx] !== 0) all.push(idx);
    const piece = pieceOf(p, all);
    p.cells.fill(0);
    p.count = 0;
    p.version++;
    return [piece];
  }
  const flood = (label: number): number => {
    let reached = 0;
    while (qh < qt) {
      const idx = queue[qh++]!;
      reached++;
      const i = idx % nx, j = Math.floor(idx / nx) % ny, k = Math.floor(idx / (nx * ny));
      if (i > 0) visit(idx - 1, label);
      if (i < nx - 1) visit(idx + 1, label);
      if (j > 0) visit(idx - nx, label);
      if (j < ny - 1) visit(idx + nx, label);
      if (k > 0) visit(idx - nx * ny, label);
      if (k < nz - 1) visit(idx + nx * ny, label);
    }
    return reached;
  };
  const visit = (m: number, label: number): void => {
    if (mark[m] !== 0 || p.cells[m] === 0) return;
    mark[m] = label;
    queue[qt++] = m;
  };
  flood(1);
  // the rest: connected components, each a piece
  const pieces: number[][] = [];
  for (let idx = 0; idx < n; idx++) {
    if (p.cells[idx] === 0 || mark[idx] !== 0) continue;
    qh = 0;
    qt = 0;
    mark[idx] = 2;
    queue[qt++] = idx;
    flood(2);
    pieces.push(Array.from(queue.subarray(0, qt)));
  }
  if (pieces.length === 0) return [];
  pieces.sort((a, b) => b.length - a.length);
  const out = pieces.map((cells) => pieceOf(p, cells));
  let removed = 0;
  for (const cells of pieces) {
    for (const idx of cells) {
      p.cells[idx] = 0;
      p.shade[idx] = 0;
    }
    removed += cells.length;
  }
  p.count -= removed;
  p.version++;
  return out;
}

/**
 * Removes the part of `bone` (and, with `includeChildren`, the parts of every bone below it)
 * from the model and returns copies of them (a limb cut off with what hangs from it). Emptied
 * parts keep their place in model.parts with count 0 and a bumped version.
 */
export function detachSubtree(model: VoxelModel, bone: number, includeChildren: boolean): VoxelPart[] {
  const sk = model.skeleton;
  const out: VoxelPart[] = [];
  for (let b = 0; b < sk.count; b++) {
    if (b !== bone && !(includeChildren && sk.isBelow(b, bone))) continue;
    const pi = model.partOfBone[b]!;
    if (pi < 0) continue;
    const p = model.parts[pi]!;
    if (p.count === 0) continue;
    const all: number[] = [];
    for (let idx = 0; idx < p.cells.length; idx++) if (p.cells[idx] !== 0) all.push(idx);
    out.push(pieceOf(p, all));
    p.cells.fill(0);
    p.shade.fill(0);
    p.count = 0;
    p.version++;
  }
  return out;
}
