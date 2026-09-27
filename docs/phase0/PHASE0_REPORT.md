# Phase 0 report: foundations, golden data, Doom content/regime study

Date: 2026-09-25. Plan: [`../PLAN.md`](../PLAN.md) §C Phase 0. Everything here is reproducible
with the tools in this repo (commands at the end).

## 1. Go / no-go

| Plan gate | Result | Verdict |
|---|---|---|
| p99 anchored structure ≤ 10⁶ cells | **Fails as worded.** At 12.5 cm with a 1 m shell, the wall shell ties each map into one connected structure: median 7.4M voxels, largest 18.4M (§3). | **Go, with the criterion replaced.** The concern behind it (verification cost) is answered by locality (§4) |
| ≥ 80% of bullet-class events need no bubble | Under v1 semantics (small pieces vanish; a bubble only for newly overloaded bonds or pieces ≥ 1000 voxels): MAP01 **99.5%** (rock) / **98.0%** (air), E1M1 **91.5%**. Rockets: 87.5% / 92.5% / 80.0%. Stricter reading (no detachment ≥ 8 voxels at all): 66–80% | **Go** |
| An S_p exists with ≤ 1% of members over S_p·P/P_cr > 0.3 | p99 member limits: S_p ≤ 47–70 on typical maps, ≤ 7.5 on MAP29 (tall outdoor walls). A few outlier walls per map need bake-time bracing (§6) | **Go** (S_p ≈ 4–7) |
| WASM kernel ≈ 5–8 ns per cell per matvec | 5–8.5 ns (f32, one thread), about native NEON speed; ~550 M cells/s on 8 threads | **Go** |

Phase 1 can start. Four plan changes follow from these results (§8).

## 2. What was built

- **`core/`: fine RBSM lattice (C++20):**
  - Material table, section and bond model with the two section fixes behind flags.
  - Matrix-free 6-DOF operator in gather form, parallel and deterministic.
  - Rigid-aggregation multigrid PCG: components within 2³ blocks, Galerkin coarse operators,
    block-Jacobi Chebyshev smoothing, W-cycle.
  - Deterministic thread pool.
  - Doom WAD reader and 6-connected voxelizer; run-length connectivity.
- **Tools:**
  - `svx_content_study`: statics, events and window validation.
  - `svx_wall_slenderness`: buckling margins.
  - `svx_fixture_compare`: golden parity.
  - `svx_bench_kernel`.
- **Golden fixtures** (`tests/fixtures/prototype/`, exporter in `tools/fixture-export/`):
  - 23 prototype scenes × 4 section variants (prototype, fixY, fixJ, fixYJ).
  - Contents: static damage-free equilibrium, collapse outcomes, XPBD feel captures.
  - Deterministic output with sha256 per file; the prototype repo is untouched.
- **Tests:**
  - doctest suite, native and WASM (Node), identical iteration counts on both.
  - ctest golden-parity gates.

## 3. Doom content at the design point (12.5 cm voxels, 1 m wall shell)

Survey of all 68 Freedoom 0.13 maps (`survey_rock.txt`):

| | median | max | min |
|---|---|---|---|
| structural voxels per map | 8.5 M | 18.7 M (E4M7) | 1.8 M |
| largest connected structure | 7.4 M | 18.4 M | — |

- 12 maps contain **floating** structural pieces (152k voxels in total): roofs that the Doom
  geometry leaves unsupported (e.g. above two-sided openings). The baker must anchor or flag
  them.
- **Rock vs air void.** In rock mode the lateral void is anchored rock; in air mode the
  shell stands free. Both give the same connected structure, but rock mode anchors walls
  laterally.

## 4. Locality: why whole-structure size is the wrong criterion

- **Anchor distance** is the number of bonds from a voxel to the nearest anchor, measured
  through the structure. MAP01:

  | Mode | p50 | p90 | p99 | max |
  |---|---|---|---|---|
  | rock | 5 | 37 | 80 | 130 |
  | air | 18 | 60 | 105 | 134 |

  E1M1 (rock): p50 6, p99 116, max 197.

  The farthest voxels are in ceiling slabs.
- **Window validation.** A carve event solved as baseline + Δ in a local window (the Δ is
  pinned at the window rim) matches a full re-solve of the 3.1M-voxel structure:
  - relative error of max utilization p50 = 1.5·10⁻³, max = 3.9·10⁻³, over 6 bullet events;
  - E1M1's 5.06M-voxel structure: 1.5·10⁻⁶ to 3·10⁻⁶;
  - window sizes 6k–19k voxels, 0.2–0.9 s, vs 12–58 s for the full Δ solve.
  - Air mode: 4·10⁻⁴ to 8·10⁻⁴.

  This is Saint-Venant locality measured on real Doom geometry.
