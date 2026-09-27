/**
 * Meshing of voxel models into the character vertex format (renderer-neutral, documented here
 * so any renderer can consume it).
 *
 * Character vertex: 20 bytes, little-endian, interleaved:
 *   0  float32x3  position in rest model space (metres)
 *   12 snorm8x4   normal xyz (axis-aligned in rest space), w = ambient occlusion (-1..1 = 0..1)
 *   16 uint32     bone (bits 0-7) | palette slot (bits 8-11) | shade (bits 12-19, 128 = 1.0)
 * Indices are uint32, counter-clockwise seen from outside.
 *
 * A vertex is drawn at skin[bone] * position (WorldPose.writeSkin): rigid skinning, so voxels
 * stay cubes. Every part is meshed on its own (faces between parts are kept), so parts can
 * rotate apart at their joints without opening holes into the body.
 */
import { partIndex, type VoxelModel, type VoxelPart } from './model.ts';

export const CHAR_VERTEX_STRIDE = 20;

export interface CharacterMesh {
  vertices: ArrayBuffer;
  vertexCount: number;
  indices: Uint32Array;
  indexCount: number;
}

/** Face directions: normal, and tangents u, v with u x v = normal (CCW quads). */
const FACES: readonly { n: [number, number, number]; u: [number, number, number]; v: [number, number, number] }[] = [
  { n: [1, 0, 0], u: [0, 1, 0], v: [0, 0, 1] },
  { n: [-1, 0, 0], u: [0, 0, 1], v: [0, 1, 0] },
  { n: [0, 1, 0], u: [0, 0, 1], v: [1, 0, 0] },
  { n: [0, -1, 0], u: [1, 0, 0], v: [0, 0, 1] },
  { n: [0, 0, 1], u: [1, 0, 0], v: [0, 1, 0] },
  { n: [0, 0, -1], u: [0, 1, 0], v: [1, 0, 0] },
];

class Builder {
  f32: Float32Array;
  u32: Uint32Array;
  i8: Int8Array;
  idx: Uint32Array;
  nv = 0;
  ni = 0;

  constructor(cap: number) {
    const buf = new ArrayBuffer(cap * CHAR_VERTEX_STRIDE);
    this.f32 = new Float32Array(buf);
    this.u32 = new Uint32Array(buf);
    this.i8 = new Int8Array(buf);
    this.idx = new Uint32Array(Math.ceil(cap * 1.5));
  }

  private grow(): void {
    const buf = new ArrayBuffer(this.f32.byteLength * 2);
    new Uint8Array(buf).set(new Uint8Array(this.f32.buffer));
    this.f32 = new Float32Array(buf);
    this.u32 = new Uint32Array(buf);
    this.i8 = new Int8Array(buf);
    const idx = new Uint32Array(this.idx.length * 2);
    idx.set(this.idx);
    this.idx = idx;
  }

  vertex(x: number, y: number, z: number, nx: number, ny: number, nz: number, ao: number, packed: number): number {
    if ((this.nv + 1) * CHAR_VERTEX_STRIDE > this.f32.byteLength) this.grow();
    const i = this.nv++;
    const f = i * 5;
    this.f32[f] = x;
    this.f32[f + 1] = y;
    this.f32[f + 2] = z;
    const b = i * CHAR_VERTEX_STRIDE + 12;
    this.i8[b] = nx * 127;
    this.i8[b + 1] = ny * 127;
    this.i8[b + 2] = nz * 127;
    this.i8[b + 3] = Math.round((ao * 2 - 1) * 127);
    this.u32[f + 4] = packed;
    return i;
  }

  tri(a: number, b: number, c: number): void {
    if (this.ni + 3 > this.idx.length) this.grow();
    this.idx[this.ni++] = a;
    this.idx[this.ni++] = b;
    this.idx[this.ni++] = c;
  }

  finish(): CharacterMesh {
    return {
      vertices: this.f32.buffer.slice(0, this.nv * CHAR_VERTEX_STRIDE) as ArrayBuffer,
      vertexCount: this.nv,
      indices: this.idx.slice(0, this.ni),
      indexCount: this.ni,
    };
  }
}

/** Packs the per-vertex word of the character vertex. */
export function packVertexWord(bone: number, slot: number, shade: number): number {
  return ((bone & 0xff) | ((slot & 0xf) << 8) | ((shade & 0xff) << 12)) >>> 0;
}

/**
 * Mesh of one part (culled faces with per-vertex AO). `bone` overrides the part's bone in the
 * vertices (gibs re-bind parts).
 */
