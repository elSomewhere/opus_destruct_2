// Closed-form references for the analytic scenes, evaluated from the SAME
// compiled k0 the oracle used (per variant) plus continuum formulas from the
// physically correct section (effDims) for comparison.
//
// "discreteRBSM" is the exact small-displacement answer of the lattice model
// itself (rigid cells, face springs, unit-load method):
//   cantilever:        delta = P * sum_j arm_j^2 / k_b + (N-1) * P / k_s,
//                      arm_j = (N - 1.5 - j) h, j = 0..N-2
//   simply supported:  delta = (P/4) * (sum_j a_j^2 / k_b + (N-1) / k_s),
//                      a_j = min(x_j, L - x_j), x_j = (j + 0.5) h, L = (N-1) h
// k_b / k_s are the k0 components of the strip edges that carry the vertical
// load: shear along whichever of t1/t2 is vertical, bending about whichever
// of t1/t2 is horizontal and perpendicular to the strip.

function componentIndices(frame) {
  const isVertical = (v) => Math.abs(v[2]) > 0.5;
  const shearIndex = isVertical(frame.t1) ? 1 : 2;
  const bendIndex = isVertical(frame.t1) ? 4 : 3; // bending about the other tangent
  return { shearIndex, bendIndex };
}

function uniqueArchetype(model, axis) {
  const ids = new Set(model.edges.filter((e) => e.axis === axis).map((e) => e.archetype));
  if (ids.size !== 1) throw new Error(`analytic: expected one archetype on axis ${axis}, got ${ids.size}`);
  return [...ids][0];
}

function sectionTerms(model, axis) {
  const arch = model.tables.cellArchetypes.rc_floor;
  const mat = model.tables.materials[arch.materialId];
  const t = arch.effDims[2];
  const w = arch.effDims[axis === 0 ? 1 : 0]; // horizontal extent perpendicular to the strip
  const I = (w * t ** 3) / 12;
  const A = w * t;
  return { E: mat.E, G: mat.G, I, A, kappa: 5 / 6, w, t };
}

function plateNavierCoefficient(maxOdd = 1999) {
  let s = 0;
  for (let m = 1; m <= maxOdd; m += 2) {
    for (let n = 1; n <= maxOdd; n += 2) s += 1 / (m * m + n * n) ** 2;
  }
  return (4 / Math.PI ** 4) * s; // w = coef * P a^2 / D
}

let plateCoefCache = null;

export function analyticBlock(scene, model, archetypes, staticBlock) {
  const a = scene.analytic;
  if (!a) return null;
  const h = model.cellSize;
  const P = a.P;
  const probeName = Object.keys(scene.probes).find((k) => k !== 'minDz');
  const dz = staticBlock?.probes?.[probeName];
  const measured = dz == null ? null : -dz;

  if (a.kind === 'cantilever_tip_load' || a.kind === 'simply_supported_midspan_load') {
    const axis = a.axis;
    const archId = uniqueArchetype(model, axis);
    const k0 = archetypes[archId].k0;
    const { shearIndex, bendIndex } = componentIndices(model.edgeFrames[axis]);
    const kb = k0[bendIndex];
    const ks = k0[shearIndex];
    const N = a.cells;
    const L = (N - 1) * h;
    const sec = sectionTerms(model, axis);
    const EI = sec.E * sec.I;
    const kGA = sec.kappa * sec.G * sec.A;
    let discrete;
    let eb;
    let shear;
    if (a.kind === 'cantilever_tip_load') {
      let s = 0;
      for (let j = 0; j <= N - 2; j++) s += ((N - 1.5 - j) * h) ** 2;
      discrete = (P * s) / kb + ((N - 1) * P) / ks;
      eb = (P * L ** 3) / (3 * EI);
      shear = (P * L) / kGA;
    } else {
      let s = 0;
      for (let j = 0; j <= N - 2; j++) {
        const x = (j + 0.5) * h;
        s += Math.min(x, L - x) ** 2;
      }
      discrete = (P / 4) * (s / kb + (N - 1) / ks);
      eb = (P * L ** 3) / (48 * EI);
      shear = (P * L) / (4 * kGA);
    }
    return {
      kind: a.kind,
      P,
      cells: N,
      span: L,
      loadCell: a.loadCell,
      edgeArchetype: archId,
      bendingComponent: bendIndex,
      shearComponent: shearIndex,
      kb,
      ks,
      section: sec,
      measuredDeflection: measured,
      predictions: {
        discreteRBSM: discrete,
        eulerBernoulli: eb,
        timoshenko: eb + shear,
      },
      ratios: measured == null ? null : {
        measuredOverDiscreteRBSM: measured / discrete,
        measuredOverTimoshenko: measured / (eb + shear),
      },
    };
  }

  if (a.kind === 'plate_ss_center_load') {
    const sec = sectionTerms(model, 0);
    const span = (a.cells - 1) * h;
    const D = (sec.E * sec.t ** 3) / 12; // nu = 0 (the lattice has no Poisson coupling)
    if (plateCoefCache == null) plateCoefCache = plateNavierCoefficient();
    const kirchhoff = (plateCoefCache * P * span ** 2) / D;
    return {
      kind: a.kind,
      P,
      cells: a.cells,
      span,
      loadCell: a.loadCell,
      edgeArchetypes: { x: uniqueArchetype(model, 0), y: uniqueArchetype(model, 1) },
      plateRigidityD: D,
      navierCoefficient: plateCoefCache,
      measuredDeflection: measured,
      predictions: { kirchhoffNavier: kirchhoff },
      ratios: measured == null ? null : { measuredOverKirchhoff: measured / kirchhoff },
      note: 'Kirchhoff thin-plate (no shear) Navier series, nu = 0, supports on the boundary cell centres; reference only, not an exact lattice answer',
    };
  }
  throw new Error(`unknown analytic kind ${a.kind}`);
}
