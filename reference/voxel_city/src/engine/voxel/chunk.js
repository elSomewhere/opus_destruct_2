import { CHUNK, PCHUNK } from "../core/units.js";

/**
 * A padded voxel chunk at some LOD, plus a writer API expressed entirely in
 * LOD0 world voxel coordinates. Generators never think about LOD: they write
 * boxes / points / columns in world voxels and the writer point-samples them
 * at the chunk's resolution (a coarse voxel takes the value of its
 * representative fine voxel). This is what lets one generator feed every LOD.
 *
 * Layout: PCHUNK^3 (34^3) Uint16, index = i + j*P + k*P*P, one voxel apron on
 * every side so the mesher sees neighbours without touching other chunks.
 */
export const P = PCHUNK;
export const P2 = P * P;
export const P3 = P * P * P;

export class ChunkBuffer {
  constructor(lod, cx, cy, cz) {
    this.lod = lod;
    this.cx = cx;
    this.cy = cy;
    this.cz = cz;
    const s = 1 << lod;
    this.s = s;
    this.half = s >> 1;
    // LOD0 coordinate of the start of padded voxel 0 on each axis
    this.bx = (cx * CHUNK - 1) * s;
    this.by = (cy * CHUNK - 1) * s;
    this.bz = (cz * CHUNK - 1) * s;
    this.data = new Uint16Array(P3);
    this.nonAir = 0;
    // (the structvox export, svx/source.js: `iso` records the material written
    // while `isolating` is on - props and furniture, which a physics host
    // writes as isolated voxels after generation; off, it costs nothing)
    this.iso = null;
    this.isolating = false;
  }

  /** Record isolated voxels from here on (the structvox export). */
  trackIsolated() {
    this.iso = new Uint16Array(P3);
    return this;
  }

  /** LOD0 world bounds covered by the padded buffer (inclusive). */
  get worldBox() {
    const e = P * this.s - 1;
    return {
      x0: this.bx,
      y0: this.by,
      z0: this.bz,
      x1: this.bx + e,
      y1: this.by + e,
      z1: this.bz + e,
    };
  }

  /** Representative LOD0 coordinate of padded index i on x/y/z. */
  wx(i) {
    return this.bx + i * this.s + this.half;
  }

  wy(j) {
    return this.by + j * this.s + this.half;
  }

  wz(k) {
    return this.bz + k * this.s + this.half;
  }

  /** Padded index range [lo, hi] whose representatives fall in [a, b]. */
  rangeX(a, b) {
    return idxRange(a, b, this.bx + this.half, this.s);
  }

  rangeY(a, b) {
    return idxRange(a, b, this.by + this.half, this.s);
  }

  rangeZ(a, b) {
    return idxRange(a, b, this.bz + this.half, this.s);
  }

  get(i, j, k) {
    return this.data[i + j * P + k * P2];
  }

  /** Material at a LOD0 world coordinate (nearest representative), 0 outside. */
  sampleWorld(x, y, z) {
    const i = Math.floor((x - this.bx) / this.s);
    const j = Math.floor((y - this.by) / this.s);
    const k = Math.floor((z - this.bz) / this.s);
    if (i < 0 || j < 0 || k < 0 || i >= P || j >= P || k >= P) return 0;
    return this.data[i + j * P + k * P2];
  }

  /** Write a single LOD0 voxel (only lands if it is a representative). */
  set(x, y, z, m) {
    const s = this.s;
    let i = x - this.bx - this.half;
    let j = y - this.by - this.half;
    let k = z - this.bz - this.half;
    if (s > 1) {
      if (i % s !== 0 || j % s !== 0 || k % s !== 0) return;
      i /= s;
      j /= s;
      k /= s;
    }
    if (i < 0 || j < 0 || k < 0 || i >= P || j >= P || k >= P) return;
    this.data[i + j * P + k * P2] = m;
  }

  /** Write only into air. */
  setIfAir(x, y, z, m) {
    const s = this.s;
    let i = x - this.bx - this.half;
    let j = y - this.by - this.half;
    let k = z - this.bz - this.half;
    if (s > 1) {
      if (i % s !== 0 || j % s !== 0 || k % s !== 0) return;
      i /= s;
      j /= s;
      k /= s;
    }
    if (i < 0 || j < 0 || k < 0 || i >= P || j >= P || k >= P) return;
    const idx = i + j * P + k * P2;
    if (this.data[idx] === 0) this.data[idx] = m;
  }

  /** Fill an inclusive LOD0 box. mode: 0 overwrite, 1 only-air, 2 only-solid. */
  fillBox(x0, y0, z0, x1, y1, z1, m, mode = 0) {
    const [i0, i1] = this.rangeX(x0, x1);
    if (i0 > i1) return;
    const [j0, j1] = this.rangeY(y0, y1);
    if (j0 > j1) return;
    const [k0, k1] = this.rangeZ(z0, z1);
    if (k0 > k1) return;
    const d = this.data;
    if (this.isolating && this.iso) {
      const iso = this.iso;
      for (let k = k0; k <= k1; k += 1)
        for (let j = j0; j <= j1; j += 1)
          for (let i = i0, idx = i0 + j * P + k * P2; i <= i1; i += 1, idx += 1)
            if (mode === 0 || (mode === 1 && d[idx] === 0) || (mode === 2 && d[idx] !== 0)) {
              d[idx] = m;
              iso[idx] = m;
            }
      return;
    }
    for (let k = k0; k <= k1; k += 1) {
      for (let j = j0; j <= j1; j += 1) {
        let idx = i0 + j * P + k * P2;
        if (mode === 0) {
          for (let i = i0; i <= i1; i += 1) d[idx++] = m;
        } else if (mode === 1) {
          for (let i = i0; i <= i1; i += 1, idx += 1) if (d[idx] === 0) d[idx] = m;
        } else {
          for (let i = i0; i <= i1; i += 1, idx += 1) if (d[idx] !== 0) d[idx] = m;
        }
      }
    }
  }

  /** Does the padded buffer's world box intersect the given LOD0 box? */
  touches(x0, y0, z0, x1, y1, z1) {
    const e = P * this.s - 1;
    return x1 >= this.bx && x0 <= this.bx + e && y1 >= this.by && y0 <= this.by + e && z1 >= this.bz && z0 <= this.bz + e;
  }

  touchesRect(r, z0, z1) {
    return this.touches(r.x0, r.y0, z0, r.x1, r.y1, z1);
  }

  countNonAir() {
    let n = 0;
    const d = this.data;
    for (let i = 0; i < d.length; i += 1) if (d[i] !== 0) n += 1;
    this.nonAir = n;
    return n;
  }
}

/** padded indices whose representative r = base + idx*s lies in [a, b] */
function idxRange(a, b, base, s) {
  let lo = Math.ceil((a - base) / s);
  let hi = Math.floor((b - base) / s);
  if (lo < 0) lo = 0;
  if (hi > P - 1) hi = P - 1;
  return [lo, hi];
}

/** World (LOD0) voxel box of a chunk's *core* (unpadded) region. */
export function chunkCoreBox(lod, cx, cy, cz) {
  const s = 1 << lod;
  const e = CHUNK * s;
  return {
    x0: cx * e,
    y0: cy * e,
    z0: cz * e,
    x1: cx * e + e - 1,
    y1: cy * e + e - 1,
    z1: cz * e + e - 1,
  };
}
