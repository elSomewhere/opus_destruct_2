/**
 * Chunked voxel storage for the mock engine: one block byte per voxel in 32^3 chunks,
 * allocated lazily (all-air chunks cost nothing). Grid coordinates are integer voxel
 * indices; world position of voxel (x,y,z)'s minimum corner is origin + (x,y,z)*h.
 */
import type { Aabb, Vec3 } from '../../engine/protocol.ts';

export const CHUNK_SIZE = 32;
export const CHUNK_SHIFT = 5;
export const CHUNK_MASK = CHUNK_SIZE - 1;
export const CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

/** Block id 0 is air; other ids index the block palette (blocks.ts). */
export const AIR = 0;

export class VoxelWorld {
  /** Size in voxels (multiples of CHUNK_SIZE). */
  readonly nx: number;
  readonly ny: number;
  readonly nz: number;
  /** Size in chunks. */
  readonly ncx: number;
  readonly ncy: number;
  readonly ncz: number;
  /** Voxel pitch in metres. */
  readonly h: number;
  /** World position of the grid's minimum corner. */
  readonly origin: Vec3;
  /** Block reported below the grid (z < 0), so the underside of the world is never meshed. */
  readonly belowBlock: number;

  private readonly chunks: (Uint8Array | null)[];
  private readonly solidPerChunk: Int32Array;
  /** Per column: 1 + highest solid z, or 0 when the column is empty (sky exposure). */
  private readonly top: Int16Array;
  private solid = 0;

  constructor(sizeVoxels: Vec3, h: number, origin: Vec3, belowBlock: number) {
    this.ncx = Math.ceil(sizeVoxels[0] / CHUNK_SIZE);
    this.ncy = Math.ceil(sizeVoxels[1] / CHUNK_SIZE);
    this.ncz = Math.ceil(sizeVoxels[2] / CHUNK_SIZE);
    this.nx = this.ncx * CHUNK_SIZE;
    this.ny = this.ncy * CHUNK_SIZE;
    this.nz = this.ncz * CHUNK_SIZE;
    if (this.nz > 0x7fff) throw new Error('world too tall');
    this.h = h;
    this.origin = [origin[0], origin[1], origin[2]];
    this.belowBlock = belowBlock;
    const n = this.ncx * this.ncy * this.ncz;
    this.chunks = new Array<Uint8Array | null>(n).fill(null);
    this.solidPerChunk = new Int32Array(n);
    this.top = new Int16Array(this.nx * this.ny);
  }

  get chunkCount(): number {
    return this.chunks.length;
  }

  /** Total solid voxels. */
  get solidCount(): number {
    return this.solid;
  }

  get bounds(): Aabb {
    const { origin: o, h } = this;
    return { min: [o[0], o[1], o[2]], max: [o[0] + this.nx * h, o[1] + this.ny * h, o[2] + this.nz * h] };
  }

  inBounds(x: number, y: number, z: number): boolean {
    return x >= 0 && y >= 0 && z >= 0 && x < this.nx && y < this.ny && z < this.nz;
  }

  chunkIndex(cx: number, cy: number, cz: number): number {
    return cx + this.ncx * (cy + this.ncy * cz);
  }

  chunkCoords(ci: number): Vec3 {
    const cx = ci % this.ncx;
    const r = (ci - cx) / this.ncx;
    const cy = r % this.ncy;
    return [cx, cy, (r - cy) / this.ncy];
  }

  chunkKey(ci: number): string {
    const [cx, cy, cz] = this.chunkCoords(ci);
    return `${cx},${cy},${cz}`;
  }

  chunkOrigin(ci: number): Vec3 {
    const [cx, cy, cz] = this.chunkCoords(ci);
    const s = CHUNK_SIZE * this.h;
    return [this.origin[0] + cx * s, this.origin[1] + cy * s, this.origin[2] + cz * s];
  }

  /** Block array of a chunk (index x + 32*y + 1024*z), or null when all air. */
  chunkData(ci: number): Uint8Array | null {
    return this.chunks[ci] ?? null;
  }

  chunkSolid(ci: number): number {
    return this.solidPerChunk[ci] ?? 0;
  }

  get(x: number, y: number, z: number): number {
    if (z < 0) return x >= 0 && y >= 0 && x < this.nx && y < this.ny ? this.belowBlock : AIR;
    if (x < 0 || y < 0 || x >= this.nx || y >= this.ny || z >= this.nz) return AIR;
    const c = this.chunks[(x >> CHUNK_SHIFT) + this.ncx * ((y >> CHUNK_SHIFT) + this.ncy * (z >> CHUNK_SHIFT))];
    return c ? c[(x & CHUNK_MASK) | ((y & CHUNK_MASK) << 5) | ((z & CHUNK_MASK) << 10)]! : AIR;
  }

