# structvox status: plan Phases 0–7

This page states, per phase of [`PLAN.md`](PLAN.md), what exists, which gates are met, and
what is still open. [`PLAN.md` §G](PLAN.md#g-revision-log-implementation-decisions-that-refine-the-plan)
logs where the implementation refined the plan.

Measurements were taken on an Apple M-series machine (6 P- + 12 E-cores) that was heavily loaded by
other applications (Cursor, Blender, Python jobs; load average up to ~30). Timings are therefore
pessimistic. Performance comparisons use thread CPU time and A/B runs under the same conditions.

## Summary

| Phase | Built | Gates |
|---|---|---|
| 0 Foundations and content study | ✅ | ✅ ([report](phase0/PHASE0_REPORT.md)) |
| 1 Reference core | ✅ | ✅ parity, analytic, fracture energy, S invariance, O(change), determinism, static+DIF vs dynamics 33/33 |
| 2 Composite bubbles | ✅ with nominate / re-solve, coarsening, background verification | Against the full-fine truth: column-removal library 8/9 at R0 12 and 16, blows 9/9 ✅; inflation calibrated at 1.0 by outcomes. Re-partition: momentum error ≤ 1e-14, kinetic energy change ≤ 1e-14 J (roundoff) ✅. PCG ✅. Steady step ✅ (2.4–2.5 ms, one WASM thread); **first step ≈** (15–20 ms against 12, below) |
| 3 Engine at scale | ✅ with counted-work commits | **Determinism ✅ (10-minute replay, 6 configurations, gates 17 and 18 on the final engine).** Memory ✅. 20 events/s ✅ (bullets: p95 0.72 ms; rockets and bullets at 6.6/s: p95 0.89 ms) |
| 4 Web runtime | ✅ | 60 fps ✅, native==WASM ✅. Structural ms/frame: mean ✅ (0.1–0.2 ms), p99 ✅ (1.2–5.0 ms vs 8 ms) |
| 5 Doom slice and feel | ✅ with all vanilla sector movers | **Freedoom: 57 of 68 maps re-checked on this build (all stand, three after the design-class fix); the full re-run was stopped.** 10 rockets/s on MAP01: p95 3.5–10 ms native, 5.4–9.6 ms WASM ✅ (was 175 ms); p99 14–28 ms **≈**. Feel: calibrated, **user sign-off open** |
| 6 Streaming and persistence | ✅ with far render tier, coarse connectivity and hydrated verification of evicted chunks | Exact round trip ✅, bounded residency ✅, stream work 1–2.6 ms/tick, detachment across the streaming boundary exact ✅, failures there verified ✅ |
| 7 Extensions | ✅ all six | Debris, contacts, plates, GPU interpolation, lockstep, sector movers |

The structural gates still short are the **first step of a bubble** on one WASM thread, and p99
under 10 rockets/s (per-frame budgets below). Numbers here are from
`svx_step_bench` (rooms world, four rockets). Native times are thread CPU time; WASM times
are single-threaded under Node. The machine was loaded (load average ~10), so treat them as upper
bounds.

| | 2026-09-25 | Now, native | Now, WASM (1 thread) |
|---|---|---|---|
| Event tick on the simulation thread (rocket: window, mutation) | 100–170 ms, setup included | 4.5–6.9 ms | 7.5–20 ms |
| Bubble setup (residual, composite, preconditioner, first step), off the simulation thread in threaded builds | (in the event tick) | 37–50 ms (R0 16) | 41–49 ms (R0 12), 52–59 ms (R0 16) |
| … of which the first step (solve and law) | — | 13–22 ms (R0 16) | **15–20 ms (R0 12, 2.9–4.3k nodes; gate 12 ms)**, 20–24 ms (R0 16) |
| Steady step, 2.9k nodes (R0 12) | — | — | **2.4–2.5 ms** (gate: 4 ms for ~3k nodes ✅) |
| Mean step over the 8 steps after the event (R0 12) | — | — | 10–12 ms without nomination; 25–30 ms with it (refining the blast interface) |

- The setup was 280–430 ms native in the middle of this work: every window re-solved its cached
  baseline. Two findings removed that (PLAN §G, baseline consistency): windows classified plates
  from their own cells, which made every window disagree with the bake at its edge; and the
  consistency check fired on the f32 rounding floor. It now solves only a cache whose kinks the
  law reads (φ > 0.75): none on the whole-baked rooms world, 45 of 65 barrage windows on MAP01.
- Of the first step at 2.9k nodes (15 ms), the solve takes 11.4 ms: 11 PCG iterations at ~1 ms. A
  better start does not help: the first step is stiffness-dominated, not mass-dominated. The kernel
  is at ~4 ns per 6×6 block, with FMA ruled out by determinism.
- Nomination (below) refines rocket bubbles at the blast interface: they grow from 2.9–4.3k to
  5–10k nodes, which doubles the transient cost. In the rooms bench the all-fine truth has no
  failure there either, so this is the price of the conservative bound (see open items).

**Per-frame budgets.** `svx_engine_demo --realtime` paces ticks at 60 Hz; times are wall-clock,
4 threads. The final engine was measured with the machine's other load at 12–14:

| Scenario | mean | p95 | p99 | worst | ticks over 16.7 ms |
|---|---|---|---|---|---|
| rooms: 3 rockets, 20 bullets (600 ticks) | 0.10 ms | 0.51 ms | 1.16 ms | 11 ms | 0 |
| rooms: 12 rockets, 120 bullets (1,200 ticks; 6.6 events/s) | 0.20 ms | 0.89 ms | **4.96 ms** | 10 ms | 0 |
| rooms: 300 bullets at 20/s (1,000 ticks) | 0.20 ms | 0.72 ms | 0.89 ms | 1.2 ms | 0 |
| MAP01: 60 rockets at 10/s (900 ticks), native, 4 runs | 0.61–1.10 ms | **3.5–10.0 ms** | 14.0–22.9 ms | 17–35 ms | 1–22 |
| MAP01: the same, WASM with threads (Node), 3 runs | 1.01–1.20 ms | **5.4–9.6 ms** | 24.3–28.5 ms | 32–40 ms | 26–33 |

The WASM runs have the same state hash as the native ones. Before the extraction below got
cheaper, the WASM p95 was 19.4 ms. Earlier engines, for comparison:

| Scenario | mean | p95 | p99 | worst |
|---|---|---|---|---|
| rooms 3 + 20, bubble steps on the simulation thread | 0.9 ms | 6.3 ms | 22.8 ms | 51 ms |
| rooms 3 + 20, pipelined steps, async setups and triage | 0.27 ms | 0.5 ms | 7.9 ms | 32 ms |
| rooms 12 + 120, bubble steps on the simulation thread | 1.8 ms | 9.9 ms | 26.5 ms | 51 ms |
| rooms 12 + 120, pipelined, setup latency scaled with the bubble | 0.4–0.5 ms | 1.2–1.6 ms | 10.0–11.8 ms | 58 ms |
| MAP01 10 rockets/s, before counted work | 48 ms | 175 ms | — | — |

What moved off the simulation thread, and what got cheaper:
- Bubble steps are pipelined: 0.1–0.2 ms per tick left. Their setup runs in the background.
- Small carves settle statically in the background and apply at the next tick (async
  triage). A bullet costs 0.5 ms here instead of 5–7 ms.
- All background jobs commit by counted work (Phase 3 below). A heavy bubble runs in slower
  simulated time rather than holding ticks.
- A rocket's event tick is ~5 ms native: the window extraction reads chunks through a table,
  and the lattice build walks plate runs through neighbour arrays (classes from the world grid,
  walked chunk by chunk) with a cached model lookup; the pre-event forces are taken only near
  the blast, and the setup assembles the rest (bitwise the same).
- On WASM the extraction was the event tick's largest part: 8.6–10.2 ms for 15–23k cells, of
  which a third was hash-map inserts. A flat coordinate map sized for the window
  (`base/coord_map.hpp`) brought it to 6.2–7.6 ms (bitwise the same).

Gates:
- Phase 4: mean ≤ 4 ms ✅, p99 ≤ 8 ms ✅ in the rooms scenarios (1.2–5.0 ms).
- Phase 3: 20 events/s at p95 ≤ 8 ms ✅ (0.72 ms).
- Phase 5, 10 rockets/s on MAP01: p95 3.5–10 ms native and 5.4–9.6 ms on WASM ✅, mean ~1 ms.
  p99 is 14–23 ms native and 24–28 ms on WASM: 1–33 of 900 ticks run over a frame. Those are
  ticks where rocket events meet commits of large bubbles; they slow simulated time slightly,
  and the render thread and client-side movement are unaffected.

## Phase 1: reference core and semantics

Details are in [`phase1/PHASE1_REPORT.md`](phase1/PHASE1_REPORT.md).

- 20/20 outcome parity with the oracle.
- Static equilibrium within 1.6e-5 of the oracle.
- Analytic beams within 1%.
- Fracture energy per area exactly invariant across h.
- Classification identical for S ∈ {1, 4, 16}.
- Connectivity work proportional to the change.
- Bit-identical across threads and native/WASM.

**Static + DIF vs dynamics:** 33 of 33 comparable scenarios (100%) classify the same at every
DIF from 1.0 to 2.0 (gate ≥ 90%). The scenarios are table, portal, propped cantilever and wall
lintel families over seven fragility levels. The calibrated DIF is 1.0
([`phase1/sim_library_dif.txt`](phase1/sim_library_dif.txt)).

**Anderson acceleration** of the secant iteration (depth 3, the default): the oracle fixtures
converge in 14,086 → 8,861 iterations at identical static parity and 20/20 outcomes. The two
scenes that became comparable (table and propped cantilever at fragility 0.02) had counted as
"failing before the event" only because the plain iteration never converged on them (19,200
iterations, no rupture); `svx_sim_library --pre` compares the three ways to their pre-event
state.

## Phase 2: composite dynamic bubbles

- Composite partition (scaled Galerkin, St-Venant torsion correction), baseline + Δ,
  nonlinear fine sub-lattice, linear coarse couplings. Re-partition uses a momentum-exact
  rigid projection. Multigrid is greedy aggregation with a V-cycle, Chebyshev(3) and SIMD
  kernels.
- **Accuracy.** The elastic near-field peak-over-time is 0.925–0.943 of the full-fine truth.
  Outcomes are what the bubbles now get right, because the fine level decides (§B6):
  - **Nominate.** Every 2 steps each aggregate's crossing bonds are bounded (baseline plus the
    rigid Δ-jump times 2 / 4 / 8 by level). Aggregates over 0.5 of onset twice in a row become
    fine, with a ring. The re-partition is momentum-exact (error ≤ 1e-14; kinetic energy changes by roundoff,
    ≤ 1e-14 J), and
    quiet refined regions go back into aggregates after 30 steps.
  - **Marginal bonds re-solve grown (§B6.3).** Fine bonds within one aggregate of the fine/coarse
    interface decide nothing. When one reaches 0.95 of onset, the step is solved again from the
    state before it, with the aggregates around it made fine.
  - **Column-removal library** (3 × 3 bays, fragility sweep across the collapse threshold,
    against the full-fine truth): 8 of 9 exactly at R0 12 and at R0 16, missing only at the
    threshold itself (44 ruptures standing, against the truth's collapse). Without nomination
    the coarse field cannot fail: 0 of 7 failing cases.
  - **Blows on the slab** (against the all-fine bubble): 9 of 9 classify like the truth with
    nomination and re-solve, 7 of 9 with nomination alone.
  - **Demand inflation** stays 1.0: with nomination, 1.00 matches the library best; 1.03 and
    1.06 over-predict (4–5 of 9).
  - **The local decide stage is built but off.** It caught none of the library's 14 failing cases
    at any threshold from 0.4 to 0.9: its patch is held at the composite's motion, which is
    stiff-biased exactly where the load path changes.
  - Steady PCG p95 is 2–5 (gate ≤ 15).
- **Background verification (§B6).** Settled bubbles are re-checked at structure level: a
  full-fine static solve with the law on every bond, off the simulation thread.
  - **Start:** once the job's own window has been quiet for 1 s (no event in it, no bubble or
    settle over it), the simulation thread takes a snapshot of the chunks the structures can
    reach (1–2.5 ms). Chunks are copied; baseline bricks are shared copy-on-write. Before, a
    job waited for 1 s without any event anywhere: the 10-minute rooms session had 0
    verifications in its first 450 s. Test: a structure hit once is verified and fails while
    another is shot every 0.4 s.
  - **Background work:** extraction and solve run on a background thread with their own
    three-thread team. The result is bitwise identical for any team size (test).
  - **Due tick:** fixed by the job's size: 2 s, longer above 72k cells at 600 cells per tick
    (98k cells: 2.7 s).
  - **Budget:** the solve has 600 PCG iterations at most, and no more than 8M cell-iterations.
    Near a mechanism the static iteration chatters (one small structure took 3,643). An
    undecided result continues dynamically, like a failure, instead of committing an
    unconverged state. A cached state with kinks the law reads is solved first (baseline
    consistency, PLAN §G), within the same budget. That pre-solve once had no budget and a
    trigger below the f32 floor: in the 10-minute session it held the simulation thread for
    minutes at a due tick, as soon as verification started under fire.
  - **Stale jobs:** a job set aside by an event, or dropped because its structures changed, is
    queued again. A stale job that had found a failure goes to the front of the queue. A job
    queued again after going stale waits until its structure's chunks have been unchanged for
    1 s, instead of starting again into the same busy structure (a 4-minute rooms session: 5.5
    → 2.5 minutes of wall time, identical result).
  - **In a busy session:** a job whose structure changed while it ran is dropped and queued
    again, so on one building under continuous fire jobs complete once the shooting pauses (open
    items). Before, the stale results thrown away included a detected failure; now such a job
    goes first.
  - Before, extraction cost the simulation thread 140–200 ms, and under load it waited 1.4–3.4 s
    at the due tick.
  - The test `verification: a far member …` shows an overloaded cantilever in the bubble's
    coarse field. It is missed without verification, and caught and failed dynamically with
    it.
  - On the rooms world, a 99k-cell structure is verified in 3 iterations.
