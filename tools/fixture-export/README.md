# Prototype fixture exporter (Phase 0 golden fixtures)

`export.mjs` runs the JavaScript prototype (`voxel_threed_discrete`, the
**oracle**) and writes deterministic JSON fixtures to
`tests/fixtures/prototype/` (schema `svx-fixture-v1`). `check.mjs` validates
them. Plan references: `docs/PLAN.md` §A1, §B1, §B3, §C Phase 0, §E.

The prototype is used **read-only**: its ES modules are imported, and two
read-only git queries (`rev-parse`, `--no-optional-locks status`) record
provenance. All instrumentation (damage freeze, section-variant patches, pass
and kill logging, extra scenes) is applied to in-memory objects created by
those modules, never to its source files.

## Regenerate

```sh
node tools/fixture-export/export.mjs            # all scenes -> tests/fixtures/prototype/
node tools/fixture-export/check.mjs             # validate (exit 1 on failure)

PROTOTYPE_DIR=/path/to/voxel_threed_discrete node tools/fixture-export/export.mjs
node tools/fixture-export/export.mjs --only building3,cantilever   # subset (index.json untouched)
node tools/fixture-export/export.mjs --variants prototype,fixY      # subset of variants
node tools/fixture-export/export.mjs --no-feel                     # skip XPBD captures
node tools/fixture-export/export.mjs --out /tmp/fx                 # other output dir
```

* `PROTOTYPE_DIR` defaults to `../../../voxel_threed_discrete` relative to this
  directory. Node >= 20 (developed on Node 25), no npm dependencies.
* Runtime: about 45 s of CPU for all 23 scenes, most of it XPBD captures.
  Wall time is 1-2 min depending on machine load. No scene comes near the
  5 min per-scene budget; a scene that exceeded it would list the skipped
  stages in its `skipped` array.
* Output is byte-for-byte deterministic (verified by exporting twice and
  comparing). Object keys are sorted, numbers are shortest round-trip doubles
  (full precision), there are no timestamps, and `index.json` records a
  sha256 per file.
* The fixtures were generated from prototype `v28.16.0`, commit `83eb18ce`
  (branch `fable_4_distill`, clean tree). See `prototype` in each file.

## What is exported (23 scenes)

| family | scenes | oracle protocol |
|---|---|---|
| `registry` | the 15 `ScenarioRegistry` scenes (`src/scenarios/index.js`): asymDiaphragm, braceCorner, building3, cantilever, cascadingCollapse, clampedCantilever, contactGuidedStack, contactSmokeStack, continuationStiffStrip, multiSpan, plasticBeam, seamCompare, simplySupported, supportCompare, wallLintel | default engine settings, detached policy `delete`, one `solveToClosure()` (96 passes max) |
| `ootest` | the 4 scenes of `tests/xpbd_fem_outcome_oracle.mjs`: `ootest_supportedBlock` (`__ooBlock`), `ootest_floatingCluster` (`__ooFloat`), `ootest_blastedTower` (`__ooTower` + blast at [0.5,0.5,0.5] r=1.5), `ootest_gentleBlastBlock` (`__ooBlock` + blast at [1.5,1.5,2] r=1) | the test's FEM branch exactly: `applySettings({cohesiveContactEnabled:true, detachedDebrisPolicy:'delete'})`, `applyBlastAtWorld(at, r, {maxPasses:6})`, then 250/80/300/250 x `stepCohesiveDynamics({})` |
| `analytic` | `analytic_stripCantileverX` / `...Y` (8 rc_floor cells, FIXED root, 1e4 N tip load, g = 0), `analytic_beamSimplySupportedX` (13 cells, PINNED end cells, 1e4 N midspan load, g = 0), `analytic_plateSimplySupported` (9x9 rc_floor, PINNED boundary ring, 1e4 N centre load, g = 0) | as registry, with g = 0 |

`supportCompare` holds its three support sub-variants side by side in one
scene (pinned band iy 0..2, semifixed iy 4..6, fixed iy 8..10); each band has
its own probe (`pinnedMidspanMinDz`, `semifixedMidspanMinDz`,
`fixedMidspanMinDz`).

