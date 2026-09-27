#!/usr/bin/env node
// Golden-fixture exporter: runs the JavaScript prototype (the ORACLE,
// read-only) and writes deterministic JSON fixtures (schema svx-fixture-v1).
//
//   node tools/fixture-export/export.mjs [--out DIR] [--only id,id] [--variants v,v] [--no-feel]
//
// PROTOTYPE_DIR (env) points at the prototype repo; default
// ../../../voxel_threed_discrete relative to this script. See README.md.

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { resolvePrototypeDir, loadPrototype, prototypeProvenance, TOOL_DIR } from './lib/prototype.mjs';
import { stableStringify } from './lib/json.mjs';
import { registerInMemoryScenes, sceneCatalog } from './lib/scenes.mjs';
import { extractModel, exportInterfaceArchetypes } from './lib/model.mjs';
import { VARIANTS, variantDescription, stVenantBeta } from './lib/variants.mjs';
import { runStatic, runOutcome, initScene, STATIC_SOLVER_SETTINGS } from './lib/oracle.mjs';
import {
  runFeel, APP_UI_SETTINGS, FEEL_MAX_TICKS, FEEL_MAX_SAMPLES, ASLEEP_STOP_STREAK, APP_PARK_QUIET_FRAMES,
  appStreamedInitOptions,
} from './lib/feel.mjs';
import { analyticBlock } from './lib/analytic.mjs';

export const SCHEMA = 'svx-fixture-v1';
const SCENE_BUDGET_MS = 5 * 60 * 1000;

function parseArgs(argv) {
  const args = {
    out: path.resolve(TOOL_DIR, '../../tests/fixtures/prototype'),
    only: null,
    variants: [...VARIANTS],
    feel: true,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--out') args.out = path.resolve(process.cwd(), argv[++i]);
    else if (a === '--only') args.only = new Set(argv[++i].split(',').map((s) => s.trim()).filter(Boolean));
    else if (a === '--variants') args.variants = argv[++i].split(',').map((s) => s.trim()).filter(Boolean);
    else if (a === '--no-feel') args.feel = false;
    else if (a === '-h' || a === '--help') {
      console.log('usage: node export.mjs [--out DIR] [--only id,id] [--variants prototype,fixY,fixJ,fixYJ] [--no-feel]');
      process.exit(0);
    } else throw new Error(`unknown argument ${a}`);
  }
  for (const v of args.variants) if (!VARIANTS.includes(v)) throw new Error(`unknown variant ${v}`);
  // 'prototype' is always exported; canonical order keeps output stable.
  args.variants = VARIANTS.filter((v) => v === 'prototype' || args.variants.includes(v));
  return args;
}