- **Engine-side performance work:**

  | change | effect |
  |---|---|
  | assembled SIMD operator for PCG | neutral |
  | GPU displacement fields | render work −78% (15.9 → 3.5 ms/tick) |
  | removing floating voxels at bake | tower rocket event 2.6 s → 0.2 ms |

## Phase 3: engine core at scale

- Windows, triage, merging, bubble budget, finalize with composite settle, continuation,
  whole-world and tile baking with the design pass, streaming, persistence, deterministic
  hashing.
- **Determinism.** The bundled libm (`svx::dm`) produces the golden digest
  `410fe26f50cb825f` on ARM, x86-64 and WASM.
  - `svx_replay` records a 10-minute scripted rooms session: 19,236 commands, 1,187 events,
    97 bubbles, 25 ruptures and 3,492 detached voxels.
    `tools/replay/gate.sh` runs the whole gate.
  - All 60 checkpoint hashes are identical on ARM with 2/4/8 threads, x86-64 with 1/4, and
    WASM, against the ARM 1-thread record ([`phase3/replay_10min.txt`](phase3/replay_10min.txt)).
    This is on the final engine of this page: counted-work commits, events waiting for
    running bubbles, baseline consistency, windows' plate classes from the world, nominate and
    the marginal re-solve, Anderson, verification started per window. Replaying the same log,
    the final binaries (after three lookup-only refactors of the window extraction) reproduce
    all 60 checkpoints on ARM, x86-64 and WASM. Gates 10, 11 and 14 passed on earlier engines.
  - The same session's destruction fell from 47,437 ruptures (gate 14) to 25, with 3,492
    detached voxels. Replayed with a tightly converged solver (fresh Jacobian every 0.005 rad,
    8 Newton iterations, tolerances 1e-5), the session has 30 ruptures and 4,802 detached
    voxels, so the drop is not a looser solve. It came with the changes since gate 14 (PLAN §G):
    windows no longer read kinks of the cached baseline, a window's rim bonds no longer lost
    their plate capacity (1.4×), and events wait for the bubbles running where they land.
  - A crash hunt on the final engine also passed: four more scripted sessions of 300 s (rooms
    with two other seeds, the tower, and the flight over the streamed city, which leaves the
    city after about 2 minutes) with 0 to 67 ruptures, and MAP01 under 90 rockets and 200
    bullets (no rupture, 42 detached voxels).
  - The unit-test session hash is 14919238000388105179 on native and on both WASM builds.
  - **The gate found a physics bug.** An earlier run differed on x86-64 from 509 s on. The
    cause was a failing settle with no trust region: it threw a window to absurd or NaN
    states, and its continuation ruptured 512 bonds per step. NaN bits differ between x86 and
    ARM. With trust regions and non-finite guards the same session has 24.7k ruptures instead
    of 105–174k; about 5× of that destruction had been numerical. Scripted sessions then ranged
    from a few hundred to about 57k ruptures.
  - A lockstep test (a peer on 4 threads applying a host's serialized command stream with a
    5-tick delay) matches at every tick.
