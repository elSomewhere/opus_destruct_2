// Scene catalog for the fixture exporter.
//
// Three families:
//   registry  - the 15 scenes of the prototype's ScenarioRegistry
//               (src/scenarios/index.js), exported exactly as authored.
//   ootest    - the 4 scenes of tests/xpbd_fem_outcome_oracle.mjs. Their
//               build functions are copied verbatim from that test (it runs
//               its checks on import, so it cannot be imported) and
//               registered into the in-memory ScenarioRegistry under the
//               test's own keys (__ooBlock/__ooFloat/__ooTower).
//   analytic  - small rc_floor beams/plates with closed-form references,
//               registered in memory under __fx_* keys.
//
// Registering extra keys mutates only the imported module OBJECT in this
// process; the prototype sources are untouched.

export const REGISTRY_SCENES = [
  'asymDiaphragm', 'braceCorner', 'building3', 'cantilever', 'cascadingCollapse',
  'clampedCantilever', 'contactGuidedStack', 'contactSmokeStack', 'continuationStiffStrip',
  'multiSpan', 'plasticBeam', 'seamCompare', 'simplySupported', 'supportCompare', 'wallLintel',
];

const ALL = () => true;
const ix = (...xs) => (c) => xs.includes(c.ijk[0]);

// Probe definitions: name -> cell predicate on {ijk}; value = min dz over the
// selected cells (most negative vertical displacement, m).
const REGISTRY_PROBES = {
  asymDiaphragm: {},
  braceCorner: { topSlabMinDz: (c) => c.ijk[2] === 5 },
  building3: { roofMinDz: (c) => c.ijk[2] === 11 },
  cantilever: { tipMinDz: ix(7) },
  cascadingCollapse: { roofMinDz: (c) => c.ijk[2] === 5 },
  clampedCantilever: { tipMinDz: ix(7) },
  contactGuidedStack: { topCellDz: (c) => c.ijk[2] === 1 },
  contactSmokeStack: { topCellDz: (c) => c.ijk[2] === 1 },
  continuationStiffStrip: { midspanMinDz: ix(6) },
  multiSpan: { span1MidMinDz: ix(3), span2MidMinDz: ix(9), span3MidMinDz: ix(14, 15) },
  plasticBeam: { tipMinDz: ix(9) },
  seamCompare: {},
  simplySupported: { midspanMinDz: ix(5, 6) },
  // supportCompare holds its three support sub-variants side by side (one
  // 10x3 strip per band): pinned iy 0..2, semifixed iy 4..6, fixed iy 8..10.
  supportCompare: {
    pinnedMidspanMinDz: (c) => (c.ijk[0] === 4 || c.ijk[0] === 5) && c.ijk[1] <= 2,
    semifixedMidspanMinDz: (c) => (c.ijk[0] === 4 || c.ijk[0] === 5) && c.ijk[1] >= 4 && c.ijk[1] <= 6,
    fixedMidspanMinDz: (c) => (c.ijk[0] === 4 || c.ijk[0] === 5) && c.ijk[1] >= 8,
  },
  wallLintel: { lintelMidDz: (c) => c.ijk[0] === 4 && c.ijk[2] === 3 },
};

const REGISTRY_FEEL_BLASTS = {
  // App default radius (2.0) centred on a ground-storey front-wall column
  // cell, clicked after the structure has settled (app protocol).
  building3: [{ id: 'appDefault_frontColumn_3_0_2', cell: [3, 0, 2], radius: 'appDefault' }],
};

// The browser app itself only ships streamed scenes (src/scenarios/streaming.js
// via scenarios/unified.js). Two of them share a name with a registry scene;
// for those, an extra capture runs the app's exact load path
// (initStreamingScenario). Their generators resolve archetypes with a
// different precedence at column/wall/floor intersections, so the geometry is
// close to, but not identical with, the registry scene of the same name.
const APP_STREAMED_FEEL = {
  building3: { scene: 'building3', blasts: [{ id: 'appDefault_frontWall_3_0_2', cell: [3, 0, 2], radius: 'appDefault' }] },
  cascadingCollapse: { scene: 'cascadingCollapse', blasts: [] },
};

const APPLIED_P = 1e4; // N, analytic point loads