The `ootest` build functions are copied verbatim from the test (it runs its
checks on import, so it cannot be imported) and registered in memory under
the test's own keys. The analytic scenes are registered as `__fx_*`.

## File layout (`svx-fixture-v1`)

One `<sceneId>.json` per scene plus `index.json` (per-scene summary, sha256,
sizes, the shared conventions and variant descriptions).

```
{
  schema: "svx-fixture-v1",
  scene:      {id, family, scenario, label, source, dimensions, gravity, oracleProtocol, probes[]},
  prototype:  {appVersion, gitCommit, gitBranch, gitDirty, engine},
  conventions:{...}                       // same text as below
  model: {                                // variant independent
    cellSize,                             // 1 (m): the prototype's lattice pitch
    cells:    [{id, ijk, x0, archetype, material, mass, inertia[3], support|null, appliedForce?, appliedMoment?}],
    edges:    [{id, a, b, axis, profile, archetype}],   // archetype = key into variants[v].interfaceArchetypes
    supports: [{id, cell, profile, label, springOverride|null, springScale}],
    edgeFrames: {"<axis>": {q, n, t1, t2, anchorA, anchorB}},
    tables: {materials, cellArchetypes (effDims, volumes, sectionsByAxis), interfaceProfiles, supportProfiles}
  },
  variants: {
    prototype|fixY|fixJ|fixYJ: {
      description, patch{flags, changedArchetypes, k0Ratios},
      interfaceArchetypes: {<id>: {k0[6], onset{}, break{}, forceCap6[6], compressionCap,
                           residualStiffness, kappaOnset, kappaBreak, damageModel, damageNorm,
                           plasticFraction, plasticHardening, plasticYield[6], plasticBreak,
                           profileId, directionWeights, label,
                           calibration{sectionA, sectionB, interfaceArea, fractureEnergyModeI/II, materialA/BId, rules}}},
      static:   {...}, outcome: {...}, analytic: {...}|null
    }
  },
  feel: {core, settings, xpbd{}, protocol{}, runs{delete|keep: {settle, blasts{}}}, appStreamed?},
  skipped: []
}
```

Tables contain only what the scene uses. Each edge's compiled values (k0,
capacities, onset/break thresholds, ...) are stored once per archetype in
`variants[v].interfaceArchetypes[edge.archetype]` rather than repeated for
every edge. Within a scene every edge of an archetype is identical, and the
ids are the prototype's compile signatures, the same in every variant. The
exporter asserts that the exported fields are everything the law reads: `k0Matrix == diag(k0)`,
`damageThresholds == {thresholds, breakThresholds}`, `yieldDelta6/breakDelta6`
consistent, `damageModel == continuum_regularized_v1`.

### Conventions

* SI units (m, kg, s, N, N·m, rad); z is up; `scene.gravity` is the signed z
  acceleration (-9.81, or 0 for analytic scenes).
* Arrays in result blocks are aligned with `model.cells` / `model.edges` /
  `model.supports`, which are sorted by prototype id.
* Edge `a -> b` points along `+axis` (`ijk(b) = ijk(a) + e_axis`). Anchors are at
  `±cellSize/2·e_axis`. Local frame `(n, t1, t2)` = columns of `R(edgeFrames[axis].q)`:
  X edges (x,y,z), Y edges (y,z,x), Z edges (z,x,y).
* Component order for k0, deformation, forces, thresholds:
  `[N (along n), V1 (along t1), V2 (along t2), M1 (about t1), M2 (about t2), T (about n)]`.
  Threshold objects are `{openN, compN, t1, t2, b1, b2, tau}`. `forceCap6 = [tension, shear1, shear2, bend1, bend2, torsion]`.