- **Budget manager (§B5).** Degradation is deterministic and driven by the active node count:
  coarser grading, then a smaller fine region, then static triage of medium events, then
  half-rate stepping of far bubbles.
  - The last resort, slower simulated time, is **counted work** (`base/work.hpp`). Background
    jobs (setups, steps, settles, triages, finalizes) count their work in node-iterations, and
    a job commits ⌈work / `tick_work`⌉ ticks after its launch (at least its minimum latency).
    The counts depend only on the computation, so replays stay bit-identical; a heavy bubble
    runs in slower simulated time instead of lengthening frames, and the simulation thread
    waits only for a job behind schedule.
  - An event inside a running bubble's window waits for that bubble's step, which then settles
    and is taken over by the new window. At most one blast is released per tick.
- **Parallelism across bubbles (§B5).** Concurrent bubbles step one per thread, each serial
  inside, and results are committed in id order. A lone small bubble runs serially.
  - A four-rocket salvo on the rooms world: mean tick 5.30 ms on 1 thread, 2.71 ms on 4.
  - The state hash is identical.
- **Determinism fixes found by the cross-platform checks:**
  - `std::hypot` in the contact law is not correctly rounded; it now uses sqrt of a sum.
  - `bake()` built its lattice in hash-map order, which differs on 32-bit WASM. Native and
    WASM session hashes diverged until the chunk keys were sorted.
