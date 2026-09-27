# Phase 1 report — reference core and semantics (the truth model)

Status 2026-09-25. All numbers are from `ctest --preset native-release` (9/9 green),
`node build/wasm-release/tests/svx_tests.js` (25/25 green) and the tools listed below.

## What was built
| Module | Contents |
|---|---|
| `mech/bond`, `mech/archetype`, `mech/law` | Section / profile model (SOLID, MONOLITHIC, WEAK_JOINT, BRACE with the prototype's continuity scales and per-edge overrides), archetype table + `resolve_profile` (compiler.js), reference law (continuum_regularized_v1, κ_on/κ_br, 512-cap rupture order) and game law (triangular softening, fracture energy exact at any h — peak lowered to √(2 w k / ρ_min) when a cell is too large for its G_f) |
| `sim/corot` | Corotational bond kinematics (slerp-mid frame, attachment lever arms, cancellation-free `R v − v`), internal forces `Jᵀσ`, rotation composition, bond frames → **corotated tangent** (material part, SPD, exact at stress-free rotated states) used by the operator, the diagonal blocks and the multigrid |
| `sim/statics` | Non-accumulating secant / modified-Newton equilibrium (MG-PCG, trust region, stagnation acceptance at the fp floor, candidate grace), `solve_to_closure` (oracle pass protocol, detached deletion, DIF amplification incl. the event pass) |
| `sim/dynamics` | Linearly implicit BDF2 (BE restarts), Rayleigh damping, compliance S_p (law sees δ/S_p), per-step law + capped rupture with carry-over, detach → vanish events with velocities, virtual impact loads (ray cast below each island column, m v / T + m g over T at the landing time), ramp-out of released loads, settle projection → sleep |
| `sim/blast` | Gameplay prefracture (core removal, padded fracture, graded damage) + radial impulse, cell-size invariant |
| `topo/connectivity` | Lockstep multi-source detachment search with the pre-change-support shortcut: O(cut) for cuts that separate nothing, O(piece) for cut-off pieces |
| `base/parallel` | Low-latency spin-then-sleep pool (fixed chunking ⇒ bitwise identical for any thread count) |

## Gates
| Gate (plan Phase 1) | Result |
|---|---|
| Outcome parity on oracle-converged scenes: classification ≥ 95 %, alive-set Δ ≤ 2 % | **20/20 scenes exact** (alive sets identical, blast removed/fractured sets identical, blast damage values to 1e-8) — with injected oracle archetypes *and* with structvox's own stiffness + thresholds, variants prototype and fixYJ (`golden_outcome_*` ctests) |
| Static Δz and demand ≤ 1 % on intact scenes | **≤ 1.6e-5 relative** on all 21 comparable scenes × 4 variants with the corotational equilibrium (cantilever and plasticBeam, 0.43 rad, included); with structvox's own section model k/k0 = 1.0000 (prototype, fixY) and ≤ 1.0006 (St-Venant J variants) (`golden_parity_*` ctests) |
| Analytic beams ≤ 1 % | fixYJ strips 0.995 of Timoshenko, simply supported beam 0.990 (identical to the oracle to 1e-10) |
| Fracture energy / area invariant across h ± 5 % (game law) | exact (h = 1/16 … 1 m, modes I and II) — `test_sim.cpp` |
| Classification identical for S ∈ {1, 4, 16} (linear kinematics) | static closure: identical alive and rupture sets; dynamics: period scales as √S (±2 %); scenario library: **31/31** identical ([`sim_library_sinv.txt`](sim_library_sinv.txt)) |
| Static + DIF vs dynamics ≥ 90 % | **31/31 (100 %) at every DIF 1.0–2.0**. 5 structure types × 7 fragility levels at h = 0.125 m, game law, S = 4; 4 cases fail before the event and are skipped. Calibrated DIF = 1.0. ([`sim_library_dif.txt`](sim_library_dif.txt), `svx_sim_library`, 45 min) |
| Connectivity O(changed) | counters: single rupture ≤ 16 cells visited, 3×3 hole ≤ 80, 4×4 cut-out ≤ 120, identical for 48² and 160² slabs |
| Determinism | statics hash identical 1 vs 8 threads and native vs WASM (6909790229774717387); dynamics with blast + ruptures identical 1 vs 8 threads and native vs WASM (13558992527312765371) |

## Notable findings
- The oracle's `static` block is a **corotational** solve: the linear solver matched it only on
  stiff scenes; the corotational equilibrium matches all of them. Jump kinematics must avoid
  `R ρ − ρ` cancellation (the residual floor was ~1e-10 × load before the fix).
- Scenes whose oracle Newton did not converge (cantilever, plasticBeam under closure) end with
  the same alive sets but different rupture lists — expected, not gated.
- Doom-scale dynamics at 60 Hz is stiffness-dominated (m c² ≪ k): linearly implicit steps with
  the residual form carry any unbalanced force to the next step, so loose solves are safe.

# Phase 2 (in progress) — composite dynamic spike

| Item | Status / result |
|---|---|
| Composite partition + scaled Galerkin + St-Venant torsion correction (`bubble/composite`) | C++ reproduces the Python spike exactly (same node counts 1,488 / 2,113; near-field peak 0.85 / 0.88 at R0 = 8, g = 2 / 3) and with the exact baseline the load-path error drops from 5.7 % to ≤ 1.2 % (`svx_composite_study`) |
| Multigrid over assembled composites | greedy plain aggregation (7–8× per level), standard γ-cycle, V-cycle for bubbles, mass folded into the diagonal blocks |
| Event bubble (`bubble/bubble`): baseline + Δ, fine-fine bonds nonlinear on a sub-lattice, coarse couplings linear, detachment with released loads, re-partition with momentum-exact rigid projection | frame column removal (8.6k cells → 2.5k nodes): near-field peak-over-time 0.93–0.95 of full-fine dynamics truth, probe sag 0.925; **steady step ≈ 4 ms native single thread, PCG p95 = 2–5 (gate ≤ 15)**, first steps ≈ 28 ms (gate 12 ms: preconditioner build) |
| Collapse capture | R0 must reach the nearest joints (plan sizing rule): R0 = 12 misses the progressive collapse, R0 ≥ 20 captures it; aggregates never fail by design → failures of far members (all storey columns in the whole-building collapse test) need nomination + background verification (next) |
