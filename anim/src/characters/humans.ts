/**
 * Procedural voxel humans: a sculpted body (skin, flesh and bone inside) dressed in layers
 * (trousers or shorts, shirts, jackets, boots or shoes, hair, headgear, tactical gear), all
 * bound to the humanoid rig. `sculptHuman` builds one geometry from a HumanSpec; colours come
 * from the palette of each instance, so a geometry serves many looks.
 */
import type { Skeleton } from '../core/skeleton.ts';
import { qfromTo } from '../math/quat.ts';
import { Rng, valueNoise } from '../math/random.ts';
import { vadd, vlerp, vnorm, vsub, type V3 } from '../math/vec.ts';
import { H, humanoidSkeleton, type HumanoidBuild } from '../humanoid/rig.ts';
import { Slot, type Palette, type SlotId, type VoxelModel } from '../voxel/model.ts';
import {
  box,
  capsule,
  clip,
  ellipsoid,
  inflate,
  Sculptor,
  shell,
  smoothUnion,
  sphere,
  subtract,
  type Shape,
} from '../voxel/sculpt.ts';
import { CAMO_SCHEMES, CLOTH_COLOURS, HAIR_COLOURS, makePalette, SHOE_COLOURS, SKIN_TONES, TROUSER_COLOURS } from './palette.ts';

export type TopStyle = 'tshirt' | 'longsleeve' | 'jacket' | 'hoodie' | 'uniform' | 'tanktop';
export type BottomStyle = 'trousers' | 'shorts' | 'uniform';
export type ShoeStyle = 'shoes' | 'sneakers' | 'boots';
export type HairStyle = 'none' | 'buzz' | 'short' | 'long' | 'ponytail' | 'bun';
export type HeadGear = 'none' | 'helmet' | 'helmetGoggles' | 'cap' | 'beanie' | 'beret' | 'balaclava';

export interface HumanSpec {
  build: HumanoidBuild;
  female: boolean;
  top: TopStyle;
  bottom: BottomStyle;
  shoes: ShoeStyle;
  hair: HairStyle;
  headgear: HeadGear;
  beard: boolean;
  glasses: boolean;
  vest: boolean;
  backpack: boolean;
  belt: boolean;
  gloves: boolean;
  kneepads: boolean;
  /** Two-tone noise pattern on Top/Bottom (Top2/Bottom2). */
  camo: boolean;
  /** Voxel pitch, metres (default 1/32: Doom's texel scale). */
  voxelSize: number;
  seed: number;
  name: string;
}

export const DEFAULT_VOXEL_SIZE = 1 / 32;