- **Memory.** About 0.75 B per voxel for MAP01 (gate ≤ 4 B).

## Phase 4: web runtime

- WASM worker with pthreads and the WebGPU renderer:
  - rendering: textures, sector light, AO, debug views, GPU displacement fields;
  - physics output: rigid debris poses, crack and impact effects;
  - collision: client-side, bit-exact with the engine (300/300 random sweeps);
  - world: OPFS persistence, streaming with a far tier.
- Debug overlays: damage / utilization and bubble-level views, and a **job and budget
  timeline** (with the HUD, `H`). It shows the worker's last 4 s of ticks as stacked bars
  (bubble steps, events, streaming, debris, verification, meshing, the rest) against the 8 ms
  budget and the 16.7 ms frame, and the active bubble nodes against the node budget.
- The browser smoke test (`web/scripts/smoke-wasm.mjs`) checks:
  - rooms, pistol, rocket (its displacement field reaches the GPU) and debris;
  - the streamed city;
  - Freedoom MAP01 with its sector movers, and a missing map falling back to rooms;
  - that there are no console, page or WebGPU errors.

## Phase 5: Doom slice and feel

- **WAD pipeline.** The voxelizer produces 6-connected shells, then textures and light, the
  bake plus design pass (rock supports included), and the sector movers.
