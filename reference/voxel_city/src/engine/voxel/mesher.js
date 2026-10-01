import { MATERIALS, IS_TRANSPARENT, MAX_MATERIALS } from "./materials.js";
import { P, P2 } from "./chunk.js";
import { CHUNK } from "../core/units.js";

/**
 * Greedy mesher with baked per-vertex ambient occlusion.
 *
 * Input: padded chunk (34^3 Uint16). Output: two meshes (opaque, transparent)
 * in a compact vertex format:
 *   position Uint8x3   – local voxel corner coords 0..32
 *   normal   Int8x3    – axis normal
 *   color    Uint8x3   – material base color
 *   aux      Uint8x4   – [ao 0..255, emissive 0..255, noise amp 0..255, opacity 0..255]
 * The renderer adds per-voxel value noise in the fragment shader from world
 * position so merged faces still read as individual voxels.
 */

const MAT_R = new Uint8Array(MAX_MATERIALS);
const MAT_G = new Uint8Array(MAX_MATERIALS);
const MAT_B = new Uint8Array(MAX_MATERIALS);
const MAT_E = new Uint8Array(MAX_MATERIALS);
export const GLOW_CODE = 3;
const MAT_N = new Uint8Array(MAX_MATERIALS);
const MAT_O = new Uint8Array(MAX_MATERIALS);
for (const m of MATERIALS) {
  MAT_O[m.id] = Math.round(m.opacity * 255);
  MAT_R[m.id] = m.rgb[0];
  MAT_G[m.id] = m.rgb[1];
  MAT_B[m.id] = m.rgb[2];
  // emissive level; the reserved code 3 flags night-glow window glass
  MAT_E[m.id] = m.glow ? GLOW_CODE : Math.round(m.emissive * 255);
  MAT_N[m.id] = Math.round(Math.min(1, m.noise * 4) * 255);
}

const AO_CURVE = [0.5, 0.68, 0.84, 1.0];

class MeshBuilder {
  constructor(capacityQuads = 1024) {
    this.quads = 0;
    this.alloc(capacityQuads);
  }

  alloc(cap) {
    const pos = new Uint8Array(cap * 4 * 3);
    const nor = new Int8Array(cap * 4 * 3);
    const col = new Uint8Array(cap * 4 * 3);
    const aux = new Uint8Array(cap * 4 * 4);
    const flips = new Uint8Array(cap);
    if (this.pos) {
      pos.set(this.pos);
      nor.set(this.nor);
      col.set(this.col);
      aux.set(this.aux);
      flips.set(this.flips);
    }
    this.flips = flips;
    this.pos = pos;
    this.nor = nor;
    this.col = col;
    this.aux = aux;
    this.cap = cap;
  }

  /**
   * corners: 4 x [x,y,z]; ao: 4 levels 0..3; flip chooses the diagonal.
   */
  quad(c, nx, ny, nz, mat, ao, flip) {
    if (this.quads >= this.cap) this.alloc(this.cap * 2);
    const q = this.quads;
    const vb = q * 4;
    for (let v = 0; v < 4; v += 1) {
      const vi = vb + v;
      this.pos[vi * 3] = c[v * 3];
      this.pos[vi * 3 + 1] = c[v * 3 + 1];
      this.pos[vi * 3 + 2] = c[v * 3 + 2];
      this.nor[vi * 3] = nx * 127;
      this.nor[vi * 3 + 1] = ny * 127;
      this.nor[vi * 3 + 2] = nz * 127;
      this.col[vi * 3] = MAT_R[mat];
      this.col[vi * 3 + 1] = MAT_G[mat];
      this.col[vi * 3 + 2] = MAT_B[mat];
      this.aux[vi * 4] = Math.round(AO_CURVE[ao[v]] * 255);
      this.aux[vi * 4 + 1] = MAT_E[mat];
      this.aux[vi * 4 + 2] = MAT_N[mat];
      this.aux[vi * 4 + 3] = MAT_O[mat];
    }
    this.flips[q] = flip ? 1 : 0;
    this.quads += 1;
  }