export function sculptHuman(spec: HumanSpec): VoxelModel {
  const skel = humanoidSkeleton(spec.build);
  const s = spec.voxelSize;
  const k = spec.build.height;
  const g = spec.build.girth;
  const sh = spec.build.shoulders;
  const hp = spec.build.hips;
  const sc = new Sculptor(skel, s, [-0.62 * k, -0.45 * k, 0], [0.62 * k, 0.5 * k, 1.95 * k], spec.seed);
  const hd = (b: number): V3 => skel.restHead[b] as V3;
  const tl = (b: number): V3 => skel.restTail[b] as V3;
  const at = (b: number, t: number): V3 => vlerp(hd(b), tl(b), t);
  const off = (p: V3, x: number, y: number, z: number): V3 => [p[0] + x, p[1] + y, p[2] + z];
  const sides = [
    { sx: -1, clav: H.clavicleL, ua: H.upperarmL, fa: H.forearmL, hand: H.handL, th: H.thighL, sn: H.shinL, ft: H.footL, toe: H.toeL },
    { sx: 1, clav: H.clavicleR, ua: H.upperarmR, fa: H.forearmR, hand: H.handR, th: H.thighR, sn: H.shinR, ft: H.footR, toe: H.toeR },
  ];
  const fem = spec.female;

  // ---- body shapes (skin), kept to dress them ----------------------------------------------
  const pelvisC: V3 = [0, -0.012 * k, 0.945 * k];
  const pelvisS = ellipsoid(pelvisC, [(fem ? 0.16 : 0.15) * hp * g, 0.105 * g, 0.105 * k]);
  const glutes = sides.map((d) => ellipsoid([d.sx * 0.068 * hp, -0.052 * g, 0.9 * k], [0.078 * hp * g, 0.072 * g, 0.088 * k]));
  const crotch = ellipsoid([0, -0.005, 0.875 * k], [0.075 * hp * g, 0.07 * g, 0.06 * k]);
  const abdomen = ellipsoid([0, -0.006 * k, 1.13 * k], [(fem ? 0.125 : 0.14) * g, 0.097 * g, 0.105 * k]);
  const chestS = ellipsoid([0, -0.012 * k, 1.315 * k], [0.162 * sh * g, 0.108 * g, 0.14 * k]);
  const yoke = capsule([-0.125 * sh, -0.02 * k, 1.41 * k], [0.125 * sh, -0.02 * k, 1.41 * k], 0.064 * g);
  const neckS = capsule(off(hd(H.neck), 0, 0, -0.02 * k), tl(H.neck), 0.05 * g * (fem ? 0.9 : 1));
  const headC = off(hd(H.head), 0, 0.012 * k, 0.1 * k);
  const headS = ellipsoid(headC, [0.082 * k, 0.097 * k, 0.11 * k]);
  const jawS = ellipsoid(off(hd(H.head), 0, 0.035 * k, 0.052 * k), [0.068 * k, 0.07 * k, 0.058 * k]);

  const hipsS = smoothUnion([pelvisS, ...glutes, crotch], 0.04);
  sc.add(hipsS, H.pelvis, Slot.Skin);
  sc.add(abdomen, H.spine, Slot.Skin);
  const bust = fem ? sides.map((d) => ellipsoid([d.sx * 0.058 * sh, 0.075 * g, 1.295 * k], [0.058 * g, 0.048 * g, 0.052 * k])) : [];
  const torsoS = smoothUnion([chestS, yoke, ...bust], 0.05);
  sc.add(torsoS, H.chest, Slot.Skin);
  sc.add(neckS, H.neck, Slot.Skin);
  sc.add(headS, H.head, Slot.Skin);
  sc.add(jawS, H.head, Slot.Skin);

  const limbs = sides.map((d) => {
    const shoulder = sphere(hd(d.ua), 0.058 * g * (fem ? 0.9 : 1));
    const upper = capsule(hd(d.ua), tl(d.ua), 0.05 * g * (fem ? 0.88 : 1), 0.042 * g * (fem ? 0.9 : 1));
    const fore = capsule(hd(d.fa), tl(d.fa), 0.042 * g * (fem ? 0.9 : 1), 0.032 * g);
    const handDir = vnorm(vsub(tl(d.hand), hd(d.hand)));
    const handQ = qfromTo([0, 0, -1], handDir);
    const palm = box(at(d.hand, 0.45), [0.021 * k, 0.037 * k, 0.07 * k], 0.012 * k, handQ);
    const thumb = capsule(at(d.hand, 0.12), vadd(at(d.hand, 0.45), [-d.sx * 0.004, 0.045 * k, 0]), 0.014 * k);
    const thigh = smoothUnion([capsule(hd(d.th), tl(d.th), 0.085 * g * hp, 0.058 * g), ellipsoid(vadd(at(d.th, 0.38), [0, 0.012, 0]), [0.072 * g, 0.076 * g, 0.15 * k])], 0.04);
    const shin = smoothUnion([capsule(hd(d.sn), tl(d.sn), 0.054 * g, 0.038 * g), ellipsoid(vadd(at(d.sn, 0.3), [0, -0.022, 0]), [0.05 * g, 0.052 * g, 0.1 * k])], 0.04);
    const ank = hd(d.ft);
    const foot = box([ank[0], 0.03 * k, 0.042 * k], [0.042 * k, 0.088 * k, 0.042 * k], 0.02 * k);
    const toe = box([hd(d.toe)[0], 0.148 * k, 0.026 * k], [0.04 * k, 0.034 * k, 0.026 * k], 0.012 * k);
    return { d, shoulder, upper, fore, palm, thumb, thigh, shin, foot, toe };
  });
  for (const l of limbs) {
    sc.add(l.shoulder, l.d.ua, Slot.Skin);
    sc.add(l.upper, l.d.ua, Slot.Skin);
    sc.add(l.fore, l.d.fa, Slot.Skin);
    sc.add(l.palm, l.d.hand, Slot.Skin);
    sc.add(l.thumb, l.d.hand, Slot.Skin);
    sc.add(l.thigh, l.d.th, Slot.Skin);
    sc.add(l.shin, l.d.sn, Slot.Skin);
    sc.add(l.foot, l.d.ft, Slot.Skin);
    sc.add(l.toe, l.d.toe, Slot.Skin);
  }

  // ---- clothes -----------------------------------------------------------------------------
  const cloth = 0.011;
  const bottomSlot: SlotId = Slot.Bottom;
  const shortsZ = 0.62 * k;
  // trousers / shorts: hips, and the legs down to the shoe tops
  sc.add(clip(inflate(hipsS, cloth), [0, 0, 0.8 * k], [0, 0, 1]), H.pelvis, bottomSlot);
  const shoeTop = spec.shoes === 'boots' ? 0.225 * k : 0.1 * k;
  for (const l of limbs) {
    const legBottom = spec.bottom === 'shorts' ? shortsZ : shoeTop - 0.02 * k;
    sc.add(clip(inflate(l.thigh, cloth * (spec.bottom === 'uniform' ? 1.6 : 1.1)), [0, 0, legBottom], [0, 0, 1]), l.d.th, bottomSlot);
    if (spec.bottom !== 'shorts') sc.add(clip(inflate(l.shin, cloth * (spec.bottom === 'uniform' ? 1.6 : 1.2)), [0, 0, legBottom], [0, 0, 1]), l.d.sn, bottomSlot);
  }

  // shirts
  const topSlot: SlotId = Slot.Top;
  const topBulk = spec.top === 'jacket' || spec.top === 'hoodie' ? 0.024 : spec.top === 'uniform' ? 0.016 : 0.01;
  const waist = 0.985 * k;
  sc.add(clip(inflate(abdomen, topBulk), [0, 0, waist], [0, 0, 1]), H.spine, topSlot);
  sc.add(inflate(torsoS, topBulk), H.chest, topSlot);
  // the shirt hangs over the waistband
  sc.add(clip(clip(inflate(hipsS, topBulk + 0.004), [0, 0, 0.97 * k], [0, 0, 1]), [0, 0, 1.05 * k], [0, 0, -1]), H.pelvis, topSlot);
  for (const l of limbs) {
    if (spec.top === 'tanktop') continue;
    const sleeveEnd = spec.top === 'tshirt' ? 0.55 : 1.0;
    const shoulderCloth = inflate(l.shoulder, topBulk);
    sc.add(shoulderCloth, l.d.ua, topSlot);
    const upperEnd = at(l.d.ua, sleeveEnd);
    const n = vnorm(vsub(tl(l.d.ua), hd(l.d.ua)));
    sc.add(clip(inflate(l.upper, topBulk * 0.9), upperEnd, [-n[0], -n[1], -n[2]]), l.d.ua, topSlot);
    if (spec.top !== 'tshirt') {
      const cuff = at(l.d.fa, 0.94);
      const nf = vnorm(vsub(tl(l.d.fa), hd(l.d.fa)));
      sc.add(clip(inflate(l.fore, topBulk * 0.8), cuff, [-nf[0], -nf[1], -nf[2]]), l.d.fa, topSlot);
    }
  }
  if (spec.top === 'jacket') {
    // open front showing the shirt, and a collar
    sc.paint(box([0, 0.12 * k, 1.2 * k], [0.035 * k, 0.05, 0.22 * k]), Slot.Top2, { bones: [H.spine, H.chest, H.pelvis] });
    sc.add(clip(shell(inflate(capsule(hd(H.neck), tl(H.neck), 0.05 * g), 0.022), 0.02), [0, 0, 1.5 * k], [0, 0, -1]), H.chest, topSlot, { organic: false });
  }
  if (spec.top === 'hoodie') {
    sc.add(ellipsoid([0, -0.1 * k, 1.47 * k], [0.1 * k, 0.05 * k, 0.05 * k]), H.chest, Slot.Top, { organic: false });
    sc.paint(box([0, 0.12 * k, 1.1 * k], [0.08 * k, 0.03, 0.05 * k]), Slot.Top2, { bones: [H.spine] });
  }
  if (spec.top === 'uniform') {
    // breast pockets and a collar
    for (const d of sides) sc.add(box([d.sx * 0.075 * sh, 0.1 * g + 0.012, 1.33 * k], [0.035, 0.012, 0.035]), H.chest, topSlot, { organic: false });
    sc.add(clip(shell(inflate(capsule(hd(H.neck), tl(H.neck), 0.05 * g), 0.02), 0.018), [0, 0, 1.49 * k], [0, 0, -1]), H.chest, topSlot, { organic: false });
  }

  // shoes / boots
  for (const l of limbs) {
    const slot: SlotId = Slot.Shoes;
    sc.add(inflate(l.foot, 0.01), l.d.ft, slot);
    sc.add(inflate(l.toe, 0.01), l.d.toe, slot);
    if (spec.shoes === 'boots') {
      sc.add(clip(capsule(hd(l.d.ft), off(hd(l.d.ft), 0, 0.005, 0.15 * k), 0.052 * g), [0, 0, shoeTop], [0, 0, -1]), l.d.sn, slot);
      sc.paint(box([hd(l.d.ft)[0], 0.02, shoeTop - 0.012 * k], [0.07, 0.1, 0.013 * k]), Slot.GearDark, { only: [Slot.Shoes] });
    } else {
      sc.add(clip(capsule(hd(l.d.ft), off(hd(l.d.ft), 0, 0.01, 0.03 * k), 0.046 * g), [0, 0, shoeTop], [0, 0, -1]), l.d.sn, slot);
    }
    // soles
    const sole: SlotId = spec.shoes === 'sneakers' ? Slot.Accent : Slot.GearDark;
    sc.paint(box([hd(l.d.ft)[0], 0.05, 0.012], [0.08, 0.2, 0.017]), sole, { only: [Slot.Shoes] });
  }

  // gloves or skin hands
  if (spec.gloves) for (const l of limbs) sc.paint(inflate(l.palm, 0.02), Slot.GearDark, { bones: [l.d.hand] });

  // ---- head: face, hair, headgear ----------------------------------------------------------
  const face = faceCells(sc, skel, k);
  // ears
  for (const d of sides) sc.add(ellipsoid(off(headC, d.sx * 0.08 * k, -0.012 * k, -0.01 * k), [0.018 * k, 0.03 * k, 0.038 * k]), H.head, Slot.Skin);
  // nose
  sc.add(box(off(headC, 0, 0.097 * k, -0.018 * k), [s * 0.9, s * 0.6, s * 0.95]), H.head, Slot.Skin, { shade: 1.04 });
  const hairTop = clip(inflate(headS, 0.016), off(headC, 0, 0, 0.018 * k), [0, 0.55, 1]);
  const hairBack = clip(clip(inflate(headS, 0.014), off(headC, 0, -0.02 * k, 0), [0, -1, 0.25]), off(headC, 0, 0, -0.07 * k), [0, 0, 1]);
  switch (spec.hair) {
    case 'buzz':
      sc.paint(clip(inflate(headS, 0.02), off(headC, 0, 0, 0.03 * k), [0, 0.5, 1]), Slot.Hair, { only: [Slot.Skin], bones: [H.head] });
      break;
    case 'short':
      sc.add(hairTop, H.head, Slot.Hair, { organic: false });
      sc.add(hairBack, H.head, Slot.Hair, { organic: false });
      break;
    case 'long':
      sc.add(hairTop, H.head, Slot.Hair, { organic: false });
      sc.add(hairBack, H.head, Slot.Hair, { organic: false });
      sc.add(clip(ellipsoid(off(headC, 0, -0.045 * k, -0.07 * k), [0.098 * k, 0.07 * k, 0.13 * k]), off(headC, 0, 0.0, 0), [0, -1, 0]), H.head, Slot.Hair, { organic: false });
      for (const d of sides) sc.add(box(off(headC, d.sx * 0.083 * k, 0.0, -0.05 * k), [0.016 * k, 0.05 * k, 0.07 * k], 0.01), H.head, Slot.Hair, { organic: false });
      break;
    case 'ponytail':
      sc.add(hairTop, H.head, Slot.Hair, { organic: false });
      sc.add(hairBack, H.head, Slot.Hair, { organic: false });
      sc.add(capsule(off(headC, 0, -0.1 * k, 0.02 * k), off(headC, 0, -0.14 * k, -0.12 * k), 0.028 * k, 0.018 * k), H.head, Slot.Hair, { organic: false });
      break;
    case 'bun':
      sc.add(hairTop, H.head, Slot.Hair, { organic: false });
      sc.add(hairBack, H.head, Slot.Hair, { organic: false });
      sc.add(sphere(off(headC, 0, -0.09 * k, 0.06 * k), 0.04 * k), H.head, Slot.Hair, { organic: false });
      break;
    case 'none':
      break;
  }
  if (spec.beard) sc.paint(clip(inflate(jawS, 0.02), off(headC, 0, 0, -0.035 * k), [0, 0.15, -1]), Slot.Hair, { only: [Slot.Skin], bones: [H.head] });
  // eyes, brows, mouth (painted on the face surface after the hair)
  face.paint(spec);

  const helmetShape = (grow: number): Shape => clip(clip(inflate(headS, grow), off(headC, 0, 0.1 * k, 0.012 * k), [0, 0.35, 1]), off(headC, 0, -0.1 * k, -0.045 * k), [0, -0.35, 1]);
  switch (spec.headgear) {
    case 'helmet':
    case 'helmetGoggles': {
      const outer = helmetShape(0.032);
      sc.add(subtract(outer, inflate(headS, 0.004)), H.head, Slot.Gear, { organic: false });
      // brim lip and cover seams
      sc.paint(clip(clip(inflate(headS, 0.04), off(headC, 0, 0, 0.022 * k), [0, 0, -1]), off(headC, 0, 0, -0.01 * k), [0, 0, 1]), Slot.GearDark, { only: [Slot.Gear] });
      if (spec.headgear === 'helmetGoggles') {
        sc.add(box(off(headC, 0, 0.1 * k, 0.05 * k), [0.07 * k, 0.018, 0.02 * k], 0.008), H.head, Slot.GearDark, { organic: false });
        for (const d of sides) sc.add(box(off(headC, d.sx * 0.035 * k, 0.118 * k, 0.05 * k), [0.022 * k, 0.012, 0.016 * k]), H.head, Slot.Metal, { organic: false });
      }
      // chin strap
      for (const d of sides) sc.paint(box(off(headC, d.sx * 0.075 * k, 0.02 * k, -0.06 * k), [0.012, 0.015, 0.05 * k]), Slot.GearDark, { bones: [H.head] });
      break;
    }
    case 'cap':
      sc.add(subtract(clip(inflate(headS, 0.02), off(headC, 0, 0, 0.03 * k), [0, 0.2, 1]), inflate(headS, 0.002)), H.head, Slot.Accent, { organic: false });
      sc.add(box(off(headC, 0, 0.12 * k, 0.045 * k), [0.07 * k, 0.05 * k, 0.008 + s * 0.3], 0.01, qfromTo([0, 0, 1], vnorm([0, -0.15, 1]))), H.head, Slot.Accent, { organic: false, shade: 0.85 });
      break;
    case 'beanie':
      sc.add(subtract(clip(inflate(headS, 0.024), off(headC, 0, 0, 0.015 * k), [0, 0.3, 1]), inflate(headS, 0.002)), H.head, Slot.Accent, { organic: false });
      sc.paint(clip(clip(inflate(headS, 0.03), off(headC, 0, 0, 0.045 * k), [0, 0.3, -1]), off(headC, 0, 0, 0.015 * k), [0, 0.3, 1]), Slot.Accent, { shade: 0.8 });
      break;
    case 'beret':
      sc.add(subtract(clip(ellipsoid(off(headC, -0.02 * k, -0.005 * k, 0.05 * k), [0.1 * k, 0.11 * k, 0.07 * k]), off(headC, 0, 0, 0.045 * k), [0.3, 0.1, 1]), inflate(headS, 0.002)), H.head, Slot.Accent, { organic: false });
      break;
    case 'balaclava':
      sc.paint(inflate(headS, 0.03), Slot.GearDark, { bones: [H.head], only: [Slot.Skin, Slot.Hair] });
      sc.paint(inflate(jawS, 0.03), Slot.GearDark, { bones: [H.head], only: [Slot.Skin, Slot.Hair, Slot.Accent] });
      sc.paint(inflate(neckS, 0.02), Slot.GearDark, { bones: [H.neck] });
      // eye slot
      sc.paint(box(off(headC, 0, 0.1 * k, 0.012 * k), [0.06 * k, 0.03, 0.022 * k]), Slot.Skin, { bones: [H.head], only: [Slot.GearDark] });
      face.paintEyes();
      break;
    case 'none':
      break;
  }
  if (spec.glasses) {
    sc.add(box(off(headC, 0, 0.106 * k, 0.012 * k), [0.066 * k, s * 0.45, s * 0.5]), H.head, Slot.Detail, { organic: false });
  }

  // ---- gear --------------------------------------------------------------------------------
  if (spec.belt) {
    const band = clip(clip(inflate(pelvisS, 0.02), [0, 0, 1.0 * k], [0, 0, 1]), [0, 0, 1.045 * k], [0, 0, -1]);
    sc.add(subtract(band, inflate(pelvisS, 0.0)), H.pelvis, Slot.GearDark, { organic: false });
    sc.add(box([0, 0.115 * g + 0.012, 1.022 * k], [0.022, 0.01, 0.018]), H.pelvis, Slot.Metal, { organic: false });
  }
  if (spec.vest) {
    const vestC: V3 = [0, -0.008 * k, 1.28 * k];
    const vestBody = box(vestC, [0.158 * sh * g, 0.128 * g, 0.135 * k], 0.04 * k);
    sc.add(subtract(vestBody, inflate(chestS, -0.004)), H.chest, Slot.Gear, { organic: false });
    // lower part of the vest over the abdomen belongs to the spine
    sc.add(clip(subtract(box([0, -0.006 * k, 1.13 * k], [0.145 * g, 0.12 * g, 0.045 * k], 0.03), inflate(abdomen, -0.004)), [0, 0, 1.08 * k], [0, 0, 1]), H.spine, Slot.Gear, { organic: false });
    // shoulder straps
    for (const d of sides) sc.add(box([d.sx * 0.095 * sh, -0.01 * k, 1.425 * k], [0.035, 0.1 * g, 0.025]), H.chest, Slot.Gear, { organic: false });
    // magazine pouches and a radio
    for (const x of [-0.07, 0, 0.07]) sc.add(box([x * sh, 0.14 * g + 0.012, 1.215 * k], [0.03, 0.022, 0.048], 0.008), H.chest, Slot.GearDark, { organic: false });
    sc.add(box([0.1 * sh, 0.13 * g + 0.01, 1.36 * k], [0.022, 0.018, 0.04], 0.006), H.chest, Slot.GearDark, { organic: false });
    sc.add(box([0.1 * sh, 0.13 * g + 0.012, 1.41 * k], [0.006, 0.006, 0.03]), H.chest, Slot.Metal, { organic: false });
  }
  if (spec.backpack) {
    sc.add(box([0, -0.19 * g, 1.27 * k], [0.12 * sh, 0.065, 0.16 * k], 0.035), H.chest, Slot.Gear, { organic: false, shade: 0.95 });
    sc.add(box([0, -0.25 * g, 1.2 * k], [0.09 * sh, 0.02, 0.07 * k], 0.015), H.chest, Slot.GearDark, { organic: false });
  }
  if (spec.kneepads) for (const l of limbs) sc.add(box(off(hd(l.d.sn), 0, 0.058 * g, -0.01), [0.042, 0.018, 0.048], 0.012), l.d.sn, Slot.GearDark, { organic: false });
  if (spec.vest || spec.belt) {
    // thigh pouch / holster on the right leg
    sc.add(box(vadd(at(H.thighR, 0.35), [0.07 * g, 0.0, 0]), [0.022, 0.045, 0.06], 0.01), H.thighR, Slot.GearDark, { organic: false });
  }

  if (spec.camo) {
    const seed = spec.seed * 7 + 3;
    sc.paintFn((x, y, z, slot) => {
      if (slot !== Slot.Top && slot !== Slot.Bottom && slot !== Slot.Top2 && slot !== Slot.Bottom2) return -1;
      const n = valueNoise(x, y, z, 0.055, seed) * 0.7 + valueNoise(x, y, z, 0.022, seed + 1) * 0.3;
      const top = slot === Slot.Top || slot === Slot.Top2;
      if (n > 0.6) return top ? Slot.Top2 : Slot.Bottom2;
      if (n < 0.3) return top ? Slot.Bottom2 : Slot.Top2;
      return top ? Slot.Top : Slot.Bottom;
    });
  }

  // ---- joints ------------------------------------------------------------------------------
  sc.joint(hd(H.spine), 0.1 * g, [H.pelvis, H.spine]);
  sc.joint(hd(H.chest), 0.1 * g, [H.spine, H.chest]);
  sc.joint(hd(H.neck), 0.055 * g, [H.chest, H.neck]);
  sc.joint(hd(H.head), 0.05 * g, [H.neck, H.head]);
  for (const d of sides) {
    sc.joint(hd(d.ua), 0.058 * g, [H.chest, d.ua]);
    sc.joint(hd(d.fa), 0.046 * g, [d.ua, d.fa]);
    sc.joint(hd(d.hand), 0.034 * g, [d.fa, d.hand]);
    sc.joint(hd(d.th), 0.08 * g, [H.pelvis, d.th]);
    sc.joint(hd(d.sn), 0.058 * g, [d.th, d.sn]);
    sc.joint(hd(d.ft), 0.048 * g, [d.sn, d.ft]);
    sc.joint(hd(d.toe), 0.03 * g, [d.ft, d.toe]);
  }
  return sc.finish({ name: spec.name });
}

