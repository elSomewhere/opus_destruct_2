/**
 * The lab's test course: a small voxel level (the mock engine's storage and mesher) with flat
 * ground, stairs, a ramp, a platform, crates and low walls, for locomotion, foot placement,
 * ragdolls and gibs. It needs no engine: the lab runs svx_anim on its own.
 */
import type { ChunkMesh, Vec3 } from '../engine/protocol.ts';
import { DOOM_TEXELS_PER_METRE } from '../engine/protocol.ts';
import { MeshBuilder } from '../engine/vertex.ts';
import { Block, resolveFaceTextures } from '../worker/mock/blocks.ts';
import { fillPaddedChunk, meshBlock, PADDED_CHUNK_VOLUME } from '../worker/mock/mesher.ts';
import { buildMockTextures } from '../worker/mock/textures.ts';
import { AIR, VoxelWorld } from '../worker/mock/world.ts';
import type { TextureInfo } from '../engine/protocol.ts';

export const LAB_H = 0.125;
const N = 256; // 32 m square
const NZ = 64;
const GROUND = 3; // voxels of ground; its top is world z = 0

export interface LabLevel {
  world: VoxelWorld;
  textures: TextureInfo[];
  meshes: ChunkMesh[];
  /** Solid test by engine convention: voxel p is centred at h p. */
  solidAt(i: number, j: number, k: number): boolean;
  /** Re-meshes after edits. */
  remesh(): ChunkMesh[];
}

export function buildLabLevel(): LabLevel {
  const h = LAB_H;
  // grid voxel (x, y, z) has its minimum corner at origin + h (x, y, z); engine voxel p is
  // centred at h p, i.e. its minimum corner is h (p - 1/2): grid = p + OFF
  const OFF: Vec3 = [N / 2, N / 2, GROUND];
  const origin: Vec3 = [-(N / 2) * h - h / 2, -(N / 2) * h - h / 2, -GROUND * h - h / 2];
  const w = new VoxelWorld([N, N, NZ], h, origin, Block.Bedrock);
  w.fillBox(0, 0, 0, N, N, 1, Block.Bedrock);
  w.fillBox(0, 0, 1, N, N, GROUND, Block.Grass);
  const at = (x: number, y: number): [number, number] => [Math.round(x / h) + OFF[0], Math.round(y / h) + OFF[1]];
  const box = (x0: number, y0: number, z0: number, x1: number, y1: number, z1: number, b: number): void => {
    const [gx0, gy0] = at(x0, y0);
    const [gx1, gy1] = at(x1, y1);
    w.fillBox(gx0, gy0, GROUND + Math.round(z0 / h), gx1, gy1, GROUND + Math.round(z1 / h), b);
  };
  // a paved strip
  box(-12, -1.5, -h, 12, 1.5, 0, Block.Concrete);
  // stairs up to a platform (+x), 0.125 m rises, 0.375 m treads
  for (let s = 0; s < 6; s++) box(4 + s * 0.375, 3, 0, 8, 6, (s + 1) * 0.125, Block.Concrete);
  box(8, 3, 0, 11, 6, 0.75, Block.Concrete);
  // a ramp down the other side
  for (let s = 0; s < 12; s++) box(11 + s * 0.25, 3, 0, 11 + (s + 1) * 0.25, 6, 0.75 - (s + 1) * 0.0625, Block.Rock);
  // crates and low walls (cover)
  box(-6, 3, 0, -5, 4, 1, Block.Metal);
  box(-4, 4, 0, -3.25, 4.75, 0.75, Block.Metal);
  box(-8, -6, 0, -3, -5.5, 1.125, Block.Brick);
  box(2, -7, 0, 2.5, -3, 1.5, Block.Brick);
  // a doorway wall
  box(-12, 8, 0, -1, 8.5, 3, Block.Plaster);
  box(-7, 8, 0, -5.75, 8.5, 2.25, AIR);
  // fallen beams and a kerb across a running lane (x -3 .. 3 at y 6.4): feet that do not clear
  // them catch
  box(-1.3, 5.7, 0, -1.175, 7.1, 0.125, Block.Rubble);
  box(0.5, 5.7, 0, 0.75, 7.1, 0.25, Block.Concrete);
  box(2.0, 5.8, 0, 2.125, 7.0, 0.125, Block.Rubble);
  // uneven rubble field
  for (let k = 0; k < 90; k++) {
    const x = 5 + ((k * 37) % 50) / 10;
    const y = -8 + ((k * 53) % 40) / 10;
    const z = ((k * 13) % 3) * 0.125 + 0.125;
    box(x, y, 0, x + 0.25 + (k % 3) * 0.125, y + 0.25 + (k % 2) * 0.125, z, Block.Rubble);
  }
  const textures = buildMockTextures(1);
  const ids = new Map<string, number>();
  for (const t of textures) ids.set(t.name, t.id);
  const faceTextures = resolveFaceTextures(ids);
  const padded = new Uint8Array(PADDED_CHUNK_VOLUME);
  const builder = new MeshBuilder(1 << 14);
  const remesh = (): ChunkMesh[] => {
    const out: ChunkMesh[] = [];
    for (let ci = 0; ci < w.chunkCount; ci++) {
      if (w.chunkSolid(ci) === 0) continue;
      const blk = fillPaddedChunk(w, ci, padded);
      builder.reset();
      meshBlock(blk, { faceTextures, texelsPerMetre: DOOM_TEXELS_PER_METRE, light: () => 255, debug: null, greedy: true }, builder);
      if (builder.indexCount === 0) continue;
      out.push({ key: w.chunkKey(ci), origin: w.chunkOrigin(ci), ...builder.finish() });
    }
    return out;
  };
  return {
    world: w,
    textures,
    meshes: remesh(),
    solidAt: (i, j, k) => w.get(i + OFF[0], j + OFF[1], k + OFF[2]) !== AIR,
    remesh,
  };
}
