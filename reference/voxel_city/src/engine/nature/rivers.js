import { SimplexNoise } from "../core/noise.js";
import { deriveSeed } from "../core/hash.js";
import { smoothstep } from "../core/math.js";
import { VOXEL_SIZE } from "../core/units.js";

/**
 * Rivers: meandering channels along the zero isolines of a domain-warped,
 * low-frequency noise. A second noise fades rivers in and out (a creek
 * narrows to nothing: a spring), so the network has sources and varying
 * widths without any global flow computation — every query is a pure
 * function of position, which keeps rivers streamable and seamless.
 *
 * Only the ground tile carves them (valleys with sloped banks in the
 * countryside, stone quays in cities); roads crossing a river become
 * bridges there. Terrain heights used for planning stay uncarved, so lots,
 * stations and sewers are planned against the street level and simply keep
 * clear of river corridors (`hitsRect`).
 *
 * The water level follows the local minimum of the terrain sampled around
 * the point, a little below it, so the surface never floats above the land
 * and steps only rarely.
 */

const SCALE = 5200; // meander wavelength (m)
const RING = 18 * 8; // terrain samples 18 m around for the water level

export class Rivers {
  constructor(world) {
    this.world = world;
    const seed = world.seed;
    this.n = new SimplexNoise(deriveSeed(seed, "river.path"));
    this.warp = new SimplexNoise(deriveSeed(seed, "river.warp"));
    this.mask = new SimplexNoise(deriveSeed(seed, "river.mask"));
    this.cfg = world.config.rivers;
    this.out = { d: 0, half: 0, water: 0, bed: 0, urban: 0, bank: 0 };
  }

  field(fx, fy, fz, fw) {
    const w = 900 * this.warp.fbmP(fx / 3000, fy / 3000, fz / 3000, fw / 3000, 2);
    return this.n.fbmP((fx + w) / SCALE, (fy - w) / SCALE, fz / SCALE, fw / SCALE, 3);
  }

  /**
   * River info at voxel (x, y), or null when there is no river within its
   * banks. Returns a shared object: { d (m from centerline), half (m),
   * water (voxel z of the water surface), bed (voxel z of the bottom
   * at this column), urban (0..1 quay-ness), bank (m of sloped bank) }.
   */
  at(x, y) {
    if (!this.cfg.enabled) return null;
    // island: rivers end at the shore
    const isl = this.world.fields.island;
    if (isl && isl.coast(x / 8, y / 8) < -12) return null;
    const chart = this.world.chart;
    const [fx, fy, fz, fw] = chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const v = this.field(fx, fy, fz, fw);
    if (Math.abs(v) > 0.06) return null; // > ~150 m from any centerline
    const e = 12;
    let gx;
    let gy;
    if (fw === undefined) {
      gx = (this.field(fx + e, fy, fz) - v) / e;
      gy = (this.field(fx, fy + e, fz) - v) / e;
    } else {
      // torus chart: step along the surface
      const R = chart.R;
      gx = (this.field(fx - (e * fy) / R, fy + (e * fx) / R, fz, fw) - v) / e;
      gy = (this.field(fx, fy, fz - (e * fw) / R, fw + (e * fz) / R) - v) / e;
    }
    const g = Math.hypot(gx, gy) || 1e-6;
    const d = Math.abs(v) / g;
    // rivers fade in and out (springs, mouths) and stay out of the mountains
    const m = smoothstep(0.05, 0.4, this.mask.fbmP(fx / 16000, fy / 16000, fz / 16000, fw / 16000, 2));
    if (m <= 0) return null;
    const mtn = this.world.fields.mountainness(x, y);
    const half = this.cfg.maxHalfWidth * m * (1 - smoothstep(0.05, 0.3, mtn));
    if (half < 2) return null;
    const urban = smoothstep(0.2, 0.4, this.world.fields.urban(x, y).u);
    const bank = 22 * (1 - urban);
    if (d > half + bank + 1) return null;
    const o = this.out;
    o.d = d;
    o.half = half;
    o.urban = urban;
    o.bank = bank;
    o.water = this.waterLevel(x, y);
    const depthM = d < half ? 1.2 + 2.2 * (1 - (d / half) ** 2) : 0;
    o.bed = Math.round(o.water - depthM * 8);
    return o;
  }

  /** Water surface z (voxels): below the lowest nearby street / ground. */
  waterLevel(x, y) {
    const t = this.world.terrain;
    let lo = t.sample(x, y).h;
    for (const [dx, dy] of [[RING, 0], [-RING, 0], [0, RING], [0, -RING]]) lo = Math.min(lo, t.sample(x + dx, y + dy).h);
    const urban = this.world.fields.urban(x, y).u > 0.3;
    // quantize in 2-voxel steps so the surface is flat over long reaches
    const level = Math.floor((lo - (urban ? 22 : 12)) / 2) * 2;
    // (an island's rivers meet the sea at sea level)
    return this.world.fields.island ? Math.max(level, Math.round(this.world.config.world.seaLevel * 8)) : level;
  }

  /** Carved ground height (voxels) for a column with surface h (voxels). */
  groundAt(info, h) {
    if (info.d < info.half) return Math.min(h, info.bed);
    if (info.urban > 0.5) return h; // quay wall at the channel edge
    const t = (info.d - info.half) / Math.max(1, info.bank);
    return Math.min(h, Math.round(info.water + 2 + t * t * (h - info.water)));
  }

  /**
   * Distance (m) from a point (voxels) to the channel's edge, negative in
   * the water; Infinity far from any river. Unlike `at` it does not stop at
   * the banks (a town river has none), so it can test margins.
   */
  channelGap(x, y) {
    const isl = this.world.fields.island;
    if (isl && isl.coast(x / 8, y / 8) < -12) return Infinity;
    const chart = this.world.chart;
    const [fx, fy, fz, fw] = chart.toField(x * VOXEL_SIZE, y * VOXEL_SIZE);
    const v = this.field(fx, fy, fz, fw);
    if (Math.abs(v) > 0.06) return Infinity;
    const e = 12;
    let gx;
    let gy;
    if (fw === undefined) {
      gx = (this.field(fx + e, fy, fz) - v) / e;
      gy = (this.field(fx, fy + e, fz) - v) / e;
    } else {
      const R = chart.R;
      gx = (this.field(fx - (e * fy) / R, fy + (e * fx) / R, fz, fw) - v) / e;
      gy = (this.field(fx, fy, fz - (e * fw) / R, fw + (e * fz) / R) - v) / e;
    }
    const d = Math.abs(v) / (Math.hypot(gx, gy) || 1e-6);
    const m = smoothstep(0.05, 0.4, this.mask.fbmP(fx / 16000, fy / 16000, fz / 16000, fw / 16000, 2));
    if (m <= 0) return Infinity;
    const half = this.cfg.maxHalfWidth * m * (1 - smoothstep(0.05, 0.3, this.world.fields.mountainness(x, y)));
    if (half < 2) return Infinity;
    return d - half;
  }

  /** Does the channel (plus a margin in m) touch a rect (voxels)? Sampled every ~10 m. */
  hitsRect(r, marginM = 6) {
    if (!this.cfg.enabled) return false;
    const step = 80;
    for (let y = r.y0; y <= r.y1 + step - 1; y += step) {
      for (let x = r.x0; x <= r.x1 + step - 1; x += step) {
        if (this.channelGap(Math.min(x, r.x1), Math.min(y, r.y1)) < marginM + (step / 8) * 0.71) return true;
      }
    }
    return false;
  }
}
