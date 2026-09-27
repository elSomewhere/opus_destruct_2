// Model + table extraction from a compiled prototype engine (WorldGrid path).

const EPS = 1e-12;

function assert(cond, msg) {
  if (!cond) throw new Error(`fixture-export invariant violated: ${msg}`);
}

function sameNumbers(a, b, tol = 0) {
  if (!a || !b || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (!(Math.abs(Number(a[i]) - Number(b[i])) <= tol)) return false;
  }
  return true;
}

const THRESHOLD_KEYS = ['openN', 'compN', 't1', 't2', 'b1', 'b2', 'tau'];

function pickThresholds(t) {
  const out = {};
  for (const k of THRESHOLD_KEYS) out[k] = t[k];
  return out;
}

function sectionOut(s) {
  return {
    axialArea: s.axialArea,
    shearAreas: [...s.shearAreas],
    bendingInertias: [...s.bendingInertias],
    fiberDistances: [...(s.fiberDistances || [0, 0])],
    torsionJ: s.torsionJ,
    torsionRadius: s.torsionRadius,
  };
}

export function edgeAxis(cellA, cellB) {
  const d = [cellB.ix - cellA.ix, cellB.iy - cellA.iy, cellB.iz - cellA.iz];
  const nz = d.filter((v) => v !== 0);
  assert(nz.length === 1 && nz[0] === 1, `edge between (${cellA.ix},${cellA.iy},${cellA.iz}) and (${cellB.ix},${cellB.iy},${cellB.iz}) is not a +axis face pair`);
  return d.findIndex((v) => v !== 0);
}

// Frames + anchors are the authoring adapter's canonical per-axis choice
// (src/authoring/index.js). Verified for every edge; exported once.
export function extractConventions(P, engine) {
  const grid = engine.grid;
  const h = engine.cellSize;
  const frames = {};
  for (const edge of grid.interfaces) {
    const a = grid.cellById.get(edge.cellAId);
    const b = grid.cellById.get(edge.cellBId);
    const axis = edgeAxis(a, b);
    const q = Array.from(edge.frameA_local_q);
    assert(sameNumbers(q, edge.frameB_local_q, 0), `edge ${edge.id}: frameA != frameB`);
    const anchorA = [0, 0, 0];
    const anchorB = [0, 0, 0];
    anchorA[axis] = 0.5 * h;
    anchorB[axis] = -0.5 * h;
    assert(sameNumbers(edge.anchorA_local_pos3, anchorA, EPS), `edge ${edge.id}: non-canonical anchorA`);
    assert(sameNumbers(edge.anchorB_local_pos3, anchorB, EPS), `edge ${edge.id}: non-canonical anchorB`);
    if (!frames[axis]) {
      const R = P.quatToMat3(q); // row-major; columns are (n, t1, t2)
      frames[axis] = {
        q,
        n: [R[0], R[3], R[6]],
        t1: [R[1], R[4], R[7]],
        t2: [R[2], R[5], R[8]],
        anchorA,
        anchorB,
      };
    } else {
      assert(sameNumbers(frames[axis].q, q, 0), `edge ${edge.id}: axis ${axis} frame differs from other edges`);
    }
  }
  return frames;
}

