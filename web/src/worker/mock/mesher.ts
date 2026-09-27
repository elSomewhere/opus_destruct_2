/**
 * Greedy (or plain culled-face) mesher producing the 28-byte protocol vertex format.
 *
 * Input is a padded block array: the voxels to mesh plus a one-voxel border of their
 * neighbours, so face culling and ambient occlusion never need lookups outside it. The
 * same code meshes world chunks (32^3 + border) and detached islands (their bounding box).
 *
 * Greedy merging only joins faces whose texture, light, debug byte and four corner AO
 * values all match, so merged quads look identical to the unmerged ones.
 */
import type { Vec3 } from '../../engine/protocol.ts';
import type { MeshBuilder } from '../../engine/vertex.ts';
import { FACE_COUNT } from './blocks.ts';
import { AIR, CHUNK_SIZE, type VoxelWorld } from './world.ts';

export interface PaddedBlock {
  /** Block ids, size (sx+2)*(sy+2)*(sz+2); inner voxel (x,y,z) is at (x+1) + px*((y+1) + py*(z+1)). */
  blocks: Uint8Array;
  sx: number;
  sy: number;
  sz: number;
  /** World position of inner voxel (0,0,0)'s minimum corner. */
  origin: Vec3;
  /** Grid coordinates of inner voxel (0,0,0), passed to the light/debug callbacks. */
  grid: Vec3;
  /** Voxel pitch in metres. */
  h: number;
}

export interface MeshStyle {
  /** Per block and face (+x,-x,+y,-y,+z,-z): vertex texture id (see blocks.ts). */
  faceTextures: Uint16Array;
  texelsPerMetre: number;
  /** Light 0..255 of the air cell in front of a face, by grid coordinates. */
  light: (gx: number, gy: number, gz: number) => number;
  /** Debug byte of a solid voxel by grid coordinates; null = 0 everywhere. */
  debug: ((gx: number, gy: number, gz: number) => number) | null;
  /** false = one quad per exposed face (no T-junctions; needed for displaced meshes). */
  greedy: boolean;
}

/** Ambient occlusion level 0 (dark) .. 3 (open) of a face corner. */
function cornerAo(side1: number, side2: number, corner: number): number {
  return side1 && side2 ? 0 : 3 - (side1 + side2 + corner);
}

let maskScratch = new Float64Array(0);

function scratchMask(n: number): Float64Array {
  if (maskScratch.length < n) maskScratch = new Float64Array(n);
  return maskScratch;
}

/**
 * Appends the surface of `src` to `out`. Returns the number of quads emitted.
 */
export function meshBlock(src: PaddedBlock, style: MeshStyle, out: MeshBuilder): number {
  const { blocks, sx, sy, sz, h } = src;
  const px = sx + 2;
  const py = sy + 2;
  const dims = [sx, sy, sz];
  const strides = [1, px, px * py];
  const [ox, oy, oz] = src.origin;
  const [gx0, gy0, gz0] = src.grid;
  const tpm = style.texelsPerMetre;
  const faceTex = style.faceTextures;
  const lightFn = style.light;
  const debugFn = style.debug;
  let quads = 0;

  const coord = [0, 0, 0];
  const solid = (i: number): number => (blocks[i] !== AIR ? 1 : 0);

  for (let f = 0; f < FACE_COUNT; f++) {
    const d = f >> 1;
    const s = (f & 1) === 0 ? 1 : -1;
    const u = (d + 1) % 3;
    const v = (d + 2) % 3;
    const nd = dims[d]!;
    const nu = dims[u]!;
    const nv = dims[v]!;
    const sd = strides[d]!;
    const su = strides[u]!;
    const sv = strides[v]!;
    const no = s * sd;
    const mask = scratchMask(nu * nv);
    const nrm: Vec3 = [0, 0, 0];
    nrm[d] = s;

    for (let q = 0; q < nd; q++) {
      coord[d] = q;
      // 1. Build the face mask of this slice.
      let any = false;
      for (let iv = 0; iv < nv; iv++) {
        coord[v] = iv;
        for (let iu = 0; iu < nu; iu++) {
          coord[u] = iu;
          const pi = coord[0]! + 1 + (coord[1]! + 1) * px + (coord[2]! + 1) * px * py;
          const b = blocks[pi]!;
          const m = iu + iv * nu;
          if (b === AIR || blocks[pi + no] !== AIR) {
            mask[m] = 0;
            continue;
          }
          const ai = pi + no;
          const um = solid(ai - su);
          const up = solid(ai + su);
          const vm = solid(ai - sv);
          const vp = solid(ai + sv);
          const a00 = cornerAo(um, vm, solid(ai - su - sv));
          const a10 = cornerAo(up, vm, solid(ai + su - sv));
          const a11 = cornerAo(up, vp, solid(ai + su + sv));
          const a01 = cornerAo(um, vp, solid(ai - su + sv));
          const ao = a00 | (a10 << 2) | (a11 << 4) | (a01 << 6);
          const gx = gx0 + coord[0]!;
          const gy = gy0 + coord[1]!;
          const gz = gz0 + coord[2]!;
          const light = lightFn(gx + nrm[0], gy + nrm[1], gz + nrm[2]) & 0xff;
          const dbg = debugFn ? debugFn(gx, gy, gz) & 0xff : 0;
          const tex = faceTex[b * FACE_COUNT + f]!;
          // < 2^40: exact in a double, compared with ===.
          mask[m] = ((tex * 256 + light) * 256 + dbg) * 256 + ao + 1;
          any = true;
        }
      }
      if (!any) continue;

      // 2. Merge equal keys into rectangles and emit them.
      const plane = q + (s > 0 ? 1 : 0);
      for (let iv = 0; iv < nv; iv++) {
        for (let iu = 0; iu < nu; ) {
          const key = mask[iu + iv * nu]!;
          if (key === 0) {
            iu++;
            continue;
          }
          let w = 1;
          let hgt = 1;
          if (style.greedy) {
            while (iu + w < nu && mask[iu + w + iv * nu] === key) w++;
            grow: while (iv + hgt < nv) {
              const row = (iv + hgt) * nu;
              for (let k = 0; k < w; k++) if (mask[iu + k + row] !== key) break grow;
              hgt++;
            }
          }
          for (let r = 0; r < hgt; r++) mask.fill(0, iu + (iv + r) * nu, iu + w + (iv + r) * nu);
          emitQuad(out, key - 1, d, s, u, v, plane, iu, iv, w, hgt, nrm, ox, oy, oz, h, tpm);
          quads++;
          iu += w;
        }
      }
    }
  }
  return quads;
}