  finish() {
    const n = this.quads;
    const vcount = n * 4;
    const indices = vcount > 65535 ? new Uint32Array(n * 6) : new Uint16Array(n * 6);
    for (let q = 0; q < n; q += 1) {
      const b = q * 4;
      const flip = this.flips[q] === 1;
      const o = q * 6;
      if (!flip) {
        indices[o] = b;
        indices[o + 1] = b + 1;
        indices[o + 2] = b + 2;
        indices[o + 3] = b;
        indices[o + 4] = b + 2;
        indices[o + 5] = b + 3;
      } else {
        indices[o] = b + 1;
        indices[o + 1] = b + 2;
        indices[o + 2] = b + 3;
        indices[o + 3] = b + 1;
        indices[o + 4] = b + 3;
        indices[o + 5] = b;
      }
    }
    return {
      quads: n,
      position: this.pos.slice(0, vcount * 3),
      normal: this.nor.slice(0, vcount * 3),
      color: this.col.slice(0, vcount * 3),
      aux: this.aux.slice(0, vcount * 4),
      index: indices,
    };
  }
}

const maskBuf = new Int32Array(CHUNK * CHUNK);

/** Is there air within three voxels above padded index idx (same column)? */
function nearSurface(data, idx) {
  for (let k = 1; k <= 3; k += 1) {
    const j = idx + k * P2;
    if (j >= data.length) return true;
    const m = data[j];
    if (m === 0 || IS_TRANSPARENT[m] === 1) return true;
  }
  return false;
}

/**
 * @param {Uint16Array} data padded chunk data
 * @param {{skirt?: boolean}} opts skirt: close the tile's side walls (the
 *        apron is treated as air on x/y borders), which hides cracks next to
 *        neighbours of a different LOD; the faces are invisible otherwise
 * @returns {{opaque, transparent}} mesh payloads (null when empty)
 */
export function meshChunk(data, opts = {}) {
  const skirt = !!opts.skirt;
  const opaque = new MeshBuilder(2048);
  const transparent = new MeshBuilder(128);
  const mask = maskBuf;
  const stride = [1, P, P2];
  const corners = new Float32Array(12);
  const ao = [3, 3, 3, 3];

  const isOpaque = (idx) => {
    const m = data[idx];
    return m !== 0 && IS_TRANSPARENT[m] === 0;
  };

  for (let d = 0; d < 3; d += 1) {
    const u = (d + 1) % 3;
    const v = (d + 2) % 3;
    const sd = stride[d];
    const su = stride[u];
    const sv = stride[v];

    for (let pass = 0; pass < 2; pass += 1) {
      const builder = pass === 0 ? opaque : transparent;
      // plane t lies between padded layer t and t+1 along d
      for (let t = 0; t <= CHUNK; t += 1) {
        let n = 0;
        let any = false;
        for (let jv = 1; jv <= CHUNK; jv += 1) {
          for (let iu = 1; iu <= CHUNK; iu += 1, n += 1) {
            const ia = t * sd + iu * su + jv * sv;
            const ib = ia + sd;
            const a = data[ia];
            const b = data[ib];
            let val = 0;
            if (pass === 0) {
              // skirts only near the surface: a border voxel with air within
              // three voxels above it in its own column
              const edge = skirt && d < 2;
              const aO = a !== 0 && IS_TRANSPARENT[a] === 0 && !(edge && t === 0 && nearSurface(data, ib));
              const bO = b !== 0 && IS_TRANSPARENT[b] === 0 && !(edge && t === CHUNK && nearSurface(data, ia));
              if (aO && !bO && t >= 1) {
                // + face of a; AO sampled in layer b
                val = packMask(a, aoPack(data, ib, su, sv, isOpaque), 1);
              } else if (bO && !aO && t + 1 <= CHUNK) {
                val = packMask(b, aoPack(data, ia, su, sv, isOpaque), 0);
              }
            } else {
              const aT = a !== 0 && IS_TRANSPARENT[a] === 1;
              const bT = b !== 0 && IS_TRANSPARENT[b] === 1;
              if (aT && b === 0 && t >= 1) val = packMask(a, 0xff, 1);
              else if (bT && a === 0 && t + 1 <= CHUNK) val = packMask(b, 0xff, 0);
              else if (aT && bT && a !== b && t >= 1) val = packMask(a, 0xff, 1);
            }
            mask[n] = val;
            if (val) any = true;
          }
        }
        if (!any) continue;

        // greedy merge
        n = 0;
        for (let j = 0; j < CHUNK; j += 1) {
          for (let i = 0; i < CHUNK; ) {
            const key = mask[n];
            if (!key) {
              i += 1;
              n += 1;
              continue;
            }
            let w = 1;
            while (i + w < CHUNK && mask[n + w] === key) w += 1;
            let h = 1;
            outer: for (; j + h < CHUNK; h += 1) {
              const row = n + h * CHUNK;
              for (let k = 0; k < w; k += 1) {
                if (mask[row + k] !== key) break outer;
              }
            }
            const raw = key - 1;
            const positive = (raw & 1) === 1;
            const aoBits = (raw >> 1) & 0xff;
            const mat = raw >> 9;
            ao[0] = aoBits & 3;
            ao[1] = (aoBits >> 2) & 3;
            ao[2] = (aoBits >> 4) & 3;
            ao[3] = (aoBits >> 6) & 3;

            // plane coordinate in local (unpadded) space is t (between t and t+1 padded -> local t)
            const pd = t;
            const pu0 = i;
            const pv0 = j;
            const pu1 = i + w;
            const pv1 = j + h;
            setCorner(corners, 0, d, u, v, pd, pu0, pv0);
            setCorner(corners, 1, d, u, v, pd, pu1, pv0);
            setCorner(corners, 2, d, u, v, pd, pu1, pv1);
            setCorner(corners, 3, d, u, v, pd, pu0, pv1);

            const nx = d === 0 ? (positive ? 1 : -1) : 0;
            const ny = d === 1 ? (positive ? 1 : -1) : 0;
            const nz = d === 2 ? (positive ? 1 : -1) : 0;
            const flip = ao[0] + ao[2] < ao[1] + ao[3];
            if (positive) {
              builder.quad(corners, nx, ny, nz, mat, ao, flip);
            } else {
              // reverse winding: 0,3,2,1
              const c = corners;
              const r = [c[0], c[1], c[2], c[9], c[10], c[11], c[6], c[7], c[8], c[3], c[4], c[5]];
              const aor = [ao[0], ao[3], ao[2], ao[1]];
              builder.quad(r, nx, ny, nz, mat, aor, aor[0] + aor[2] < aor[1] + aor[3]);
            }

            for (let hh = 0; hh < h; hh += 1) {
              const row = n + hh * CHUNK;
              for (let k = 0; k < w; k += 1) mask[row + k] = 0;
            }
            i += w;
            n += w;
          }
        }
      }
    }
  }

  return {
    opaque: opaque.quads ? opaque.finish() : null,
    transparent: transparent.quads ? transparent.finish() : null,
  };
}