export const CONVENTIONS = {
  units: 'SI: m, kg, s, N, N*m, rad. Lattice pitch = model.cellSize (the prototype runs at cellSize = 1 m).',
  axes: 'World z is up; gravity acts along -z (scene.gravity is the signed z acceleration, -9.81 or 0).',
  ordering: 'model.cells / model.edges / model.supports are sorted by id; every per-cell / per-edge / per-support array in a result block is aligned with that order. Ids are the prototype ids.',
  edgeOrientation: 'Edge a->b points along +axis: ijk(b) = ijk(a) + e_axis. Anchors at +cellSize/2*e_axis (A, in A local frame) and -cellSize/2*e_axis (B).',
  edgeFrame: 'Local frame (n, t1, t2) = columns of R(q), q = model.edgeFrames[axis].q, same for both attachments. X edges (x,y,z); Y edges (y,z,x); Z edges (z,x,y).',
  componentOrder: [
    '0 N  axial / normal along n (positive = opening)',
    '1 V1 shear along t1',
    '2 V2 shear along t2',
    '3 M1 bending about t1',
    '4 M2 bending about t2',
    '5 T  torsion about n',
  ],
  k0: 'Interface archetype k0[6] (units N/m x3, N*m/rad x3) in componentOrder; k0Matrix = diag(k0).',
  thresholds: 'onset/break: generalized-deformation thresholds {openN (tension), compN (compression magnitude), t1, t2, b1, b2, tau} (m or rad). Law (damageModel continuum_regularized_v1, damageNorm quadratic): q_i = clamp((|delta_i| - onset_i)/(break_i - onset_i), 0, 1) (component 0 uses openN or compN by the sign of delta_0), p = min(1, ||q||_2), d = max(d_committed, p), kappa = max(kappa_committed, 1 + p). Secant scale s(d) = residualStiffness + (1 - residualStiffness)(1 - d).',
  rupture: 'An edge is a rupture candidate when d >= 0.999 or margin = kappa - kappaBreak >= 0 (kappaBreak = 2). Candidates are killed in order score = max(d, margin, phi) desc, then d desc, then id asc, at most maxBreaksPerStep (512) per pass; then detached (unanchored) components are deleted.',
  capacities: 'forceCap6 = [tension, shear1, shear2, bend1, bend2, torsion] capacities (N or N*m); compressionCap (N). onset_i = cap_i / k0_i * profile.kappaOnset.',
  force6: 'Committed generalized force r = k0 * s(d) * (delta - plastic) (plastic is identically 0: plasticFraction = 0 in every profile) in the edge\'s current co-rotated frame (slerp-average of the two attachment frames), componentOrder.',
  phi: 'Equivalent demand phi = sqrt(sum_i (delta_i/onset_i)^2) with the tension/compression split on component 0 (constitutive.js equivalentDemand). phi >= 1 does NOT by itself imply damage: onset is per-component (box).',
  cellKinematics: 'u = x - x0 (world, m); theta = world rotation vector with q = exp(theta) * q0 (q0 = identity). Quaternions are [x, y, z, w].',
  reaction6: 'Total force (world, N) and moment about the cell centre (world, N*m) exerted by the support on its cell = sum of edge internal forces J^T r on the cell minus the cell\'s external load.',
  massInertia: 'mass = density * gravityMassVolume; inertia = principal local [m(sy^2+sz^2)/12, m(sx^2+sz^2)/12, m(sx^2+sy^2)/12] from effDims (oriented_interface_core_base.js getCellMass / getCellInertiaLocal).',
  nonFinite: 'JSON has no Infinity/NaN: such numbers are written as the strings "Infinity", "-Infinity", "NaN".',
  numbers: 'Shortest round-trip decimal of each IEEE-754 double (full precision).',
};

const VARIANT_INFO = Object.fromEntries(VARIANTS.map((v) => [v, variantDescription(v)]));

function sha256(text) {
  return crypto.createHash('sha256').update(text).digest('hex');
}

function staticSummary(s) {
  return {
    status: s.status,
    converged: s.converged,
    residualNormalized: s.residualNormalized,
    maxPhi: s.maxPhi,
    elasticRegime: s.elasticRegime,
    probes: s.probes,
  };
}

function outcomeSummary(o) {
  return {
    converged: o.converged,
    allPassesNewtonConverged: o.allPassesNewtonConverged,
    passes: o.passes,
    removedCount: o.removedCount,
    ruptured: o.rupturedEdgeIds.length,
    alive: o.aliveCellIds.length,
  };
}

function feelSummary(f) {
  if (!f) return null;
  const out = {};
  if (f.appStreamed) out.appStreamed = feelSummary({ runs: f.appStreamed.runs });
  for (const [policy, runs] of Object.entries(f.runs)) {
    const s = runs.settle.summary;
    out[policy] = {
      settle: { ticks: s.ticks, stopReason: s.stopReason, allAsleepTick: s.allAsleepTick, peakSag: s.peakSag, finalAlive: s.finalAlive, totalRuptures: s.totalRuptures },
    };
    for (const [id, b] of Object.entries(runs.blasts || {})) {
      out[policy][id] = { ticks: b.summary.ticks, stopReason: b.summary.stopReason, finalAlive: b.summary.finalAlive, totalRuptures: b.summary.totalRuptures, collapsed: b.summary.collapsed };
    }
  }
  return out;
}