- **Consequence for the plan.** Background verification (plan §B6.4) runs on
  anchor-bounded windows, sized about 1.5× the local anchor distance (worst case radius
  ~150–200 voxels, a few 10⁵ cells), not on whole map-spanning components. Load-path events
  (support removal) still need the load-path mode / composite (§B5–B6).

## 5. Statics and events (full-fine self-weight baseline + sampled carve events)

Setup:
- Concrete everywhere.
- Utilization is reported against **nominal** capacity. The prototype's SOLID onset is at 5×
  nominal.
- Bullet: sphere r = 1.5 voxels. Rocket: r = 8 voxels (1 m).
- Events are drawn on surface voxels, weighted by structure size.

| Map / mode | Structure solved | PCG iters | Self-weight util p99 / max | Max sag | Bullets: no-bubble (v1) | Rockets: no-bubble (v1) |
|---|---|---|---|---|---|---|
| MAP01 rock | 3.13M voxels, 82 s (16 threads) | 66 (W-cycle) | 0.170 / 4.74 | 1.2 cm | 99.5% (1 of 200 newly overloads a bond) | 87.5% |
| MAP01 air | 3.13M, 69 s | 65 | 0.171 / 4.74 | 1.2 cm | 98.0% | 92.5% |
| E1M1 rock | 5.06M, 385 s | 118 | 0.412 / **15.9** | 6.0 cm | 91.5% | 80.0% |

Observations:
- **E1M1 is overloaded under its own weight.**
  - 24k bonds exceed nominal capacity, and 109 exceed even the prototype's 5× onset. Peak
    utilization is 15.9× nominal, with a 6 cm sag.
  - The cause is the voxelizer's uniform 37.5 cm concrete ceiling slab spanning E1M1's
    large rooms.
  - MAP01 stays below onset (max 4.7).
  - So Doom geometry needs a **bake-time structural design pass**: size slab thickness by
    span (≈ span/30), use RC for slabs, and strengthen what remains. It is mandatory, not
    optional.
- **Fragility interacts with self-weight.** With the XPBD-look fragility F (plan §B3),
  those hot spots would exceed onset at load. The bake must compare self-weight utilization
  against the *game-mode* onset and strengthen members locally. Otherwise levels collapse on
  load.
- **Pieces cut off by hits** are mostly crumbs and fins at the crater rim: median 3–7
  voxels, max about 1,000 (hanging ceiling details). v1 removes them with a visual. A
  bubble is triggered by newly overloaded bonds, which are rare: 0.5–2% of bullets, 5–7.5%
  of rockets.

## 6. Buckling margin of the compliance knob S_p

Model: each free-standing vertical member is treated as a self-weight cantilever strip,
H_cr = (7.837 E t² / 12ρg)^(1/3). This is conservative, since walls are plates with braced
edges.

| Map | Free members | H p50 / max | Max S_p at p99 | at p99.9 |
|---|---|---|---|---|
| MAP01 | 42.6k | 4.6 / 21.4 m | 49.7 | 6.9 |
| MAP11 | 225k | 5.4 / 20.3 m | 52.4 | 11.7 |
| MAP29 | 57.7k | 16.3 / 20.3 m | 7.5 | 3.6 |
| E1M1 | 65k | 4.6 / 22.0 m | 70.0 (masonry 46.7) | 26.1 (17.4) |
| E4M7 | 264k | 4.6 / 35.4 m | 70.0 (46.7) | 3.6 (2.4) |

- **Choice of S_p.** S_p ≈ 4–7 satisfies the red-team's buckling margin on every map
  checked.
- **Outliers.** Each map has a handful of outlier members, e.g. a 35 m tall section in
  E4M7. The strip model says they would buckle even at S = 1. The baker flags and braces
  them.

## 7. Solver and golden parity

- **Multigrid:**
  - Frame buildings (4k–15k cells): 23–24 W-cycle iterations to 1e-8.
  - MAP01's 3.1M-voxel structure: 66 iterations to 1e-6.
  - V-cycle: residual 0.3 after 60 iterations. W-cycle: 2·10⁻⁶.
  - Other variants tested at 60 iterations: coarse scale 0.35 or 0.7 → 0.98 and 0.19;
    stronger Chebyshev → 0.037; strength-of-connection 0.1 → 0.8. **The W-cycle is now the
    default.**
