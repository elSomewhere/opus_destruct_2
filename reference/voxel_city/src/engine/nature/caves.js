import { SimplexNoise } from "../core/noise.js";
import { deriveSeed, hash32 } from "../core/hash.js";
import { MAT, IS_SOLID } from "../voxel/materials.js";
import { TREELINE } from "./landcover.js";
import { P, P2 } from "../voxel/chunk.js";
import { VOXEL_SIZE } from "../core/units.js";

/**
 * Natural caves under open country:
 *
 *   worm tunnels  where two 3D noise fields are both near zero (the
 *                 intersection of two isosurfaces is a winding tube); where
 *                 a tube meets the surface it opens as a cave mouth
 *   caverns       deeper chambers where a low-frequency 3D noise is high
 *   decoration    gravel / moss floors, stalactites and stalagmites, rare
 *                 glowing crystal clusters
 *
 * Caves appear in cave regions (a 2D mask), within `caves.depth` meters of
 * the surface, never under towns, lots, sites, roads or open water. Noise
 * is sampled on a coarse grid and interpolated, so a chunk costs ~1 ms.
 */

const G = 4; // coarse grid spacing (padded index units)
const TUBE = 0.14;

export class Caves {
  constructor(world) {
    this.world = world;
    this.cfg = world.config.caves;
    const seed = world.seed;
    this.nA = new SimplexNoise(deriveSeed(seed, "cave.a"));
    this.nB = new SimplexNoise(deriveSeed(seed, "cave.b"));
    this.nC = new SimplexNoise(deriveSeed(seed, "cave.cavern"));
    this.nMask = new SimplexNoise(deriveSeed(seed, "cave.mask"));
  }

  /** 0..1 cave-region strength at a column. */
  region(x, y) {
    const m = this.nMask.fbm2((x * VOXEL_SIZE) / 2600, (y * VOXEL_SIZE) / 2600, 2);
    return Math.max(0, Math.min(1, (m - 0.05) * 3));
  }

  /**
   * Cave field at a voxel (x, y, z) at depth d (m) below the surface:
   * negative inside a cave.
   */
  field(x, y, z, d) {
    const xm = x * VOXEL_SIZE;
    const ym = y * VOXEL_SIZE;
    const zm = z * VOXEL_SIZE;
    const a = this.nA.n3(xm / 46, ym / 46, zm / 26);
    const b = this.nB.n3(xm / 46 + 31.7, ym / 46 - 11.3, zm / 26);
    let f = a * a + b * b - TUBE * TUBE;
    if (d > 20) {
      const c = this.nC.fbm3(xm / 120, ym / 120, zm / 45, 2);
      f = Math.min(f, 0.42 - c);
    }
    return f;
  }

  /** Can caves exist under this ground-tile column? */
  columnOk(tile, idx) {
    const k = tile.kind[idx];
    // natural ground without water; caves do not break through snowfields and glaciers
    return k === 4 && tile.water[idx] === -2147483648 && tile.top[idx] !== MAT.SNOW && tile.top[idx] !== MAT.SNOW_WIND;
  }
}

