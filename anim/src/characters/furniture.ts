/**
 * Voxel furniture to sit at: benches, chairs and desks (one-bone props, origin on the floor
 * under the seat's centre, facing +y: a sitter faces +y). `seat` gives the sitting surface and
 * `kind` what the animator does there.
 */
import type { V3 } from '../math/vec.ts';
import { propSkeleton } from '../humanoid/rig.ts';
import { makePalette } from './palette.ts';
import { Slot, type Palette, type VoxelModel } from '../voxel/model.ts';
import { box, Sculptor } from '../voxel/sculpt.ts';
import { DEFAULT_VOXEL_SIZE } from './humans.ts';

export interface Furniture {
  model: VoxelModel;
  palette: Palette;
  /** Sitting surface centre, relative to the prop origin (model space of the prop). */
  seat: V3;
  /** Desk top height, for desks (m above the floor). */
  deskHeight?: number;
  /** A backrest to lean on. */
  backrest: boolean;
  kind: 'bench' | 'chair' | 'desk';
  /**
   * Where a sitter stands before sitting down: this far (m) in front of the seat, along +y
   * (default 0.38; closer at a table, so the body stays clear of the table top).
   */
  approach?: number;
}

export const FURNITURE_PALETTE: Palette = makePalette({
  skin: 0x000000,
  hair: 0x000000,
  top: 0x000000,
  top2: 0x000000,
  bottom: 0x000000,
  bottom2: 0x000000,
  shoes: 0x000000,
  gear: 0x6b4a2e,
  gearDark: 0x2c2f33,
  metal: 0x3d4146,
  furniture: 0x8a6440,
  detail: 0x1a1a1a,
  accent: 0x55606b,
});

/** A park bench (1.6 m) with a backrest. */
export function makeBench(voxelSize = DEFAULT_VOXEL_SIZE): Furniture {
  const sc = new Sculptor(propSkeleton(), voxelSize, [-0.85, -0.35, 0], [0.85, 0.3, 0.95], 61);
  const o = { organic: false } as const;
  for (const x of [-0.7, 0.7]) {
    sc.add(box([x, 0.08, 0.22], [0.03, 0.03, 0.22]), 0, Slot.Metal, o);
    sc.add(box([x, -0.14, 0.44], [0.03, 0.03, 0.44]), 0, Slot.Metal, o);
    sc.add(box([x, -0.03, 0.42], [0.03, 0.14, 0.02]), 0, Slot.Metal, o);
  }
  for (const y of [-0.12, 0.0, 0.12]) sc.add(box([0, y, 0.44], [0.8, 0.045, 0.018]), 0, Slot.Furniture, { ...o, jitter: 0.06 });
  for (const z of [0.6, 0.76]) sc.add(box([0, -0.17, z], [0.8, 0.016, 0.055]), 0, Slot.Furniture, { ...o, jitter: 0.06 });
  return { model: sc.finish({ name: 'bench' }), palette: FURNITURE_PALETTE, seat: [0, -0.02, 0.46], backrest: true, kind: 'bench' };
}

/** A chair (seat 0.46 m). */
export function makeChair(voxelSize = DEFAULT_VOXEL_SIZE): Furniture {
  const sc = new Sculptor(propSkeleton(), voxelSize, [-0.28, -0.3, 0], [0.28, 0.28, 1.0], 62);
  const o = { organic: false } as const;
  for (const x of [-0.19, 0.19]) for (const y of [-0.18, 0.18]) sc.add(box([x, y, 0.22], [0.02, 0.02, 0.22]), 0, Slot.Metal, o);
  sc.add(box([0, 0, 0.455], [0.22, 0.21, 0.025], 0.01), 0, Slot.Accent, o);
  sc.add(box([0, -0.2, 0.72], [0.21, 0.02, 0.17], 0.02), 0, Slot.Accent, o);
  for (const x of [-0.19, 0.19]) sc.add(box([x, -0.2, 0.62], [0.02, 0.02, 0.18]), 0, Slot.Metal, o);
  return { model: sc.finish({ name: 'chair' }), palette: FURNITURE_PALETTE, seat: [0, 0.02, 0.48], backrest: true, kind: 'chair' };
}