/** Face features painted on the head surface (eyes, brows, mouth), one voxel each. */
function faceCells(sc: Sculptor, skel: Skeleton, k: number): { paint(spec: HumanSpec): void; paintEyes(): void } {
  const s = sc.s;
  const hd = skel.restHead[H.head]!;
  const zc = hd[2] + 0.1 * k;
  const cell = (v: number): number => (Math.floor(v / s) + 0.5) * s;
  const eyeZ = cell(zc + 0.012 * k);
  const eyeX = cell(0.034 * k);
  // the front-most solid cell of the head at (x, z)
  const front = (x: number, z: number): V3 | null => {
    const i = Math.floor(x / s) - sc.lo[0];
    const kk = Math.floor(z / s) - sc.lo[2];
    const [nx, ny] = sc.dims;
    for (let j = ny - 1; j >= 0; j--) {
      const idx = i + nx * (j + ny * kk);
      if (sc.cells[idx] !== 0 && sc.bone[idx] === H.head) return [x, (j + sc.lo[1] + 0.5) * s, z];
    }
    return null;
  };
  const dot = (x: number, z: number, slot: SlotId): void => {
    const p = front(x, z);
    if (!p) return;
    sc.paint(sphere(p, s * 0.45), slot, { bones: [H.head], jitter: 0.02 });
  };
  const paintEyes = (): void => {
    for (const sx of [-1, 1]) dot(sx * eyeX, eyeZ, Slot.Detail);
  };
  return {
    paintEyes,
    paint(spec: HumanSpec): void {
      paintEyes();
      // lips: darker skin, two voxels
      const mouthZ = cell(zc - 0.05 * k);
      for (const x of [-0.5, 0.5]) {
        const p = front(x * s, mouthZ);
        if (p) sc.paint(sphere(p, s * 0.45), spec.beard ? Slot.Hair : Slot.Skin, { bones: [H.head], shade: spec.beard ? 0.7 : 0.72, jitter: 0 });
      }
    },
  };
}

