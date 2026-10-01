/**
 * Parts in the viewer (the angled world in parts mode): a part's voxels live
 * in its own lattice, placed by its exact integer matrix (core/placement.js:
 * world = origin + M local / D). What the walker's collision asks of a part,
 * without THREE: which of its cells a world voxel's centre falls in (the
 * placement's exact integer inverse, as Placement.toLocal) and whether that
 * cell is solid (its chunks' bitsets, bit i + 32 j + 1024 k as the world
 * grid's). Plain arithmetic, so node tests it (test/viewerParts.test.js).
 */

const floorDiv = (a, b) => Math.floor(a / b);

/** The local cell [u, v, w] of a part holding world voxel (x, y, z)'s centre. */
export function partLocal(part, x, y, z) {
  const { m, d, origin } = part;
  const dx = 2 * (x - origin.x) + 1;
  const dy = 2 * (y - origin.y) + 1;
  const dz = 2 * (z - origin.z) + 1;
  const d2 = 2 * d;
  return [floorDiv(m[0] * dx + m[3] * dy + m[6] * dz, d2), floorDiv(m[1] * dx + m[4] * dy + m[7] * dz, d2), floorDiv(m[2] * dx + m[5] * dy + m[8] * dz, d2)];
}

/**
 * Bit `layer` ("solid" or "climb") of a part at world voxel (x, y, z):
 * true / false, or null outside its box. `part.bits[layer]` maps "cx,cy,cz"
 * of its lattice chunks to bitsets.
 */
export function partBit(part, layer, x, y, z) {
  const b = part.aabb;
  if (x < b.x0 || x > b.x1 || y < b.y0 || y > b.y1 || z < b.z0 || z > b.z1) return null;
  const [u, v, w] = partLocal(part, x, y, z);
  const e = part.extent;
  if (u < e.u0 || u > e.u1 || v < e.v0 || v > e.v1 || w < e.w0 || w > e.w1) return false;
  const cx = Math.floor(u / 32);
  const cy = Math.floor(v / 32);
  const cz = Math.floor(w / 32);
  const bits = part.bits?.[layer]?.get(`${cx},${cy},${cz}`);
  if (!bits) return false;
  const n = u - cx * 32 + (v - cy * 32) * 32 + (w - cz * 32) * 1024;
  return (bits[n >>> 5] & (1 << (n & 31))) !== 0;
}

/**
 * The 4 x 4 model matrix (row-major, metres) of a part's lattice: its local
 * point p (in voxels) at origin + M p / D, times the voxel size.
 */
export function partMatrix(part, voxel) {
  const { m, d, origin } = part;
  return [m[0] / d, m[1] / d, m[2] / d, origin.x * voxel, m[3] / d, m[4] / d, m[5] / d, origin.y * voxel, m[6] / d, m[7] / d, m[8] / d, origin.z * voxel, 0, 0, 0, 1];
}
