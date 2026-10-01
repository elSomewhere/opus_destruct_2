import { hash32, Rng } from "../core/hash.js";
import { wrapOf } from "../world/wrap.js";
import { LRU } from "../core/lru.js";
import { vx } from "../core/units.js";
import { MAT } from "../voxel/materials.js";
import { P, P2 } from "../voxel/chunk.js";
import { groundTile } from "../voxel/compose.js";
import { TREELINE, SNOWLINE } from "./landcover.js";
import { rotationMatrix, YAW_QUARTER } from "../core/placement.js";

/**
 * Boulders: loose rocks strewn over mountain ground. One candidate per 6 m
 * lattice cell; dense on scree and alpine ground above the tree line, a
 * few in mountain forests and foothills, some on canyon floors, glacial
 * erratics (granite, their tops mossy, lichen on the flanks) through
 * rugged woods and heath. Each is an
 * irregular, half-sunk ellipsoid of rock (with a snow cap near the snow
 * line). Placement shares the forest's clear-ground rules (no roads, lots,
 * site pads, water, streams or highways). Rasterized at LOD 0-2, filling
 * air only.
 *
 * In the angled world (world.angles, features.vegetation) every boulder is
 * turned and tipped: an exact yaw, pitch and roll of core/placement.js
 * (integer matrices, no sin or cos), so their long axes point every way and
 * their beds tilt, each still within the lattice's reach (MAX_R).
 */

const CELL = vx(6);
const MAX_R = vx(3.2);
const ROCKS = [MAT.ROCK, MAT.ROCK_DARK, MAT.GRANITE, MAT.ROCK_LIGHT];
const ERRATIC = [MAT.GRANITE, MAT.GRANITE_PINK, MAT.ROCK_LIGHT, MAT.GRANITE];

export class Boulders {
  constructor(world) {
    this.world = world;
    this.cache = new LRU(40000);
    // a wrapping world: the boulder lattice repeats with it
    this.wrap = wrapOf(world.config);
    this.n = this.wrap.on ? this.wrap.sizeV / CELL : 0;
    const a = world.config.world.angles;
    this.tipped = !!(a?.enabled && a.features?.vegetation !== false);
  }

  at(gx, gy) {
    const key = gx * 100003 + gy;
    let b = this.cache.get(key);
    if (b !== undefined) return b;
    b = this.compute(gx, gy);
    this.cache.set(key, b);
    return b;
  }

  compute(gx, gy) {
    const w = this.world;
    const seed = w.seed;
    const cgx = this.wrap.canon(gx, this.n);
    const cgy = this.wrap.canon(gy, this.n);
    const h0 = hash32(seed, cgx, cgy, 521) >>> 0;
    const roll = ((h0 >>> 20) & 1023) / 1024;
    // most cells hold nothing: reject before sampling the terrain
    if (roll > 0.32) return null;
    const x = gx * CELL + (h0 & 1023) * (CELL / 1024);
    const y = gy * CELL + ((h0 >>> 10) & 1023) * (CELL / 1024);
    const ts = w.terrain.sample(x, y);
    if (ts.u > 0.03) return null;
    const hM = ts.h / 8;
    const { t } = w.landCover.climate(x, y, hM);
    let dens = ts.mountain > 0.05 ? (t < TREELINE ? 0.32 : 0.04 + 0.1 * ts.mountain) : 0.004;
    if (ts.canyon > 0.2) dens = Math.max(dens, 0.12);
    // glacial erratics: boulders left by the ice through rugged woods and heath
    const erratic = ts.rugged > 0.35 && ts.mountain < 0.3 ? 0.13 * Math.pow((ts.rugged - 0.35) / 0.65, 1.3) : 0;
    dens = Math.max(dens, erratic);
    if (roll > dens) return null;
    // buried under permanent snow
    if (t < SNOWLINE - 0.03) return null;
    const biome = w.landCover.biomeAt(x, y, hM);
    if (!w.forest.clearGround(x, y, ts, biome)) return null;
    const h1 = hash32(seed, cgx, cgy, 522) >>> 0;
    const a = (h1 & 255) / 255;
    const big = t < TREELINE ? 0.5 + 2.4 * a * a : erratic > dens * 0.5 ? 0.35 + 1.9 * a * a * a : 0.4 + 1.3 * a * a;
    const r = Math.max(3, Math.round(vx(big)));
    const b = {
      x: Math.round(x),
      y: Math.round(y),
      r,
      ry: Math.max(3, Math.round(r * (0.7 + 0.5 * (((h1 >>> 8) & 255) / 255)))),
      rz: Math.max(2, Math.round(r * (0.5 + 0.35 * (((h1 >>> 16) & 255) / 255)))),
      seed: h1,
      mat: erratic > 0 ? ERRATIC[(h1 >>> 24) & 3] : ROCKS[(h1 >>> 24) & 3],
      snow: t < SNOWLINE + 0.03,
      // (below the tree line in moist country the tops grow moss, the flanks lichen)
      moss: t > TREELINE && ts.rugged > 0.3 ? 0.35 + 0.4 * (((h1 >>> 12) & 255) / 255) : 0,
    };
    if (this.tipped) tip(b, Rng.from(seed, "boulders.tip", cgx, cgy));
    else b.bb = { x0: b.x - b.r - 2, y0: b.y - b.ry - 2, x1: b.x + b.r + 2, y1: b.y + b.ry + 2 };
    return b;
  }

