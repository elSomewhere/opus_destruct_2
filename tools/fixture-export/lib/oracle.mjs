// ORACLE runs: the implicit f64 cohesive_dynamics core (CohesiveVoxelDynamicsSolver).
//
//  static  - one quasi-static Newton solve of the intact scene with damage
//            FROZEN at 0 on every edge (edge.damageFrozen = 0, the
//            prototype's own operator-split hook honoured by
//            CorotationalInterfaceElement.evaluate and the analytic tangent),
//            no ruptures (maxBreaksPerStep = 0), no detached deletion, tight
//            tolerances. This is the damage-free corotational equilibrium.
//  outcome - the scene run to closure with the oracle's default settings and
//            detached policy 'delete' (plus the scene's own protocol).

import { makeEngine } from './prototype.mjs';
import { applyVariant } from './variants.mjs';

export const STATIC_SOLVER_SETTINGS = Object.freeze({
  maxBreaksPerStep: 0,
  detachedDebrisPolicy: 'keep',
  deleteDetachedFragments: false,
  iterations: 500,
  activeSetIterations: 4,
  // Wider per-iteration Newton step clamps than the engine defaults
  // (0.5 m / 0.25 rad): they only limit step length, not the equilibrium,
  // and let large-deflection elastic solves (plasticBeam) converge.
  maxTranslationIncrement: 2,
  maxRotationIncrement: 1,
  // Floor: _solveAnchoredComponentGen clamps residualTolerance to >= 1e-12
  // (max_i |r_i| / K_ii, i.e. a displacement-like measure).
  residualTolerance: 1e-12,
  incrementTolerance: 1e-15,
  linearMaxIterations: 20000,
  linearRelativeTolerance: 1e-12,
  linearTolerance: 1e-16,
});

function vsub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function vnorm(a) { return Math.hypot(a[0], a[1], a[2]); }

export function initScene(P, scene, physicsCoreId) {
  // initScenario() silently falls back to building3 for unknown names.
  if (!P.ScenarioRegistry[scene.scenario]) throw new Error(`scenario ${scene.scenario} is not registered`);
  const engine = makeEngine(P, physicsCoreId);
  if (scene.dimensions) engine.initScenario(scene.scenario, { dimensions: scene.dimensions });
  else engine.initScenario(scene.scenario);
  return engine;
}

// Cells reachable from an anchorage-providing support through alive edges.
export function anchoredCellIds(P, engine, policy = 'SOLVE', container = engine.grid) {
  const grid = container;
  const adj = new Map();
  for (const c of grid.cells) if (c.alive) adj.set(c.id, []);
  for (const e of grid.interfaces) {
    if (!e.alive || !adj.has(e.cellAId) || !adj.has(e.cellBId)) continue;
    adj.get(e.cellAId).push(e.cellBId);
    adj.get(e.cellBId).push(e.cellAId);
  }
  const seen = new Set();
  const queue = [];
  for (const s of grid.supports) {
    if (!s.alive || !adj.has(s.cellId)) continue;
    if (!P.supportProvidesAnchorage(s, engine.library, P.ReachabilityPolicy[policy])) continue;
    if (!seen.has(s.cellId)) { seen.add(s.cellId); queue.push(s.cellId); }
  }
  for (let i = 0; i < queue.length; i++) {
    for (const nb of adj.get(queue[i])) {
      if (!seen.has(nb)) { seen.add(nb); queue.push(nb); }
    }
  }
  return seen;
}