export const caveSource = {
  id: "caves",
  order: 1,
  // cave interiors are seen from inside or through a mouth up close
  maxLod: 1,
  zRange(world, rect, lod, tile) {
    const cv = world.caves;
    if (!cv || !cv.cfg.enabled || !tile) return null;
    const cx = (rect.x0 + rect.x1) / 2;
    const cy = (rect.y0 + rect.y1) / 2;
    if (cv.region(cx, cy) <= 0 && cv.region(rect.x0, rect.y0) <= 0 && cv.region(rect.x1, rect.y1) <= 0) return null;
    // coarse probe of the cave band under the tile
    const depth = cv.cfg.depth * 8;
    let lo = Infinity;
    for (let j = 2; j < P; j += 8)
      for (let i = 2; i < P; i += 8) {
        const idx = i + j * P;
        if (!cv.columnOk(tile, idx)) continue;
        const x = rect.x0 + ((rect.x1 - rect.x0) * i) / P;
        const y = rect.y0 + ((rect.y1 - rect.y0) * j) / P;
        if (cv.region(x, y) <= 0) continue;
        const gz = tile.z[idx];
        for (let dz = 0; dz <= depth; dz += 12) if (cv.field(x, y, gz - dz, dz / 8) < 0.008) lo = Math.min(lo, gz - dz - 24);
      }
    return lo === Infinity ? null : [lo, tile.zMax];
  },
  rasterize(world, chunk, tile) {
    const cv = world.caves;
    if (!cv || !cv.cfg.enabled || !tile) return;
    const box = chunk.worldBox;
    const depth = cv.cfg.depth * 8;
    if (box.z0 > tile.zMax + 2 || box.z1 < tile.zMin - depth) return;
    const cx = (box.x0 + box.x1) / 2;
    const cy = (box.y0 + box.y1) / 2;
    if (cv.region(cx, cy) <= 0 && cv.region(box.x0, box.y0) <= 0 && cv.region(box.x1, box.y1) <= 0 && cv.region(box.x0, box.y1) <= 0 && cv.region(box.x1, box.y0) <= 0) return;
    // coarse grid of field values over the padded chunk
    const n = Math.ceil((P - 1) / G) + 1;
    const grid = new Float32Array(n * n * n);
    const regionAt = new Float32Array(n * n);
    for (let gj = 0; gj < n; gj += 1)
      for (let gi = 0; gi < n; gi += 1) regionAt[gi + gj * n] = cv.region(chunk.wx(Math.min(P - 1, gi * G)), chunk.wy(Math.min(P - 1, gj * G)));
    let any = false;
    for (let gk = 0; gk < n; gk += 1)
      for (let gj = 0; gj < n; gj += 1)
        for (let gi = 0; gi < n; gi += 1) {
          const i = Math.min(P - 1, gi * G);
          const j = Math.min(P - 1, gj * G);
          const k = Math.min(P - 1, gk * G);
          const col = i + j * P;
          const r = regionAt[gi + gj * n];
          const d = (tile.z[col] - chunk.wz(k)) / 8;
          let f = 1;
          if (r > 0 && d > -1 && d < cv.cfg.depth) f = cv.field(chunk.wx(i), chunk.wy(j), chunk.wz(k), d) + (1 - r) * 0.03;
          grid[gi + gj * n + gk * n * n] = f;
          if (f < 0) any = true;
        }
    if (!any) return;
    const data = chunk.data;
    const at = (i, j, k) => {
      const fi = i / G;
      const fj = j / G;
      const fk = k / G;
      const i0 = Math.min(n - 2, Math.floor(fi));
      const j0 = Math.min(n - 2, Math.floor(fj));
      const k0 = Math.min(n - 2, Math.floor(fk));
      const tx = fi - i0;
      const ty = fj - j0;
      const tz = fk - k0;
      const g = (a, b, c) => grid[a + b * n + c * n * n];
      const c00 = g(i0, j0, k0) * (1 - tx) + g(i0 + 1, j0, k0) * tx;
      const c10 = g(i0, j0 + 1, k0) * (1 - tx) + g(i0 + 1, j0 + 1, k0) * tx;
      const c01 = g(i0, j0, k0 + 1) * (1 - tx) + g(i0 + 1, j0, k0 + 1) * tx;
      const c11 = g(i0, j0 + 1, k0 + 1) * (1 - tx) + g(i0 + 1, j0 + 1, k0 + 1) * tx;
      return (c00 * (1 - ty) + c10 * ty) * (1 - tz) + (c01 * (1 - ty) + c11 * ty) * tz;
    };
    const seed = world.seed;
    for (let j = 0; j < P; j += 1) {
      for (let i = 0; i < P; i += 1) {
        const col = i + j * P;
        if (!cv.columnOk(tile, col)) continue;
        const gz = tile.z[col];
        let carved = false;
        for (let k = 0; k < P; k += 1) {
          const z = chunk.wz(k);
          if (z > gz || gz - z > depth) continue;
          const idx = col + k * P2;
          if (!IS_SOLID[data[idx]]) continue;
          if (at(i, j, k) < 0) {
            data[idx] = 0;
            carved = true;
          }
        }
        if (!carved) continue;
        // decorate: floors, stalactites / stalagmites, crystals; moss round the
        // mouth only where plants grow (above the tree line it is scree)
        const x = chunk.wx(i);
        const y = chunk.wy(j);
        const mossy = world.landCover.climate(x, y, gz / 8).t > TREELINE;
        for (let k = 1; k < P - 1; k += 1) {
          const idx = col + k * P2;
          const below = data[idx - P2];
          const m = data[idx];
          if (m === 0 && IS_SOLID[below]) {
            const z = chunk.wz(k);
            const h = hash32(seed, x, y, z) >>> 0;
            const nearMouth = gz - z < 24;
            data[idx - P2] = nearMouth ? (mossy ? MAT.CAVE_MOSS : MAT.GRAVEL) : (h & 7) < 2 ? MAT.GRAVEL : MAT.CAVE_FLOOR;
            if (chunk.lod === 0) {
              if (h % 997 < 3) {
                const c = (h >> 12) & 1 ? MAT.CRYSTAL_CYAN : MAT.CRYSTAL_VIOLET;
                for (let q = 0; q < 1 + ((h >> 8) & 3) && k + q < P; q += 1) if (data[idx + q * P2] === 0) data[idx + q * P2] = c;
              } else if (h % 211 < 4) {
                for (let q = 0; q < 1 + ((h >> 9) & 3) && k + q < P; q += 1) if (data[idx + q * P2] === 0) data[idx + q * P2] = MAT.ROCK;
              }
            }
          } else if (m === 0 && k + 1 < P && IS_SOLID[data[idx + P2]] && chunk.lod === 0) {
            const h = hash32(seed, x, y, chunk.wz(k), 5) >>> 0;
            if (h % 173 < 5) for (let q = 0; q < 1 + ((h >> 7) & 3) && k - q > 0; q += 1) if (data[idx - q * P2] === 0) data[idx - q * P2] = MAT.ROCK;
          }
        }
      }
    }
  },
};
