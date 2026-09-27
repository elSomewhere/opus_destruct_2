# Research: methodology spike (2026-09-25)

This spike was the evidence behind [`docs/PLAN.md`](../docs/PLAN.md) §A5.

## `mlexp/`: 3D RBSM lattice in Python (numpy/scipy)

Setup: `python3 -m venv venv && ./venv/bin/pip install numpy scipy`.

| Script | What it shows |
|---|---|
| `lattice.py` | 6-DOF rigid-cell lattice, face bonds (Timoshenko segments), matrix assembly |
| `test_cantilever.py` | The lattice matches beam theory (ratio 1.03–1.09, from effective span) and statics exactly |
| `hierarchy.py` | Rigid aggregation (components in 2³ blocks), multigrid, composite partitions, fibre-length scaling, St-Venant torsion correction |
| `test_coarse_beam.py`, `test_coarse_beam2.py` | Coarse-model accuracy: beams ±2% at 8× coarsening with scaling; plates 0.85–0.93 |
| `exp_bubble.py`, `exp_bubble2.py` | Fixed kinematic bubble (prototype) vs telescoping composite after a column removal |
| `exp_scale.py` | Composite node count stays ~flat as the building grows 9× |
| `exp_calib.py` | A single coarse-softening factor does not remove the bias uniformly |
| `exp_indicator.py` | Coarse-face resultant indicators underestimate local peaks up to 4–8× |
| `exp_mg.py` | Multigrid PCG iterations vs size (Braess-style scaled coarse operators) |
| `exp_rooms.py` | Plate-dominated Doom-like rooms: rocket craters accurate to ±5% at R0=12 |
| `exp_dynamics.py` | Implicit-dynamics MG-PCG iterations per step; dynamic composite (baseline + Δ) |

Key numbers (column removal in a 3×3-bay, 2-storey frame; reference = full fine solve):
- **Fixed kinematic bubble:**
  - R=8: near-field peak stress 0.29–0.44 of the reference.
  - R=16 (prototype default): 0.55–0.70.
  - R=24: 0.98.
- **Telescoping composite:** R0=8, g=2 with 1,488 nodes gives 0.83–0.86. With g=3 (2,113
  nodes), 0.89–0.91.
- **Doom-like rooms, rocket craters:** R0=12 gives 3–4k nodes (6–8% of cells) and a peak
  within −3%…+5%.

## `wasmbench/`: C++ matrix-free lattice kernel (Emscripten 5.0.7, Node 25, Apple M5 Pro)

`lattice_kernel2.cpp` is the vectorization-friendly version; it is also built as the
`svx_bench_kernel` tool. Results:
- f32: 5–8.5 ns per cell per matvec on one thread, about the same as native NEON.
- f64: 11–17 ns.
- 8 threads: about 550 M cells/s.
- Without SIMD: 2–3.5× slower.
