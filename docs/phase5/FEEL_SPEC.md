# Phase 5: feel spec and knob calibration

This page records how structvox's look knobs (plan §B3) were fitted to the prototype, and
where that fit stops.

The knobs:

- **Compliance S_p:** physical softness, which slows the dynamics by √S_p.
- **Amplification A:** scales event displacements when rendering.
- **Fragility F:** the game-law capacity multiplier.
- **Damping ζ:** fraction of critical damping.

The fit target is the XPBD **feel captures** in the prototype fixtures (`feel.runs.delete` of
`tests/fixtures/prototype/*.json`). They were recorded with the prototype app's own settings:
the settle under self-weight, and the app's gameplay blasts.

## Harness

`svx_fixture_compare --feel` runs each captured scene with the game law and the knobs,
using the fixture's stiffness (k0 injected). The steps:

1. Remove unsupported pieces (a collapse, as the engine's bake does).
2. Settle to elastic equilibrium, then run implicit dynamics until asleep. This mirrors the
   XPBD settle.
3. Replay the first captured blast (same centre and radius) and run until quiet.

The harness compares the following against the capture:

- fail or stand after settle, where a collapse means deleted pieces or a mechanism with more
  than 0.5 m of sag;
- per blast: fail or stand, ruptures, deletions, the event-induced peak sag (× A, as rendered)
  and the time to quiet.

`--feel-grid` scans S_p ∈ {4, 6, 9}, F ∈ {0.25, 0.5, 1} and ζ ∈ {0.05, 0.1, 0.2}. It fits A
per setting as the median ratio between the captured peak and ours.

## Results

The full grid is in [`feel_grid.txt`](feel_grid.txt). The report at the chosen defaults is in
[`feel_spec_S9_A1_F1_z005.txt`](feel_spec_S9_A1_F1_z005.txt).

- **Visible magnitude.** The fitted A always gives **S_p·A ≈ 8.6**:

  | S_p | fitted A |
  |---|---|
  | 4 | 2.20 |
  | 6 | 1.43 |
  | 9 | 0.93 |

  The prototype shows event responses about 8.6× the stiff response. This matches its
  solver-driven softness: the Phase 0 settled sag is 22× the oracle's. The peak error is
  exp(0.12–0.17), i.e. within about 15–20% of the captured peaks.
- **Timing.** The time from blast to quiet matches best at high compliance and light damping:

  | setting | median log error |
  |---|---|
  | S_p 9, ζ 0.05 | 0.87 (a factor of about 2.4) |
  | S_p 4 | 1.5 |

  XPBD's quiet time also depends on its iteration count, so no setting matches it closely.
  For example, building3 is quiet after 1.27 s in XPBD and after 0.63 s here.
- **Fail / stand.** Settle agreement is 15–16 of 23 scenes, and blast agreement 1 of 3, for
  every F. This is expected, not a calibration miss:
  - Every fixture uses **1 m cells**. The game law is cell-size consistent (plan §B1): peak
    strength ≤ √(2 G_f E / h). At h = 1 m that cap, not F, governs.
  - Some oracle scenes on pin or roller supports become mechanisms. XPBD keeps them
    standing, because its under-converged solver never reaches the fracture strains.
  - Matching XPBD's raw fail/stand at 1 m would mean matching solver noise. The plan rules
    that out.

  At the engine's 12.5 cm voxels, fail/stand is governed by the design pass (members at most
  50% utilized at rest) and by F.

## Chosen defaults

The engine's `EngineParams` and the web's `DEFAULT_PARAMS` are now identical:

| knob | before (engine / web) | now |
|---|---|---|
| compliance S_p | 4 / 6 | **9** |
| amplification A | 3 / 3 | **1** |
| fragility F | 0.25 / 1 | **1** |
| damping ζ | α 0.5 / 0.1 | **0.05** (α = π/s) |

The Fragility slider's hint was also wrong: F is a strength multiplier, so its hint now reads
"strength scale (lower snaps sooner)".

## Open (needs the user)

- **Side-by-side sign-off.** The Phase 5 gate asks the user to compare the new engine with the
  prototype and sign off. The knobs are live in the settings panel for that session.
- **Feel captures at the engine's scale.** The prototype's captures are at 1 m cells.
  Captures at 12.5 cm, or a play-test fail/stand spec, would let F be fitted beyond its
  nominal value.
