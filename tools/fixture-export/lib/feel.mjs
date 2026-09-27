// "Feel" captures of the XPBD game core (XpbdRuntime via engine.tick()).
//
// Settings = the browser app's UI defaults, i.e. exactly the object
// src/app.js syncSettingsFromUi() builds from index.html's default control
// values (sync mode). Differences from the running app are documented in
// README.md (grid path instead of the streamed world store, CPU f64 tick
// instead of the WebGPU f32 substep executor, debris policy set per run).

import { initScene, anchoredCellIds } from './oracle.mjs';

export const APP_UI_SETTINGS = Object.freeze({
  blastRadius: 2.0,
  blastDamageRadiusScale: 2.0,
  blastFracturePadding: 0.4,
  blastDamagePeak: 0.85,
  maxOuterIterations: 24,
  residualTolerance: Math.pow(10, -6),
  maxRotationIncrement: 0.25,
  maxTranslationIncrement: 0.5,
  forceFiniteDifferenceTangent: false,
  useCoupledPlasticity: false,
  cohesiveContactPairScope: 'none',
  contactPairScope: 'none',
  contactEnabled: false,
  cohesiveContactEnabled: false,
  useContact: false,
  detachedDebrisPolicy: 'keep',
  debrisDespawnSeconds: 30,
  cohesiveDeleteDetachedContactBridges: false,
  deleteDetachedContactBridges: false,
  allowSynchronousGlobalEscalation: false,
  contactFrictionCoefficient: 0.6,
  contactRestitution: 0,
  xpbdLinearDamping: 0.988,
  xpbdAngularDamping: 1,
  groundConstraintEnabled: false,
  groundZ: 0,
  groundFriction: 0,
  killPlaneEnabled: true,
  killZ: -4096,
  gyroscopicEnabled: true,
  contactRollingEnabled: true,
  contactNarrowphaseMode: 'sphere',
  closureTimeBudgetMs: null,
});

export const FEEL_MAX_TICKS = 600;
export const FEEL_MAX_SAMPLES = 200;
export const ASLEEP_STOP_STREAK = 5; // same count as the app's SIM_QUIET_PARK_FRAMES
export const APP_PARK_QUIET_FRAMES = 5; // src/app.js SIM_QUIET_PARK_FRAMES

// Authoritative container: the WorldGrid, or the streamed world store.
function containerOf(engine) {
  return engine.worldStore && engine.enableStreaming ? engine.worldStore : engine.grid;
}

function measure(P, engine) {
  const grid = containerOf(engine);
  const anchored = anchoredCellIds(P, engine, 'DETACHED_CLEANUP', grid);
  let maxDisp = 0;
  let minDz = 0;
  let maxDispAnchored = 0;
  let minDzAnchored = 0;
  let alive = 0;
  for (const c of grid.cells) {
    if (!c.alive) continue;
    alive += 1;
    const dx = c.xCommit[0] - c.x0[0];
    const dy = c.xCommit[1] - c.x0[1];
    const dz = c.xCommit[2] - c.x0[2];
    const d = Math.hypot(dx, dy, dz);
    if (d > maxDisp) maxDisp = d;
    if (dz < minDz) minDz = dz;
    if (anchored.has(c.id)) {
      if (d > maxDispAnchored) maxDispAnchored = d;
      if (dz < minDzAnchored) minDzAnchored = dz;
    }
  }
  return { maxDisp, minDz, maxDispAnchored, minDzAnchored, alive, anchored: anchored.size };
}

function maxDamage(engine) {
  let m = 0;
  for (const e of containerOf(engine).interfaces) if (e.alive) m = Math.max(m, Number(e.committed.damage) || 0);
  return m;
}

// Bucketed decimation to <= FEEL_MAX_SAMPLES samples. Each sample closes a
// bucket of consecutive ticks: extremal columns keep the bucket extreme,
// state columns keep the value at the bucket's last tick.
function downsample(rows) {
  const n = rows.length;
  const bucket = Math.max(1, Math.ceil(n / FEEL_MAX_SAMPLES));
  const cols = {
    tick: [], maxDisp: [], minDz: [], maxDispAnchored: [], minDzAnchored: [],
    awake: [], alive: [], anchored: [], ruptures: [], deleted: [],
  };
  for (let i = 0; i < n; i += bucket) {
    const slice = rows.slice(i, Math.min(n, i + bucket));
    const last = slice[slice.length - 1];
    cols.tick.push(last.tick);
    cols.maxDisp.push(Math.max(...slice.map((r) => r.maxDisp)));
    cols.minDz.push(Math.min(...slice.map((r) => r.minDz)));
    cols.maxDispAnchored.push(Math.max(...slice.map((r) => r.maxDispAnchored)));
    cols.minDzAnchored.push(Math.min(...slice.map((r) => r.minDzAnchored)));
    cols.awake.push(last.awake);
    cols.alive.push(last.alive);
    cols.anchored.push(last.anchored);
    cols.ruptures.push(last.ruptures);
    cols.deleted.push(last.deleted);
  }
  return { bucketTicks: bucket, samples: cols.tick.length, ...cols };
}

