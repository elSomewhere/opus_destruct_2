# Plan — New scalable structural-destruction engine (C++20 → WASM core, TS/WebGPU front end)

> **Revision log**
> - 2026-09-25: approved.
> - 2026-09-25: Phase 0 complete ([`phase0/PHASE0_REPORT.md`](phase0/PHASE0_REPORT.md)).
>   Changes:
>   - the multigrid default is the W-cycle;
>   - the structure-size gate is replaced by anchor-distance locality;
>   - v1 triage: small detached pieces are not bubble triggers;
>   - bake checks: a structural design pass (slab thickness by span, RC slabs; E1M1
>     reaches 15.9× nominal under self-weight otherwise), self-weight vs game-mode onset,
>     buckling outliers, floating pieces.


## Context

**Goal (user).** Build a Doom-like FPS in which a voxelized Doom level is fully destructible
and has real structural integrity. Overloaded parts crack, sag and collapse. Detached parts
simply vanish in v1. The engine is rewritten from scratch as a C++ core running in the
browser via WebAssembly, with a JS front end. It must scale to large streaming worlds with
many simultaneous impacts, using event-driven local "physics bubbles" that coarsen with
distance. It must be tunable and must not lose the prototype's fidelity.

**Decisions taken with the user:**
- Match the current XPBD look: soft, visibly bending, dynamically collapsing structures.
- Voxel design point: 12.5 cm, i.e. 4 Doom map units.

**Why a rewrite, not a port.** Four parallel investigations went into this: physics
contract, scaling bottlenecks, branch history, and literature/industry. The findings are in
§A. The scaling limits come from the method, not from JavaScript:
- **Structural integrity comes from under-converged XPBD dynamics.**
  - About 96% of the strain driving damage is solver error.
  - Rest sag is 2–25× the converged value.
  - Every hit wakes 1.5k–10k cells for seconds, at 17–27 µs per awake cell per frame.
- **Memory:** 13–25 KB per voxel, capped at 50k resident cells.
- **Scans:** full O(resident) passes remain.
- **Bubbles:** they have rigid frontiers or one-way proxies, so there are phantom supports
  and no two-way coarse/fine coupling.
- **Earlier implicit solver:** its Newton iteration diverged at limit points under softening.

**Evidence and review.** A methodology spike in this session (3D lattice in Python plus a
C++→WASM kernel benchmark; §A5) and an independent red-team review shaped the design below.

**Intended outcome.**
- A converged, resolution-consistent lattice solver.
- Telescoping multi-resolution bubbles whose cost per event is independent of world size.
- About 2–4 B per resident static voxel.
- The XPBD look reproduced with explicit knobs rather than solver artifacts.
- A playable, destructible Freedoom map.

---

## A. Investigation findings (evidence behind the design)

### A1. The model to keep: this is what "fidelity" means
- **Model:** a rigid-body–spring lattice (RBSM).
  - Each voxel is a 6-DOF rigid cell.
  - Face-neighbour bonds carry 6 springs [N, V1, V2, T, M1, M2] at the face centroid.
  - Stiffness comes from half-cell Timoshenko sections in series. The oracle matches
    Timoshenko to 0.14%.
  - Material-pair profiles (SOLID, MONOLITHIC, WEAK_JOINT, BRACE) scale the stiffness.
  - `effDims` gives sub-voxel member sections.
  - Corotational kinematics (`src/element/interface.js:618-758`).
- **Damage law** (`src/constitutive.js:841-1040`):
  - An isotropic scalar per bond, with per-component box onset (δ_on = cap/k·κ_on; κ_on=5
    for SOLID) and L2 progress after onset.
  - d and κ only increase. Secant stiffness is s(d)=r+(1−r)(1−d).
  - A bond ruptures at d≥0.999 or κ≥κ_br. Break displacement = max(ratio·δ_on, 2G_f·A/cap).
  - Plasticity is off in every profile.
- **Defects:**
  - Y-edge bending axes are swapped: Y-spanning slabs are 15.75× too stiff out of plane.
  - Torsion uses the polar moment instead of the St-Venant J.
  - Fracture work is inconsistent with cell size. At 12.5 cm the ratio term dominates and
    dissipates about 19× G_f.
- **The oracle vs the game core:** the implicit f64 oracle is the faithful model, and the
  XPBD game core is 2–25× softer than it.
- **Golden numbers:** building3 self-weight Δz = −1.5054e-4 m; clampedCantilever Δz =
  −0.048084 with demand 0.961; plasticBeam → 27 ruptures; the `rc_floor` calibration table.

### A2. Why the prototype does not scale (measured on this machine)
- **Memory:** 13–25 KB per voxel.
  - A trial pool costs 2,120 B per edge and is never read by XPBD.
  - About 8.6 KB of JS heap per cell.
  - The fixed arena overflows before the LRU evicts.
- **XPBD cost:** 17–27 µs per awake cell per frame on CPU, 6 µs on WebGPU. A frame fits
  about 1k awake cells on CPU and about 2.5k on GPU.
- **Per event:** an r=2 blast wakes about (5r+4)³ cells for about 150 ticks, i.e. seconds of
  CPU.
- **O(resident) passes:** awake-list refresh (7.5 ms at 50k), island reflood (6.7 ms),
  rebind (181 ms), eviction (about 40 ms per chunk), and a render re-scan on every motion
  frame.
- **Coarse graph:** no mass, stiffness or strength.
  - Proxy stiffness ≈ 0.72^hops, independent of the number of cut bonds.
  - "Bubble-by-bubble" propagation actually escalates to the whole component.
- **f32 GPU:** plans never reach sleep. Sleep needs about 10 interacting heuristics.

### A3. Lessons from prior branches (do not re-learn)
- Architecture beats kernels: in blasts, the kernels took about 300 ms of 14 s.
- One fracture authority. The coarse graph deciding collapse was reverted in v28.10.
- Rigid frontiers are phantom supports (astra_0 audit F05).
- Price capacity by matter, not by space.
- Order everything by stable ids.
- The host owns the clock.
- Use an offline equilibrium bake instead of a gravity ramp.
- Reduced models fail against an integrator whose behaviour depends on iteration count
  (Craig–Bampton).
- Keep separate: storage chunk, structural component, simulation island, render chunk.

### A4. Industry and literature
- **Shipped games** either skip mechanics (Teardown, Valheim, 7 Days to Die, Space Engineers
  heuristics) or solve equilibrium on a coarse rigid-piece graph:
  - NVIDIA Blast: warm-started CGNR, iterations capped per frame.
  - THE FINALS (GDC 2024): sparse and incremental Cholesky, breaking against a rest-state
    "baseline".
  - None solves fine-voxel elasticity at world scale.
- **Closest academic analogues:**
  - FAC / MLAT composite grids.
  - Dick et al.: split coarse cells by connected component on every level.
  - Eliáš's adaptive discrete model: refine at 70% of strength; 8–20× faster.
  - Sequentially linear analysis (Rots).
  - Nukala: rank updates per broken bond.
  - Liu et al. (SPGrid): mixed precision; f32 MGPCG fails beyond 10⁸ voxels with high
    stiffness contrast.
- **Platform (2026):**
  - WebAssembly threads and SIMD128 run everywhere.
  - Avoid memory64: 10–100% slower, and only behind a flag in Safari.
  - Relaxed SIMD is not in Safari.
  - WGSL has no f64.
  - `mapAsync` readback takes 1–15 ms.
  - WebGPU is on by default in Chrome and Safari 26, and in Firefox 141+ on Windows and
    Apple-Silicon macOS.
- **Doom scale:** about 32 map units per metre.
  - The median Freedoom map is about 120×108×20 m.
  - A 1 m shell at 12.5 cm is about 12M solid voxels (median) and 29M (largest). A whole map
    fits in memory.

### A5. Methodology spike (this session; scripts in the scratchpad `mlexp/` and `wasmbench/`, moved into the repo in Phase 0)
Setup: a 3D RBSM lattice. The reference is a full fine solve.