* Law (`continuum_regularized_v1`, quadratic norm): `q_i = clamp((|δ_i|-onset_i)/(break_i-onset_i),0,1)`
  (component 0 uses openN or compN by the sign of δ_0), `p = min(1, ||q||_2)`, `d = max(d_c, p)`,
  `κ = max(κ_c, 1+p)`, secant `s(d) = r + (1-r)(1-d)`. An edge is a rupture candidate when
  `d >= 0.999` or `κ - κ_break >= 0` (κ_break = 2). Candidates are killed in the order
  `max(d, margin, φ)` desc, d desc, id asc, at most 512 per pass. Plasticity is off in every
  profile (`plasticFraction = 0`).
* `φ = sqrt(Σ (δ_i/onset_i)²)` with the tension/compression split
  (`constitutive.js equivalentDemand`). Onset is per component (box), so φ >= 1 does not
  by itself mean damage.
* Cell kinematics: `u = x - x0`, `θ` = world rotation vector (`q = exp(θ)·q0`, q0 = identity);
  quaternions are `[x,y,z,w]`.
* `force6` = committed generalized force `k0·s(d)·(δ - plastic)` in the edge's current
  co-rotated frame (the slerp average of the two attachment frames).
* `reaction6` = total force (world) and moment about the cell centre (world) that the support
  exerts on its cell, i.e. `Σ_edges (J^T r)_cell - f_ext,cell`.
* Mass = density · gravityMassVolume. Inertia = principal local
  `m·(s_y²+s_z², s_x²+s_z², s_x²+s_y²)/12` from effDims.
* Non-finite numbers are written as the strings `"Infinity"`, `"-Infinity"`, `"NaN"`.

## Oracle blocks

The oracle is `CohesiveVoxelDynamicsSolver` (the implicit f64 `cohesive_dynamics` core) on
the non-streaming `WorldGrid` path: `new DestructionEngine({enableStreaming:false})` followed
by `initScenario()`.

### `static`: damage-free equilibrium

One quasi-static step (`stepCohesiveDynamics`) with:

* `edge.damageFrozen = 0` on every edge. This is the prototype's own operator-split hook,
  honoured by `CorotationalInterfaceElement.evaluate` and by the analytic tangent. The
  secant scale is therefore 1 and κ/d stay 0.
* `maxBreaksPerStep: 0` and no detached deletion.
* Tight solver settings (`STATIC_SOLVER_SETTINGS`): `residualTolerance 1e-12` (the solver's
  floor; the measure is `max_i |r_i|/K_ii`), 500 Newton iterations × 4 active-set passes,
  PCG to 1e-12. The Newton step clamps are widened to 2 m / 1 rad. They limit only the step
  length, not the equilibrium, and they let the large-deflection plasticBeam solve converge.
* The scene's load case: gravity plus the scene's `appliedForce`s.

Contents:

* `cells.u`, `cells.theta`: null for cells outside anchored components.
* `edges.force6`, `edges.phi`: null for edges that are not between anchored cells.
* `supports.reaction6`.
* `equilibrium`: Σ loads, Σ reactions, and the worst free-cell residual force/moment in N.
* `maxPhi`, `edgesBeyondOnset` / `elasticRegime`: whether any component exceeds its box
  onset, i.e. whether the damage-free answer is inside the elastic regime.
* Per-component Newton reports, and `probes`: named min-Δz values, e.g. `tipMinDz`.

`status` is one of:

* `converged`: every anchored component converged (and the contact set is stable when
  contact is on).
* `not_converged`
* `no_anchored_cells`

Unanchored components get only one Newton iteration in the oracle and are reported as null.