export function extractModel(P, engine) {
  const grid = engine.grid;
  const lib = engine.library;
  const h = engine.cellSize;

  const supportIdByCell = new Map();
  for (const s of grid.supports) {
    if (!s.alive) continue;
    assert(!supportIdByCell.has(s.cellId), `cell ${s.cellId} has two supports`);
    supportIdByCell.set(s.cellId, s.id);
  }

  const cells = [...grid.cells].sort((x, y) => x.id - y.id).map((c) => {
    const arch = lib.getCellArchetype(c.cellArchetypeId);
    const mass = P.getCellMass(c, lib, h);
    const inertia = Array.from(P.getCellInertiaLocal(c, lib, h, mass));
    const out = {
      id: c.id,
      ijk: [c.ix, c.iy, c.iz],
      x0: Array.from(c.x0),
      archetype: c.cellArchetypeId,
      material: arch.materialId,
      mass,
      inertia,
      support: supportIdByCell.has(c.id) ? supportIdByCell.get(c.id) : null,
    };
    assert(c.q0[0] === 0 && c.q0[1] === 0 && c.q0[2] === 0 && c.q0[3] === 1, `cell ${c.id} q0 not identity`);
    if (c.appliedForce) out.appliedForce = Array.from(c.appliedForce);
    if (c.appliedMoment) out.appliedMoment = Array.from(c.appliedMoment);
    return out;
  });

  const edges = [...grid.interfaces].sort((x, y) => x.id - y.id).map((e) => {
    const a = grid.cellById.get(e.cellAId);
    const b = grid.cellById.get(e.cellBId);
    const arch = lib.getInterfaceArchetype(e.interfaceArchetypeId);
    assert(arch, `edge ${e.id}: missing archetype ${e.interfaceArchetypeId}`);
    assert(arch.profileId === e.interfaceProfileId, `edge ${e.id}: profile mismatch ${arch.profileId} vs ${e.interfaceProfileId}`);
    return {
      id: e.id,
      a: e.cellAId,
      b: e.cellBId,
      axis: edgeAxis(a, b),
      profile: e.interfaceProfileId,
      archetype: e.interfaceArchetypeId,
    };
  });

  const supports = [...grid.supports].filter((s) => s.alive).sort((x, y) => x.id - y.id).map((s) => {
    const cell = grid.cellById.get(s.cellId);
    assert(sameNumbers(s.xAnchor, cell.x0, 0), `support ${s.id}: anchor != cell x0`);
    assert(sameNumbers(s.anchorLocalPos3, [0, 0, 0], 0), `support ${s.id}: non-zero local anchor`);
    return {
      id: s.id,
      cell: s.cellId,
      profile: s.supportProfileId,
      label: s.label,
      // Per-support spring override (streaming proxies only); authored
      // supports use the profile's spring table.
      springOverride: s.spring ? Array.from(s.spring) : null,
      springScale: s.springScale,
    };
  });

  const usedMaterials = new Set(cells.map((c) => c.material));
  const usedCellArchetypes = new Set(cells.map((c) => c.archetype));
  const usedProfiles = new Set(edges.map((e) => e.profile));
  const usedSupportProfiles = new Set(supports.map((s) => s.profile));

  const materials = {};
  for (const id of [...usedMaterials].sort()) materials[id] = { ...lib.getMaterial(id) };
  const cellArchetypes = {};
  for (const id of [...usedCellArchetypes].sort()) {
    const a = lib.getCellArchetype(id);
    cellArchetypes[id] = {
      id: a.id,
      label: a.label,
      family: a.family,
      kind: a.kind,
      materialId: a.materialId,
      effDims: [...a.effDims],
      geometryVolume: a.geometryVolume,
      gravityMassVolume: a.gravityMassVolume,
      sectionsByAxis: a.sectionsByAxis.map(sectionOut),
    };
  }
  const interfaceProfiles = {};
  for (const id of [...usedProfiles].sort()) {
    const p = { ...lib.getInterfaceProfile(id) };
    delete p.description;
    interfaceProfiles[id] = p;
  }
  const supportProfiles = {};
  for (const id of [...usedSupportProfiles].sort()) {
    const p = lib.getSupportProfile(id);
    supportProfiles[id] = {
      id: p.id,
      label: p.label,
      exactMask: [...p.exactMask],
      spring: [...p.spring],
      unilateral: [...p.unilateral],
    };
  }

  return {
    cellSize: h,
    cells,
    edges,
    supports,
    tables: { materials, cellArchetypes, interfaceProfiles, supportProfiles },
    edgeFrames: extractConventions(P, engine),
  };
}

// Interface archetypes actually referenced by alive edges, keyed by id.
export function exportInterfaceArchetypes(P, engine) {
  const lib = engine.library;
  const used = new Set(engine.grid.interfaces.map((e) => e.interfaceArchetypeId));
  const out = {};
  for (const id of [...used].sort()) {
    const a = lib.getInterfaceArchetype(id);
    // --- lossless-ness guards: everything the law reads is exported ---
    const diag = P.mat6Diag(a.k0Diag);
    assert(sameNumbers(a.k0Matrix, diag, 0), `${id}: k0Matrix is not diag(k0Diag)`);
    assert(sameNumbers(THRESHOLD_KEYS.map((k) => a.damageThresholds.onset[k]), THRESHOLD_KEYS.map((k) => a.thresholds[k]), 0), `${id}: damage onset != thresholds`);
    assert(sameNumbers(THRESHOLD_KEYS.map((k) => a.damageThresholds.break[k]), THRESHOLD_KEYS.map((k) => a.breakThresholds[k]), 0), `${id}: damage break != breakThresholds`);
    assert(sameNumbers(a.yieldDelta6, ['openN', 't1', 't2', 'b1', 'b2', 'tau'].map((k) => a.thresholds[k]), 0), `${id}: yieldDelta6 mismatch`);
    assert(sameNumbers(a.breakDelta6, ['openN', 't1', 't2', 'b1', 'b2', 'tau'].map((k) => a.breakThresholds[k]), 0), `${id}: breakDelta6 mismatch`);
    assert(a.compressionYieldDelta === a.thresholds.compN && a.compressionBreakDelta === a.breakThresholds.compN, `${id}: compression deltas mismatch`);
    assert(a.damageModel === 'continuum_regularized_v1', `${id}: unexpected damage model ${a.damageModel}`);
    const cc = a.continuumCalibration;
    out[id] = {
      id,
      label: a.label,
      profileId: a.profileId,
      directionWeights: [...a.directionWeights],
      k0: [...a.k0Diag],
      onset: pickThresholds(a.thresholds),
      break: pickThresholds(a.breakThresholds),
      forceCap6: [...a.forceCap6],
      compressionCap: a.compressionCap,
      residualStiffness: a.residualStiffness,
      kappaOnset: a.kappaOnset,
      kappaBreak: a.kappaBreak,
      damageModel: a.damageModel,
      damageNorm: a.damageThresholds.norm,
      plasticFraction: a.plasticFraction,
      plasticHardening: a.plasticHardening,
      plasticYield: [...a.plasticYield],
      plasticBreak: a.plasticBreak,
      calibration: {
        materialAId: cc.materialAId,
        materialBId: cc.materialBId,
        sectionA: sectionOut(cc.sectionA),
        sectionB: sectionOut(cc.sectionB),
        interfaceArea: cc.interfaceArea,
        fractureEnergyModeI: cc.fractureEnergyModeI,
        fractureEnergyModeII: cc.fractureEnergyModeII,
        stiffnessRule: cc.stiffnessRule,
        strengthRule: cc.strengthRule,
        fractureRule: cc.fractureRule,
      },
    };
  }
  return out;
}