| Scenario and approach | Solve size | Near-field peak stress (fraction of true) | Far load error |
|---|---|---|---|
| Column removal, fixed bubble, kinematic boundary (prototype), R=8 | 416 cells | 0.29–0.44 | 19% (no redistribution) |
| Same, R=16 (prototype default) | 2,319 cells | 0.55–0.70 | 16% |
| Same, R=24 (≈60% of the building) | 5,268 cells | 0.98 | 2% |
| Column removal, **telescoping composite** R0=8, g=2 | **1,488 nodes** | **0.83–0.86** | **5.7%** |
| Same, g=3 | 2,113 nodes | 0.89–0.91 | 4.8% |
| Same, pure Galerkin (no series scaling) | 1,488 nodes | 0.41–0.62 | 21% |
| Doom-like rooms (51k cells), rocket crater, composite R0=12 | 3–4k nodes | 0.97–1.05 | – |

**Scaling and coarse model:**
- As the structure grows 7.3k → 68k cells, composite nodes go 1,488 → 2,160. The full
  direct solve goes 1 s → 136 s.
- The coarse model alone is within ±2% for beams and columns at 8× coarsening.
- Thin plates reach 0.93 / 0.85 / 0.89 of fine at L1 / L2 / L3 after the torsion fix
  (0.77 / 0.51 before it).

**Multigrid and dynamics:**
- Scaled rigid-aggregation MG-PCG: 44 → 51 iterations (to 1e-8) for 44k → 236k DOF.
- Implicit dynamics: 5–14 MG-PCG iterations per step on average. The first steps after an
  event take 20–35.
- Dynamic "baseline + Δ" composite: peak-over-time 0.83–0.88 in the column-removal worst
  case. This is the same bias as the static case, and stable.

**Limits and throughput:**
- Beam-theory stress from coarse face resultants underestimates the true peak inside
  aggregates by up to 4× (L≤2) and 8× (L3).
- WASM kernel (matrix-free 6-DOF operator, SIMD128 + pthreads, M5 Pro):
  - f32: 5–8.5 ns per cell per matvec on one thread, about the same as native NEON.
  - f64: 11–17 ns.
  - 8 threads: about 550 M cells/s.

**Takeaways:**
- Pinned bubbles are mechanically wrong.
- The composite is accurate for local events at a few thousand nodes.
- Events that change the load path carry a 10–17% unsafe bias. That is why §B6 makes the
  fine level the decider.

---

## B. Recommended approach

### B1. Fine-level model (ported, with fixes; two law modes)
- **Cell** = voxel of pitch h (default 0.125 m).
  - State: x, q, v, ω.
  - Mass and inertia from `effDims` (`oriented_interface_core_base.js:189-206`).
- **Bonds:**
  - Implicit: any face-adjacent solid pair is a bond. There are no bond records.
  - Store a broken-bit per bond.
  - Damaged bonds go in a sparse overlay {d, κ, plastic6}.
  - Stiffness, capacity and threshold tables are indexed by (matA, matB, axis).
- **Kinematics:** corotational at the fine level and L1. The far field (L≥2) stays linear.
- **Reference mode:** a bit-for-bit port of `computeInterfaceResponseInto` /
  `equivalentDemand`, plus the Y-edge and St-Venant J fixes (each behind a flag). This mode
  exists for parity and regression.
- **Game mode:** a cell-size-consistent law.
  - Fracture energy is regularized. Peak strength is ≤ √(2G_f·E/h), so κ_on=5 cannot
    survive.
  - The RC reserve is expressed as ductility (a residual plateau through the plastic path),
    not as extra strength.
  - The fragility scale F is set per profile.

### B2. One solver, two modes, one exact baseline
Every solve is a linear SPD system with a matrix-free operator.

- **Baseline, independent of the partition.**
  - For each anchored structure, run a full-fine static equilibrium u₀. It is computed at
    bake for authored levels, or lazily in the background with multigrid (0.2–2 s on 8
    threads for 10⁵–10⁶ cells) and cached in OPFS.
  - Store it as f32 displacements, 24 B per structural voxel, kept in memory only for
    touched structures. f16 would lose about 5% of the strain.
  - Authored geometry is its own equilibrium. Bubbles solve only for the event Δ, driven by
    the exact event residual r = f − K_post·u₀, which is local.
  - Total bond forces = baseline forces + Δ forces.
  - The baseline prestress rotates with the corotational bond frames.
  - This is THE FINALS' "baseline" done exactly. There is no gravity ramp, no grace period,
    and no pops when bubbles re-partition.
- **Dynamic mode** (visible bubbles):
  - BDF2 (or generalized-α with ρ∞≈0.5) with a fixed tick. Restart with backward Euler after
    ruptures or re-partitions. Backward Euler alone under-shoots overshoot at 1.6–1.7× vs 2×.
  - Linearly implicit: solve (M/h²+K_t)Δx=rhs once per step, with Rayleigh damping folded
    into the operator.
  - Damage is frozen within a step and updated after it.
  - Rupture is capped per step, with carry-over. Ordering follows the oracle: score =
    max(d, margin, φ) descending, then d descending, then id ascending
    (`oriented_interface_core_base.js:1717-1730`).
  - Geometric stiffness is dropped. The system stays SPD, but there is no buckling in v1.
    Negative curvature in CG (pᵀAp ≤ 0) is treated as an instability event.
- **Static mode:**
  - A non-accumulating secant (Picard) fixed point at full load, with Anderson acceleration.
    Rupture happens only at convergence, with the oracle's cap and order.
  - Used for the settle projection (exact rest state), for unseen or remote events, and for
    the bake.
  - Dynamic effects in static mode:
    - either a dynamic increase factor (DIF), calibrated against the dynamic mode per
      structure class and applied at each cascade rupture;
    - or Izzuddin's pseudo-static energy criterion on the same solver.
- **Sleep:** when kinetic energy and the static residual are low, run the settle projection.
  If that produces new damage, wake again; since damage is monotone, this can only repeat a
  finite number of times. Blend visuals over a few frames.

### B3. The XPBD look through explicit, resolution-independent knobs
- **Physical compliance S_p ≈ 4–9.** This slows dynamics by √S_p (2–3×, the "jelly" feel).
  - It is capped by a bake-time buckling check: S_p·P/P_cr ≤ 0.3.
  - With S=25, slender masonry walls would buckle under their own weight, which is why this
    is not a single knob.
- **Render amplification A on Δu.** Visible sag = S_p·A. A is also applied to player
  collision, so the player never clips through visibly sagging geometry.
- **The law sees S_p-normalized strains.** S_p then affects only the look and the overshoot.
- **Fragility F** (a per-profile strength scale): the prototype's tendency to snap things.
- **Damping:** Rayleigh αM+βK.
- **Feel spec.** The calibration target is a numeric "feel spec" derived from prototype
  captures with the app's settings (damping 0.988/1.0, 12×1; `src/app.js:272-285`): sag on
  an 8 m span, sway of a thin wall, time from event to collapse, fail/stand per scene.
  Matching raw captures would mean matching solver noise.
- **Known difference:** prototype softness grows with structure size. Here it is uniform,
  so large structures look stiffer than today.

### B4. Multi-resolution lattice hierarchy (one structure, three uses)
- **Levels:** level ℓ+1 nodes are the connected components (via intact bonds) of level-ℓ
  nodes within each 2³ block. Anchored cells are Dirichlet.
- **Storage:**
  - L1–L2 are built on demand in bubbles (sums over ≤512 cells per brick).
  - L3+ (8³ bricks and up) persist: mass properties, anchor flag, 3 face couplings with
    section properties, fibre lengths, baseline utilization and resultants. That is about
    105 floats per brick node, ≈0.9 B per voxel.
  - Coarse operators are never cached per brick world-wide.
- **Uses:**
  - Exact hierarchical connectivity and anchorage.
  - Multigrid coarse spaces.
  - Physics LOD.
- **Coarse physics:** Galerkin projection of the fine bonds onto the aggregates' rigid
  motions.
  - Crossing bonds are scaled by 2/(nᵢ+nⱼ), where n is the fibre length. This is exact
    RBSM at the coarse pitch for prismatic members.
  - A St-Venant torsion correction applies per coarse face.
  - Galerkin mass and inertia are exact.
  - Aggregation is gated on solidity. Shells with openings or lintels do not become rigid
    blobs: they stay finer, or use energy-minimizing bases (a Phase 2 variant).
  - Aggregation never crosses WEAK_JOINT at L1.
- **Coarse levels only nominate; they never decide failure.**

