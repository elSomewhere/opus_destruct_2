/**
 * Character palettes: one linear-RGB colour per slot (model.ts Slot). Colours are written as
 * sRGB hex and converted here.
 */
import { SLOT_COUNT, Slot, type Palette } from '../voxel/model.ts';

export type Rgb = [number, number, number];

function srgbToLinear(c: number): number {
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}

/** Linear RGB of an sRGB hex colour (0xRRGGBB). */
export function hex(h: number): Rgb {
  return [srgbToLinear(((h >> 16) & 255) / 255), srgbToLinear(((h >> 8) & 255) / 255), srgbToLinear((h & 255) / 255)];
}

export const SKIN_TONES: readonly number[] = [0xf1c8a6, 0xe0ac86, 0xc68a62, 0xa66b45, 0x7d4a2d, 0x5a3420, 0xd9a07a, 0xefbf9a];
export const HAIR_COLOURS: readonly number[] = [0x1f1712, 0x3a2618, 0x5c3b22, 0x8a6036, 0xb8894a, 0x2b2b2b, 0x6e6e6e, 0x9c3c1c];

export interface PaletteSpec {
  skin: number;
  hair: number;
  top: number;
  top2: number;
  bottom: number;
  bottom2: number;
  shoes: number;
  gear: number;
  gearDark: number;
  metal: number;
  furniture: number;
  detail: number;
  accent: number;
  flesh?: number;
  bone?: number;
  blood?: number;
}

export function makePalette(p: PaletteSpec): Palette {
  const out: Rgb[] = new Array(SLOT_COUNT);
  out[Slot.Skin] = hex(p.skin);
  out[Slot.Hair] = hex(p.hair);
  out[Slot.Top] = hex(p.top);
  out[Slot.Top2] = hex(p.top2);
  out[Slot.Bottom] = hex(p.bottom);
  out[Slot.Bottom2] = hex(p.bottom2);
  out[Slot.Shoes] = hex(p.shoes);
  out[Slot.Gear] = hex(p.gear);
  out[Slot.GearDark] = hex(p.gearDark);
  out[Slot.Metal] = hex(p.metal);
  out[Slot.Furniture] = hex(p.furniture);
  out[Slot.Detail] = hex(p.detail);
  out[Slot.Accent] = hex(p.accent);
  out[Slot.Flesh] = hex(p.flesh ?? 0x8c1c1c);
  out[Slot.Bone] = hex(p.bone ?? 0xe0d6c0);
  out[Slot.Blood] = hex(p.blood ?? 0x5c0808);
  return out;
}

/** Soldier camouflage schemes: top / top2 / bottom / bottom2 / gear / gearDark / shoes. */
export const CAMO_SCHEMES: readonly Pick<PaletteSpec, 'top' | 'top2' | 'bottom' | 'bottom2' | 'gear' | 'gearDark' | 'shoes'>[] = [
  // woodland
  { top: 0x5b6440, top2: 0x3a4529, bottom: 0x5b6440, bottom2: 0x6e5a3c, gear: 0x4d5534, gearDark: 0x262a22, shoes: 0x2d241c },
  // desert
  { top: 0xb5a07a, top2: 0x8c7552, bottom: 0xb5a07a, bottom2: 0x6e5c40, gear: 0x9b8660, gearDark: 0x4a3f2e, shoes: 0x7a6344 },
  // urban
  { top: 0x6d7074, top2: 0x3f4246, bottom: 0x6d7074, bottom2: 0x55585c, gear: 0x2f3236, gearDark: 0x1d1f21, shoes: 0x1c1c1e },
  // mercenary black / olive
  { top: 0x2c2e2a, top2: 0x3d4234, bottom: 0x4a4f3a, bottom2: 0x33372a, gear: 0x3a3d33, gearDark: 0x191a17, shoes: 0x1e1b17 },
];

export const CLOTH_COLOURS: readonly number[] = [
  0xc0392b, 0x2e6fb0, 0x2f8f5b, 0xe0b43a, 0x8e44ad, 0xd35400, 0x34495e, 0xecf0f1, 0x7f8c8d, 0x16a085, 0xb03060, 0x1b2a49, 0x6b4f2a, 0xa9cce3, 0xf5e6c8, 0x222222,
];
export const TROUSER_COLOURS: readonly number[] = [0x2b3a55, 0x3d4f73, 0x1f2328, 0x6b5b45, 0x8a7a5c, 0x4a4a4a, 0x55402b, 0x23324d];
export const SHOE_COLOURS: readonly number[] = [0x1c1c1c, 0x3b2a1e, 0xe8e8e8, 0x7a1f1f, 0x2d3e50, 0x5a4632];