// Records per-step (= per pass) oracle statistics by wrapping the solver
// INSTANCE's step(); a convergenceLog callback is injected into the step's
// settings copy to capture the inner-Newton convergence flags.
function instrumentSteps(engine) {
  const solver = engine.cohesiveDynamics;
  const orig = solver.step.bind(solver);
  const rec = { phase: 'closure', passes: [], components: [] };
  solver.step = (container, settings) => {
    let stepLog = null;
    let enter = null;
    const passIndex = rec.passes.length + 1;
    const convergenceLog = (r) => {
      if (r.phase === 'enter') enter = r;
      else if (r.phase === 'exit') {
        rec.components.push({
          pass: passIndex,
          component: enter?.componentId ?? null,
          cells: enter?.cellCount ?? null,
          dofs: enter?.dofCount ?? null,
          anchored: !!enter?.anchored,
          converged: !!r.converged,
          iterations: r.iterations,
          stopReason: r.stopReason,
          initialResidual: enter?.initialResidual ?? null,
          finalResidual: r.finalResidual,
          pcgIterations: r.totalPcgIterations,
        });
        enter = null;
      } else if (r.phase === 'step') stepLog = r;
    };
    const s = orig(container, { ...settings, convergenceLog });
    // Last Newton solve per ANCHORED component in this pass. Unanchored
    // components only ever get one iteration (they are debris), so the
    // step-level innerConverged flag is false whenever one exists.
    const lastByComponent = new Map();
    for (const c of rec.components) if (c.pass === passIndex && c.anchored) lastByComponent.set(c.component, c);
    const anchoredSolves = [...lastByComponent.values()];
    rec.passes.push({
      pass: passIndex,
      phase: rec.phase,
      quiet: !!s.quiet,
      anchoredConverged: anchoredSolves.every((c) => c.converged),
      anchoredMaxResidual: anchoredSolves.reduce((m, c) => Math.max(m, c.finalResidual), 0),
      innerConverged: stepLog ? !!stepLog.innerConverged : null,
      contactActiveSetStable: stepLog ? !!stepLog.contactActiveSetStable : null,
      newtonIterations: s.iterations,
      pcgIterations: s.pcgIterations,
      residual: s.residual,
      ruptured: s.ruptured,
      detachedDeleted: s.detachedDeleted,
      maxDemand: s.maxDemand,
      maxDamage: s.maxDamage,
      contacts: s.contacts,
      reason: s.diagnostic?.reason ?? null,
    });
    return s;
  };
  return rec;
}

// Logs rupture / deletion ORDER by wrapping the grid INSTANCE's killEdge and
// killCell. Edges killed inside killCell (incident edges of a deleted cell)
// are not ruptures and are not logged as such.
function instrumentKills(engine, rec) {
  const grid = engine.grid;
  const origKillEdge = grid.killEdge.bind(grid);
  const origKillCell = grid.killCell.bind(grid);
  let inKillCell = 0;
  const kills = { rupturedEdgeIds: [], deletedCellIds: [], blastKilledCellIds: [], blastFracturedEdgeIds: [] };
  grid.killEdge = (idOrKey) => {
    const edge = typeof idOrKey === 'string' ? grid.interfaceByKey.get(idOrKey) : grid.interfaceById.get(idOrKey);
    const ok = origKillEdge(idOrKey);
    if (ok && inKillCell === 0) {
      if (rec.phase === 'blast_mutation') kills.blastFracturedEdgeIds.push(edge.id);
      else kills.rupturedEdgeIds.push(edge.id);
    }
    return ok;
  };
  grid.killCell = (cellOrId) => {
    const cell = typeof cellOrId === 'object' ? cellOrId : grid.cellById.get(cellOrId);
    inKillCell += 1;
    try {
      const n = origKillCell(cellOrId);
      if (n && cell) {
        if (rec.phase === 'blast_mutation') kills.blastKilledCellIds.push(cell.id);
        else kills.deletedCellIds.push(cell.id);
      }
      return n;
    } finally {
      inKillCell -= 1;
    }
  };
  return kills;
}

function probeValues(scene, cellsView) {
  const out = {};
  for (const [name, pred] of Object.entries(scene.probes)) {
    let v = null;
    for (const c of cellsView) {
      if (c.u == null || !pred(c)) continue;
      v = v == null ? c.u[2] : Math.min(v, c.u[2]);
    }
    out[name] = v;
  }
  return out;
}

function boxOnsetExceeded(delta, onset) {
  const d0 = delta[0];
  if (d0 >= 0 ? d0 > onset.openN : -d0 > onset.compN) return true;
  const keys = ['t1', 't2', 'b1', 'b2', 'tau'];
  for (let i = 0; i < 5; i++) if (Math.abs(delta[i + 1]) > onset[keys[i]]) return true;
  return false;
}