### B5. Telescoping event bubbles and scheduling (the scaling mechanism)
- **Composite partition per job:**
  - Fine cells within R0 of the seeds. R0 is sized in structural units:
    max(2 × event radius, member depth + distance to the nearest joint, 16 voxels).
  - Level-ℓ aggregates at distance ≥ R0·g^(ℓ−1), with g = 2–3.
  - The persistent coarse model for the rest of the anchored structure.
- **Composite operator:** the scaled Galerkin projection. It is SPD and has no phantom
  supports.
  - MG-PCG uses smoothed-aggregation (rigid near-nullspace) coarse spaces, with the scaled
    rigid variant as a benchmark.
  - Mixed precision: f32 storage, f64 accumulation.
  - PCG stops on a CG energy-norm error estimate (about 1e-3 in demand) with a load-scaled
    tolerance. Solving to 1e-8 would be over-solving.
- **Re-partition:**
  - **Coarsen** only quiet regions with hysteresis, using a mass-weighted projection: exact
    linear and angular momentum, and internal energy is discarded. Reuse
    `src/dynamics/rigid_aggregate_math.js`.
  - **Refine** with rigid prolongation, then a harmonic (static) relaxation of the interior
    with the boundary fixed. Momentum is preserved and there is no energy gain.
  - After a re-partition, reset the BDF history.
- **Throughput.** Active bubbles = event rate × bubble lifetime.
  - **Severity triage per event:**
    - Carve, then check connectivity.
    - Compare the baseline force released at the removed bonds with the neighbours'
      capacity.
    - If the event is low severity, run a static settle only. Otherwise spawn a dynamic
      bubble.
    - Target: ≥80% of bullet-class events spawn no bubble.
  - **Merging:** merge events that are close in space or time. Stagger first steps across
    frames (1–2 frames of latency is fine).
  - **Parallelism across bubbles.** Small bubbles (< ~10k nodes) run one per thread, because a
    WASM barrier costs µs and a V-cycle needs about 40 of them. Large bubbles use a thread
    team.
  - **Cost:** a ~3k-node bubble takes about 3–4 ms per steady step on one thread, and about
    10 ms for its first steps.
- **Budget manager (the "tweakable" system).**
  - Decisions come from deterministic work estimates (node counts, iteration caps), never
    from wall-clock time.
  - Degrade order:
    1. larger g;
    2. smaller R0;
    3. low-severity bubbles go static;
    4. far bubbles update at a lower rate;
    5. finally, slow simulated time. Never drop work.

### B6. Error control: the coarse level nominates, the fine level decides
1. **Nominate.** Each aggregate stores its baseline utilization and face resultants.
   - Flag it when the conservative bound u_base + SCF_max·Δu(ΔR) exceeds 0.5×onset.
   - Also flag it when the load-path signal fires: large ΔR on faces far from the event.
2. **Decide: local oversampled reconstruction.**
   - Take the aggregate plus a one-ring of neighbours, with composite motions as Dirichlet
     conditions, and do a fine Δ solve (about 10⁴ DOF, milliseconds).
   - Failure is decided only on fine bonds that are not within one aggregate of a
     fine/coarse interface.
3. **Near field.**
   - A demand inflation factor, calibrated offline with CRE or adjoint error bounds on the
     spike library, corrects the stiff-far-field bias.
   - Marginal bonds (±ε) re-solve with the fine region grown by one ring.
4. **Background verification (mandatory).**
   - A low-priority full-fine MG static and dynamic check of every structure touched by an
     event.
   - Eventual consistency: a collapse delayed by at most 2 s reads as "creak, then
     collapse".
   - This is the safety net that makes missed far-field failures impossible rather than
     merely unlikely.

### B7. Failure, cascades, connectivity, detachment
- **Connectivity** uses hierarchical labels on the same hierarchy.
  - After ruptures or carves, recompute brick components.
  - Update the coarse adjacency.
  - Anchor reachability uses a lockstep two-sided search (Even–Shiloach) at the smallest
    enclosing level, descending only at frontier bricks.
  - Blast-style fast-route pointers handle the common case.
  - Nothing is O(resident).
- **Detached islands** are removed (v1) and emitted as `DetachedIsland{voxels, mesh, v, ω}`
  for a ballistic, collision-free fade with dust.
  - To avoid spring-back, the load the island exerted ramps out over its predicted fall time.
  - **Virtual impact.** A ray-cast landing point receives an equivalent impact load (mass ×
    impact speed). Pancake-style progressive collapse still happens without simulating
    debris.
- **Deferred:**
  - Rigid debris (DebrisPool-style aggregates, about 100 B per member).
  - Ruptured bonds as unilateral contacts (compression + Coulomb, via an active set) so
    that a crack is not a detachment.

### B8. World representation, streaming, persistence
- **Layout:** 8³ bricks, grouped into 32³ chunks.
  - One material byte per voxel, with uniform-brick and palette compression.
  - Sparse per-brick overlays: broken bits, damage, and rest offsets (int16).
- **Memory:**
  - Static: ≤ 2–4 B per resident voxel (≈1 B material + ≈0.9 B hierarchy + brick map).
  - Baselines: 24 B per structural voxel, only for touched structures.
  - Active nodes: ≤ 400 B each.
  - A median Freedoom map is about 50 MB of static data, fully resident.
- **Persistence:** base (level or generator) + binary coordinate-keyed deltas in OPFS.
  - A clean chunk costs nothing.
  - Only event regions ever dirty a chunk.
- **Streaming** (large worlds):
  - Byte-budget eviction.
  - L3+ stays resident for evicted chunks, so load paths cross boundaries.
  - Remote events hydrate fine bricks on demand.
  - Render residency is decoupled from physics residency.
- **Procedural worlds:** port the `src/worldgen/*` passes.

### B9. Runtime architecture and determinism
- **Core:** C++20, no exceptions or RTTI, SoA, stable ids, fixed arenas with typed errors.
- **Build:**
  - Emscripten `-O3 -msimd128 -pthread -sPROXY_TO_PTHREAD -sMALLOC=mimalloc`.
  - Fixed wasm32 heap of ≤ 2 GB; no memory64, no memory growth.
  - The core runs in a worker with a thread pool.
  - The boundary is SharedArrayBuffer command/event rings plus shared mesh and displacement
    buffers with generation counters.
  - COOP/COEP headers are required (as `tools/serve.mjs` does).
- **Native build** of the same code for tests, tools and benchmarks.
- **Determinism rules** (fixed from day 1):
  - Fixed tick and a fixed number of steps per tick.
  - Epochs: bubbles read the committed state, step, then commit in sorted bubble-id order.
    Coupling between bubbles lags by one epoch.
  - Deterministic merges.
  - Partitions and reductions independent of the thread count.
  - No fast-math, no relaxed SIMD in solver paths; `-ffp-contract=off` for native builds.
  - A bundled libm for sin, cos and atan2 in both builds.
  - CI checks replay hashes across native/WASM and 1/N threads.
  - Multiplayer caveat: visibility-dependent mode switching must be avoided, or destruction
    must be server-authoritative.
- **Front end:** TypeScript + Vite + WebGPU.
  - Greedy chunk meshes from the core.
  - Deformation through smooth, skinning-like interpolation of node displacements in the
    vertex shader, so there are no rigid-aggregate stair-steps. Uploads happen for active
    chunks only.
  - Crack remesh on rupture.
  - Doom texturing and lighting.
  - Debug overlays: utilization, bubble levels, job and budget timeline.
- **GPU compute solver:** not authoritative. At most, later, for visual interpolation, VFX,
  and non-authoritative verification of huge structures.

### B10. Doom pipeline
1. Parse the WAD and rasterize sectors at 4 map units per voxel.
2. The voxelizer guarantees **6-connectivity**: diagonal walls are thickened so voxels share
   faces, otherwise they have no bonds.
3. Extrude:
   - Floors sit on anchored bedrock.
   - Lateral void becomes a solid shell of T≈1 m, then anchored rock. Thin gaps between rooms
     therefore become free-standing walls.
   - Ceilings are slabs with air above; sky sectors are open.
4. Map materials from texture names. Doors and lifts are static in v1.
5. Bake:
   - Full-fine baselines.
   - Buckling check to choose S_p.
   - Auto-strengthen members with self-weight utilization > 0.5, and report them.
   - Build L3+ and the chunked level file.
6. Content: Freedoom (BSD).