  /** Sets a voxel; returns the previous block. Out-of-bounds writes are ignored. */
  set(x: number, y: number, z: number, block: number): number {
    if (!this.inBounds(x, y, z)) return AIR;
    const ci = (x >> CHUNK_SHIFT) + this.ncx * ((y >> CHUNK_SHIFT) + this.ncy * (z >> CHUNK_SHIFT));
    let c = this.chunks[ci];
    if (!c) {
      if (block === AIR) return AIR;
      c = new Uint8Array(CHUNK_VOLUME);
      this.chunks[ci] = c;
    }
    const li = (x & CHUNK_MASK) | ((y & CHUNK_MASK) << 5) | ((z & CHUNK_MASK) << 10);
    const prev = c[li]!;
    if (prev === block) return prev;
    c[li] = block;
    const col = x + this.nx * y;
    if (prev === AIR) {
      this.solid++;
      this.solidPerChunk[ci]!++;
      if (z >= this.top[col]!) this.top[col] = z + 1;
    } else if (block === AIR) {
      this.solid--;
      const left = --this.solidPerChunk[ci]!;
      if (left === 0) this.chunks[ci] = null;
      if (z + 1 === this.top[col]) {
        let t = z;
        while (t > 0 && this.get(x, y, t - 1) === AIR) t--;
        this.top[col] = t;
      }
    }
    return prev;
  }

  /** Fills the half-open box [x0,x1) x [y0,y1) x [z0,z1) (clipped to the grid). */
  fillBox(x0: number, y0: number, z0: number, x1: number, y1: number, z1: number, block: number): void {
    const ax = Math.max(0, x0);
    const ay = Math.max(0, y0);
    const az = Math.max(0, z0);
    const bx = Math.min(this.nx, x1);
    const by = Math.min(this.ny, y1);
    const bz = Math.min(this.nz, z1);
    for (let z = az; z < bz; z++) for (let y = ay; y < by; y++) for (let x = ax; x < bx; x++) this.set(x, y, z, block);
  }

  /** 1 + highest solid z of a column (0 if empty). */
  columnTop(x: number, y: number): number {
    if (x < 0 || y < 0 || x >= this.nx || y >= this.ny) return 0;
    return this.top[x + this.nx * y]!;
  }

  /** Packs grid coordinates into one integer (< 2^31 for any grid this class accepts). */
  pack(x: number, y: number, z: number): number {
    return x + this.nx * (y + this.ny * z);
  }

  unpackX(i: number): number {
    return i % this.nx;
  }

  unpackY(i: number): number {
    return Math.floor(i / this.nx) % this.ny;
  }

  unpackZ(i: number): number {
    return Math.floor(i / (this.nx * this.ny));
  }

  /** World position of a voxel's centre. */
  voxelCenter(x: number, y: number, z: number): Vec3 {
    const { origin: o, h } = this;
    return [o[0] + (x + 0.5) * h, o[1] + (y + 0.5) * h, o[2] + (z + 0.5) * h];
  }

  /** Continuous grid coordinates of a world position. */
  toGrid(p: Vec3): Vec3 {
    const { origin: o, h } = this;
    return [(p[0] - o[0]) / h, (p[1] - o[1]) / h, (p[2] - o[2]) / h];
  }

  /** Chunk indices whose block data changes when voxel (x,y,z) changes (itself + face neighbours across borders). */
  affectedChunks(x: number, y: number, z: number, out: Set<number>): void {
    const cx = x >> CHUNK_SHIFT;
    const cy = y >> CHUNK_SHIFT;
    const cz = z >> CHUNK_SHIFT;
    out.add(this.chunkIndex(cx, cy, cz));
    // Faces (and AO of faces) in neighbouring chunks depend on voxels up to one cell away.
    const lx = x & CHUNK_MASK;
    const ly = y & CHUNK_MASK;
    const lz = z & CHUNK_MASK;
    const dxs = lx === 0 ? [-1, 0] : lx === CHUNK_MASK ? [0, 1] : [0];
    const dys = ly === 0 ? [-1, 0] : ly === CHUNK_MASK ? [0, 1] : [0];
    const dzs = lz === 0 ? [-1, 0] : lz === CHUNK_MASK ? [0, 1] : [0];
    for (const dz of dzs) {
      const z2 = cz + dz;
      if (z2 < 0 || z2 >= this.ncz) continue;
      for (const dy of dys) {
        const y2 = cy + dy;
        if (y2 < 0 || y2 >= this.ncy) continue;
        for (const dx of dxs) {
          const x2 = cx + dx;
          if (x2 < 0 || x2 >= this.ncx) continue;
          out.add(this.chunkIndex(x2, y2, z2));
        }
      }
    }
  }

  /** Bytes of block storage currently allocated. */
  get allocatedBytes(): number {
    let n = 0;
    for (const c of this.chunks) if (c) n += c.byteLength;
    return n + this.top.byteLength + this.solidPerChunk.byteLength;
  }
}