- **Freedoom maps** (`svx_map_check --movers`; the earlier full record, 68 of 68 standing, is
  [`phase5/freedoom_check.txt`](phase5/freedoom_check.txt)).
  - On this build 57 of the 68 maps were re-checked (all 32 of Freedoom 2 and 25 of Freedoom 1)
    before the run was stopped to focus on the destruction physics. The check runs at fragility
    0.25, four times weaker than the game default.
  - 54 stood as they were. MAP26, MAP30 and E4M8 had members no design class up to × 64 could
    hold. MAP30's worst is a 3-voxel ceiling slab spanning about 104 × 81 m over a 28 m void
    (95 × onset under its own weight; the whole-world bake: 165), the plan's "ceilings are
    slabs with air above" rule applied to a huge room. With classes up to × 1,024, and a class
    scaling the fracture energy too (PLAN §G, Bake), MAP30 and E4M8 stand; MAP26 was not
    re-checked.
  - The design pass strengthens far more than in the earlier record (MAP01: 78,637 voxels, was
    2,857): the earlier tile bake pinned tile rims at zero, which cut long spans into tile-sized
    pieces and under-loaded them. So the earlier 68 of 68 is superseded.
  - Every sector mover ran every one of its moves on the re-checked maps, with none stuck.
- **Buckling margin of S_p** (plan §B3, §B10). On import, each map's free-standing wall strips
  give the largest S_p with S_p·P/P_cr ≤ 0.3 for 99% of them, and the engine caps S_p there.
  MAP01: 49.7 (the default 9 stands). MAP29, with its tall outdoor walls: 7.5. `svx_map_check`
  reports the cap per map (`S_p<=`), and the web HUD shows it (`complianceCap`).
