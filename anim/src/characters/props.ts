/**
 * Voxel props held by characters (weapons). A prop is a one-bone model whose origin is the
 * grip of the hand that holds it, +y along the barrel, z up. Its attach points place the other
 * hand, the shoulder stock and the muzzle.
 */
import type { V3 } from '../math/vec.ts';
import { propSkeleton } from '../humanoid/rig.ts';
import { Slot, type VoxelModel } from '../voxel/model.ts';
import { box, cylinder, Sculptor } from '../voxel/sculpt.ts';
import { DEFAULT_VOXEL_SIZE } from './humans.ts';

export interface Prop {
  model: VoxelModel;
  /** Prop-space points. */
  grip: V3;
  support: V3;
  stock: V3;
  muzzle: V3;
  /** Where the magazine sits (reloads). */
  magazine: V3;
  kind: 'rifle' | 'smg' | 'pistol';
}

/** An assault rifle (~0.86 m, pistol grip 0.32 m from the butt). */
export function makeRifle(voxelSize = DEFAULT_VOXEL_SIZE, variant = 0): Prop {
  const s = voxelSize;
  const L = 0.8; // (lengths below were drawn for a 1.1 m rifle)
  const y = (v: number): number => v * L;
  const sc = new Sculptor(propSkeleton(), s, [-0.06, -0.4, -0.2], [0.06, 0.6, 0.2], 11 + variant);
  const M = Slot.Metal, F = Slot.Furniture, D = Slot.GearDark;
  const o = { organic: false } as const;
  // receiver
  sc.add(box([0, y(0.06), 0.064], [0.021, y(0.15), 0.032], 0.006), 0, M, o);
  // stock and butt pad
  sc.add(box([0, y(-0.25), 0.05], [0.019, y(0.17), 0.036], 0.01), 0, F, o);
  sc.add(box([0, y(-0.2), 0.014], [0.017, y(0.1), 0.02], 0.008), 0, F, o);
  sc.add(box([0, y(-0.425), 0.045], [0.021, 0.012, 0.048], 0.004), 0, D, o);
  // pistol grip (raked back) and trigger guard
  sc.add(box([0, -0.012, -0.032], [0.017, 0.02, 0.048], 0.008), 0, F, o);
  sc.add(box([0, 0.028, 0.012], [0.011, 0.026, 0.011], 0.004), 0, M, o);
  // curved magazine
  sc.add(box([0, y(0.115), -0.028], [0.016, 0.026, 0.056], 0.006), 0, M, { ...o, shade: 0.8 });
  sc.add(box([0, y(0.135) + 0.01, -0.094], [0.015, 0.026, 0.032], 0.006), 0, M, { ...o, shade: 0.8 });
  // handguard, barrel, gas block, front sight, muzzle
  sc.add(box([0, y(0.33), 0.062], [0.025, y(0.12), 0.029], 0.01), 0, F, o);
  sc.add(cylinder([0, y(0.44), 0.068], [0, y(0.66), 0.068], 0.012), 0, M, o);
  sc.add(box([0, y(0.46), 0.074], [0.014, 0.018, 0.02]), 0, M, o);
  sc.add(box([0, y(0.44), 0.115], [0.006, 0.01, 0.028]), 0, M, o);
  sc.add(cylinder([0, y(0.62), 0.068], [0, y(0.67), 0.068], 0.016), 0, D, o);
  // rear sight / optic rail
  sc.add(box([0, y(0.08), 0.106], [0.013, y(0.05), 0.013]), 0, D, o);
  if (variant % 2 === 1) sc.add(cylinder([0, y(0.03), 0.13], [0, y(0.14), 0.13], 0.021), 0, D, o); // scope
  const model = sc.finish({ name: `rifle-${variant}` });
  return { model, grip: [0, 0, 0], support: [0, y(0.3), 0.03], stock: [0, y(-0.43), 0.05], muzzle: [0, y(0.68), 0.068], magazine: [0, y(0.12), -0.06], kind: 'rifle' };
}

/** A compact submachine gun / carbine. */
export function makeSmg(voxelSize = DEFAULT_VOXEL_SIZE): Prop {
  const s = voxelSize;
  const sc = new Sculptor(propSkeleton(), s, [-0.06, -0.34, -0.2], [0.06, 0.5, 0.2], 21);
  const M = Slot.Metal, F = Slot.Furniture, D = Slot.GearDark;
  const o = { organic: false } as const;
  sc.add(box([0, 0.08, 0.06], [0.022, 0.13, 0.034], 0.008), 0, M, o);
  sc.add(box([0, -0.19, 0.055], [0.012, 0.13, 0.02], 0.006), 0, D, o);
  sc.add(box([0, -0.31, 0.05], [0.02, 0.012, 0.04]), 0, D, o);
  sc.add(box([0, -0.01, -0.03], [0.018, 0.022, 0.048], 0.008), 0, F, o);
  sc.add(box([0, 0.1, -0.06], [0.016, 0.02, 0.07], 0.005), 0, M, { ...o, shade: 0.8 });
  sc.add(cylinder([0, 0.2, 0.066], [0, 0.4, 0.066], 0.014), 0, M, o);
  sc.add(box([0, 0.26, 0.05], [0.02, 0.06, 0.02], 0.006), 0, F, o);
  const model = sc.finish({ name: 'smg' });
  return { model, grip: [0, 0, 0], support: [0, 0.25, 0.025], stock: [0, -0.3, 0.05], muzzle: [0, 0.41, 0.066], magazine: [0, 0.1, -0.08], kind: 'smg' };
}