/** A desk (0.74 m) with a chair behind it: the sitter faces +y across the desk top. */
export function makeDesk(voxelSize = DEFAULT_VOXEL_SIZE): Furniture {
  const sc = new Sculptor(propSkeleton(), voxelSize, [-0.7, -0.35, 0], [0.7, 1.0, 1.0], 63);
  const o = { organic: false } as const;
  // desk top and legs, in front of the chair
  sc.add(box([0, 0.62, 0.725], [0.62, 0.32, 0.02]), 0, Slot.Furniture, { ...o, jitter: 0.05 });
  for (const x of [-0.58, 0.58]) sc.add(box([x, 0.62, 0.36], [0.025, 0.28, 0.35]), 0, Slot.Metal, o);
  // a monitor or papers
  sc.add(box([0.1, 0.82, 0.9], [0.22, 0.02, 0.15]), 0, Slot.Detail, o);
  sc.add(box([0.1, 0.84, 0.76], [0.03, 0.02, 0.02]), 0, Slot.Metal, o);
  sc.add(box([-0.25, 0.5, 0.75], [0.1, 0.13, 0.006]), 0, Slot.Bone, o);
  // the chair
  for (const x of [-0.19, 0.19]) for (const y of [-0.18, 0.18]) sc.add(box([x, y, 0.22], [0.02, 0.02, 0.22]), 0, Slot.Metal, o);
  sc.add(box([0, 0, 0.455], [0.22, 0.21, 0.025], 0.01), 0, Slot.Accent, o);
  sc.add(box([0, -0.2, 0.72], [0.21, 0.02, 0.17], 0.02), 0, Slot.Accent, o);
  return { model: sc.finish({ name: 'desk' }), palette: FURNITURE_PALETTE, seat: [0, 0.02, 0.48], deskHeight: 0.745, backrest: true, kind: 'desk' };
}

/**
 * A café table (0.74 m) with a chair and an open laptop: the sitter faces +y across the table
 * and works at it (the 'desk' sitting variant). Its edge is 0.4 m in front of the seat, so a
 * sitter can step in between chair and table before sitting down.
 */
export function makeCafeTable(voxelSize = DEFAULT_VOXEL_SIZE): Furniture {
  const sc = new Sculptor(propSkeleton(), voxelSize, [-0.5, -0.35, 0], [0.5, 1.05, 1.0], 67);
  const o = { organic: false } as const;
  // round-ish top on one pedestal
  sc.add(box([0, 0.7, 0.725], [0.36, 0.3, 0.02], 0.08), 0, Slot.Furniture, { ...o, jitter: 0.05 });
  sc.add(box([0, 0.7, 0.36], [0.03, 0.03, 0.35]), 0, Slot.Metal, o);
  sc.add(box([0, 0.7, 0.02], [0.2, 0.2, 0.02], 0.02), 0, Slot.Metal, o);
  // an open laptop and a cup
  sc.add(box([0.02, 0.55, 0.755], [0.15, 0.1, 0.008]), 0, Slot.GearDark, o);
  sc.add(box([0.02, 0.66, 0.85], [0.15, 0.012, 0.1]), 0, Slot.GearDark, o);
  sc.add(box([0.02, 0.648, 0.85], [0.13, 0.004, 0.085]), 0, Slot.Detail, { ...o, shade: 1.3 });
  sc.add(box([-0.26, 0.62, 0.785], [0.035, 0.035, 0.045], 0.01), 0, Slot.Bone, o);
  // the chair
  for (const x of [-0.19, 0.19]) for (const y of [-0.18, 0.18]) sc.add(box([x, y, 0.22], [0.02, 0.02, 0.22]), 0, Slot.Metal, o);
  sc.add(box([0, 0, 0.455], [0.22, 0.21, 0.025], 0.01), 0, Slot.Accent, o);
  sc.add(box([0, -0.2, 0.72], [0.21, 0.02, 0.17], 0.02), 0, Slot.Accent, o);
  return { model: sc.finish({ name: 'cafe table' }), palette: FURNITURE_PALETTE, seat: [0, 0.02, 0.48], deskHeight: 0.745, backrest: true, kind: 'desk', approach: 0.2 };
}