export function runStatic(P, scene, variant) {
  const engine = initScene(P, scene, P.PhysicsCoreId.COHESIVE_DYNAMICS);
  const patch = applyVariant(P, engine, variant);
  engine.applySettings({ physicsCoreId: P.PhysicsCoreId.COHESIVE_DYNAMICS, ...scene.oracle.settings });
  const grid = engine.grid;
  const lib = engine.library;
  for (const e of grid.interfaces) e.damageFrozen = 0;
  const rec = instrumentSteps(engine);
  rec.phase = 'static';
  const settings = { ...STATIC_SOLVER_SETTINGS, gravity: scene.gravity };
  const t0 = performance.now();
  const step = engine.stepCohesiveDynamics(settings);
  const wallMs = performance.now() - t0;
  const pass = rec.passes[0];

  const anchored = anchoredCellIds(P, engine);
  const cellsSorted = [...grid.cells].sort((a, b) => a.id - b.id);
  const edgesSorted = [...grid.interfaces].sort((a, b) => a.id - b.id);
  const supportsSorted = [...grid.supports].filter((s) => s.alive).sort((a, b) => a.id - b.id);

  // Evaluate every edge at the committed configuration (trials are reset
  // after commit, so the increment is zero): this reproduces the committed
  // generalized forces and yields phi and the 12-DOF internal force
  // f = J^T r (world forces + world moments about each cell centre).
  const element = new P.CorotationalInterfaceElement(engine.cellSize);
  const fint = new Map(cellsSorted.map((c) => [c.id, new Float64Array(6)]));
  const edgeForce = [];
  const edgePhi = [];
  let maxPhi = 0;
  let edgesBeyondOnset = 0;
  let maxResultantMismatch = 0;
  for (const e of edgesSorted) {
    const a = grid.cellById.get(e.cellAId);
    const b = grid.cellById.get(e.cellBId);
    if (!e.alive || !a.alive || !b.alive || !anchored.has(a.id) || !anchored.has(b.id)) {
      edgeForce.push(null);
      edgePhi.push(null);
      continue;
    }
    const arch = lib.getInterfaceArchetype(e.interfaceArchetypeId);
    const r = element.evaluate(e, a, b, arch, { writeBack: false, needTangent: false });
    // Export the committed resultant; the re-evaluation must reproduce it up
    // to kinematic round-off (|delta_inc| ~ 1e-16 at the commit state).
    const f6 = Array.from(e.committed.resultant6);
    let scale = 1;
    for (let i = 0; i < 6; i++) scale = Math.max(scale, Math.abs(f6[i]));
    const k0max = Math.max(...arch.k0Diag);
    for (let i = 0; i < 6; i++) {
      const diff = Math.abs(r.constitutive.resultant6[i] - f6[i]);
      maxResultantMismatch = Math.max(maxResultantMismatch, diff / (1e-9 * scale + 1e-15 * k0max));
    }
    edgeForce.push(f6);
    edgePhi.push(r.constitutive.phi);
    maxPhi = Math.max(maxPhi, r.constitutive.phi);
    const delta = Array.from(e.committed.total6, (v, i) => v - e.committed.plastic6[i]);
    if (boxOnsetExceeded(delta, arch.thresholds)) edgesBeyondOnset += 1;
    const f12 = r.fint12;
    const fa = fint.get(a.id);
    const fb = fint.get(b.id);
    for (let i = 0; i < 6; i++) { fa[i] += f12[i]; fb[i] += f12[6 + i]; }
  }
  for (const e of grid.interfaces) e.damageFrozen = null;
  if (maxResultantMismatch > 1) {
    throw new Error(`${scene.id}/${variant}: committed resultants not reproduced at commit state (mismatch/tolerance ${maxResultantMismatch})`);
  }

  const g = scene.gravity;
  const extOf = (c) => {
    const m = P.getCellMass(c, lib, engine.cellSize);
    return [
      (c.appliedForce?.[0] || 0),
      (c.appliedForce?.[1] || 0),
      m * g + (c.appliedForce?.[2] || 0),
      (c.appliedMoment?.[0] || 0),
      (c.appliedMoment?.[1] || 0),
      (c.appliedMoment?.[2] || 0),
    ];
  };
  const supportedCells = new Set(supportsSorted.map((s) => s.cellId));
  const totalExternal = [0, 0, 0];
  const totalReaction = [0, 0, 0];
  let maxFreeResidualForce = 0;
  let maxFreeResidualMoment = 0;
  let loadScale = 0;
  const cellsOut = { u: [], theta: [] };
  const view = [];
  for (const c of cellsSorted) {
    const isAnchored = c.alive && anchored.has(c.id);
    if (!isAnchored) {
      cellsOut.u.push(null);
      cellsOut.theta.push(null);
      view.push({ ijk: [c.ix, c.iy, c.iz], u: null });
      continue;
    }
    const u = vsub(c.xCommit, c.x0);
    const qRel = P.quatMul(c.qCommit, P.quatConjugate(c.q0));
    const theta = Array.from(P.quatToRotvec(qRel));
    cellsOut.u.push(u);
    cellsOut.theta.push(theta);
    view.push({ ijk: [c.ix, c.iy, c.iz], u });
    const fe = extOf(c);
    for (let i = 0; i < 3; i++) totalExternal[i] += fe[i];
    loadScale = Math.max(loadScale, vnorm(fe));
    if (!supportedCells.has(c.id)) {
      const fi = fint.get(c.id);
      maxFreeResidualForce = Math.max(maxFreeResidualForce, Math.hypot(fi[0] - fe[0], fi[1] - fe[1], fi[2] - fe[2]));
      maxFreeResidualMoment = Math.max(maxFreeResidualMoment, Math.hypot(fi[3] - fe[3], fi[4] - fe[4], fi[5] - fe[5]));
    }
  }
  // Support reaction on the supported cell = sum of edge internal forces on
  // it minus its external load (world frame; moment about the cell centre).
  const reactions = supportsSorted.map((s) => {
    const c = grid.cellById.get(s.cellId);
    if (!c.alive || !anchored.has(c.id)) return null;
    const fi = fint.get(c.id);
    const fe = extOf(c);
    const r = [0, 1, 2, 3, 4, 5].map((i) => fi[i] - fe[i]);
    for (let i = 0; i < 3; i++) totalReaction[i] += r[i];
    return r;
  });

  // Converged = every ANCHORED component's last Newton solve converged and
  // the contact active set is stable. Unanchored components only ever get
  // one Newton iteration in the oracle (and are reported as null here).
  const lastByComponent = new Map();
  for (const c of rec.components) if (c.anchored) lastByComponent.set(c.component, c);
  const anchoredCount = anchored.size;
  const anchoredComponentsConverged = [...lastByComponent.values()].every((c) => c.converged);
  // The step only reports contactActiveSetStable=true together with a fully
  // converged pass (unanchored components included), so it is consulted
  // only when contacts are actually enabled.
  const contactsEnabled = !!engine._cohesiveSettings(settings).contactEnabled;
  const converged = anchoredCount > 0 && anchoredComponentsConverged
    && (!contactsEnabled || !!pass?.contactActiveSetStable);
  return {
    engine,
    patch,
    block: {
      method: 'single cohesive_dynamics quasi-static step; edge.damageFrozen=0 on all edges; maxBreaksPerStep=0; no detached deletion',
      solverSettings: settings,
      status: anchoredCount === 0 ? 'no_anchored_cells' : (converged ? 'converged' : 'not_converged'),
      converged,
      anchoredCellCount: anchoredCount,
      newtonIterations: step?.iterations ?? null,
      pcgIterations: step?.pcgIterations ?? null,
      residualNormalized: step?.residual ?? null,
      residualNormalizedAnchored: [...lastByComponent.values()].reduce((m, c) => Math.max(m, c.finalResidual), 0),
      components: rec.components,
      anchoredComponentsConverged,
      stepInnerConverged: !!pass?.innerConverged,
      contactsEnabled,
      maxPhi,
      edgesBeyondOnset,
      elasticRegime: edgesBeyondOnset === 0,
      equilibrium: {
        totalExternalForce: totalExternal,
        totalReactionForce: totalReaction,
        maxFreeCellResidualForce: maxFreeResidualForce,
        maxFreeCellResidualMoment: maxFreeResidualMoment,
        maxCellLoad: loadScale,
      },
      cells: cellsOut,
      edges: { force6: edgeForce, phi: edgePhi },
      supports: { reaction6: reactions },
      probes: probeValues(scene, view),
    },
    wallMs,
  };
}