### B11. Tunables (runtime-adjustable)
| Knob | Default | Controls |
|---|---|---|
| Physical compliance S_p / render amplification A | calibrated (4–9 / 2–5) | jelly feel / visible sag |
| Fragility F per profile | calibrated | how easily things snap |
| Damping α, β | calibrated | wobble and settle time |
| R0, grading g, max level | structural sizing / 2–3 / global | accuracy vs cost |
| Nominate threshold / marginal ε / inflation | 0.5×onset / 0.05 / calibrated | far-field recall vs work |
| Rupture cap per step | 512 | cascade pace |
| Node and work budget, per-thread bubble size | 200k / ~10k | graceful degradation |
| Verification delay target | ≤ 2 s | eventual-consistency latency |

### B12. Alternatives considered
| Alternative | Verdict |
|---|---|
| Port XPBD to C++ (optionally with an MGPBD global AMG solve) | No. Its look depends on the iteration count, so levels can never agree. Converged, it is just implicit Euler in the dual space (about 18 λ per cell vs 6 DOF, plus self-stress null spaces AMG doesn't target), and it would still need the S/F knobs. Keep XPBD-style contact for v2 debris only. |
| Pinned or proxy fixed-radius bubbles (today) | No. Phantom supports: 0.29–0.70× the true stress at R≤16. |
| Coarse graph or heuristic load paths | No. Not mechanics; already reverted (v28.10). |
| Sparse Cholesky (THE FINALS) or a lagged-Cholesky preconditioner | Benchmark for bubbles ≤ 30k DOF and static waves (robust near mechanisms). Not at world scale. |
| Cached Schur complements / H-matrices / FETI | No. Dense, and invalidated by damage. The per-aggregate local version is the §B6 reconstruction. |
| GPU-authoritative solver | No. No f64, readback latency, not bitwise across vendors, and the GPU is busy rendering. |

---

## C. Phased roadmap (riskiest assumptions first; every phase has quantitative gates)

**Phase 0 — Foundations, golden data, and the Doom content/regime study (go/no-go)**
- **Repo and tooling:**
  - New sibling repo (working name `structvox/`; the prototype stays untouched as the
    oracle).
  - CMake presets for native and wasm. doctest.
  - CI runs native tests and WASM tests under Node.
  - Perf harness. Move the spike scripts into `research/`.
- **Prototype fixture exporter (Node):**
  - Oracle static and outcome fixtures, with the fixes behind flags, for the 15
    `ScenarioRegistry` scenes (asymDiaphragm, braceCorner, building3, cantilever,
    cascadingCollapse, clampedCantilever, contactGuidedStack, contactSmokeStack,
    continuationStiffStrip, multiSpan, plasticBeam, seamCompare, simplySupported,
    supportCompare, wallLintel).
  - The 4 `tests/xpbd_fem_outcome_oracle.mjs` scenes and analytic beams and plates.
  - XPBD captures for the feel spec.
- **Content study:**
  - Minimal geometry-only WAD voxelizer (6-connected shells, 12.5 cm).
  - Native full-fine reference statics (simple MG).
  - Run on 3 Freedoom maps.
  - Measure: anchored-structure sizes, utilization histograms, buckling ratios, openings,
    share of diagonal walls. Sample 500 events per map.
- **Go:**
  - 99th-percentile anchored structure ≤ 10⁶ cells. Otherwise verification needs its own
    hierarchy.
  - ≥ 80% of bullet-class events need no bubble.
  - An S_p exists with ≤ 1% of members over S_p·P/P_cr > 0.3.
  - WASM kernel ≈ 5–8 ns per cell per matvec.

**Phase 1 — Reference core and semantics (the truth model)**
- **Model code:**
  - Brick storage.
  - Material, archetype and profile tables (port `src/archetypes.js`,
    `src/calibration/continuum_to_voxel.js`, `src/compiler.js`).
  - The reference and game law modes.
  - Corotational kinematics; mass and inertia.
- **Solvers:**
  - Picard static mode.
  - Full-fine BDF2 dynamic mode.
  - Hierarchical connectivity; detach → vanish with the virtual impact load.
- **Gates:**
  - On scenes where the oracle converged: collapse classification matches on ≥ 95% of
    scenes, and the alive-set symmetric difference is ≤ 2%.
  - Static Δz and demand ≤ 1% on intact scenes. Analytic beams ≤ 1%.
  - Fracture energy per area invariant across h within ±5% (game law).
  - With linear kinematics, collapse classification is identical for S ∈ {1, 4, 16}.
  - Calibrated static+DIF agrees with dynamics on ≥ 90% of classifications.
  - Connectivity updates are O(changed), checked with counters.

**Phase 2 — Composite dynamic spike including WASM performance (go/no-go)**
- **Build:**
  - Composite partitions: scaled rigid vs SA vs energy-minimizing bases for shells.
  - Baseline+Δ; re-partition; nominate/decide; background verification.
- **Benchmarks:**
  - Sparse or lagged Cholesky for small bubbles.
  - The scenario library plus ≥ 50 events from real map excerpts, against full-fine dynamic
    truth.
- **Gates:**
  - Collapse classification: ≥ 90% immediately, and ≥ 98% after verification with ≤ 2 s
    delay.
  - Zero missed member-level failures after verification.
  - Near-field peak ratio within [0.95, 1.15] after inflation.
  - At re-partition: momentum relative error ≤ 1e-9, and no kinetic-energy gain.
  - A ~3k-node bubble: steady step ≤ 4 ms and first step ≤ 12 ms on one WASM thread.
  - PCG p95: ≤ 15 iterations steady, ≤ 35 at event start.
  - Composite node count flat (±25%) from 10⁴ to 10⁶ cells.
- **No-go fallback:** larger fine regions; a static core with render amplification;
  background verification.

**Phase 3 — Engine core at scale**
- Persistent L3+ and baselines (bake and lazy), in OPFS.
- Scheduler with epochs, merging, triage, the budget manager, and parallelism across
  bubbles.
- Determinism infrastructure.
- **Gates:**
  - Bitwise-identical 10-minute replays across 1/2/4/8 threads, x86/ARM, native/WASM.
  - 20 events/s sustained on 4 threads with physics ≤ 8 ms/frame at p95.
  - ≤ 4 B per inactive voxel and ≤ 400 B per active cell.
  - Zero O(world) work on the event path, checked with counters.

**Phase 4 — Web runtime and sandbox**
- The WASM worker and SAB rings; the TypeScript API.
- WebGPU renderer: greedy meshes, smooth displacement, crack remesh, island fade and dust,
  debug overlays.
- Ported procedural city; FPS camera, hitscan and rocket.
- **Gates** (desktop, 10⁷ resident voxels):
  - 60 fps; structural work ≤ 4 ms/frame on average and ≤ 8 ms at p99.
  - A rocket shows a visible response within ≤ 2 frames.
  - Replay hash native == WASM.

**Phase 5 — Feel calibration and the Doom vertical slice**
- Fit S_p, A, F and damping to the feel spec.
- Full WAD pipeline: textures, lighting, bake with auto-strengthening.
- Player collision (AABB sweep, amplified Δu).
- Weapons mapped onto damage models: carve and local damage; rockets get blast prefracture
  plus impulse per `core.js:6132-6300`.
- **Gates:**
  - Feel-spec metrics within tolerance; the user signs off side-by-side against the
    prototype.
  - Every Freedoom map imports and bakes with no spontaneous collapse.
  - A median map holds 60 fps under sustained combat destruction (10 rockets/s) within
    budget.

**Phase 6 — Streaming and persistence for large worlds**
- 32³ chunks; binary deltas in OPFS; byte-budget eviction.
- Resident L3+ for evicted chunks; remote-event hydration.
- Far render LOD from hierarchy occupancy.
- **Gates:**
  - A 1 km² procedural city streams while flying with no hitch > 2 ms.
  - Exact persistence round-trip.
  - Remote events resolve without loading the surrounding world.

**Phase 7 — Extensions**
- Rigid debris with world collision (XPBD-style contacts).
- Unilateral cracked contacts.
- Poisson and plate capacity factors.
- GPU visual interpolation and VFX.
- Networking: server-authoritative or deterministic.
- Doors and lifts.

---

## D. Repository layout and reuse

```
structvox/
  core/      base/ (arena, jobs, simd, libm) world/ (bricks, chunks, deltas, baseline)
             mech/ (law ref+game, bonds, kinematics) solve/ (operator, hierarchy, mg, bdf2, picard)
             bubble/ (partition, nominate/decide, verify, scheduler, budget) topo/ (connectivity)
             mesh/ (greedy, displacement) api/ (C ABI, rings)
  tools/     wadvox (WAD→voxels), bake, bench, fixture-compare, feel-spec
  web/       TS + Vite: worker host, WebGPU renderer, FPS game, debug UI
  tests/     doctest native + wasm; fixtures/ (from prototype); perf budgets
  research/  Python spike (mlexp), wasm kernel bench
```

**Port or reference from `voxel_threed_discrete` (read-only oracle):**
- `src/constitutive.js`: the law.
- `src/archetypes.js` and `src/calibration/continuum_to_voxel.js`: materials, sections,
  capacities, thresholds.
- `src/compiler.js`: profile selection.
- `src/element/interface.js:618` `computeKinematics`: corotational kinematics.
- `src/dynamics/oriented_interface_core_base.js:189-206` (mass and inertia) and `:1717-1730`
  (rupture ordering).
- `src/dynamics/rigid_aggregate_math.js`: momentum-conserving aggregation.
- `src/structural/reachability_policy.js`: anchorage semantics.
- `src/core.js:6132-6300`: blast rules.
- `src/worldgen/*`: procedural passes.
- `tools/serve.mjs`: COOP/COEP server.
- **Test scenes:**
  - `tests/xpbd_fem_outcome_oracle.mjs`
  - `tests/structural_validation_suite.mjs`
  - `tests/continuum_voxel_calibration.mjs`
  - `tests/analytic_tangent_validation.mjs`
  - `src/scenarios/index.js`

## E. Verification (end to end)
1. **Unit tests:** native and WASM (Node, with threads) in CI.
2. **Golden parity** (`fixture-compare`), reference mode only, on oracle-converged scenes:
   - static Δz and demand;
   - alive and rupture sets;
   - analytic beams and plates;
   - fracture-energy invariance across h (game law).
3. **Truth harness:** full-fine dynamic and static truth vs composite on the library and
   map excerpts. Metrics:
   - classification agreement;
   - peak ratios;
   - missed failures after verification;
   - R0/g/Lmax sweeps showing no phantom-support dependence;
   - momentum and energy at re-partition.
4. **Feel-spec harness:** the prototype captures turned into metrics, compared with the new
   engine, plus side-by-side video for user sign-off.
5. **Performance and scale**, native and headless Chrome (puppeteer, as the prototype does):
   - per-step and per-frame ms, PCG p95;
   - bytes per voxel;
   - events/s sustained;
   - flat cost 10⁵ → 10⁸ voxels;
   - zero-O(world) counters.
6. **Determinism:** replay command logs and compare hashes across threads, architectures and
   native/WASM.
7. **Browser smoke** with COOP/COEP: a Freedoom map under sustained rocket fire. Watch the
   budget overlay and check the console for WebGPU validation errors.

## F. Risks and mitigations
| Risk | Mitigation |
|---|---|
| The far field lets failures pass unchecked, or near-field bias is unsafe | Fine level decides (§B6); mandatory background verification; calibrated inflation; interface exclusion; Phase 2 gates |
| Softening causes buckling (P_cr/S) | Split S_p/A; bake-time buckling cap; CG negative-curvature detection |
| Doom geometry (openings, diagonal staircases, shells) breaks the assumptions | Phase 0 content study first; 6-connected voxelizer; solidity-gated aggregation |
| Event throughput exceeds budget | Severity triage; merging; static settle for low severity; parallelism across bubbles; slow simulated time |
| Law calibration conflicts at 12.5 cm (κ_on vs G_f) | Reference mode for parity; game law with ductility plateau and F |
| Determinism erodes | Rules fixed in Phase 0/3; epochs; bundled libm; hash CI |
| The look differs from XPBD (its softness depends on size) | Feel spec plus user sign-off; knobs exposed in the UI |
| Platform (COOP/COEP, Safari gaps, readback latency) | wasm32 fixed heap; feature detection; no GPU in the authoritative loop |

## G. Revision log (implementation decisions that refine the plan)

These changes were made during implementation. Evidence is in [`STATUS.md`](STATUS.md) and
the phase reports.

- **Threading in the browser.** The web module is built with pthreads
  (`wasm-release-threads`, COOP/COEP).
  - Ticks run on one thread: a V-cycle's barriers cost more than small bubbles gain from
    threads.
  - The load-time bake uses up to 6 threads.
  - Background verification runs on its own thread, with a private three-thread team.
  - An event's bubble is set up on a background thread (below). The pool has 16 threads.
- **Asynchronous triage (§B5).** A small carve's static settle runs on a background thread
  and applies at the next tick: baseline, damage and offsets, or a bubble if the window nears
  failure. Anything that writes to cells of its window first applies it, which keeps the order
  of grid writes: a new event's window, a bubble's commit or finalize, or detached pieces
  reaching it.
  - A settle can be applied while the bubble list is being walked. Its bubble is therefore
    spawned at the next safe point of the tick. Spawning at once could finalize a bubble in the
    middle of its own commit; a 10-minute record crashed that way.
  - The settles to apply are taken out of the list before any is applied.
- **Event tick.**
  - Window extraction reads chunks through a table.
  - The lattice build walks plate runs through neighbour arrays and caches the last bond-model
    lookup.
  - A blast's pre-event internal forces are taken only for the cells its bonds can reach. The
    setup assembles the rest from its post-event pass, bit for bit, because a cell's force
    depends on its own bonds only.
  - A window's voxel → cell index is a flat open-addressing map sized for the window
    (`base/coord_map.hpp`) instead of `std::unordered_map`: on WASM the extraction went from
    8.6–10.2 to 6.2–7.6 ms. The WASM barrage measured p95 19.4 ms in one run before, 5.4–9.6 ms
    in three runs after (on a machine with other load).
- **Solve budgets.** Near a mechanism a static iteration never converges; each linear solve
  would otherwise run to its cap. Every solve therefore has a PCG budget:
  - a settle projection: 240;
  - a static triage of a carve or a debris impact: 240, else a bubble decides;
  - a verification: 600, an undecided result continues dynamically;
  - a bubble's linear solve: 60, the preconditioner-rebuild threshold; the residual carries
    over.
  Under 10 rockets/s on MAP01 this took the worst tick from 5.3 s to 0.67 s, with normal
  events bit for bit unchanged.
- **Numerical guards.** The settle projection and the dynamic Newton step now have trust
  regions:
  - The settle moves at most 0.25 cells and 0.2 rad per iteration; a dynamic Newton increment
    at most 1 cell and 0.5 rad.
  - A bubble whose residual becomes non-finite stops, and finalize drops its state. The law
    sweep ignores non-finite kinematics.
  - Why: near a mechanism the static operator is nearly singular. An unlimited settle threw
    failing windows to absurd states (1e11 m) or NaN, and their continuations ruptured the
    512-bond cap every step.
  - Effect: this inflated the 10-minute session's destruction about 5×. NaN sign bits then
    differ between x86 and ARM, which is how the replay gate found it.
- **Pipelined bubble steps (§B5).** Each tick first commits the step launched the tick before,
  then launches the next on the bubble's own thread; large bubbles get a private team. So the
  simulation thread only commits: 0.1–0.2 ms per bubble tick.
  - Meshing, displacement fields and stats read a snapshot taken at the commit: displacement,
    live cells, node count.
  - Anything that touches a bubble with a step in flight joins and commits it first: an event
    in its window, a debris landing on it, or the bubble-count limit.
  - Replays stay bit-identical across threads, architectures and WASM (the WASM build without
    threads runs each job at its commit).
- **Staggered first steps (§B5).** An event's carve or blast is applied in the event tick. Its
  bubble setup runs on a background thread and joins the stepping `spawn_latency_ticks` (3)
  later. Continuations from settles and verifications are set up the same way:
  - The setup covers the released-force residual, composite, preconditioner and first step.
  - The simulation thread does not touch a bubble still being set up. An event in its window,
    a debris landing on it, or the bubble-count limit makes it ready first.
  - The budget counts it by a deterministic estimate: a third of its window's free cells.
  - The world state is identical to setting it up within the event tick. The event tick went
    from 100–170 ms to 13–20 ms of CPU.
- **Successive right-hand sides (§B2 dynamics).** Each bubble keeps an A-orthonormal basis of
  its last 8 step solutions (Fischer's projection). The basis is valid while the assembled
  Krylov operator is unchanged, so it resets with every preconditioner or composite rebuild. PCG
  starts from the A-projection and solves the remainder to the unchanged tolerance: half the
  iterations.
- **Bubble linear tolerance stays at 0.1% of the load scale.** A looser solve while the window
  moves halves the PCG iterations with unchanged near-field peaks against the full-fine truth.
  Over two 10-minute sessions it changed the ruptures by −29% and +59%: no systematic effect.
  It stays off as the conservative choice, because monotone damage could ratchet up solver
  error near the thresholds, the prototype's failure mode (§A1).
- **Background verification (§B6)** runs at structure level, never on the event window: a
  pinned rim is a phantom support, and a test shows the window version missing a far failure.
  - **Start.** A job starts once its own window has been quiet for 1 s: no event in it, no
    bubble or settle over it. The rest of the world may be busy. Before, a job waited for 1 s
    without any event anywhere, so under sustained fire nothing was verified: the 10-minute
    rooms session had 0 verifications in 450 s. The simulation thread takes a snapshot of the
    chunks the structures can reach (1–2.5 ms): chunks are copied, and the baseline bricks are
    shared copy-on-write.
  - **Background work.** Extraction and the solve run on the job's thread. A structure reaching
    beyond the snapshot bound (`verify_max_chunks`) is verified around its window.
  - **Due tick.** The result is applied at a tick fixed by the job's size, whatever the
    threading, so replays stay bit-identical: `verify_latency_ticks` (2 s), longer above
    `verify_cells_per_tick` × 120 cells.
  - **Staleness.**
    - An event in the job's window sets it aside at once. A newer event on the same window
      supersedes it; a started job finishes in the background.
    - Any other change to its structures (voxels, damage or baselines bump the chunk version)
      drops the result at the due tick.
    - Set-aside and dropped jobs are queued again: the event may have settled statically, with
      no verification of its own. A dropped job that had found a failure goes first. A dropped
      job waits until its structure's chunks have been unchanged for 1 s before it starts again.
    - So one structure under continuous fire is verified once the shooting pauses. Applying the
      result of a partly changed structure would need a partial-staleness rule (not built).
  - **Budget.** A job has at most 600 PCG iterations. An undecided result (no equilibrium: near
    a mechanism, where the static iteration chatters) continues as a dynamic bubble, like a
    failure. Contacts need dynamics to resolve. A cached state with kinks the law reads is
    solved first, within the same budget (baseline consistency, below).
  - It complements the bubbles' own nominate / re-solve checks (below): those decide inside the
    running bubble, this one catches what no bubble reached.
- **Unilateral contacts (§B7, Phase 7)** are cracked bonds. A bond that fails **closed**
  (shear or crushing) between live cells keeps its bond with per-component contact secants.
  - The normal stiffness is compression-only. Shear and torsion follow Coulomb friction;
    bending is limited by rocking about the face edge.
  - A bond that fails **open** (tension) leaves a gap and breaks, as before.
  - A crack separates for good past a gap of h/4, a slip of h/2, or a face-edge opening of h/4
    by rocking. Without the slip and rocking criteria, sliding and tipping pieces never
    separated and dragged on their neighbours: 55k ruptures in a 3-minute session.
  - A separation counts as a failure for triage, settle and verification.
  - Cracks are stored in spare bits of the grid's broken-bond byte.
- **Render deformation.** Chunks inside a bubble move by per-bubble displacement fields on the
  GPU (rgba16f, solid-weighted trilinear) instead of being re-meshed every tick (§B9's
  "skinning-like interpolation").
- **Client-side collision.** The worker streams 1-bit chunk occupancy, and the front end
  sweeps the player locally with the engine's exact rules. A busy worker never stalls
  movement.
- **Determinism (§B9).** A bundled fdlibm-derived libm (`svx::dm`) replaces platform
  sin/cos/atan2/exp/log/pow. The 10-minute replay gate is checked with `svx_replay` on ARM,
  x86-64 (Rosetta) and WASM.
- **Bake.**
  - Source-world pieces that reach no support are removed. They made event windows singular.
  - Rock voxels at supports can be strengthened: a bond takes the weaker class of its voxels.
  - **A strength class scales the whole softening curve.** The game law caps a bond's peak at
    √(2·G_f·A·k / r_min), so its fracture energy stays exact at any cell size. A class that
    multiplied only the capacity did nothing once that cap bound, while the design pass counted
    on it: members it reported fixed were still overloaded. A class of capacity × m now also
    multiplies the fracture energy by m², so onset and break scale by m together, the cap
    included.
  - **Classes go up to × 1,024** (was × 64). The honest progressive bake (coarse-field rims)
    exposed members no smaller class can hold. MAP30's worst is a 3-voxel (37.5 cm) ceiling
    slab spanning about 104 × 81 m over a 28 m void, the plan's "ceilings are slabs with air
    above" rule applied to a huge room: at the check's fragility (0.25) the whole-world bake
    puts it at 165 × onset under its own weight. The old tile bake, which pinned tile rims at
    zero, had split such spans into tile-sized pieces and reported MAP30 at 0.57.
  - The bake baseline uses linear kinematics. Self-weight rotations are second order, and the
    increment formulation stays exact. (Checked corotationally, a linear bake leaves a
    relative residual of ~7% in rotating slabs; a corotational bake gets 3e-4, the f32 floor,
    at 3.2× the cost. Nothing reads the residual as a force, so the bake stays linear; see
    baseline consistency below.)
- **Debris (Phase 7)** are engine-side rigid bodies: sampled surface contacts and PGS impulses,
  with no transcendental functions. A heavy landing on free structure becomes an impact load:
  static triage, or delivered straight into a running bubble.
- **Movers (Phase 7).** Doom sector planes are anchored spans bonded to nothing, run by small
  programs (move to a height, go and return, cycle, stop) at vanilla speeds and waits.
  - Every vanilla plane special is mapped (`doom/specials.cpp`): doors of all kinds (normal,
    open/close only, close for 30 s, blazing, locked, gun-triggered), lifts, floors (to
    lowest/highest/next neighbour, by 24 or 512 units, donut), platforms (perpetual, raise and
    change), ceilings, crushers (with stop lines) and both stair builders.
  - Triggers: use, walk-over and gunshot lines; once-only lines fire once.
  - A closing door or a descending crusher waits or reverses on the player; a rising floor
    waits while the player would not fit.
  - All Freedoom maps are exercised by `svx_map_check --movers` (every move of every mover).
- **Feel calibration (Phase 5)** uses the XPBD captures.
  - Fitted: S_p·A ≈ 8.6, and the quiet time is best at S_p 9 with light damping.
  - Defaults are now S_p 9, A 1, F 1, ζ 0.05 in both engine and web.
  - The captures' fail/stand cannot be matched at their 1 m cells with a cell-size-consistent
    law (by design).
- **Far render tier (Phase 6)** comes from the chunk source's coarse view (1 m cells, greedy
  meshed, 32 m tiles beyond the eviction radius), not from a persistent L3+ brick hierarchy.
- **Corotated reuse of the bubble's operator (§B2).** Between preconditioner rebuilds, the
  Krylov operator is the tangent assembled at the last rebuild: a modified Newton step.
  - For a rigidly rotated piece the corotated tangent is exactly T A Tᵀ, with T = diag(R, R)
    the rotation of each node since the build. The operator, the multigrid and the projection
    basis are therefore applied in rotated coordinates: y = T A Tᵀ x and z = T MG(Tᵀ r).
  - The basis is rotated with T (v ← T′Tᵀv, same for W = AV), which keeps it A-orthonormal.
  - A rebuild for rotation is due only when the rotation *across* an intact fine bond changes
    by 0.05 rad (hinging). Before, any rotation of 0.05 rad since the build triggered one.
  - MAP01 under 10 rockets/s: 54 → 30 ms per step, 1,885 → 765 rebuilds, 38.5 → 28 PCG and
    2.29 → 1.91 Newton iterations per step.
  - **It is also more accurate.** Replays of recorded sessions against a tightly converged
    reference (fresh Jacobian every 0.005 rad, 8 Newton iterations, tolerances 1e-5):
    - The unrotated operator left Newton unconverged in rotating collapses. Monotone damage
      turned that into spurious failures: in one session, a collapse and 10,278 ruptures by
      300 s, where the reference and the rotated default have 7.
    - In a session with a real collapse, the unrotated default detached 4,816 voxels against
      the reference's ~1,750; the rotated default detached 1,903.
  - Bubbles without large rotations are bitwise unchanged (unit session, rooms benchmark).
- **Verification work scaled with the job (§B6).** A job's result is applied at a tick fixed by
  its size, and the simulation thread waits if the background solve is late.
  - The solve's budget is now at most 8M cell-iterations (between 120 and 600 PCG
    iterations), and a single linear solve may not overrun what is left.
  - Before, a 71k-cell job ran 541 iterations (4.3 s) and the simulation thread waited 4.7 s
    at its due tick.
  - A job over budget is undecided and continues dynamically, as before.