- **Golden parity:** static, damage-free, fixture stiffness injected (`svx_fixture_compare`).
  Partial supports (pinned or semifixed DOF masks and support springs) were added at the end
  of Phase 0, so every scene with an anchored solve now compares.
  - **Elastic, small deflection** (ctest gates, relative displacement error):

    | Scene | Error | Note |
    |---|---|---|
    | building3 | 2·10⁻⁵ | minDz −1.505330e-4 vs oracle −1.505360e-4 |
    | cascadingCollapse | 4·10⁻⁵ | |
    | wallLintel | 4·10⁻⁶ | |
    | braceCorner | 7·10⁻⁵ | |
    | seamCompare | 2·10⁻⁷ | |
    | pinned plate | 9·10⁻⁷ | |
    | multiSpan | 1·10⁻⁴ | |

  - **Centimetre sag, restrained or long spans.** These differ because the oracle is
    geometrically nonlinear (large rotation; tension stiffening with pinned ends):
    - pinned beam 0.3% (the exporter measured the same 0.3% stiffening independently);
    - clampedCantilever 0.4%, supportCompare 0.6%, asymDiaphragm 0.8%, cantilever 1.2%;
    - simplySupported 3.0%, continuationStiffStrip 4.4%;
    - plasticBeam 18% (2.5 m sag, beyond onset).

    The corotational stepping of Phase 1 addresses this. At 12.5 cm and real stiffness,
    Doom-scale deflections are far smaller.
  - **Not comparable:**
    - contactGuidedStack: the oracle does not converge.
    - contactSmokeStack and ootest_floatingCluster: nothing anchored to solve against.
- **Model parity:** structvox's own bond model vs the compiled stiffness.
  - SOLID bonds: exact (ratio 1.0000).
  - The fixY/fixJ variants: 1.0000–1.0001, from the closed-form St-Venant β.
  - Remaining differences come from profile selection (MONOLITHIC, BRACE, WEAK_JOINT in
    `compiler.js`), not ported yet.
- **Determinism:** the building solve is bitwise identical for 1 vs 8 threads, and the
  native (arm64) and WASM builds produce the same solution hash.

## 8. Changes to the plan

1. **Multigrid default is the W-cycle.** Unsmoothed aggregation with V-cycles stalls on
   thin-slab Doom geometry. SA / K-cycle remain Phase 2 options.
2. **Structure-size gate → anchor-distance locality.** Verification and baselines work on
   anchor-bounded windows.
3. **Triage semantics.** Small detached pieces are not bubble triggers in v1. The bubble
   trigger is newly overloaded bonds, or a detached mass above a threshold.
4. **Bake checks** (from Doom content):
   - structural design: slab thickness by span (≈ span/30) and RC slabs, because E1M1's
     uniform 37.5 cm slabs reach 15.9× nominal under self-weight;
   - self-weight utilization vs game-mode onset (with fragility F) → local strengthening;
   - buckling → bracing of outlier walls;
   - floating pieces → anchor or flag.

## 9. Known limitations and follow-ups

- **Memory.** The Phase 0 solver uses about 2 KB per voxel (f64 everywhere, anchored cells
  stored explicitly), so MAP11's 16.6M-voxel structure was not solved. Phase 1/3 targets
  ≤ 400 B per active cell: f32 storage, implicit anchors, fewer temporaries.
- **Speed.** The event evaluator does O(component) work per event (residual, copies). A
  real engine does O(window).
- **Prototype app bug** (found by the fixture agent). On the CPU path, stiff scenes report
  "quiet" during the gravity ramp, so the app parks at tick 5. The display freezes at about
  17% of the settled sag. The feel spec must use settled values, not what the app shows.
- **XPBD feel data** from the fixtures:
  - building3 settled sag is 3.4e-3 m in XPBD vs 1.5e-4 m in the oracle (22×).
  - XPBD ruptures clampedCantilever, the rc_floor strips and plasticBeam right after its
    gravity ramp.

## Reproduce

```bash
cmake --preset native-release && cmake --build --preset native-release -j
(cd build/native-release && ctest --output-on-failure)
docs/phase0/run_study.sh                      # content study runs -> docs/phase0/*.json|log
./build/native-release/tools/svx_wall_slenderness --wad data/freedoom/freedoom2.wad --map MAP29
./build/native-release/tools/svx_fixture_compare --variant prototype tests/fixtures/prototype/*.json
node tools/fixture-export/export.mjs          # regenerate fixtures from the prototype oracle
```