async function exportScene(P, scene, args, provenance) {
  const t0 = performance.now();
  const timings = {};
  const budgetLeft = () => SCENE_BUDGET_MS - (performance.now() - t0);
  const skipped = [];

  // --- model (variant independent) -------------------------------------------
  const modelEngine = initScene(P, scene, P.PhysicsCoreId.COHESIVE_DYNAMICS);
  const model = extractModel(P, modelEngine);

  // --- oracle static + outcome per variant -----------------------------------
  const variants = {};
  for (const variant of args.variants) {
    if (budgetLeft() <= 0) { skipped.push({ stage: `variant:${variant}`, reason: 'scene time budget exceeded' }); continue; }
    const st = runStatic(P, scene, variant);
    const archetypes = exportInterfaceArchetypes(P, st.engine);
    const out = runOutcome(P, scene, variant);
    timings[`static:${variant}`] = st.wallMs;
    timings[`outcome:${variant}`] = out.wallMs;
    variants[variant] = {
      description: VARIANT_INFO[variant],
      patch: st.patch,
      interfaceArchetypes: archetypes,
      static: st.block,
      outcome: out.block,
      analytic: analyticBlock(scene, model, archetypes, st.block),
    };
  }

  // --- XPBD feel captures (prototype sections only) --------------------------
  let feel = null;
  if (args.feel) {
    feel = {
      core: 'xpbd_oriented (XpbdRuntime, engine.tick on the CPU f64 path)',
      settings: { ...APP_UI_SETTINGS },
      settingsNote: 'src/app.js syncSettingsFromUi() with index.html defaults; detachedDebrisPolicy is overridden per run (UI default is keep)',
      xpbd: {
        frameDt: P.XpbdDefaults.frameDt,
        substeps: P.XpbdDefaults.substeps,
        sweeps: P.XpbdDefaults.sweeps,
        settleRampTicks: P.XpbdDefaults.settleRampTicks,
        sleepLinearVelocity: P.XpbdDefaults.sleepLinearVelocity,
        sleepAngularVelocity: P.XpbdDefaults.sleepAngularVelocity,
        sleepFrames: P.XpbdDefaults.sleepFrames,
      },
      protocol: {
        maxTicks: FEEL_MAX_TICKS,
        stopRule: `stop after ${ASLEEP_STOP_STREAK} consecutive ticks with awakeCells == 0 (virgin settle: not before the gravity ramp has finished), else maxTicks`,
        appParkRule: `appPark = first tick completing ${APP_PARK_QUIET_FRAMES} consecutive stats.quiet ticks (src/app.js SIM_QUIET_PARK_FRAMES): where the real app would stop ticking`,
        maxSamples: FEEL_MAX_SAMPLES,
        downsampling: 'each sample closes a bucket of bucketTicks consecutive ticks; maxDisp*/minDz* are bucket extrema, awake/alive/anchored/ruptures/deleted are the values at the bucket\'s last tick; ruptures/deleted are cumulative',
        appliedLoads: 'the XPBD runtime ignores cell.appliedForce/appliedMoment: feel runs are gravity-only (app gravity -9.81) even for scenes whose oracle load case has point loads or g = 0',
        blast: 'blasts are applied to the SETTLED state with {deferSolve:true, captureTransition:false} (the app\'s real-time click path) and followed by a fresh tick phase',
      },
      runs: {},
    };
    const captureRuns = (label, blasts, streamed) => {
      const out = {};
      for (const policy of ['delete', 'keep']) {
        if (budgetLeft() <= 0) { skipped.push({ stage: `${label}:${policy}`, reason: 'scene time budget exceeded' }); continue; }
        const base = runFeel(P, scene, policy, null, streamed);
        timings[`${label}:${policy}`] = base.wallMs;
        const runs = { settle: base.run.settle, blasts: {} };
        for (const blast of blasts) {
          if (budgetLeft() <= 0) { skipped.push({ stage: `${label}:${policy}:${blast.id}`, reason: 'scene time budget exceeded' }); continue; }
          const b = runFeel(P, scene, policy, blast, streamed);
          timings[`${label}:${policy}:${blast.id}`] = b.wallMs;
          // Determinism guard: every blast run re-settles from scratch.
          if (stableStringify(b.run.settle) !== stableStringify(base.run.settle)) {
            throw new Error(`${scene.id}: XPBD settle phase is not reproducible (${label}, policy ${policy})`);
          }
          runs.blasts[blast.id] = b.run.blast;
        }
        out[policy] = runs;
      }
      return out;
    };
    feel.runs = captureRuns('feel', scene.feel.blasts, null);
    const st = scene.feel.appStreamed;
    if (st) {
      const def = P.resolveUnifiedScenarioDefinition(st.scene);
      feel.appStreamed = {
        scene: st.scene,
        source: 'src/scenarios/streaming.js via scenarios/unified.js - the scene the browser app actually loads',
        note: 'Captured through the app\'s own path: new DestructionEngine({physicsCoreId: xpbd_oriented}) (streaming on), applySettings(settings with the run policy), initStreamingScenario(scene, initOptions). The streamed generator assigns archetypes with a different precedence than the ScenarioRegistry scene of the same name, so cell counts/ids and the model/oracle blocks of this file do NOT describe this geometry.',
        initOptions: appStreamedInitOptions(def),
        runs: captureRuns('feelStreamed', st.blasts, st),
      };
    }
  }

  const fixture = {
    schema: SCHEMA,
    scene: {
      id: scene.id,
      family: scene.family,
      scenario: scene.scenario,
      label: scene.label,
      source: scene.source,
      dimensions: scene.dimensions,
      gravity: scene.gravity,
      oracleProtocol: scene.oracle.protocol,
      probes: Object.keys(scene.probes),
    },
    prototype: {
      appVersion: P.APP_VERSION,
      ...provenance,
      engine: 'DestructionEngine({enableStreaming:false}) + initScenario() (WorldGrid path)',
    },
    conventions: CONVENTIONS,
    model,
    variants,
    feel,
    skipped,
  };
  return { fixture, timings, wallMs: performance.now() - t0 };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const prototypeDir = resolvePrototypeDir();
  const P = await loadPrototype(prototypeDir);
  const provenance = prototypeProvenance(prototypeDir);
  registerInMemoryScenes(P);
  let scenes = sceneCatalog(P);
  if (args.only) scenes = scenes.filter((s) => args.only.has(s.id));
  fs.mkdirSync(args.out, { recursive: true });

  console.log(`prototype: ${prototypeDir} (v${P.APP_VERSION}, ${provenance.gitCommit?.slice(0, 12) ?? 'no git'}${provenance.gitDirty ? ', DIRTY' : ''})`);
  console.log(`output:    ${args.out}`);
  const tStart = performance.now();
  const index = [];
  for (const scene of scenes) {
    const { fixture, timings, wallMs } = await exportScene(P, scene, args, provenance);
    const text = stableStringify(fixture);
    const file = `${scene.id}.json`;
    fs.writeFileSync(path.join(args.out, file), text);
    const vsum = {};
    for (const [v, blk] of Object.entries(fixture.variants)) {
      vsum[v] = { static: staticSummary(blk.static), outcome: outcomeSummary(blk.outcome) };
    }
    index.push({
      id: scene.id,
      family: scene.family,
      file,
      bytes: Buffer.byteLength(text),
      sha256: sha256(text),
      cells: fixture.model.cells.length,
      edges: fixture.model.edges.length,
      supports: fixture.model.supports.length,
      variants: vsum,
      feel: feelSummary(fixture.feel),
      skipped: fixture.skipped,
    });
    const p = fixture.variants.prototype;
    const worst = Object.entries(timings).sort((a, b) => b[1] - a[1])[0];
    console.log(
      `${scene.id.padEnd(32)} cells ${String(fixture.model.cells.length).padStart(4)} edges ${String(fixture.model.edges.length).padStart(5)}`
      + ` | static ${p.static.status} phi ${p.static.maxPhi.toFixed(3)}`
      + ` | outcome ${p.outcome.converged ? 'conv' : 'NOCONV'} passes ${p.outcome.passes} alive ${p.outcome.aliveCellIds.length}/${p.outcome.initialCellCount}`
      + ` | ${(Buffer.byteLength(text) / 1024).toFixed(0)} KiB ${(wallMs / 1000).toFixed(2)} s (max ${worst?.[0]} ${(worst?.[1] / 1000).toFixed(2)} s)`,
    );
  }
  const indexDoc = {
    schema: SCHEMA,
    kind: 'index',
    prototype: { appVersion: P.APP_VERSION, ...provenance },
    generator: 'structvox tools/fixture-export/export.mjs',
    variants: VARIANT_INFO,
    stVenantBetaExamples: {
      'rc_floor X/Y section 1 x 0.25': stVenantBeta(1, 0.25),
      'square section': stVenantBeta(1, 1),
    },
    staticSolverSettings: STATIC_SOLVER_SETTINGS,
    conventions: CONVENTIONS,
    scenes: index,
  };
  if (!args.only) {
    fs.writeFileSync(path.join(args.out, 'index.json'), stableStringify(indexDoc));
  } else {
    console.log('(--only given: index.json not rewritten)');
  }
  console.log(`done: ${scenes.length} scenes in ${((performance.now() - tStart) / 1000).toFixed(1)} s`);
}

main().catch((err) => {
  console.error(err?.stack || String(err));
  process.exitCode = 1;
});