// ---- presets -----------------------------------------------------------------------------

export interface HumanVariant {
  model: VoxelModel;
  palette: Palette;
  spec: HumanSpec;
}

/** Geometry key: variants sharing it can share meshes (only their palettes differ). */
export function geometryKey(s: HumanSpec): string {
  const b = s.build;
  return [s.female ? 'f' : 'm', b.height.toFixed(2), b.shoulders.toFixed(2), b.hips.toFixed(2), b.girth.toFixed(2), s.top, s.bottom, s.shoes, s.hair, s.headgear, +s.beard, +s.glasses, +s.vest, +s.backpack, +s.belt, +s.gloves, +s.kneepads, +s.camo, s.camo ? s.seed : 0, s.voxelSize.toFixed(4)].join(':');
}

/** A soldier / mercenary spec: helmet or balaclava, plate carrier, camouflage, boots. */
export function soldierSpec(seed: number, opts: { voxelSize?: number } = {}): HumanSpec {
  const r = new Rng(seed * 2654435761 + 17);
  const heavy = r.chance(0.3);
  return {
    build: { height: r.range(0.97, 1.05), shoulders: r.range(1.0, 1.08), hips: 1, girth: heavy ? 1.1 : r.range(0.98, 1.04) },
    female: false,
    top: 'uniform',
    bottom: 'uniform',
    shoes: 'boots',
    hair: r.pick(['buzz', 'short', 'none'] as const),
    headgear: r.pick(['helmet', 'helmetGoggles', 'helmet', 'balaclava', 'beret'] as const),
    beard: r.chance(0.2),
    glasses: false,
    vest: r.chance(0.85),
    backpack: r.chance(0.4),
    belt: true,
    gloves: r.chance(0.7),
    kneepads: r.chance(0.6),
    camo: true,
    voxelSize: opts.voxelSize ?? DEFAULT_VOXEL_SIZE,
    seed: seed & 3,
    name: `soldier-${seed}`,
  };
}

