// Section-property defect variants, applied WITHOUT touching the prototype.
//
// After initScenario() has compiled the grid, every interface archetype in
// engine.library.interfaceArchetypes is REBUILT in memory by the prototype's
// own buildInterfaceArchetypeFromCells() (archetypes.js ->
// calibration/continuum_to_voxel.js), fed with cell archetypes whose
// `sectionsByAxis` have been corrected. The rebuilt fields replace the old
// ones on the SAME archetype object (same id), so every edge referencing it
// picks the change up. This must happen before the first solve/tick (the XPBD
// runtime and the oracle read k0Diag/thresholds per call; nothing is cached
// before the first step).
//
// For the 'prototype' variant the rebuild is also run and asserted to be a
// bit-identical no-op, which validates the method.
//
// Defect (a) fixY: rectangularSectionsForAxis(effDims, axis=1) returns
//   bendingInertias [sx*sz^3/12, sz*sx^3/12] and fiberDistances [sz/2, sx/2],
//   i.e. ordered [about x, about z], but the Y-edge frame is
//   (n,t1,t2) = (y,z,x) and component 3 bends about t1 = z, component 4 about
//   t2 = x. Corrected: bendingInertias [sz*sx^3/12, sx*sz^3/12],
//   fiberDistances [sx/2, sz/2]. (X and Z edges are already consistent.)
// Defect (b) fixJ: torsionJ = A*(s1^2+s2^2)/12 is the polar moment.
//   Corrected: Saint-Venant torsion constant of the a x b rectangle
//   (a >= b): J = beta*a*b^3 with the exact series
//   beta = (1/3) * [1 - (192/pi^5)*(b/a)*sum_{n=1,3,5..} tanh(n*pi*a/(2b))/n^5].
//   torsionRadius (only used as the torsion-capacity lever arm) is left
//   unchanged, so the onset rotation tau/(G*r)*kappa is unchanged and the
//   torsion capacity scales with J.

import { isDeepStrictEqual } from 'node:util';
import { toPlain } from './json.mjs';

export const VARIANTS = ['prototype', 'fixY', 'fixJ', 'fixYJ'];

export function variantFlags(variant) {
  switch (variant) {
    case 'prototype': return { fixY: false, fixJ: false };
    case 'fixY': return { fixY: true, fixJ: false };
    case 'fixJ': return { fixY: false, fixJ: true };
    case 'fixYJ': return { fixY: true, fixJ: true };
    default: throw new Error(`unknown variant ${variant}`);
  }
}

export function stVenantBeta(a0, b0) {
  const a = Math.max(a0, b0);
  const b = Math.min(a0, b0);
  if (!(b > 0)) return 0;
  const r = b / a;
  let sum = 0;
  for (let n = 1; n < 20001; n += 2) {
    const term = Math.tanh((n * Math.PI) / (2 * r)) / n ** 5;
    sum += term;
    if (term < 1e-20 * sum) break;
  }
  return (1 / 3) * (1 - (192 / Math.PI ** 5) * r * sum);
}

export function stVenantJ(p, q) {
  const a = Math.max(p, q);
  const b = Math.min(p, q);
  return stVenantBeta(a, b) * a * b ** 3;
}

// Section dims (the two lattice extents orthogonal to the edge axis) in the
// order the prototype's rectangularSectionsForAxis uses.
function sectionDims(effDims, axis) {
  const [sx, sy, sz] = effDims;
  if (axis === 0) return [sy, sz];
  if (axis === 1) return [sx, sz];
  return [sx, sy];
}

export function correctedSections(cellArchetype, flags) {
  const [sx, , sz] = cellArchetype.effDims;
  return cellArchetype.sectionsByAxis.map((sec, axis) => {
    const out = {
      axialArea: sec.axialArea,
      shearAreas: [...sec.shearAreas],
      bendingInertias: [...sec.bendingInertias],
      fiberDistances: [...(sec.fiberDistances || [0, 0])],
      torsionJ: sec.torsionJ,
      torsionRadius: sec.torsionRadius,
    };
    if (flags.fixY && axis === 1) {
      out.bendingInertias = [sz * sx ** 3 / 12, sx * sz ** 3 / 12];
      out.fiberDistances = [sx * 0.5, sz * 0.5];
      // Sanity: the correction is exactly the [0]<->[1] swap.
      if (out.bendingInertias[0] !== sec.bendingInertias[1] || out.bendingInertias[1] !== sec.bendingInertias[0]
        || out.fiberDistances[0] !== sec.fiberDistances[1] || out.fiberDistances[1] !== sec.fiberDistances[0]) {
        // Not fatal (floating-point association), but must be within rounding.
        const rel = (x, y) => Math.abs(x - y) / Math.max(Math.abs(x), Math.abs(y), 1e-300);
        if (rel(out.bendingInertias[0], sec.bendingInertias[1]) > 1e-12 || rel(out.bendingInertias[1], sec.bendingInertias[0]) > 1e-12) {
          throw new Error(`fixY correction for ${cellArchetype.id} is not a bending swap`);
        }
      }
    }
    if (flags.fixJ) {
      const [p, q] = sectionDims(cellArchetype.effDims, axis);
      out.torsionJ = stVenantJ(p, q);
    }
    return out;
  });
}