  near(rect, out = []) {
    const gx0 = Math.floor((rect.x0 - MAX_R) / CELL);
    const gy0 = Math.floor((rect.y0 - MAX_R) / CELL);
    const gx1 = Math.floor((rect.x1 + MAX_R) / CELL);
    const gy1 = Math.floor((rect.y1 + MAX_R) / CELL);
    for (let gy = gy0; gy <= gy1; gy += 1)
      for (let gx = gx0; gx <= gx1; gx += 1) {
        const b = this.at(gx, gy);
        if (b && b.bb.x1 >= rect.x0 && b.bb.x0 <= rect.x1 && b.bb.y1 >= rect.y0 && b.bb.y0 <= rect.y1) out.push(b);
      }
    return out;
  }
}

/** Lumps reach this much past the ellipsoid (sqrt(1 + 7.5 * 0.025)). */
const LUMP = 1.09;

/**
 * Turn and tip a boulder: any yaw of the table (a half turn is the same
 * ellipsoid), a roll about its long axis up to 22.6° and a pitch along it
 * up to 8.2°: b.rot { m, d } (local -> world, core/placement.js), b.ext
 * (world half extents of the ellipsoid). One too big to stay within the
 * lattice's reach (MAX_R) turned this way shrinks until it does.
 */
function tip(b, rng) {
  const rot = rotationMatrix(rng.int(0, 2 * YAW_QUARTER - 1), rng.int(-4, 4), rng.int(0, 7));
  const m = rot.m;
  const ext = (row) => Math.sqrt((m[row * 3] * b.r) ** 2 + (m[row * 3 + 1] * b.ry) ** 2 + (m[row * 3 + 2] * b.rz) ** 2) / rot.d;
  // (the rounded centre may sit a voxel off its cell: its whole reach within MAX_R - 1)
  const room = Math.floor(MAX_R) - 2;
  const e = Math.max(ext(0), ext(1)) * LUMP + 1;
  if (e > room) {
    const f = room / e;
    b.r = Math.max(2, Math.floor(b.r * f));
    b.ry = Math.max(2, Math.floor(b.ry * f));
    b.rz = Math.max(2, Math.floor(b.rz * f));
  }
  b.rot = rot;
  b.ext = [ext(0), ext(1), ext(2)];
  const ex = Math.ceil(b.ext[0] * LUMP) + 1;
  const ey = Math.ceil(b.ext[1] * LUMP) + 1;
  b.bb = { x0: b.x - ex, y0: b.y - ey, x1: b.x + ex, y1: b.y + ey };
}

