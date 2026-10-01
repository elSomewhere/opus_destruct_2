import { Rng } from "./hash.js";

/**
 * Seeded simplex noise (2D + 3D) and fractal helpers. Output range ≈ [-1, 1].
 * 3D noise is what macro fields use so the same code can later sample a
 * sphere (planet charts map (x, y) to a 3D point on the surface).
 */

const F2 = 0.5 * (Math.sqrt(3) - 1);
const G2 = (3 - Math.sqrt(3)) / 6;
const F3 = 1 / 3;
const G3 = 1 / 6;
/**
 * 3D kernel radius². Gustavson's reference uses 0.6, but a simplex only sums
 * its own 4 corners and a 0.6 kernel reaches past them, so the field jumps by
 * up to ~0.005 at cell boundaries (metre-high steps once scaled to mountain
 * relief). 0.5 is continuous; K3 rescales to the old standard deviation.
 */
const R3 = 0.5;
const K3 = 82;

/** 4D (torus charts): the same continuity rule as 3D; K4 rescales to the 3D standard deviation. */
const F4 = (Math.sqrt(5) - 1) / 4;
const G4 = (5 - Math.sqrt(5)) / 20;
const R4 = 0.5;
const K4 = 104;

const GRAD4 = new Float32Array([
  0, 1, 1, 1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, -1, -1,
  0, -1, 1, 1, 0, -1, 1, -1, 0, -1, -1, 1, 0, -1, -1, -1,
  1, 0, 1, 1, 1, 0, 1, -1, 1, 0, -1, 1, 1, 0, -1, -1,
  -1, 0, 1, 1, -1, 0, 1, -1, -1, 0, -1, 1, -1, 0, -1, -1,
  1, 1, 0, 1, 1, 1, 0, -1, 1, -1, 0, 1, 1, -1, 0, -1,
  -1, 1, 0, 1, -1, 1, 0, -1, -1, -1, 0, 1, -1, -1, 0, -1,
  1, 1, 1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, -1, -1, 0,
  -1, 1, 1, 0, -1, 1, -1, 0, -1, -1, 1, 0, -1, -1, -1, 0,
]);

const GRAD3 = new Float32Array([
  1, 1, 0, -1, 1, 0, 1, -1, 0, -1, -1, 0,
  1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, -1,
  0, 1, 1, 0, -1, 1, 0, 1, -1, 0, -1, -1,
]);

export class SimplexNoise {
  constructor(seed) {
    const rng = new Rng(seed);
    const p = new Uint8Array(256);
    for (let i = 0; i < 256; i += 1) p[i] = i;
    for (let i = 255; i > 0; i -= 1) {
      const j = Math.floor(rng.next() * (i + 1));
      const t = p[i];
      p[i] = p[j];
      p[j] = t;
    }
    this.perm = new Uint8Array(512);
    this.permMod12 = new Uint8Array(512);
    for (let i = 0; i < 512; i += 1) {
      this.perm[i] = p[i & 255];
      this.permMod12[i] = this.perm[i] % 12;
    }
  }

  n2(xin, yin) {
    const perm = this.perm;
    const pm = this.permMod12;
    let n0 = 0;
    let n1 = 0;
    let n2 = 0;
    const s = (xin + yin) * F2;
    const i = Math.floor(xin + s);
    const j = Math.floor(yin + s);
    const t = (i + j) * G2;
    const x0 = xin - (i - t);
    const y0 = yin - (j - t);
    let i1;
    let j1;
    if (x0 > y0) {
      i1 = 1;
      j1 = 0;
    } else {
      i1 = 0;
      j1 = 1;
    }
    const x1 = x0 - i1 + G2;
    const y1 = y0 - j1 + G2;
    const x2 = x0 - 1 + 2 * G2;
    const y2 = y0 - 1 + 2 * G2;
    const ii = i & 255;
    const jj = j & 255;
    let t0 = 0.5 - x0 * x0 - y0 * y0;
    if (t0 >= 0) {
      const gi = pm[ii + perm[jj]] * 3;
      t0 *= t0;
      n0 = t0 * t0 * (GRAD3[gi] * x0 + GRAD3[gi + 1] * y0);
    }
    let t1 = 0.5 - x1 * x1 - y1 * y1;
    if (t1 >= 0) {
      const gi = pm[ii + i1 + perm[jj + j1]] * 3;
      t1 *= t1;
      n1 = t1 * t1 * (GRAD3[gi] * x1 + GRAD3[gi + 1] * y1);
    }
    let t2 = 0.5 - x2 * x2 - y2 * y2;
    if (t2 >= 0) {
      const gi = pm[ii + 1 + perm[jj + 1]] * 3;
      t2 *= t2;
      n2 = t2 * t2 * (GRAD3[gi] * x2 + GRAD3[gi + 1] * y2);
    }
    return 70 * (n0 + n1 + n2);
  }