const cornerU = [0, 1, 1, 0];
const cornerV = [0, 0, 1, 1];

function emitQuad(
  out: MeshBuilder,
  packed: number,
  d: number,
  s: number,
  u: number,
  v: number,
  plane: number,
  u0: number,
  v0: number,
  w: number,
  hgt: number,
  nrm: Vec3,
  ox: number,
  oy: number,
  oz: number,
  h: number,
  tpm: number,
): void {
  const ao = packed % 256;
  let rest = (packed - ao) / 256;
  const dbg = rest % 256;
  rest = (rest - dbg) / 256;
  const light = rest % 256;
  const tex = (rest - light) / 256;
  const aoAt = [ao & 3, (ao >> 2) & 3, (ao >> 4) & 3, (ao >> 6) & 3];
  const p = [0, 0, 0];
  const idx = [0, 0, 0, 0];
  // Seen from outside, corners (u0,v0) (u1,v0) (u1,v1) (u0,v1) are CCW for +normal because
  // e_u x e_v = e_d; reverse them for -normal.
  for (let k = 0; k < 4; k++) {
    const c = s > 0 ? k : (4 - k) % 4;
    p[d] = plane;
    p[u] = u0 + cornerU[c]! * w;
    p[v] = v0 + cornerV[c]! * hgt;
    const wx = ox + p[0]! * h;
    const wy = oy + p[1]! * h;
    const wz = oz + p[2]! * h;
    // Texel coordinates: horizontal axis = "right" when looking at the face from outside
    // with z up; vertical axis = -z so textures stay upright (row 0 is the top).
    let tu: number;
    let tv: number;
    if (d === 0) {
      tu = s * wy;
      tv = -wz;
    } else if (d === 1) {
      tu = -s * wx;
      tv = -wz;
    } else {
      tu = wx;
      tv = s > 0 ? -wy : wy;
    }
    idx[k] = out.vertex(wx, wy, wz, nrm[0], nrm[1], nrm[2], aoAt[c]! / 3, tu * tpm, tv * tpm, tex, light, dbg);
  }
  // Split along the diagonal that interpolates AO symmetrically (0fps "ambient occlusion
  // for Minecraft-like worlds"). idx[0..3] map to corners c0..c3 or c0,c3,c2,c1; either way
  // idx[0]-idx[2] is the c0-c2 diagonal.
  const c0 = aoAt[0]!;
  const c1 = aoAt[1]!;
  const c2 = aoAt[2]!;
  const c3 = aoAt[3]!;
  if (c0 + c2 > c1 + c3) {
    out.triangle(idx[1]!, idx[2]!, idx[3]!);
    out.triangle(idx[1]!, idx[3]!, idx[0]!);
  } else {
    out.triangle(idx[0]!, idx[1]!, idx[2]!);
    out.triangle(idx[0]!, idx[2]!, idx[3]!);
  }
}

const PADDED = CHUNK_SIZE + 2;

/** Copies chunk `ci` plus a one-voxel border into `out` (length (32+2)^3). */
export function fillPaddedChunk(world: VoxelWorld, ci: number, out: Uint8Array): PaddedBlock {
  const [cx, cy, cz] = world.chunkCoords(ci);
  const gx = cx * CHUNK_SIZE;
  const gy = cy * CHUNK_SIZE;
  const gz = cz * CHUNK_SIZE;
  const data = world.chunkData(ci);
  const P2 = PADDED * PADDED;
  for (let z = -1; z <= CHUNK_SIZE; z++) {
    for (let y = -1; y <= CHUNK_SIZE; y++) {
      const row = (y + 1) * PADDED + (z + 1) * P2;
      const inner = z >= 0 && z < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE;
      if (inner) {
        // Interior row: one bulk copy plus the two border voxels.
        out[row] = world.get(gx - 1, gy + y, gz + z);
        if (data) {
          const src = (y << 5) | (z << 10);
          out.set(data.subarray(src, src + CHUNK_SIZE), row + 1);
        } else {
          out.fill(AIR, row + 1, row + 1 + CHUNK_SIZE);
        }
        out[row + PADDED - 1] = world.get(gx + CHUNK_SIZE, gy + y, gz + z);
      } else {
        for (let x = -1; x <= CHUNK_SIZE; x++) out[row + x + 1] = world.get(gx + x, gy + y, gz + z);
      }
    }
  }
  const h = world.h;
  return {
    blocks: out,
    sx: CHUNK_SIZE,
    sy: CHUNK_SIZE,
    sz: CHUNK_SIZE,
    origin: [world.origin[0] + gx * h, world.origin[1] + gy * h, world.origin[2] + gz * h],
    grid: [gx, gy, gz],
    h,
  };
}

export const PADDED_CHUNK_VOLUME = PADDED * PADDED * PADDED;