- **Sustained combat (10 rockets/s on MAP01, fully baked, `svx_engine_demo --wad … --rockets
  60 --rocket-every 6 --bake-tiles 100000`, 4 threads).**
  - **Now** (paced at 60 Hz with `--realtime`): 64 events, 44 bubbles, 0 ruptures and 94
    detached voxels (crater debris). Natively mean 0.6–1.1 ms, p95 3.5–10 ms, p99 14–23 ms;
    WASM with threads mean 1.0–1.2 ms, p95 5.4–9.6 ms, p99 24–28 ms, the same state hash
    (per-frame budgets above).
  - Before the baseline-consistency work, 60 rockets in 6 s spawned about 70 bubbles, 7–10k
    ruptures and 1–1.7k detached voxels. Most of those ruptures were read from kinks of the
    tile-baked cache (PLAN §G); the history below is of that engine.
  - **Corotated reuse of the operator** (PLAN §G) halved the collapse steps:

    | | before | now |
    |---|---|---|
    | ms per bubble step | 54.1 | 28.2 |
    | multigrid rebuilds (from rotation drift) | 1,885 (1,311) | 765 (257) |
    | PCG / Newton iterations per step | 38.5 / 2.29 | 28.0 / 1.91 |
    | tick mean / p50 / p95 | 43.3 / 17.8 / 137 ms | 21.8 / 0.23 / 94 ms |
    | ticks over 16.7 ms (of 900) | 458 | 209 |

  - With it, the physics kept up on average (about 46 Hz; before about 23 Hz), but ticks with
    several large collapsing bubbles still exceeded the frame at p95. Counted work and the
    baseline consistency later brought the paced p95 to 4.5 ms (above).
  - **The rotated Jacobian also removed numerical destruction.** Recorded 10-minute sessions,
    replayed against a tightly converged reference, show the old default's extra collapses
    to be solver error: in one session a collapse with 10,278 ruptures by 300 s, where the
    reference and the new default have 7. Where a real collapse happens, the new default's
    detached voxels match the reference (1,903 vs ~1,750; the old default: 4,816).
  - Tried without gain on the barrage: aggregation by connection strength (0.05: 71 ms per
    step), a symmetric Gauss-Seidel smoother (32 ms), a W-cycle (32 ms).
  - The barrage exposed unbounded solves near mechanisms, which gave single ticks of 2–5 s. All
    solves now have budgets:
    - a settle projection: 240 PCG, when a new event forces a collapsing bubble to settle;
    - static triages of carves and debris impacts: 240 PCG, else a bubble decides;
    - a bubble's linear solve: 60 PCG, then the residual carries over and the preconditioner
      is rebuilt;
    - a verification: at most 8M cell-iterations (120–600 PCG), each linear solve capped at
      what is left. A 71k-cell job had run 541 iterations and held the simulation thread for
      4.7 s at its due tick (12M: still 1.25 s on this loaded machine).