export function soldierPalette(seed: number, scheme?: number): Palette {
  const r = new Rng(seed * 40503 + 5);
  const c = CAMO_SCHEMES[scheme ?? r.int(0, CAMO_SCHEMES.length - 1)]!;
  return makePalette({
    skin: r.pick(SKIN_TONES),
    hair: r.pick(HAIR_COLOURS),
    ...c,
    metal: 0x2a2c2e,
    furniture: 0x1e1f21,
    detail: 0x121212,
    accent: r.chance(0.5) ? 0x7a2a26 : 0x2b3b52,
  });
}

/** A civilian spec: varied builds, clothes, hair and accessories. */
export function civilianSpec(seed: number, opts: { voxelSize?: number } = {}): HumanSpec {
  const r = new Rng(seed * 2246822519 + 3);
  const female = r.chance(0.5);
  const heavy = r.chance(0.2);
  return {
    build: female
      ? { height: r.range(0.9, 0.98), shoulders: r.range(0.86, 0.92), hips: r.range(1.02, 1.08), girth: heavy ? 1.1 : r.range(0.92, 1.0) }
      : { height: r.range(0.95, 1.04), shoulders: r.range(0.96, 1.04), hips: 1, girth: heavy ? 1.14 : r.range(0.95, 1.03) },
    female,
    top: r.pick(['tshirt', 'tshirt', 'longsleeve', 'jacket', 'hoodie', female ? 'tanktop' : 'tshirt'] as const),
    bottom: r.chance(0.25) ? 'shorts' : 'trousers',
    shoes: r.pick(['shoes', 'sneakers', 'sneakers'] as const),
    hair: female ? r.pick(['long', 'ponytail', 'bun', 'short'] as const) : r.pick(['short', 'short', 'buzz', 'none', 'long'] as const),
    headgear: r.chance(0.2) ? r.pick(['cap', 'beanie'] as const) : 'none',
    beard: !female && r.chance(0.3),
    glasses: r.chance(0.2),
    vest: false,
    backpack: r.chance(0.2),
    belt: r.chance(0.4),
    gloves: false,
    kneepads: false,
    camo: false,
    voxelSize: opts.voxelSize ?? DEFAULT_VOXEL_SIZE,
    seed,
    name: `civilian-${seed}`,
  };
}