- **Near-field demand inflation (§B6.3): built, off by default.** The law on a bubble's fine
  bonds can see its demand scaled (`BubbleOptions::demand_inflation`; contacts excepted).
  - The composite's stiff far field puts near-field peaks at 0.925–0.943 of the full-fine
    truth in the column-removal study; 1.06 puts them at 0.98–1.0 (gate: [0.95, 1.15]).
  - But under sustained fire 1.06 doubles the destruction: MAP01 at 10 rockets/s went from
    7.0k to 14.9k ruptures, and steps from 28 to 72 ms. Damage is monotone and the full-fine
    verification only adds failures the bubbles missed, so a uniform factor near thresholds
    ratchets. No full-fine truth exists for the barrage to say which count is right.
  - Near a failure threshold, inflation does not fix outcomes either: the full-fine truth can
    collapse where the bubble stands (post-onset softening redistributes load that the
    composite's far field damps). The background verification decides those, as §B6 intends.
  - **Calibrated against outcomes, it stays at 1.0.** With nomination on (below), the
    column-removal library (fragility sweep across the collapse threshold, against the full-fine
    truth) matches best at 1.0: 7 of 9 at R0 12 and 8 of 9 at R0 16, missing only at the
    threshold. 1.03 and 1.06 over-predict (4–5 of 9).
- **Buckling margin of the compliance knob (§B3, §B10 step 5).** On import, each map's
  free-standing wall strips give the S_p that keeps S_p·P/P_cr ≤ 0.3 for 99% of them
  (`doom::wall_slenderness`, the Phase 0 model), and the engine caps S_p there
  (`Engine::set_compliance_cap`).
  - MAP01: 49.7, so the default 9 stands. MAP29 (tall outdoor walls): 7.5.
  - v1 drops geometric stiffness, so nothing would buckle; the cap keeps tall slender walls
    from looking implausibly soft.
  - The ratio is computed in plain arithmetic (no pow/cbrt), because the cap sets the
    simulated compliance and must be the same on every platform.
- **Byte-budget eviction (§B8).** Streaming optionally keeps the resident grid under a byte
  budget (`StreamConfig::max_resident_mb`). Every 30 ticks, chunks with storage beyond the
  load radius are evicted farthest first; they come back only once the viewer nears them.
  In a test the 1 km² city stayed at 67 MB against a 67 MB budget (95 MB without one).
- **Setup latency scaled with the bubble (§B5)**, superseded by counted work (below). A
  bubble's setup joined the stepping 3 ticks later, or one tick per 1,800 estimated nodes.
  - The timeline overlay had shown WASM setups of 43–56 ms holding the ticks they were due in
    at 3 ticks (50 ms).
- **Meshing and generation on the pool (§B8, §B9).** Chunks are meshed in parallel (each mesh
  reads only the grid, offsets and the bubbles' commit snapshots), and streamed chunks are
  generated in parallel batches, then inserted in the serial order under the same budgets.
  Both outputs are identical to the serial ones.
  - Mesh providers with unsynchronized caches opt out (`MeshOptions::concurrent`); the Doom
    world's texture lookup memoizes its linedef search, so Doom maps mesh serially.
  - Flying through the city at 20 m/s (`svx_stream_bench`, 4 threads): meshing 5.8 → 1.8 ms
    per tick at p50 (p99 8.4 → 2.3).
  - This exposed a latent race: the verification worker ran its extraction (baseline solves)
    on the shared pool, outside its thread team. The pool is not reentrant, and once the
    simulation thread used the pool every tick, both livelocked. The worker now holds its team
    for the whole job.
- **Connectivity across the streaming boundary (§B4, §B8).** What stays resident for evicted
  chunks is a per-chunk connectivity summary, not the L3 brick model with mechanics.
  - A summary holds the connected components of the chunk's non-anchored voxels (intact or
    cracked bonds), whether each touches an anchored voxel inside the chunk, and the component
    or anchor of each of the 6 × 32² face voxels. Bond bits are folded into the + faces.
  - It is a pure function of the chunk's content: the source chunk plus its archived edits. It
    is made when a detachment search first crosses into the chunk, whether the chunk was
    evicted or never generated, and cached until the chunk becomes resident or its record
    changes.
  - Components are exact connectivity, so the search decides what a fine search of the whole
    world decides. In a randomized test the results were identical in every trial; a
    resident-only search was wrong in 31 of 73 trials with detached pieces.
  - A detached piece that reaches into non-resident chunks makes those chunks resident and is
    searched again whole. Its voxels go with it, and eviction archives the result.
  - Verification snapshots leave non-resident chunks out. Before, they were read as air: a
    structure hanging from an evicted support looked like a floating piece, and verification
    failed it and detached it. A structure reaching beyond the resident chunks is now verified
    around the window.
  - Result: load paths through evicted chunks are exact for connectivity.
  - **Mechanics across the boundary: hydrated verification.** A verification whose structures
    reach into chunks that are not resident regenerates them inside its own snapshot, from the
    chunk source and their archived edits, and designs them as a bake would. The structure is
    then checked whole, up to the snapshot bound; the live world is never touched. Its result
    writes only resident cells. A failure streams the chunks in and continues dynamically.
    Test: a steel beam cantilevers 18 m from a wall in evicted chunks onto a resident prop.
    With the prop taken away, the hydrated check sees the evicted root fail and the beam
    comes down; a window-only check leaves it standing.
  - This replaces the plan's persistent mechanical L3 model (resident mass and section
    couplings for evicted chunks): the check regenerates what it needs instead of keeping a
    coarse model of everything.