export const boulderSource = {
  id: "boulders",
  order: 6.5,
  maxLod: 2,
  zRange(world, rect, lod, tile) {
    if (!tile || !world.boulders) return null;
    // cheap: only mountain / canyon country can hold boulders
    if (!world.boulders.near(rect).length) return null;
    return [tile.zMin - 4, tile.zMax + 2 * MAX_R];
  },
  rasterize(world, chunk, tile) {
    if (!tile || !world.boulders) return;
    const box = chunk.worldBox;
    const bs = world.boulders.near(box);
    if (!bs.length) return;
    const d = chunk.data;
    const s = chunk.s;
    for (const b of bs) {
      // ground under the boulder's centre, from the tile that holds it
      const span = 32 * s;
      const tcx = Math.floor(b.x / span);
      const tcy = Math.floor(b.y / span);
      const gt = tcx === tile.cx && tcy === tile.cy ? tile : groundTile(world, chunk.lod, tcx, tcy);
      const i = Math.floor((b.x - (tcx * 32 - 1) * s) / s);
      const j = Math.floor((b.y - (tcy * 32 - 1) * s) / s);
      const gz = gt.z[i + j * P];
      if (gt.kind[i + j * P] !== 4) continue;
      if (b.rot) {
        rasterTipped(chunk, b, gz);
        continue;
      }
      const cz = gz + Math.round(b.rz * 0.45);
      if (cz - b.rz > box.z1 || cz + b.rz < box.z0) continue;
      const [i0, i1] = chunk.rangeX(b.x - b.r - 1, b.x + b.r + 1);
      const [j0, j1] = chunk.rangeY(b.y - b.ry - 1, b.y + b.ry + 1);
      const [k0, k1] = chunk.rangeZ(cz - b.rz - 1, cz + b.rz + 1);
      for (let k = k0; k <= k1; k += 1) {
        const dz = (chunk.wz(k) - cz) / b.rz;
        for (let jj = j0; jj <= j1; jj += 1) {
          const dy = (chunk.wy(jj) - b.y) / b.ry;
          for (let ii = i0; ii <= i1; ii += 1) {
            const idx = ii + jj * P + k * P2;
            if (d[idx] !== 0) continue;
            const wx = chunk.wx(ii);
            const dx = (wx - b.x) / b.r;
            const lump = (((hash32(wx >> 1, chunk.wy(jj) >> 1, chunk.wz(k) >> 1, b.seed) >>> 0) & 15) - 7.5) * 0.025;
            if (dx * dx + dy * dy + dz * dz >= 1 + lump) continue;
            if (b.snow && dz > 0.55) d[idx] = MAT.SNOW;
            else if (b.moss && dz > 1 - b.moss + lump * 4) d[idx] = (hash32(wx, chunk.wy(jj), b.seed, 5) & 7) === 0 ? MAT.LICHEN : MAT.MOSS;
            else d[idx] = (b.moss && (hash32(wx >> 1, chunk.wz(k) >> 1, b.seed, 6) & 15) === 0) ? MAT.LICHEN : b.mat;
          }
        }
      }
    }
  },
};

/**
 * A turned and tipped boulder: a voxel belongs to it where its offset from
 * the centre, taken into the boulder's own axes (Mᵀ p / d: exact integer
 * numerators), lies within the lumpy ellipsoid. Snow and moss by the world's
 * up, as on any boulder. Half sunk in the ground under its centre (gz).
 */
function rasterTipped(chunk, b, gz) {
  const d = chunk.data;
  const box = chunk.worldBox;
  const { m, d: D } = b.rot;
  const hz = b.ext[2];
  const cz = gz + Math.round(hz * 0.45);
  const top = Math.ceil(cz + hz * LUMP) + 1;
  const bottom = Math.floor(cz - hz * LUMP) - 1;
  if (bottom > box.z1 || top < box.z0) return;
  const [i0, i1] = chunk.rangeX(b.bb.x0, b.bb.x1);
  const [j0, j1] = chunk.rangeY(b.bb.y0, b.bb.y1);
  const [k0, k1] = chunk.rangeZ(bottom, top);
  const su = b.r * D;
  const sv = b.ry * D;
  const sw = b.rz * D;
  for (let k = k0; k <= k1; k += 1) {
    const wz = chunk.wz(k);
    const pz = wz - cz;
    const dz = pz / hz;
    for (let jj = j0; jj <= j1; jj += 1) {
      const wy = chunk.wy(jj);
      const py = wy - b.y;
      for (let ii = i0; ii <= i1; ii += 1) {
        const idx = ii + jj * P + k * P2;
        if (d[idx] !== 0) continue;
        const wx = chunk.wx(ii);
        const px = wx - b.x;
        const u = (m[0] * px + m[3] * py + m[6] * pz) / su;
        const v = (m[1] * px + m[4] * py + m[7] * pz) / sv;
        const w = (m[2] * px + m[5] * py + m[8] * pz) / sw;
        const lump = (((hash32(wx >> 1, wy >> 1, wz >> 1, b.seed) >>> 0) & 15) - 7.5) * 0.025;
        if (u * u + v * v + w * w >= 1 + lump) continue;
        if (b.snow && dz > 0.55) d[idx] = MAT.SNOW;
        else if (b.moss && dz > 1 - b.moss + lump * 4) d[idx] = (hash32(wx, wy, b.seed, 5) & 7) === 0 ? MAT.LICHEN : MAT.MOSS;
        else d[idx] = b.moss && (hash32(wx >> 1, wz >> 1, b.seed, 6) & 15) === 0 ? MAT.LICHEN : b.mat;
      }
    }
  }
}