export function civilianPalette(seed: number): Palette {
  const r = new Rng(seed * 69069 + 11);
  const top = r.pick(CLOTH_COLOURS);
  return makePalette({
    skin: r.pick(SKIN_TONES),
    hair: r.pick(HAIR_COLOURS),
    top,
    top2: r.pick(CLOTH_COLOURS),
    bottom: r.pick(TROUSER_COLOURS),
    bottom2: r.pick(TROUSER_COLOURS),
    shoes: r.pick(SHOE_COLOURS),
    gear: r.pick([0x3b3f46, 0x6b4f2a, 0x1f3a5f, 0x5a2d2d]),
    gearDark: 0x222326,
    metal: 0x9a9a9a,
    furniture: 0x3a2a1c,
    detail: 0x15120f,
    accent: r.pick([0x8a3b35, 0xa04848, 0x7a3030, 0xd8d8d8, 0x2a2a2a, 0x3060a0]),
  });
}

export function makeSoldier(seed: number, opts: { voxelSize?: number; scheme?: number } = {}): HumanVariant {
  const spec = soldierSpec(seed, opts);
  return { model: sculptHuman(spec), palette: soldierPalette(seed, opts.scheme), spec };
}

export function makeCivilian(seed: number, opts: { voxelSize?: number } = {}): HumanVariant {
  const spec = civilianSpec(seed, opts);
  return { model: sculptHuman(spec), palette: civilianPalette(seed), spec };
}