- **Counted-work commits (§B5 budget manager, degrade 5).** Background jobs count their work
  in node-iterations through a thread-local counter (`base/work.hpp`): a PCG iteration over n
  nodes counts n, a multigrid build 12n, a law sweep n, a bubble step's fixed part 2n. Phases
  count as they start.
  - A job commits max(its minimum latency, ⌈work / `tick_work`⌉) ticks after its launch. The
    counts depend only on the computation, so replays stay bit-identical on any machine and
    thread count; a slow machine only makes the simulation thread wait, and only when a job
    is behind schedule.
  - This covers bubble setups and steps, settle projections, static triages of carves and
    debris impacts, and finalizes.
  - Heavy bubbles therefore run in slower simulated time instead of stalling frames: the
    plan's last-resort degrade, now deterministic. MAP01 under 10 rockets/s, paced, native,
    before and after (with the baseline-consistency work below): mean 48 → 0.6–1.1 ms per tick,
    p95 175 → 3.5–10 ms (WASM with threads: 5.4–9.6 ms).
- **Events wait for running bubbles (§B5 merging).** An event inside a running bubble's
  window waits until that bubble has committed its step. The bubble is then taken over: its
  state is settled and stored, with no continuation, and the new event's window starts from
  it. At most one blast is released per tick, and events overlapping a pending settle wait
  for it. Debris landings on a running bubble are queued on it.