  n3(xin, yin, zin) {
    const perm = this.perm;
    const pm = this.permMod12;
    let n0 = 0;
    let n1 = 0;
    let n2 = 0;
    let n3 = 0;
    const s = (xin + yin + zin) * F3;
    const i = Math.floor(xin + s);
    const j = Math.floor(yin + s);
    const k = Math.floor(zin + s);
    const t = (i + j + k) * G3;
    const x0 = xin - (i - t);
    const y0 = yin - (j - t);
    const z0 = zin - (k - t);
    let i1;
    let j1;
    let k1;
    let i2;
    let j2;
    let k2;
    if (x0 >= y0) {
      if (y0 >= z0) {
        i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0;
      } else if (x0 >= z0) {
        i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1;
      } else {
        i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1;
      }
    } else if (y0 < z0) {
      i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1;
    } else if (x0 < z0) {
      i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1;
    } else {
      i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0;
    }
    const x1 = x0 - i1 + G3;
    const y1 = y0 - j1 + G3;
    const z1 = z0 - k1 + G3;
    const x2 = x0 - i2 + 2 * G3;
    const y2 = y0 - j2 + 2 * G3;
    const z2 = z0 - k2 + 2 * G3;
    const x3 = x0 - 1 + 3 * G3;
    const y3 = y0 - 1 + 3 * G3;
    const z3 = z0 - 1 + 3 * G3;
    const ii = i & 255;
    const jj = j & 255;
    const kk = k & 255;
    let t0 = R3 - x0 * x0 - y0 * y0 - z0 * z0;
    if (t0 > 0) {
      const gi = pm[ii + perm[jj + perm[kk]]] * 3;
      t0 *= t0;
      n0 = t0 * t0 * (GRAD3[gi] * x0 + GRAD3[gi + 1] * y0 + GRAD3[gi + 2] * z0);
    }
    let t1 = R3 - x1 * x1 - y1 * y1 - z1 * z1;
    if (t1 > 0) {
      const gi = pm[ii + i1 + perm[jj + j1 + perm[kk + k1]]] * 3;
      t1 *= t1;
      n1 = t1 * t1 * (GRAD3[gi] * x1 + GRAD3[gi + 1] * y1 + GRAD3[gi + 2] * z1);
    }
    let t2 = R3 - x2 * x2 - y2 * y2 - z2 * z2;
    if (t2 > 0) {
      const gi = pm[ii + i2 + perm[jj + j2 + perm[kk + k2]]] * 3;
      t2 *= t2;
      n2 = t2 * t2 * (GRAD3[gi] * x2 + GRAD3[gi + 1] * y2 + GRAD3[gi + 2] * z2);
    }
    let t3 = R3 - x3 * x3 - y3 * y3 - z3 * z3;
    if (t3 > 0) {
      const gi = pm[ii + 1 + perm[jj + 1 + perm[kk + 1]]] * 3;
      t3 *= t3;
      n3 = t3 * t3 * (GRAD3[gi] * x3 + GRAD3[gi + 1] * y3 + GRAD3[gi + 2] * z3);
    }
    return K3 * (n0 + n1 + n2 + n3);
  }

  /** 4D simplex noise (Gustavson), ≈ [-1, 1]: torus charts sample fields on a flat torus in R^4. */
  n4(x, y, z, w) {
    const perm = this.perm;
    const s = (x + y + z + w) * F4;
    const i = Math.floor(x + s);
    const j = Math.floor(y + s);
    const k = Math.floor(z + s);
    const l = Math.floor(w + s);
    const t = (i + j + k + l) * G4;
    const x0 = x - (i - t);
    const y0 = y - (j - t);
    const z0 = z - (k - t);
    const w0 = w - (l - t);
    // rank the coordinates to find the simplex
    let rx = 0;
    let ry = 0;
    let rz = 0;
    let rw = 0;
    if (x0 > y0) rx += 1;
    else ry += 1;
    if (x0 > z0) rx += 1;
    else rz += 1;
    if (x0 > w0) rx += 1;
    else rw += 1;
    if (y0 > z0) ry += 1;
    else rz += 1;
    if (y0 > w0) ry += 1;
    else rw += 1;
    if (z0 > w0) rz += 1;
    else rw += 1;
    const ii = i & 255;
    const jj = j & 255;
    const kk = k & 255;
    const ll = l & 255;
    let sum = 0;
    for (let c = 0; c < 5; c += 1) {
      // corner c: offsets are 1 where the rank is at least 4 - c
      const oi = c === 0 ? 0 : c === 4 ? 1 : rx >= 4 - c ? 1 : 0;
      const oj = c === 0 ? 0 : c === 4 ? 1 : ry >= 4 - c ? 1 : 0;
      const ok = c === 0 ? 0 : c === 4 ? 1 : rz >= 4 - c ? 1 : 0;
      const ol = c === 0 ? 0 : c === 4 ? 1 : rw >= 4 - c ? 1 : 0;
      const dx = x0 - oi + c * G4;
      const dy = y0 - oj + c * G4;
      const dz = z0 - ok + c * G4;
      const dw = w0 - ol + c * G4;
      let tt = R4 - dx * dx - dy * dy - dz * dz - dw * dw;
      if (tt <= 0) continue;
      const gi = (perm[ii + oi + perm[jj + oj + perm[kk + ok + perm[ll + ol]]]] & 31) * 4;
      tt *= tt;
      sum += tt * tt * (GRAD4[gi] * dx + GRAD4[gi + 1] * dy + GRAD4[gi + 2] * dz + GRAD4[gi + 3] * dw);
    }
    return K4 * sum;
  }

