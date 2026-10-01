/**
 * Drawing the angled world's parts in their own lattices (parts mode,
 * world.angles.partsMode "separate"): what a renderer adopting structvox's
 * per-part meshes does (ANGLED_WORLD_PLAN.md §6). A part's content comes
 * from world/partRaster.js; its placement maps rays and points into its
 * lattice exactly (Mᵀ / D, a rotation's inverse).
 */
import { partChunk } from "../../src/engine/world/partRaster.js";
import { partsIn } from "../../src/engine/world/parts.js";
import { P, P2 } from "../../src/engine/voxel/chunk.js";

export { partsIn };

/** Material of a part's local cell (u, v, w) (0: air). */
export function partVoxel(world, part, u, v, w) {
  const e = part.extent;
  if (u < e.u0 || u > e.u1 || v < e.v0 || v > e.v1 || w < e.w0 || w > e.w1) return 0;
  const cx = Math.floor(u / 32);
  const cy = Math.floor(v / 32);
  const cz = Math.floor(w / 32);
  return partChunk(world, part, 0, cx, cy, cz).data[u - cx * 32 + 1 + (v - cy * 32 + 1) * P + (w - cz * 32 + 1) * P2];
}

/** A world point (continuous) in a part's lattice (continuous). */
export function toLocal(part, x, y, z) {
  const { m, d, origin } = part.placement;
  const px = x - origin.x;
  const py = y - origin.y;
  const pz = z - origin.z;
  return [(m[0] * px + m[3] * py + m[6] * pz) / d, (m[1] * px + m[4] * py + m[7] * pz) / d, (m[2] * px + m[5] * py + m[8] * pz) / d];
}

/** Material of the parts at a world point (continuous; the first part holding it), or 0. */
export function partsAt(world, parts, x, y, z) {
  for (const p of parts) {
    const b = p.aabb;
    if (x < b.x0 - 1 || x > b.x1 + 2 || y < b.y0 - 1 || y > b.y1 + 2 || z < b.z0 - 1 || z > b.z1 + 2) continue;
    const [u, v, w] = toLocal(p, x, y, z);
    const m = partVoxel(world, p, Math.floor(u), Math.floor(v), Math.floor(w));
    if (m) return m;
  }
  return 0;
}

/**
 * The first solid cell of a part along a ray o + t dir (world, dir a unit
 * vector) with t < tMax: { t, m, n (world normal of the face it enters) }
 * or null. A DDA through the part's own lattice.
 */
export function partHit(world, part, o, dir, tMax) {
  const { m, d } = part.placement;
  const e = part.extent;
  const lo = toLocal(part, o[0], o[1], o[2]);
  const ld = [(m[0] * dir[0] + m[3] * dir[1] + m[6] * dir[2]) / d, (m[1] * dir[0] + m[4] * dir[1] + m[7] * dir[2]) / d, (m[2] * dir[0] + m[5] * dir[1] + m[8] * dir[2]) / d];
  const lo3 = [e.u0, e.v0, e.w0];
  const hi3 = [e.u1 + 1, e.v1 + 1, e.w1 + 1];
  let t0 = 0;
  let t1 = tMax;
  for (let a = 0; a < 3; a += 1) {
    if (Math.abs(ld[a]) < 1e-12) {
      if (lo[a] < lo3[a] || lo[a] > hi3[a]) return null;
      continue;
    }
    let ta = (lo3[a] - lo[a]) / ld[a];
    let tb = (hi3[a] - lo[a]) / ld[a];
    if (ta > tb) [ta, tb] = [tb, ta];
    t0 = Math.max(t0, ta);
    t1 = Math.min(t1, tb);
    if (t0 > t1) return null;
  }
  const p = lo.map((c, a) => c + ld[a] * (t0 + 1e-7));
  const cell = p.map((c, a) => Math.min(hi3[a] - 1, Math.max(lo3[a], Math.floor(c))));
  const step = ld.map((c) => (c > 0 ? 1 : -1));
  const td = ld.map((c) => (Math.abs(c) < 1e-12 ? Infinity : Math.abs(1 / c)));
  const tm = ld.map((c, a) => (Math.abs(c) < 1e-12 ? Infinity : t0 + (c > 0 ? cell[a] + 1 - p[a] : p[a] - cell[a]) * td[a]));
  let face = ld.map(Math.abs).indexOf(Math.max(...ld.map(Math.abs)));
  let t = t0;
  for (let n = 0; n < 4096 && t <= t1; n += 1) {
    const mat = partVoxel(world, part, cell[0], cell[1], cell[2]);
    if (mat) {
      const nl = [0, 0, 0];
      nl[face] = -step[face];
      return { t, m: mat, n: [(m[0] * nl[0] + m[1] * nl[1] + m[2] * nl[2]) / d, (m[3] * nl[0] + m[4] * nl[1] + m[5] * nl[2]) / d, (m[6] * nl[0] + m[7] * nl[1] + m[8] * nl[2]) / d] };
    }
    const a = tm[0] < tm[1] ? (tm[0] < tm[2] ? 0 : 2) : tm[1] < tm[2] ? 1 : 2;
    t = tm[a];
    cell[a] += step[a];
    tm[a] += td[a];
    face = a;
    if (cell[a] < lo3[a] || cell[a] >= hi3[a]) return null;
  }
  return null;
}