export function runOutcome(P, scene, variant) {
  const engine = initScene(P, scene, P.PhysicsCoreId.COHESIVE_DYNAMICS);
  applyVariant(P, engine, variant);
  engine.applySettings({ physicsCoreId: P.PhysicsCoreId.COHESIVE_DYNAMICS, ...scene.oracle.settings });
  const grid = engine.grid;
  const initialCellCount = grid.cells.filter((c) => c.alive).length;
  const rec = instrumentSteps(engine);
  const kills = instrumentKills(engine, rec);
  const t0 = performance.now();
  let blastInfo = null;
  const overrides = scene.gravity === P.GRAVITY ? {} : { gravity: scene.gravity };
  let closureResult = null;

  if (scene.oracle.blast) {
    const b = scene.oracle.blast;
    const origMutation = engine._applyBlastMutation.bind(engine);
    engine._applyBlastMutation = (center, radius, options) => {
      rec.phase = 'blast_mutation';
      try {
        const res = origMutation(center, radius, options);
        blastInfo = res?.blastInfo ? { ...res.blastInfo } : null;
        return res;
      } finally {
        rec.phase = 'blast_closure';
      }
    };
    rec.phase = 'blast_closure';
    const blastResult = engine.applyBlastAtWorld([...b.center], b.radius, { ...b.options });
    closureResult = { converged: !!blastResult?.converged };
  }
  if (scene.oracle.protocol === 'solveToClosure') {
    rec.phase = 'closure';
    const r = engine.solveToClosure({ ...overrides });
    closureResult = { converged: !!r.converged, reason: r.stats?.lastSolveDiagnostic?.reason ?? null };
  } else if (scene.oracle.protocol === 'stepLoop') {
    rec.phase = 'stepLoop';
    for (let i = 0; i < scene.oracle.ticks; i++) engine.stepCohesiveDynamics({ ...overrides });
  } else {
    throw new Error(`unknown oracle protocol ${scene.oracle.protocol}`);
  }
  const wallMs = performance.now() - t0;

  // Closure = first pass from which every later pass is quiet.
  const passes = rec.passes;
  let closurePass = null;
  for (let i = passes.length - 1; i >= 0; i--) {
    if (!passes[i].quiet) break;
    closurePass = passes[i].pass;
  }
  const converged = closurePass != null;
  const keep = converged ? passes.filter((p) => p.pass <= closurePass) : passes;
  const trailingQuietPasses = passes.length - keep.length;

  const aliveCells = [...grid.cells].filter((c) => c.alive).sort((a, b) => a.id - b.id);
  const view = aliveCells.map((c) => ({ ijk: [c.ix, c.iy, c.iz], u: vsub(c.xCommit, c.x0) }));
  const damagedEdges = [...grid.interfaces]
    .filter((e) => e.alive && grid.cellById.get(e.cellAId)?.alive && grid.cellById.get(e.cellBId)?.alive && Number(e.committed.damage) > 0)
    .sort((a, b) => a.id - b.id)
    .map((e) => [e.id, e.committed.damage, e.committed.kappa]);
  const aliveEdgeCount = grid.interfaces.filter((e) => e.alive && grid.cellById.get(e.cellAId)?.alive && grid.cellById.get(e.cellBId)?.alive).length;

  const effectiveSettings = engine._cohesiveSettings({ ...overrides });
  const settingsOut = {};
  for (const k of [
    'maxSteps', 'iterations', 'activeSetIterations', 'residualTolerance', 'incrementTolerance',
    'linearMaxIterations', 'linearTolerance', 'linearRelativeTolerance', 'lineSearchSteps', 'lineSearchMinAlpha',
    'maxTranslationIncrement', 'maxRotationIncrement', 'quasiStaticRegularization', 'gravity',
    'detachedDebrisPolicy', 'deleteDetachedFragments', 'contactPairScope', 'contactEnabled', 'contactFriction',
    'useAnalyticTangent', 'useConsistentTangent', 'linearPreconditioner', 'modifiedNewtonEnabled',
  ]) settingsOut[k] = effectiveSettings[k];
  settingsOut.maxBreaksPerStep = effectiveSettings.maxBreaksPerStep ?? P.CohesiveDynamicsDefaults.maxBreaksPerStep;
  settingsOut.impulseScale = effectiveSettings.impulseScale ?? P.CohesiveDynamicsDefaults.impulseScale;

  return {
    engine,
    block: {
      protocol: scene.oracle.protocol,
      protocolTicks: scene.oracle.protocol === 'stepLoop' ? scene.oracle.ticks : null,
      settings: settingsOut,
      settingsPatch: scene.oracle.settings,
      blast: scene.oracle.blast ? { ...scene.oracle.blast, info: blastInfo } : null,
      converged,
      closureCallConverged: closureResult?.converged ?? null,
      passes: converged ? closurePass : passes.length,
      totalPassesRun: passes.length,
      trailingQuietPasses,
      // Newton convergence of every anchored component in every pass up to
      // closure (the plan gates only on scenes where this holds).
      allPassesNewtonConverged: keep.every((p) => p.anchoredConverged === true),
      passLog: keep,
      nonConvergedAnchoredComponentSolves: rec.components.filter((c) => c.anchored && !c.converged && c.pass <= (converged ? closurePass : Infinity)).length,
      initialCellCount,
      aliveCellIds: aliveCells.map((c) => c.id),
      aliveEdgeCount,
      removedCount: initialCellCount - aliveCells.length,
      blastRemovedCellIds: kills.blastKilledCellIds,
      blastFracturedEdgeIds: kills.blastFracturedEdgeIds,
      rupturedEdgeIds: kills.rupturedEdgeIds,
      deletedCellIds: kills.deletedCellIds,
      damagedEdges,
      finalU: view.map((v) => v.u),
      probes: probeValues(scene, view),
    },
    wallMs,
  };
}