- **Baseline consistency (§B2).** A baseline cache that is no equilibrium moves nothing:
  events load a window with f_int(u0), the increment formulation. It matters where the law
  reads a jump of the cache as strain.
  - **Where jumps come from.** Tiles of a progressive bake meet with kinks. On MAP01 the law
    read φ up to 18.6 at chunk borders, and most barrage destruction had been spurious. Tile
    rims are now held at a coarse field (4³ blocks, solved once), which reduces the kinks but
    does not remove them.
  - **Windows classify plates from the world.** A window's lattice had classified plates from
    its own cells. Runs stopped at its boundary and at its pinned rim, so bonds into the rim
    lost the plate stiffness (4%) and capacity (1.4×). Every window then disagreed with the
    bake at its edge. The classification now reads the world grid (`CellIn::plate`).
  - **Consistency at consumption.** Background jobs (bubble setups, settles) swap in the
    window's pre-event state and check the cache. They solve it (rim held, bounded) only if
    the residual is above the f32 floor (relative 1e-3) *and* a bond off the rim reads more
    than 0.75 of onset from it. On the whole-baked rooms world no window needs it (bubble
    setup 280–430 → 37–50 ms); on MAP01 45 of 65 barrage windows do (median φ 1.3, up to 23).
- **Nominate / decide (§B6.1–2), coarsening (§B5).** Every 2 steps a bubble bounds the demand
  of its aggregates' crossing bonds: the baseline jump plus the rigid Δ-jump times the level's
  stress-concentration factor (2, 4, 8). Aggregates over 0.5 of onset in 2 checks in a row
  become fine cells, with a ring (momentum-exact re-partition, fine sub-lattice rebuilt with
  its init baseline forces). A refined region quiet for 30 steps goes back into aggregates.
  - The column-removal study now matches the full-fine truth at R0 12 (60 ruptures, 7,016
    detached); without nomination the coarse field cannot fail at all.
  - **The local decide stage is built but off** (`BubbleOptions::decide`). It solves the
    flagged aggregates with a ring of neighbouring nodes, the ring's outer layer held at the
    composite's motion, and refines only where the fine law nears onset. On the column-removal
    library it caught none of the 14 failing cases at any threshold from 0.4 to 0.9, while
    refining what is nominated matches 16 of 18: the patch's boundary data is the composite's
    motion, which is stiff-biased exactly where an event changes the load path. On rocket
    bubbles its solves cost what the smaller refinement saves.
  - The finer side's factor (1 at fine–coarse crossings) cut rocket bubbles by ~20%, but lost a
    collapse at the library's threshold, so the coarser side's factor stays.
  - The §B11 error-control tunables are engine configuration: `EngineConfig::nominate_phi`
    (0.5), `marginal_eps` (0.05) and `demand_inflation` (1.0), next to R0, grading and the
    level count.