function setCorner(out, idx, d, u, v, pd, pu, pv) {
  const o = idx * 3;
  const p = [0, 0, 0];
  p[d] = pd;
  p[u] = pu;
  p[v] = pv;
  out[o] = p[0];
  out[o + 1] = p[1];
  out[o + 2] = p[2];
}

function packMask(mat, aoBits, positive) {
  return ((mat << 9) | (aoBits << 1) | positive) + 1;
}

/** AO for the 4 quad corners, sampled in the air layer at padded index `air`. */
function aoPack(data, air, su, sv, isOpaque) {
  const s_u0 = isOpaque(air - su) ? 1 : 0;
  const s_u1 = isOpaque(air + su) ? 1 : 0;
  const s_v0 = isOpaque(air - sv) ? 1 : 0;
  const s_v1 = isOpaque(air + sv) ? 1 : 0;
  const c00 = isOpaque(air - su - sv) ? 1 : 0;
  const c10 = isOpaque(air + su - sv) ? 1 : 0;
  const c11 = isOpaque(air + su + sv) ? 1 : 0;
  const c01 = isOpaque(air - su + sv) ? 1 : 0;
  const a0 = s_u0 && s_v0 ? 0 : 3 - (s_u0 + s_v0 + c00);
  const a1 = s_u1 && s_v0 ? 0 : 3 - (s_u1 + s_v0 + c10);
  const a2 = s_u1 && s_v1 ? 0 : 3 - (s_u1 + s_v1 + c11);
  const a3 = s_u0 && s_v1 ? 0 : 3 - (s_u0 + s_v1 + c01);
  return a0 | (a1 << 2) | (a2 << 4) | (a3 << 6);
}