- **Feel.** The `svx_fixture_compare --feel` harness compares with the prototype's XPBD
  captures ([`phase5/FEEL_SPEC.md`](phase5/FEEL_SPEC.md)).
  - Fitted: visible response S_p·A ≈ 8.6; the time to quiet is best at S_p 9 with light
    damping.
  - Defaults are now S_p 9, A 1, F 1, ζ 0.05 in both engine and web.
  - **Side-by-side video for the sign-off:**
    [`phase5/feel_side_by_side.mp4`](phase5/feel_side_by_side.mp4), recorded with
    `web/scripts/record.mjs`; the action scripts are in `phase5/recording/`.
    - Both engines lose the front row of ground columns: structvox's 10-storey tower (fragility
      0.1) and the prototype's building3.
    - The prototype's frame drops and tumbles. structvox's tower stands on its two remaining
      rows: its slabs cantilever, its members were strengthened by the bake's design pass,
      and the solve is converged.
    - This is the plan's known difference (§B3): the prototype is 2–25× softer than converged
      and its softness grows with size.
    - The two structures are not the same scene (the prototype's cells are 1 m), so the video
      shows the look, not a same-scene equivalence.
  - Side-by-side sign-off by the user is open.

## Phase 6: streaming and persistence

- The 1 km² procedural city streams with bounded residency. The smoke test shows resident
  chunks going from 2,199 to at most about 4,000, with about 4,000 evicted.
- **Streaming cost while flying** (`svx_stream_bench`: 400 m through the city at 20 m/s,
  19k chunks generated, 25k evicted):

  | per tick | p50 | p95 | p99 | max |
  |---|---|---|---|---|
  | stream work on the simulation thread (generation, eviction, far tiles) | 0.65 ms | 1.3 ms | 1.4–2.0 ms | 2–3 ms (1–12 of 1,200 ticks) |
  | meshing, 4 threads (before: serial) | 1.8 ms (5.8) | 2.1 ms (6.8) | 2.3 ms (8.4) | 2.9 ms (11.7) |

  The gate's "no hitch > 2 ms" holds at p99 for the stream work; the rare 2–3 ms ticks are
  single outliers on this loaded machine (a far tile of ~0.6 ms plus a generation batch).
- Exact persistence round trip, including edits of evicted chunks.
- **Byte budget** (plan §B8, `StreamConfig::max_resident_mb`): beyond it, chunks outside the
  load radius are evicted farthest first. Test: the city stays at 67 MB against a 67 MB budget
  (95 MB without one), and the ground under the viewer stays resident.
- **Far render tier.** Beyond the eviction radius and out to 320 m, the chunk source's coarse
  view is meshed into greedy 32 m tiles (≈ 3.5k vertices per tile).
- **Connectivity across the streaming boundary** (`world/coarse.hpp`, `test_coarse.cpp`).
  - A detachment search that leaves the resident chunks continues on per-chunk summaries of
    the others: exact components, their anchorage, and face labels.
  - Summaries are made on demand from the source chunk plus its archived edits.
  - A piece that detaches into non-resident chunks makes them resident and goes whole.
  - Randomized check against a fine search of the whole world: identical in every trial. The
    old resident-only search was wrong in 31 of 73 trials with pieces.
  - Engine check: a bridge whose piers are both evicted stands when cut in the middle. Before,
    one half fell. A cantilever cut at its root goes whole, including its evicted tip.
  - **Bug found by the engine check.** Background verification read non-resident chunks as
    air: a structure hanging from an evicted support looked like a floating piece, failed, and
    was detached. Snapshots now leave those chunks out.
- **Mechanics across the streaming boundary: hydrated verification.** A verification whose
  structures reach evicted chunks regenerates them in its own snapshot (source chunk plus
  archived edits), designs them as a bake would, and checks the structure whole. Only resident
  cells take its result; a failure streams the chunks in and continues dynamically.
  - Test: a steel beam cantilevers 18 m from a wall in evicted chunks onto a resident prop.
    With the prop taken away, the hydrated check fails the evicted root and the beam comes
    down; a window-only check leaves it standing.
  - This takes the place of the plan's resident mechanical L3 model: the check regenerates
    what it needs.

## Phase 7: extensions

| Extension | Implementation | Tests |
|---|---|---|
| Rigid debris with world collision | `engine/debris.cpp`, `debris` protocol message | `test_debris.cpp`, smoke |
| Unilateral cracked contacts | bonds failing closed (shear, crushing) crack into per-component contacts; bonds failing open break; separation on gap, slip or rocking | `test_contact.cpp`, 10-minute replay |
| Poisson / plate capacity factors | plate classification (`build_lattice`; windows classify from the world grid, `world/region.cpp`) | `plates: …` |
| GPU visual interpolation, VFX | displacement fields; particles, dust, cracks | smoke, `svx_engine_demo --gpu-displacement` |
| Networking (deterministic) | command logs `engine/replay.hpp`, `svx_replay` | `test_replay.cpp`, 10-minute gate |
| Sector movers | `engine/movers.cpp` (plane programs), `doom/specials.cpp` (every vanilla plane special: doors, lifts, floors, platforms, ceilings, crushers, stairs, donut), `doom/movers.cpp` (use, walk-over and gunshot lines) | `test_movers.cpp`, `svx_map_check --movers`, smoke |

## Destruction play-test: structure-spanning bubbles (2026-09-27)

The visual play-test found local destruction plausible but no global collapse: the procedural
tower, left on its north wall and 3 of its 16 ground columns, never fell. The window's pinned
rim was a phantom support at the scale of a building. Rocket bubbles now span the structure
they hit (PLAN §G, structure-spanning bubbles): later rockets on it merge into the running
bubble, nomination orders by the composite's own estimate, pieces standing only on cracks fall
as debris, and heavy bubbles keep pace by longer steps.

| Scenario | Unbudgeted (each step commits the next tick) | Default budget |
|---|---|---|
| Small tower (5 storeys) left on a corner column, 8 rockets 0.3 s apart | falls 2.6 s after the last rocket | falls 22.6 s after it with one-tick steps, ~20 s with k ≤ 2, **~7 s with k ≤ 8 (default)** |
| Tower (10 storeys) left on its north wall and 3 columns, 13 rockets | falls ~8 s after the last rocket (it stood 20 s before the nomination order was fixed) | stood 90 s (k = 1) and 60 s (k ≤ 4, 8): its bubble steps at ~1/10 of the tick rate, and the rockets merge within its first half second |
| Tower left on its north-east column, 15 rockets | — | falls ~5 s after the last rocket (native) |

- **Costs.** A structure bubble's setup extracts the structure off the simulation thread: the
  tower (203k cells → 4.5k nodes, 1k fine) ~150 ms of ~0.4 s CPU; the rooms (99k cells → 6.8k
  nodes) 1.07M work units, so its first step commits 30 ticks after the rocket (a window's: 4).
  A step of the ~10k-node tower bubble counts ~0.5M units (one Newton iteration).
- **Browser (WASM, threads).** The tower loads and plays at 32–60 fps while its bubble runs.
  The displacement field had covered the whole tower at every tick and stalled the page until
  it was killed; fields are now rebuilt per commit and sampled coarser when large (PLAN §G).
  Rockets fired on wall-clock timers land at other ticks than the native test's: the corner
  scenario then stood for 60 s with 18k ruptures and 3.4k voxels detached. The smoke test passes.
- **Tests.** The small tower is the regression test (default budget, 15 s); the 10-storey
  tower is an opt-in physics check (`SVX_T_BIG=1`, ~5 min); the verification tests check both
  windows and spanning bubbles; a merge test checks that a second rocket's crater shows at once
  and the session is identical on 1 and 4 threads. Native and WASM: 69 of 69.

## Open items

1. **A bubble's first step on one WASM thread:** 15–20 ms at ~3k nodes against the 12 ms gate
   (the steady step meets its gate). In the threaded web build the setup, including the first
   step, runs off the simulation thread under counted work.
2. **10 rockets/s on MAP01, p99:** 14–23 ms native and 24–28 ms on WASM (p95 is within the
   budget). The rocket event ticks (window extraction, mutation, triage; 6–15 ms on WASM) stay
   on the simulation thread; they slow simulated time, not the rendering.
3. **User sign-off on the feel.**
4. **Verification under sustained fire on one structure.** A job starts once its own window is
   quiet, whatever happens elsewhere. But its result is dropped if its structure changed while it
   ran (at least 2 s), and queued again. On one building under continuous fire (the 10-minute
   rooms session) jobs therefore complete only once the shooting pauses. Applying results of a
   partly changed structure needs a partial-staleness rule that is not built.
5. **Nomination cost.** Its bound (baseline plus the rigid Δ-jump times 2 / 4 / 8) is
   conservative by design; at rocket craters it refines the interface ring although the fine law
   then breaks nothing there. Every cheaper variant tried lost failures on the truth libraries:
   the local decide stage (0 of 14), the finer side's concentration factor (a collapse at the
   threshold), a higher threshold (1.5: the column-removal collapse).

6. **Pace of structure-wide collapses.** A structure-spanning bubble of ~10k nodes spans 8–30
   ticks of counted work per step. Longer steps (k ≤ 8) keep the pace, but marginal outcomes
   depend on the pace: the 10-storey tower's one-row case falls unbudgeted and stands on the
   default budget. Faster steps (fewer PCG iterations, larger teams) are the next lever.
7. **Verification under merges.** A structure under continuous fire keeps its bubble alive,
   so its verification waits until the bubble sleeps (the 10-minute rooms record: no
   verification in its first 500 s).

All plan details are now built: Anderson acceleration (§B2), coarsening quiet regions (§B5),
nominate / decide (§B6.1–2, decide off by evidence), the marginal-bond re-solve (§B6.3), and the
mechanics of evicted chunks (§B4/§B8) as hydrated verification instead of a resident L3 model.
