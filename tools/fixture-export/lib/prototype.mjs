// Loads the JavaScript prototype (the ORACLE) read-only from PROTOTYPE_DIR.
//
// Nothing in the prototype repo is ever written. We only import its ES
// modules and run two read-only git queries for provenance. All
// instrumentation the exporter needs (damage freeze, section-variant patches,
// kill/step logging) is applied to in-memory OBJECTS created by those modules,
// never to their source.

import path from 'node:path';
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const LIB_DIR = path.dirname(fileURLToPath(import.meta.url));
export const TOOL_DIR = path.resolve(LIB_DIR, '..');

// Default: ../../../voxel_threed_discrete relative to tools/fixture-export/.
export function resolvePrototypeDir() {
  const raw = process.env.PROTOTYPE_DIR;
  const dir = raw ? path.resolve(process.cwd(), raw) : path.resolve(TOOL_DIR, '../../../voxel_threed_discrete');
  if (!fs.existsSync(path.join(dir, 'src', 'core.js'))) {
    throw new Error(`PROTOTYPE_DIR does not look like the prototype repo (missing src/core.js): ${dir}`);
  }
  return dir;
}

function gitRead(dir, args) {
  try {
    // --no-optional-locks: never refresh/write the prototype's index.
    return execFileSync('git', ['--no-optional-locks', '-C', dir, ...args], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim();
  } catch {
    return null;
  }
}

export function prototypeProvenance(dir) {
  const commit = gitRead(dir, ['rev-parse', 'HEAD']);
  const branch = gitRead(dir, ['rev-parse', '--abbrev-ref', 'HEAD']);
  const status = gitRead(dir, ['status', '--porcelain', '--untracked-files=no']);
  return {
    gitCommit: commit,
    gitBranch: branch,
    // Tracked-file modifications only; untracked scratch files are ignored.
    gitDirty: status == null ? null : status.length > 0,
  };
}

export async function loadPrototype(dir = resolvePrototypeDir()) {
  const mod = (rel) => import(pathToFileURL(path.join(dir, 'src', rel)).href);
  const [
    core, constants, scenarios, archetypes, dynamics, cohesive, element,
    constitutive, so3, reach, linear,
  ] = await Promise.all([
    mod('core.js'),
    mod('constants.js'),
    mod('scenarios/index.js'),
    mod('archetypes.js'),
    mod('dynamics/index.js'),
    mod('dynamics/cohesive_voxel_solver.js'),
    mod('element/interface.js'),
    mod('constitutive.js'),
    mod('math/so3.js'),
    mod('structural/reachability_policy.js'),
    mod('math/linear.js'),
  ]);
  const helpers = await mod('scenarios/helpers.js');
  const unified = await mod('scenarios/unified.js');
  return {
    dir,
    DestructionEngine: core.DestructionEngine,
    APP_VERSION: constants.APP_VERSION,
    GRAVITY: constants.GRAVITY,
    PhysicsCoreId: constants.PhysicsCoreId,
    SupportProfileId: constants.SupportProfileId,
    InterfaceProfileId: constants.InterfaceProfileId,
    ScenarioRegistry: scenarios.ScenarioRegistry,
    authorOrthogonalFaceEdges: helpers.authorOrthogonalFaceEdges,
    resolveUnifiedScenarioDefinition: unified.resolveUnifiedScenarioDefinition,
    buildInterfaceArchetypeFromCells: archetypes.buildInterfaceArchetypeFromCells,
    createDefaultLibrary: archetypes.createDefaultLibrary,
    CohesiveDynamicsDefaults: dynamics.CohesiveDynamicsDefaults,
    XpbdDefaults: dynamics.XpbdDefaults,
    getCellMass: cohesive.getCellMass,
    getCellInertiaLocal: cohesive.getCellInertiaLocal,
    CorotationalInterfaceElement: element.CorotationalInterfaceElement,
    equivalentDemand: constitutive.equivalentDemand,
    quatToRotvec: so3.quatToRotvec,
    quatToMat3: so3.quatToMat3,
    quatMul: so3.quatMul,
    quatConjugate: so3.quatConjugate,
    supportProvidesAnchorage: reach.supportProvidesAnchorage,
    ReachabilityPolicy: reach.ReachabilityPolicy,
    mat6Diag: linear.mat6Diag,
  };
}

// A fresh engine on the non-streaming WorldGrid path (the path every oracle
// test and ScenarioRegistry scene uses). Logging is silenced.
export function makeEngine(P, physicsCoreId) {
  const engine = new P.DestructionEngine({ enableStreaming: false, physicsCoreId });
  engine.setLogger(() => {});
  return engine;
}