export function registerInMemoryScenes(P) {
  const { ScenarioRegistry, SupportProfileId, authorOrthogonalFaceEdges } = P;
  // --- tests/xpbd_fem_outcome_oracle.mjs (verbatim build functions) ---------
  ScenarioRegistry.__ooBlock = {
    label: '[oracle] supported block',
    build(grid) {
      for (let z = 0; z < 4; z++) for (let y = 0; y < 3; y++) for (let x = 0; x < 3; x++) grid.addCell(x, y, z, 'rc_floor');
      grid.setGroundSupport(SupportProfileId.FIXED, 0);
      authorOrthogonalFaceEdges(grid);
    },
  };
  ScenarioRegistry.__ooFloat = {
    label: '[oracle] unanchored floating cluster',
    build(grid) {
      for (let z = 0; z < 2; z++) for (let y = 0; y < 2; y++) for (let x = 0; x < 2; x++) grid.addCell(x, y, z + 3, 'rc_floor');
      authorOrthogonalFaceEdges(grid);
    },
  };
  ScenarioRegistry.__ooTower = {
    label: '[oracle] tall tower',
    build(grid) {
      for (let z = 0; z < 10; z++) for (let y = 0; y < 2; y++) for (let x = 0; x < 2; x++) grid.addCell(x, y, z, 'rc_floor');
      grid.setGroundSupport(SupportProfileId.FIXED, 0);
      authorOrthogonalFaceEdges(grid);
    },
  };

  // --- analytic scenes (g = 0, point loads) ---------------------------------
  const pointLoad = (engine, ijk, force) => {
    const cell = engine.grid.getCell(ijk[0], ijk[1], ijk[2]);
    if (!cell) throw new Error(`analytic load cell ${ijk} missing`);
    cell.appliedForce = new Float64Array(force);
  };
  ScenarioRegistry.__fx_stripCantileverX = {
    label: '[fixture] rc_floor cantilever strip along X (8 cells, FIXED root, tip load)',
    build(grid) {
      for (let x = 0; x < 8; x++) grid.addCell(x, 0, 0, 'rc_floor');
      grid.addSupportByCoords([[0, 0, 0]], SupportProfileId.FIXED, 'strip-root');
      authorOrthogonalFaceEdges(grid);
    },
    postCompile(engine) { pointLoad(engine, [7, 0, 0], [0, 0, -APPLIED_P]); },
  };
  ScenarioRegistry.__fx_stripCantileverY = {
    label: '[fixture] rc_floor cantilever strip along Y (8 cells, FIXED root, tip load)',
    build(grid) {
      for (let y = 0; y < 8; y++) grid.addCell(0, y, 0, 'rc_floor');
      grid.addSupportByCoords([[0, 0, 0]], SupportProfileId.FIXED, 'strip-root');
      authorOrthogonalFaceEdges(grid);
    },
    postCompile(engine) { pointLoad(engine, [0, 7, 0], [0, 0, -APPLIED_P]); },
  };
  ScenarioRegistry.__fx_beamSimplySupportedX = {
    label: '[fixture] rc_floor simply supported beam along X (13 cells, PINNED ends, midspan load)',
    build(grid) {
      for (let x = 0; x < 13; x++) grid.addCell(x, 0, 0, 'rc_floor');
      grid.addSupportByCoords([[0, 0, 0], [12, 0, 0]], SupportProfileId.PINNED, 'beam-ends');
      authorOrthogonalFaceEdges(grid);
    },
    postCompile(engine) { pointLoad(engine, [6, 0, 0], [0, 0, -APPLIED_P]); },
  };
  ScenarioRegistry.__fx_plateSimplySupported = {
    label: '[fixture] rc_floor 9x9 plate, PINNED boundary ring, centre point load',
    build(grid) {
      const coords = [];
      for (let x = 0; x < 9; x++) {
        for (let y = 0; y < 9; y++) {
          grid.addCell(x, y, 0, 'rc_floor');
          if (x === 0 || y === 0 || x === 8 || y === 8) coords.push([x, y, 0]);
        }
      }
      grid.addSupportByCoords(coords, SupportProfileId.PINNED, 'plate-boundary');
      authorOrthogonalFaceEdges(grid);
    },
    postCompile(engine) { pointLoad(engine, [4, 4, 0], [0, 0, -APPLIED_P]); },
  };
}

// Oracle protocol for ScenarioRegistry and analytic scenes: default engine
// settings + detached policy 'delete', one solveToClosure() call.
const CLOSURE = { protocol: 'solveToClosure' };