function normalizeLikeLibrary(P, archetype) {
  // Mirrors ArchetypeLibrary.getOrCreateInterfaceArchetype post-processing.
  archetype.k0Diag = [...archetype.k0Diag];
  archetype.k0Matrix = Float64Array.from(archetype.k0Matrix || P.mat6Diag(archetype.k0Diag));
  archetype.plasticYield = [...(archetype.plasticYield || [0, 0, 0, 0, 0, 0])];
  return archetype;
}

function snapshot(a) {
  // Order-independent, typed-array-normalised view; the per-archetype
  // useCoupledPlasticity stamp (set by applySettings) is not part of the
  // compiled calibration.
  const { useCoupledPlasticity, ...rest } = a;
  return toPlain(rest);
}

// Rebuild every compiled interface archetype for `variant`. Returns a patch
// report (which archetypes changed and how).
export function applyVariant(P, engine, variant) {
  const flags = variantFlags(variant);
  const lib = engine.library;
  const grid = engine.grid;
  const patchedCellArch = new Map();
  for (const [id, ca] of lib.cellArchetypes) {
    patchedCellArch.set(id, { ...ca, sectionsByAxis: correctedSections(ca, flags) });
  }
  // The FIRST edge (in authoring order) that references an archetype is the
  // one that created it; reuse its (A, B) orientation for the rebuild.
  const firstEdge = new Map();
  for (const e of [...grid.interfaces].sort((x, y) => x.id - y.id)) {
    if (!firstEdge.has(e.interfaceArchetypeId)) firstEdge.set(e.interfaceArchetypeId, e);
  }
  const changed = [];
  for (const [id, arch] of lib.interfaceArchetypes) {
    const edge = firstEdge.get(id);
    if (!edge) continue; // compiled but unused (cannot happen on a fresh grid)
    const A = patchedCellArch.get(grid.cellById.get(edge.cellAId).cellArchetypeId);
    const B = patchedCellArch.get(grid.cellById.get(edge.cellBId).cellArchetypeId);
    const rebuilt = normalizeLikeLibrary(P, P.buildInterfaceArchetypeFromCells({
      library: lib,
      cellArchetypeA: A,
      cellArchetypeB: B,
      interfaceProfile: lib.getInterfaceProfile(arch.profileId),
      directionWeights: arch.directionWeights,
      id,
    }));
    const before = snapshot(arch);
    const after = snapshot(rebuilt);
    const identical = isDeepStrictEqual(before, after);
    if (variant === 'prototype') {
      if (!identical) throw new Error(`prototype-variant rebuild of ${id} is not bit-identical`);
      continue;
    }
    if (identical) continue;
    const k0Before = [...arch.k0Diag];
    const coupled = arch.useCoupledPlasticity;
    for (const key of Object.keys(arch)) delete arch[key];
    Object.assign(arch, rebuilt);
    if (coupled !== undefined) arch.useCoupledPlasticity = coupled;
    changed.push({ id, k0Before, k0After: [...arch.k0Diag] });
  }
  return {
    variant,
    flags,
    changedArchetypes: changed.map((c) => c.id).sort(),
    k0Ratios: Object.fromEntries(changed.sort((x, y) => (x.id < y.id ? -1 : 1)).map((c) => [c.id, c.k0After.map((v, i) => v / c.k0Before[i])])),
  };
}

export function variantDescription(variant) {
  const flags = variantFlags(variant);
  const parts = [];
  if (!flags.fixY && !flags.fixJ) parts.push('prototype as-is (both section defects present)');
  if (flags.fixY) {
    parts.push('fixY: axis-1 (Y-edge) sections get bendingInertias=[sz*sx^3/12, sx*sz^3/12] and fiberDistances=[sx/2, sz/2] (swap of the prototype order), because the Y frame is (n,t1,t2)=(y,z,x): component 3 bends about t1=z, component 4 about t2=x');
  }
  if (flags.fixJ) {
    parts.push('fixJ: every axis section gets torsionJ = beta*a*b^3 (Saint-Venant, a>=b the two section extents orthogonal to the axis, beta = (1/3)[1-(192/pi^5)(b/a) sum_{n odd} tanh(n*pi*a/(2b))/n^5]); torsionRadius unchanged');
  }
  return parts.join('; ');
}