export function meshPart(part: VoxelPart, voxelSize: number, bone = part.bone): CharacterMesh {
  const [nx, ny, nz] = part.dims;
  const b = new Builder(Math.max(64, part.count * 6));
  const cells = part.cells;
  const solid = (x: number, y: number, z: number): number =>
    x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz ? 0 : cells[x + nx * (y + ny * z)]! !== 0 ? 1 : 0;
  const s = voxelSize;
  const [ox, oy, oz] = part.origin;
  const ao: number[] = [0, 0, 0, 0];
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++)
      for (let x = 0; x < nx; x++) {
        const c = cells[x + nx * (y + ny * z)]!;
        if (c === 0) continue;
        const packed = packVertexWord(bone, c - 1, part.shade[partIndex(part, x, y, z)]!);
        for (const f of FACES) {
          const lx = x + f.n[0], ly = y + f.n[1], lz = z + f.n[2];
          if (solid(lx, ly, lz)) continue;
          // corner (du, dv) in CCW order: (0,0), (1,0), (1,1), (0,1)
          for (let q = 0; q < 4; q++) {
            const du = q === 1 || q === 2 ? 1 : -1;
            const dv = q >= 2 ? 1 : -1;
            const s1 = solid(lx + f.u[0] * du, ly + f.u[1] * du, lz + f.u[2] * du);
            const s2 = solid(lx + f.v[0] * dv, ly + f.v[1] * dv, lz + f.v[2] * dv);
            const cr = solid(lx + f.u[0] * du + f.v[0] * dv, ly + f.u[1] * du + f.v[1] * dv, lz + f.u[2] * du + f.v[2] * dv);
            ao[q] = s1 && s2 ? 0 : (3 - s1 - s2 - cr) / 3;
          }
          // base corner of the face: the cell's min corner, moved to the far side along +n
          const bx = x + (f.n[0] > 0 ? 1 : 0), by = y + (f.n[1] > 0 ? 1 : 0), bz = z + (f.n[2] > 0 ? 1 : 0);
          const v: number[] = [];
          for (let q = 0; q < 4; q++) {
            const du = q === 1 || q === 2 ? 1 : 0;
            const dv = q >= 2 ? 1 : 0;
            const px = (ox + bx + f.u[0] * du + f.v[0] * dv) * s;
            const py = (oy + by + f.u[1] * du + f.v[1] * dv) * s;
            const pz = (oz + bz + f.u[2] * du + f.v[2] * dv) * s;
            v.push(b.vertex(px, py, pz, f.n[0], f.n[1], f.n[2], ao[q]!, packed));
          }
          // split along the brighter diagonal (no AO anisotropy artefacts)
          if (ao[0]! + ao[2]! >= ao[1]! + ao[3]!) {
            b.tri(v[0]!, v[1]!, v[2]!);
            b.tri(v[0]!, v[2]!, v[3]!);
          } else {
            b.tri(v[1]!, v[2]!, v[3]!);
            b.tri(v[1]!, v[3]!, v[0]!);
          }
        }
      }
  return b.finish();
}

/** Concatenates meshes (indices rebased). */
export function mergeMeshes(meshes: readonly CharacterMesh[]): CharacterMesh {
  let nv = 0, ni = 0;
  for (const m of meshes) {
    nv += m.vertexCount;
    ni += m.indexCount;
  }
  const vbytes = new Uint8Array(nv * CHAR_VERTEX_STRIDE);
  const idx = new Uint32Array(ni);
  let vo = 0, io = 0;
  for (const m of meshes) {
    vbytes.set(new Uint8Array(m.vertices, 0, m.vertexCount * CHAR_VERTEX_STRIDE), vo * CHAR_VERTEX_STRIDE);
    for (let k = 0; k < m.indexCount; k++) idx[io + k] = m.indices[k]! + vo;
    vo += m.vertexCount;
    io += m.indexCount;
  }
  return { vertices: vbytes.buffer, vertexCount: nv, indices: idx, indexCount: ni };
}

/**
 * Meshes a model, reusing the cached meshes of parts whose version did not change. Pass the
 * same cache object for a model on every call (a model's damage only re-meshes the parts hit).
 */
export class ModelMesher {
  private readonly cache = new Map<VoxelPart, { version: number; mesh: CharacterMesh }>();

  mesh(model: VoxelModel, parts: readonly VoxelPart[] = model.parts): CharacterMesh {
    const meshes: CharacterMesh[] = [];
    for (const p of parts) {
      if (p.count === 0) continue;
      let c = this.cache.get(p);
      if (!c || c.version !== p.version) {
        c = { version: p.version, mesh: meshPart(p, model.voxelSize) };
        this.cache.set(p, c);
      }
      meshes.push(c.mesh);
    }
    return mergeMeshes(meshes);
  }
}
