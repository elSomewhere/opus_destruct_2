/**
 * A vehicle's wheel as a mesh (render/wheels.ts draws it): a tyre with rounded shoulders on an
 * alloy rim recessed into its outer face, the rim's spokes (or their blur, for a wheel spinning
 * fast) and its hub. Environment-neutral (tested under node).
 */
import { Material, Paint, TEXTURE_MATERIAL_BASE, TEXTURE_PAINT_BASE, type Vec3 } from '../engine/protocol.ts';
import type { MeshBuilder } from '../engine/vertex.ts';

/** Palette slot of the blurred spokes (renderer.ts PALETTE). */
export const RIM_BLUR_SLOT = 59;
const RUBBER = TEXTURE_MATERIAL_BASE + Material.Tyre;
const RIM = TEXTURE_PAINT_BASE + Paint.Silver;
const DARK = TEXTURE_MATERIAL_BASE + Material.CarFrame;
const BLUR = TEXTURE_MATERIAL_BASE + RIM_BLUR_SLOT;
const SEGMENTS = 28;
const SPOKES = 5;

/**
 * A wheel in its frame (x the way it rolls, y its axle, z up; centred): a tyre with rounded
 * shoulders on a rim recessed into its outer face, the rim's spokes (or their blur) and hub.
 */
export function buildWheel(b: MeshBuilder, r: number, w: number, side: number, blur: boolean): void {
  const s = side >= 0 ? 1 : -1;
  const hw = w / 2;
  const bevel = Math.min(0.035, 0.18 * w, 0.12 * r);
  const rimR = 0.66 * r;
  const recess = Math.min(0.06, 0.28 * w);
  // (a point at angle a about the axle, radius rho, along the axle y)
  const P = (a: number, rho: number, y: number): Vec3 => [Math.cos(a) * rho, y, Math.sin(a) * rho];
  const radial = (a: number): Vec3 => [Math.cos(a), 0, Math.sin(a)];
  const tri = (p: Vec3, q: Vec3, t: Vec3, n: Vec3, tex: number, ao: number): void => {
    // (wound counter-clockwise seen from where n points)
    const e1: Vec3 = [q[0] - p[0], q[1] - p[1], q[2] - p[2]];
    const e2: Vec3 = [t[0] - p[0], t[1] - p[1], t[2] - p[2]];
    const c: Vec3 = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
    const flip = c[0] * n[0] + c[1] * n[1] + c[2] * n[2] < 0;
    const i0 = b.vertex(p[0], p[1], p[2], n[0], n[1], n[2], ao, 0, 0, tex, 255, 0);
    const i1 = b.vertex(q[0], q[1], q[2], n[0], n[1], n[2], ao, 0, 0, tex, 255, 0);
    const i2 = b.vertex(t[0], t[1], t[2], n[0], n[1], n[2], ao, 0, 0, tex, 255, 0);
    if (flip) b.triangle(i0, i2, i1);
    else b.triangle(i0, i1, i2);
  };
  // a band between two rings (rho0, y0) -> (rho1, y1), smooth-shaded by its own normal per segment
  const band = (rho0: number, y0: number, rho1: number, y1: number, nOf: (a: number) => Vec3, tex: number, ao: number, segs = SEGMENTS): void => {
    for (let k = 0; k < segs; k++) {
      const a0 = (k / segs) * Math.PI * 2;
      const a1 = ((k + 1) / segs) * Math.PI * 2;
      const n = nOf((a0 + a1) / 2);
      const p00 = P(a0, rho0, y0);
      const p01 = P(a1, rho0, y0);
      const p10 = P(a0, rho1, y1);
      const p11 = P(a1, rho1, y1);
      tri(p00, p01, p11, n, tex, ao);
      tri(p00, p11, p10, n, tex, ao);
    }
  };
  const axial = (sgn: number) => (): Vec3 => [0, sgn, 0];
  const shoulder = (sgn: number) => (a: number): Vec3 => {
    const rd = radial(a);
    const l = Math.SQRT1_2;
    return [rd[0] * l, sgn * l, rd[2] * l];
  };
  // tread and shoulders
  band(r, -(hw - bevel), r, hw - bevel, radial, RUBBER, 1);
  band(r, hw - bevel, r - bevel, hw, shoulder(1), RUBBER, 0.95);
  band(r, -(hw - bevel), r - bevel, -hw, shoulder(-1), RUBBER, 0.95);
  // sidewalls, down to the rim
  band(r - bevel, s * hw, rimR, s * hw, axial(s), RUBBER, 0.9);
  band(r - bevel, -s * hw, rimR, -s * hw, axial(-s), RUBBER, 0.8);
  // the rim's lip (inside the recess, facing the axle) and its face at the bottom of the recess
  band(rimR, s * hw, rimR, s * (hw - recess), (a) => {
    const rd = radial(a);
    return [-rd[0], 0, -rd[2]];
  }, RIM, 0.7);
  const face = s * (hw - recess);
  band(rimR, face, 0.9 * rimR, face, axial(s), RIM, 0.75);
  if (blur) {
    band(0.9 * rimR, face, 0.24 * rimR, face, axial(s), BLUR, 0.8);
  } else {
    // spokes: SPOKES bright sectors, dark between them
    const segs = SPOKES * 4;
    for (let k = 0; k < segs; k++) {
      const a0 = (k / segs) * Math.PI * 2;
      const a1 = ((k + 1) / segs) * Math.PI * 2;
      const spoke = k % 4 === 0 || k % 4 === 1;
      const n: Vec3 = [0, s, 0];
      const q00 = P(a0, 0.9 * rimR, face);
      const q01 = P(a1, 0.9 * rimR, face);
      const q10 = P(a0, 0.24 * rimR, face);
      const q11 = P(a1, 0.24 * rimR, face);
      tri(q00, q01, q11, n, spoke ? RIM : DARK, spoke ? 0.85 : 0.35);
      tri(q00, q11, q10, n, spoke ? RIM : DARK, spoke ? 0.85 : 0.35);
    }
  }
  // the hub, a little proud of the face
  const hub = face + s * 0.012;
  band(0.24 * rimR, face, 0.24 * rimR, hub, radial, RIM, 0.8, 12);
  band(0.24 * rimR, hub, 0, hub, axial(s), RIM, 1, 12);
  // the inner face: a dark disc behind the spokes
  band(rimR, -s * hw, 0, -s * (hw - recess), axial(-s), DARK, 0.4, 16);
}