// Tick until everything is asleep for ASLEEP_STOP_STREAK consecutive ticks
// (and, for the virgin settle, the gravity ramp is over) or FEEL_MAX_TICKS.
function tickPhase(P, engine, { minTicks }) {
  const rows = [];
  let ruptures = 0;
  let deleted = 0;
  let asleepStreak = 0;
  let quietStreak = 0;
  let appPark = null;
  let stopReason = 'max_ticks';
  let firstRuptureTick = null;
  let lastRuptureTick = null;
  const start = measure(P, engine);
  for (let t = 1; t <= FEEL_MAX_TICKS; t++) {
    const s = engine.tick({});
    ruptures += s?.ruptured || 0;
    deleted += s?.detachedDeleted || 0;
    if ((s?.ruptured || 0) > 0) {
      if (firstRuptureTick == null) firstRuptureTick = t;
      lastRuptureTick = t;
    }
    const m = measure(P, engine);
    const awake = Number(s?.awakeCells ?? 0);
    rows.push({ tick: t, ...m, awake, ruptures, deleted, quiet: s?.quiet === true });
    quietStreak = s?.quiet === true ? quietStreak + 1 : 0;
    if (!appPark && quietStreak >= APP_PARK_QUIET_FRAMES) {
      appPark = { tick: t, maxDisp: m.maxDisp, minDz: m.minDz, minDzAnchored: m.minDzAnchored };
    }
    asleepStreak = awake === 0 ? asleepStreak + 1 : 0;
    if (asleepStreak >= ASLEEP_STOP_STREAK && t >= minTicks) {
      stopReason = 'all_asleep';
      break;
    }
  }
  const last = rows[rows.length - 1];
  const firstOfFinalStreak = (pred) => {
    let k = null;
    for (let i = rows.length - 1; i >= 0; i--) {
      if (!pred(rows[i])) break;
      k = rows[i].tick;
    }
    return k;
  };
  let peakSag = 0;
  let peakSagTick = null;
  let peakDisp = 0;
  for (const r of rows) {
    if (-r.minDzAnchored > peakSag) { peakSag = -r.minDzAnchored; peakSagTick = r.tick; }
    if (r.maxDisp > peakDisp) peakDisp = r.maxDisp;
  }
  const summary = {
    ticks: rows.length,
    stopReason,
    allAsleepTick: firstOfFinalStreak((r) => r.awake === 0),
    quietTick: firstOfFinalStreak((r) => r.quiet),
    appPark,
    peakDisp,
    peakSag,
    peakSagTick,
    finalSag: last ? -last.minDzAnchored : 0,
    final: last ? {
      maxDisp: last.maxDisp, minDz: last.minDz, maxDispAnchored: last.maxDispAnchored,
      minDzAnchored: last.minDzAnchored, alive: last.alive, anchored: last.anchored, awake: last.awake,
    } : null,
    initialAlive: start.alive,
    finalAlive: last ? last.alive : start.alive,
    collapsed: last ? last.alive < start.alive : false,
    totalRuptures: ruptures,
    totalDeleted: deleted,
    firstRuptureTick,
    lastRuptureTick,
    finalMaxDamage: maxDamage(engine),
  };
  return { summary, series: downsample(rows) };
}

// The app's own load path (src/app.js loadScenario, sync mode, UI defaults):
// streamed engine, settings applied first, then initStreamingScenario() with
// the unified scene definition and the default bubble / stream-policy UI.
export function appStreamedInitOptions(def) {
  return {
    ...def.options,
    boundaryPolicy: 'strict',
    scenarioName: def.id,
    maxBubbleCells: Infinity,
    maxBubbleRadius: Infinity,
  };
}

function makeFeelEngine(P, scene, policy, streamed) {
  const settings = { ...APP_UI_SETTINGS, detachedDebrisPolicy: policy, physicsCoreId: P.PhysicsCoreId.XPBD_ORIENTED };
  if (!streamed) {
    const engine = initScene(P, scene, P.PhysicsCoreId.XPBD_ORIENTED);
    engine.applySettings(settings);
    return engine;
  }
  const def = P.resolveUnifiedScenarioDefinition(streamed.scene);
  if (def.id !== streamed.scene) throw new Error(`unified scene ${streamed.scene} missing`);
  const engine = new P.DestructionEngine({ physicsCoreId: P.PhysicsCoreId.XPBD_ORIENTED });
  engine.setLogger(() => {});
  engine.applySettings(settings);
  engine.initStreamingScenario(def.scene, appStreamedInitOptions(def));
  return engine;
}

export function runFeel(P, scene, policy, blast = null, streamed = null) {
  const engine = makeFeelEngine(P, scene, policy, streamed);
  const t0 = performance.now();
  const rampTicks = Number(P.XpbdDefaults.settleRampTicks ?? 30);
  const settle = tickPhase(P, engine, { minTicks: rampTicks + 1 });
  if (!blast) return { run: { settle }, wallMs: performance.now() - t0 };

  const radius = blast.radius === 'appDefault' ? engine.settings.blastRadius : blast.radius;
  const options = { deferSolve: true, captureTransition: false };
  let result;
  let center;
  if (blast.cell) {
    const cell = engine.getCellGlobal(blast.cell[0], blast.cell[1], blast.cell[2]);
    if (!cell?.alive) throw new Error(`${scene.id}: blast cell ${blast.cell} not alive after settle`);
    center = Array.from(cell.xCommit);
    // Same entry point as a click in the app (facade.applyBlast -> engine.applyBlast).
    result = engine.applyBlast(blast.cell[0], blast.cell[1], blast.cell[2], radius, options);
  } else {
    center = [...blast.center];
    result = engine.applyBlastAtWorld([...blast.center], radius, options);
  }
  const after = tickPhase(P, engine, { minTicks: 1 });
  return {
    run: {
      settle,
      blast: {
        id: blast.id,
        target: blast.cell ? { cell: [...blast.cell] } : { center: [...blast.center] },
        center,
        radius,
        options,
        info: result?.blastInfo ? { ...result.blastInfo } : null,
        ...after,
      },
    },
    wallMs: performance.now() - t0,
  };
}
