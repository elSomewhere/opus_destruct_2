/**
 * Block palette of the mock engine. A voxel stores a block id (1 byte); the block gives
 * its structural material and its per-face appearance. Block 0 is air.
 */
import type { MaterialId } from '../../engine/protocol.ts';
import { Material, TEXTURE_MATERIAL_BASE } from '../../engine/protocol.ts';

export interface BlockDef {
  name: string;
  material: MaterialId;
  /** Texture names for side (x/y), top (+z) and bottom (-z) faces; null = material colour. */
  side: string | null;
  top: string | null;
  bottom: string | null;
  /** Anchored to the ground (supports everything connected to it). */
  anchored: boolean;
  /** Blasts and carves leave it alone. */
  indestructible: boolean;
}

export const Block = {
  Air: 0,
  Bedrock: 1,
  Soil: 2,
  Grass: 3,
  Rock: 4,
  Concrete: 5,
  Slab: 6,
  Brick: 7,
  Steel: 8,
  Plaster: 9,
  Metal: 10,
  Hazard: 11,
  Rubble: 12,
} as const;

function def(
  name: string,
  material: MaterialId,
  side: string | null,
  top: string | null = side,
  bottom: string | null = side,
  anchored = false,
): BlockDef {
  return { name, material, side, top, bottom, anchored, indestructible: anchored };
}

/** Indexed by block id. */
export const BLOCKS: readonly BlockDef[] = [
  def('air', Material.Concrete, null),
  def('bedrock', Material.Bedrock, null, null, null, true),
  def('soil', Material.Soil, 'DIRT'),
  def('grass', Material.Soil, 'DIRT', 'GRASS', 'DIRT'),
  def('rock', Material.Rock, 'ROCK'),
  def('concrete', Material.Concrete, 'CONCRETE'),
  def('rc slab', Material.Rc, 'CONCRETE', 'TILE', 'PLASTER'),
  def('brick', Material.Masonry, 'BRICK', 'CONCRETE', 'CONCRETE'),
  // Untextured on purpose: exercises the material-colour path.
  def('steel', Material.Steel, null),
  def('plaster', Material.Masonry, 'PLASTER'),
  def('metal', Material.Steel, 'METAL'),
  def('hazard', Material.Steel, 'HAZARD', 'METAL', 'METAL'),
  def('rubble', Material.Concrete, null),
];

/** Face order used by the mesher: +x, -x, +y, -y, +z, -z. */
export const FACE_COUNT = 6;

/**
 * Per block and face, the vertex texture id: a texture index, or
 * TEXTURE_MATERIAL_BASE + material for untextured faces.
 */
export function resolveFaceTextures(textureIds: ReadonlyMap<string, number>): Uint16Array {
  const out = new Uint16Array(BLOCKS.length * FACE_COUNT);
  BLOCKS.forEach((b, id) => {
    const pick = (name: string | null): number => {
      const t = name === null ? undefined : textureIds.get(name);
      return t ?? TEXTURE_MATERIAL_BASE + b.material;
    };
    const side = pick(b.side);
    out[id * FACE_COUNT + 0] = side;
    out[id * FACE_COUNT + 1] = side;
    out[id * FACE_COUNT + 2] = side;
    out[id * FACE_COUNT + 3] = side;
    out[id * FACE_COUNT + 4] = pick(b.top);
    out[id * FACE_COUNT + 5] = pick(b.bottom);
  });
  return out;
}

export function isAnchored(block: number): boolean {
  return BLOCKS[block]?.anchored ?? false;
}

export function isIndestructible(block: number): boolean {
  return BLOCKS[block]?.indestructible ?? false;
}

export function blockMaterial(block: number): MaterialId {
  return BLOCKS[block]?.material ?? Material.Concrete;
}