  fbm4(x, y, z, w, octaves = 4, lacunarity = 2, gain = 0.5) {
    let amp = 1;
    let freq = 1;
    let sum = 0;
    let norm = 0;
    for (let o = 0; o < octaves; o += 1) {
      sum += amp * this.n4(x * freq + o * 17.31, y * freq - o * 9.73, z * freq + o * 3.17, w * freq - o * 5.41);
      norm += amp;
      amp *= gain;
      freq *= lacunarity;
    }
    return sum / norm;
  }

  /** 4D ridged multifractal, ≈ [0, 1] (see ridged3). */
  ridged4(x, y, z, w, octaves = 6, lacunarity = 2.05, gain = 0.5) {
    let amp = 0.5;
    let freq = 1;
    let sum = 0;
    let norm = 0;
    let weight = 1;
    for (let o = 0; o < octaves; o += 1) {
      let n = 1 - Math.abs(this.n4(x * freq + o * 31.7, y * freq + o * 11.3, z * freq - o * 7.9, w * freq + o * 4.3));
      n *= n;
      n *= weight;
      weight = Math.min(1, n * 1.8);
      sum += n * amp;
      norm += amp;
      amp *= gain;
      freq *= lacunarity;
    }
    return sum / norm;
  }

  /**
   * Chart-agnostic sampling of a field point: charts return [fx, fy, fz]
   * (flat plane, cube-sphere face) or [fx, fy, fz, fw] (torus). Callers
   * pass the 4th coordinate divided like the others; undefined / NaN means
   * the 3D noise (so planar worlds are unchanged).
   */
  nP(x, y, z, w) {
    return typeof w === "number" && w === w ? this.n4(x, y, z, w) : this.n3(x, y, z);
  }

  fbmP(x, y, z, w, octaves = 4, lacunarity = 2, gain = 0.5) {
    return typeof w === "number" && w === w ? this.fbm4(x, y, z, w, octaves, lacunarity, gain) : this.fbm3(x, y, z, octaves, lacunarity, gain);
  }

  ridgedP(x, y, z, w, octaves = 6, lacunarity = 2.05, gain = 0.5) {
    return typeof w === "number" && w === w ? this.ridged4(x, y, z, w, octaves, lacunarity, gain) : this.ridged3(x, y, z, octaves, lacunarity, gain);
  }

  /** Fractal Brownian motion over 2D simplex; returns ≈ [-1, 1]. */
  fbm2(x, y, octaves = 4, lacunarity = 2, gain = 0.5) {
    let amp = 1;
    let freq = 1;
    let sum = 0;
    let norm = 0;
    for (let o = 0; o < octaves; o += 1) {
      sum += amp * this.n2(x * freq + o * 17.31, y * freq - o * 9.73);
      norm += amp;
      amp *= gain;
      freq *= lacunarity;
    }
    return sum / norm;
  }

  fbm3(x, y, z, octaves = 4, lacunarity = 2, gain = 0.5) {
    let amp = 1;
    let freq = 1;
    let sum = 0;
    let norm = 0;
    for (let o = 0; o < octaves; o += 1) {
      sum += amp * this.n3(x * freq + o * 17.31, y * freq - o * 9.73, z * freq + o * 3.17);
      norm += amp;
      amp *= gain;
      freq *= lacunarity;
    }
    return sum / norm;
  }

  /**
   * 3D ridged multifractal (sphere-consistent mountain ridges), ≈ [0, 1].
   * Each octave is weighted by the previous one, so detail concentrates on
   * the ridges and valleys stay smooth, like eroded terrain.
   */
  ridged3(x, y, z, octaves = 6, lacunarity = 2.05, gain = 0.5) {
    let amp = 0.5;
    let freq = 1;
    let sum = 0;
    let norm = 0;
    let weight = 1;
    for (let o = 0; o < octaves; o += 1) {
      let n = 1 - Math.abs(this.n3(x * freq + o * 31.7, y * freq + o * 11.3, z * freq - o * 7.9));
      n *= n;
      n *= weight;
      weight = Math.min(1, n * 1.8);
      sum += n * amp;
      norm += amp;
      amp *= gain;
      freq *= lacunarity;
    }
    return sum / norm;
  }

  /** Ridged multifractal (mountain ridges), returns ≈ [0, 1]. */
  ridged2(x, y, octaves = 5) {
    let amp = 0.5;
    let freq = 1;
    let sum = 0;
    let norm = 0;
    let weight = 1;
    for (let o = 0; o < octaves; o += 1) {
      let n = 1 - Math.abs(this.n2(x * freq + o * 31.7, y * freq + o * 11.3));
      n *= n;
      n *= weight;
      weight = Math.min(1, n * 2);
      sum += n * amp;
      norm += amp;
      amp *= 0.5;
      freq *= 2.1;
    }
    return sum / norm;
  }
}