The static answer is tighter than the prototype's default closure solve (residual tolerance
1e-7). clampedCantilever tip Δz is -0.04808670 here, against -0.0480836 from `outcome`
(the plan's golden -0.048084). building3 Δz is -1.50536e-4, against the plan's golden
-1.5054e-4.

### `outcome`: run to closure with default oracle settings

The engine is instrumented with instance-level wrappers only:

* `cohesiveDynamics.step`: per-pass statistics. A `convergenceLog` is injected into the
  step's settings copy to get per-component Newton results.
* `grid.killEdge` / `grid.killCell`: rupture and deletion order. Edges killed as incident
  edges of a deleted cell are not counted as ruptures.
* `engine._applyBlastMutation`: separates blast carve and fracture from later passes.

Fields:

* `converged`: some pass is quiet and every later pass is quiet.
* `passes`: the index of that pass.
* `trailingQuietPasses`: extra quiet steps of the `stepLoop` protocol.
* `allPassesNewtonConverged`: every anchored component's Newton solve converged in every
  pass up to closure. PLAN §C gates only on scenes where this holds.
* `passLog[]`: `quiet`, `anchoredConverged`, `innerConverged` (step level; this is also
  false whenever an unanchored debris component exists), Newton/PCG iterations, residual,
  `ruptured`, `detachedDeleted`, maxDemand, maxDamage.
* `rupturedEdgeIds` (in kill order), `deletedCellIds` (detached, in order),
  `blastRemovedCellIds`, `blastFracturedEdgeIds`, `blast.info` (the prototype's
  `blastInfo`).
* `aliveCellIds`, `removedCount`, `finalU` (aligned with `aliveCellIds`),
  `damagedEdges` `[[id, d, κ]]`, `probes`, and the effective solver `settings`.

On the WorldGrid path, `applyBlastAtWorld` drops its latency-guard overrides. Its closure is
a plain default `solveToClosure()`, so `maxPasses:6` has no effect there. This is recorded
as-is.

### `analytic`: closed-form references (analytic scenes, per variant)

* `discreteRBSM` is the exact small-displacement answer of the lattice itself (rigid cells,
  face springs, unit-load method) computed from the variant's k0:
  * cantilever: `P·Σ arm_j²/k_b + (N-1)·P/k_s`
  * simply supported: `(P/4)(Σ a_j²/k_b + (N-1)/k_s)`
* `eulerBernoulli` and `timoshenko` use the physically correct section.
* `kirchhoffNavier` (plate) is a Navier-series thin-plate reference with ν = 0. It is not an
  exact lattice answer.

## Variants (`prototype`, `fixY`, `fixJ`, `fixYJ`): all feasible without touching the prototype

After `initScenario()` has compiled the grid, every interface archetype in
`engine.library.interfaceArchetypes` is rebuilt by the prototype's own
`buildInterfaceArchetypeFromCells()` (`archetypes.js` → `calibration/continuum_to_voxel.js`),
fed with cell archetypes whose `sectionsByAxis` were corrected in memory. All fields of the
existing archetype object (same id) are then replaced, so every edge and both cores see the
change. This happens before the first solve. For `prototype` the same rebuild runs and is
asserted to be bit-identical, which validates the method. `patch.k0Ratios` records the
per-archetype k0 change.

* **fixY**: `rectangularSectionsForAxis(effDims, 1)` orders the Y-edge section as
  `[about x, about z]`: `bendingInertias [sx·sz³/12, sz·sx³/12]`, `fiberDistances [sz/2, sx/2]`.
  The Y frame is `(n,t1,t2) = (y,z,x)`, so component 3 bends about z and component 4 about x.
  The fix sets `bendingInertias = [sz·sx³/12, sx·sz³/12]` and `fiberDistances = [sx/2, sz/2]`,
  an exact swap. Effect: k0[3]↔k0[4], capacities, onset/break b1↔b2 and plasticYield swap
  on Y-edge archetypes. rc_floor Y edges get ×16 / ×1/16 bending stiffness. X and Z edges are
  already consistent and are untouched.
* **fixJ**: `torsionJ = A(s1²+s2²)/12` (polar) is replaced on every axis section by the
  Saint-Venant constant of the a×b rectangle (a ≥ b):
  `J = β·a·b³, β = (1/3)[1 - (192/π⁵)(b/a) Σ_{n odd} tanh(nπa/(2b))/n⁵]` (exact series).
  Examples: β = 0.28081 for 1×0.25 and 0.14058 for a square.
  `torsionRadius`, the torsion-capacity lever arm, is unchanged. The torsion onset rotation
  `τ/(G·r)·κ_on` is therefore unchanged, while k0[5], the torsion capacity and the
  fracture-energy break threshold scale with J. rc_floor: k0[5] × 0.1982.
* **fixYJ**: both fixes.

Each variant has its own `interfaceArchetypes`, `static`, `outcome` and `analytic`. `model`
and `feel` are shared, and `feel` uses prototype sections only.

## `feel`: XPBD game-core captures

* Core: `XpbdRuntime` via `engine.tick()` on the CPU f64 path.
* Settings: `APP_UI_SETTINGS` is exactly the object that `src/app.js syncSettingsFromUi()`
  builds from `index.html`'s default controls in sync mode. That means xpbdLinearDamping
  0.988, xpbdAngularDamping 1.0, contact scope `none` (both contact checkboxes default off),
  friction 0.6, gyroscopic on, kill plane at -4096, blast radius 2.0 / damage-radius scale
  2.0 / padding 0.4 / peak 0.85, and the default 12 substeps × 1 sweep at 1/60 s.
* Debris policy: `delete` and `keep` (the UI default is `keep`).
* `runs[policy].settle`: from the virgin state. The prototype ramps gravity over
  `settleRampTicks` (30) with inelastic bookkeeping frozen.
* `runs[policy].blasts[id]`: blasts are applied to the settled state with
  `{deferSolve:true, captureTransition:false}`. This is the app's real-time click path
  (`engine.applyBlast(ix,iy,iz,r)` for a clicked cell), followed by a new tick phase.
  * building3: app default r = 2.0 at cell (3,0,2).
  * ootest_blastedTower: app default at mid-height (0,0,4), and the oracle test's base blast.
  * ootest_gentleBlastBlock: the oracle test's gentle blast.
* Stop rule: up to 600 ticks. A phase stops after 5 consecutive ticks with
  `awakeCells == 0`; the virgin settle cannot stop before its ramp has finished.
  * `appPark` records where the app's own rule (5 consecutive `stats.quiet` ticks) would have
    stopped ticking.
* Series (≤ 200 samples, bucketed; `bucketTicks` is the bucket size):
  * `maxDisp`, `minDz`, and the same over anchored cells (`maxDispAnchored`, `minDzAnchored`)
    are bucket extrema.
  * `awake`, `alive`, `anchored`, cumulative `ruptures` and `deleted` are end-of-bucket
    values.
* Summary: peakDisp, peakSag (+tick), finalSag, `final{}`, allAsleepTick, quietTick,
  appPark, first/last rupture tick, collapsed, totalRuptures/Deleted, finalMaxDamage.
* `appStreamed` (building3, cascadingCollapse): the same captures through the app's exact
  load path. That is a streaming engine, `initStreamingScenario()` with the unified scene
  from `src/scenarios/streaming.js`, `boundaryPolicy strict`, and unclipped solve bubbles.
  Those generators give column/wall/floor intersections different archetypes than the
  registry scene of the same name. Cell ids and counts, and the `model` and oracle blocks,
  therefore do not describe that geometry.

Differences from the live app that remain:

* The CPU f64 tick is used, not the WebGPU f32 substep executor. The app uses WebGPU when
  available and ≥ 64 cells are awake.
* Registry scenes are only reachable through `initScenario` (the grid path). The app itself
  only ships the streamed scenes.
* **The XPBD runtime ignores `cell.appliedForce`.** Feel runs are therefore gravity-only,
  even for plasticBeam, continuationStiffStrip and the analytic scenes. Those have point
  loads or g = 0 in their oracle load case.

## Checker (`check.mjs`)

Validates, from the fixture files alone:

* `index.json` sha256 and sizes.
* Model integrity:
  * ids are sorted and unique;
  * each edge is a `+axis` face pair;
  * `x0 = ijk·cellSize` and `mass = density·gravityMassVolume`;
  * support and table references resolve;
  * edge frames are right-handed.
* Archetype sanity: `k0 > 0`, `onset < break`. Variant consistency: fixY is exactly a
  k0[3]↔k0[4] swap on Y archetypes only, fixJ changes only k0[5] and never increases it, and
  neither changes k0[0..2].
* Static equilibrium:
  * Σ loads is recomputed from `model` and must equal the exported value.
  * Σ reactions + Σ loads ≈ 0 (≤ 1e-5 relative).
  * The free-cell residual must be ≤ 1e-4 of the largest cell load.
  * φ recomputed from `force6/k0` and `onset` must match to 1e-9.
  * `maxPhi`, `edgesBeyondOnset` and `elasticRegime` must be consistent.
* Outcome bookkeeping:
  * the alive, deleted and blast-removed sets partition the cells;
  * the per-pass rupture and deletion sums equal the id lists;
  * convergence flags agree with `passLog`.
* Feel series: shapes, monotonicity, and consistency between the summary and the series.
* Analytic: measured / discrete-RBSM within 1 %.

Current result: 23 fixtures, 0 failures.

## Findings and caveats (read before using a fixture as a gate)

* **The oracle closes with non-converged Newton passes on two scenes.** On `cantilever`
  and `plasticBeam`, pass 1 ruptures 63 / 27 edges although the anchored Newton solve did
  not converge (normalized residual 0.46 / 0.21 after 24×6 iterations). Pass 2 is then quiet.
  Their rupture sets depend on where the unconverged Newton stopped: cantilever gives
  63 / 51 / 26 / 37 ruptures across prototype/fixY/fixJ/fixYJ. `allPassesNewtonConverged =
  false`, so exclude them from parity gates. plasticBeam's 27 ruptures still match the plan's
  golden number.
* **`contactGuidedStack` never converges** (96 passes). With the default contact scope
  `none`, the SLIDER_Z-guided top cell has a free z DOF and falls. It also fails with contact
  enabled: no contact is ever detected and the cell falls through the base.
  `contactSmokeStack` just deletes its unbonded top cell. `ootest_floatingCluster` has no
  anchored cells.
* **Static results beyond the elastic regime.** The damage-free equilibrium exceeds box onset
  on cantilever (φ_max 1.88) and plasticBeam (φ_max 27, 2.55 m tip deflection). Both are
  well-defined elastic answers but physically meaningless for the damage model; see
  `elasticRegime`.
* **Variant sensitivity.** Static roof/tip sags change with the fixes:
  * building3: × 1.83 (fixYJ);
  * braceCorner: × 2.9;
  * plate: × 5.4;
  * Y cantilever strip: × 15.8. The prototype is 16× too stiff there, and fixY reproduces
    the X strip to 1e-11.
  * Pure X beams are unchanged.

  Lattice vs discrete-RBSM prediction:
  * X strip: 0.999987.
  * SS beam: 0.99713. This is corotational tension stiffening: the pinned ends are axially
    fixed.
  * Plate (fixYJ) / Kirchhoff: 0.77.
* **XPBD vs oracle.** The XPBD core ruptures clampedCantilever, the 8-cell strips and
  plasticBeam (self-weight only) on ticks 31-33, right after the gravity ramp. The oracle keeps
  clampedCantilever standing (φ 0.96). Settled XPBD sags are 2-25× the static oracle
  (building3 3.4e-3 vs 1.5e-4 m).
* **App park during the settle ramp.** In the CPU path, stiff scenes report `stats.quiet`
  on every ramp tick, so the app's park rule (5 quiet ticks) fires at tick 5. This happens on
  cascadingCollapse, multiSpan, seamCompare, wallLintel and the ootest blocks: the app would
  stop ticking at about 1/6 of the gravity ramp and display about 17 % of the settled sag. On
  the streamed building3 it fires only after the ramp, at tick 41. This is a likely app bug,
  and it matters when turning captures into a feel spec. `appPark` records it per run.
* With `keep` and contact off (the app defaults), detached debris free-falls toward the kill
  plane. Those runs end at 600 ticks (`stopReason max_ticks`). Use the `*Anchored` columns
  for structural motion.
* `cellSize` is 1 m in all fixtures; the prototype never runs at the 12.5 cm design point.