export function sceneCatalog(P) {
  const scenes = [];
  for (const name of REGISTRY_SCENES) {
    const def = P.ScenarioRegistry[name];
    if (!def) throw new Error(`ScenarioRegistry scene '${name}' missing from prototype`);
    scenes.push({
      id: name,
      family: 'registry',
      scenario: name,
      label: def.label,
      source: 'src/scenarios/index.js (ScenarioRegistry)',
      dimensions: null, // initScenario default [20,20,20]; only bounds the authoring box
      gravity: P.GRAVITY,
      oracle: { ...CLOSURE, settings: { detachedDebrisPolicy: 'delete' }, blast: null },
      feel: { blasts: REGISTRY_FEEL_BLASTS[name] || [], appStreamed: APP_STREAMED_FEEL[name] || null },
      probes: REGISTRY_PROBES[name] || {},
      analytic: null,
    });
  }

  // tests/xpbd_fem_outcome_oracle.mjs: the FEM branch of run(core, ...)
  // exactly: applySettings({physicsCoreId, cohesiveContactEnabled:true,
  // detachedDebrisPolicy:'delete'}), optional applyBlastAtWorld(at, r,
  // {maxPasses:6}), then `ticks` x stepCohesiveDynamics({}).
  const ooSettings = { cohesiveContactEnabled: true, detachedDebrisPolicy: 'delete' };
  const oo = (id, scenario, dims, topZ, ticks, blast, feelBlasts, label) => ({
    id,
    family: 'ootest',
    scenario,
    label,
    source: 'tests/xpbd_fem_outcome_oracle.mjs',
    dimensions: dims,
    gravity: P.GRAVITY,
    oracle: { protocol: 'stepLoop', ticks, settings: ooSettings, blast },
    feel: { blasts: feelBlasts },
    probes: { topMinDz: (c) => c.ijk[2] === topZ },
    analytic: null,
  });
  scenes.push(oo('ootest_supportedBlock', '__ooBlock', [3, 3, 4], 3, 250, null, [],
    '[oracle test 1] supported 3x3x4 block settles'));
  scenes.push(oo('ootest_floatingCluster', '__ooFloat', [2, 2, 6], 4, 80, null, [],
    '[oracle test 2] unanchored 2x2x2 cluster is culled'));
  scenes.push(oo('ootest_blastedTower', '__ooTower', [2, 2, 10], 9, 300,
    { center: [0.5, 0.5, 0.5], radius: 1.5, options: { maxPasses: 6 } },
    [
      { id: 'appDefault_midHeight_0_0_4', cell: [0, 0, 4], radius: 'appDefault' },
      { id: 'oracleTest_base', center: [0.5, 0.5, 0.5], radius: 1.5 },
    ],
    '[oracle test 3] 2x2x10 tower blasted at its base collapses'));
  scenes.push(oo('ootest_gentleBlastBlock', '__ooBlock', [3, 3, 4], 3, 250,
    { center: [1.5, 1.5, 2.0], radius: 1.0, options: { maxPasses: 6 } },
    [{ id: 'oracleTest_gentle', center: [1.5, 1.5, 2.0], radius: 1.0 }],
    '[oracle test 4] gently blasted 3x3x4 block loses a chunk'));

  const an = (id, scenario, dims, analytic, probes, label) => ({
    id,
    family: 'analytic',
    scenario,
    label,
    source: 'tools/fixture-export/lib/scenes.mjs',
    dimensions: dims,
    gravity: 0,
    oracle: { ...CLOSURE, settings: { detachedDebrisPolicy: 'delete' }, blast: null },
    feel: { blasts: [] },
    probes,
    analytic,
  });
  scenes.push(an('analytic_stripCantileverX', '__fx_stripCantileverX', [8, 1, 1],
    { kind: 'cantilever_tip_load', axis: 0, cells: 8, loadCell: [7, 0, 0], P: APPLIED_P },
    { tipDz: (c) => c.ijk[0] === 7 },
    'rc_floor cantilever strip along X, 8 cells, FIXED root cell, 1e4 N tip load, g = 0'));
  scenes.push(an('analytic_stripCantileverY', '__fx_stripCantileverY', [1, 8, 1],
    { kind: 'cantilever_tip_load', axis: 1, cells: 8, loadCell: [0, 7, 0], P: APPLIED_P },
    { tipDz: (c) => c.ijk[1] === 7 },
    'rc_floor cantilever strip along Y, 8 cells, FIXED root cell, 1e4 N tip load, g = 0'));
  scenes.push(an('analytic_beamSimplySupportedX', '__fx_beamSimplySupportedX', [13, 1, 1],
    { kind: 'simply_supported_midspan_load', axis: 0, cells: 13, loadCell: [6, 0, 0], P: APPLIED_P },
    { midspanDz: (c) => c.ijk[0] === 6 },
    'rc_floor simply supported beam along X, 13 cells, PINNED end cells, 1e4 N midspan load, g = 0'));
  scenes.push(an('analytic_plateSimplySupported', '__fx_plateSimplySupported', [9, 9, 1],
    { kind: 'plate_ss_center_load', cells: 9, loadCell: [4, 4, 0], P: APPLIED_P },
    { centreDz: (c) => c.ijk[0] === 4 && c.ijk[1] === 4 },
    'rc_floor 9x9 plate, PINNED boundary ring, 1e4 N centre load, g = 0'));

  for (const s of scenes) s.probes = { minDz: ALL, ...s.probes };
  return scenes;
}