- **Marginal bonds re-solve grown (§B6.3).** A fine bond within one aggregate (2 cells) of the
  fine/coarse interface decides no failure. If one there reaches 0.95 of onset, the step is
  solved again from the state before it (displacement, velocity, BDF history, one-step forces),
  with the aggregates around it made fine: at most 8 times per bubble.
  - Holding the bond and deciding a step later lost transient failures (a passing blast wave
    had moved on). The re-solve decides at the peak.
  - Blows on a 3 × 3-bay frame against the all-fine truth: with nomination and re-solve 9 of 9
    classify the same, nomination alone 7 of 9.
- **Anderson acceleration of the secant iteration (§B2)**, depth 3, restarted when the
  residual grows. The oracle fixtures converge in 37% fewer iterations (14,086 → 8,861) at
  identical parity (20/20 outcomes). On the sim library it converges on two marginal scenes
  where the plain iteration ran 19,200 iterations without converging: 33 of 33 classify the
  same as the dynamics.
- **Structure-spanning bubbles (§B5: the composite over the anchored structure).** Play-testing
  found the window's pinned rim acting as a phantom support at the scale of a building: the
  procedural tower left on its north wall and 3 of its 16 ground columns never fell. A rocket's
  bubble now spans the whole structure it hits (`EngineConfig::structure_max_cells` 400k within
  `structure_max_chunks` 512; larger ones - a whole Doom level - keep a window).
  - **Setup.** The event tick snapshots the chunks holding structure around the window (chunks
    with non-anchored voxels, face-connected, and the rock they stand on: a street's ground no
    longer joins two buildings) and records the event as the world took it: removed voxels and
    detached pieces, broken and damaged bonds, one-step forces. The bubble's background setup
    finds the structure in the snapshot, extracts it with its cached baseline (checked in the
    window as before, the window's solution spliced in), replays the event on it and builds the
    composite: fine near the event, aggregates elsewhere (the tower: 203k cells, 4.5k nodes, 1k
    fine; the extraction ~150 ms of the setup, off the simulation thread). The structure's
    region is built aside and put in place when the setup commits: the simulation thread reads
    the old one's cell index meanwhile.
  - **Events merge (§B5 merging).** An event on a structure a running bubble spans no longer
    waits for that bubble's step and settle: its crater shows at once, and the bubble takes it
    at its next job (`Bubble::apply_event`) - its cells become fine, then its bonds break, its
    damage rises and its cells go (every changed bond fine at both ends, so it releases its
    total force through the sub-lattice as a rupture does), and its forces act. Thirteen
    rockets on the tower, 0.3 s apart: one bubble.
  - **Nomination orders by the composite's own estimate.** With a whole structure in one bubble
    thousands of aggregates pass the bound's threshold at once, and the fine budget goes to those
    nearest onset first - by the unamplified estimate, then by the bound. Ordered by the bound,
    the level factor (8 at level 3) put any loaded coarse aggregate ahead of the finer ones at
    the tower's hinge, whose coarse bonds never break: unbudgeted, the tower stood 20 s with
    3,072 ruptures; ordered by the estimate it came down 8 s after the last rocket.
  - **Budgets.** Nomination adds at most `structure_max_fine_cells` (4000) above the cells events
    asked for, counting each ring cell's 2³ block (a partition cannot aggregate a block holding a
    fine cell: the ring had doubled every refinement past its cap). Its rounds, and the marginal
    re-solve's growths, are renewed each second of bubble time and by each merged event. The
    growth itself stays uncapped: capped by the same budget, the tower's hinge stayed coarse and
    it stood. Quiet crater regions coarsen again; a cell leaving the fine level takes the loads
    its ruptures and damage released into the composite's load (the aggregate's increments are
    elastic about the baseline). Structure bubbles take one Newton iteration per step
    (`structure_newton_iters`, the plan's linearly implicit step; the tower falls 2 s later than
    with 3).
  - **Pieces standing on cracks fall (§B7).** A piece held to its supports only through cracked
    bonds (failed closed: contacts, still connected for detachment) that moves off them faster
    than 0.5 m/s is released as a detached piece: a structure tipping over the crushed side of
    its hinge becomes rigid debris rather than a large rotation of the composite's linear far
    field.
  - **Keeping pace (§B5 degrade order, before slower simulated time).** A step spanning several
    ticks of work makes the bubble's next steps k ticks long (k a power of two up to
    `max_dt_multiplier` 8, from the last step's span with a factor-two hysteresis; one-step
    forces keep their impulse). A small tower left on a corner column comes down 2.6 s after the
    last rocket unbudgeted, and on the default budget 22.6 s after it with one-tick steps, ~20 s
    with k ≤ 2 and ~7 s with k ≤ 8.
  - **The tower is a marginal case.** Its bake-strengthened base gives way only by progressive
    dynamic failure (~8 s unbudgeted). On the default budget its bubble steps at a tenth of the
    tick rate (~10k nodes), the 13 rockets merge within the bubble's first half second, and it
    stood for 90 s with one-tick steps and 60 s with longer ones. Mass-proportional damping (α =
    π) also slows any tipping onset (growth rate 0.15/s instead of 0.7/s; with α = 0.3 the tower
    fell 2.6 s sooner); the feel calibration keeps it.
  - Two verification tests assumed a window: in a bubble spanning the structures, the far
    cantilever of the scene is nominated, refined and failed by the bubble itself (with the
    verification off); the tests now check verification under windows as well.
  - **Displacement fields.** A bubble's field covers the cells that move, now a whole building:
    rebuilt and sent every tick, the tower's (81 × 81 × 240 voxels, 12.6 MB) stalled the page
    until the browser killed it. A field is now rebuilt only when its bubble commits, sampled
    coarser when its box exceeds 64³ texels (stride 2 for the tower: each texel the mean of its
    voxels' solid-weighted displacement, 1.6 MB), and the worker sends a set again only when a
    field's version changed.
