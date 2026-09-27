# structvox

A C++20 structural-destruction engine for large streaming voxel worlds. It runs in the browser
as WebAssembly (a pthreads worker with a TypeScript/WebGPU front end) and natively for tools
and tests.

The goal is a Doom-like FPS in which a voxelized Doom level is fully destructible and has real
structural integrity:

- Overloaded parts crack, sag and collapse.
- Cracked parts rest on their cracks, or separate and fall as rigid debris.
- Heavy debris loads what it lands on.

Scaling comes from event-driven, telescoping multi-resolution **bubbles**, not from simulating
every voxel. A background full-fine verification makes sure no far failure is missed.

- **Plan:** [`docs/PLAN.md`](docs/PLAN.md), approved 2026-09-25. Its revision log (§G)
  records the implementation decisions.
- **Status by phase, with gates and evidence:** [`docs/STATUS.md`](docs/STATUS.md).
- **Engine ↔ front-end contract:** [`docs/API.md`](docs/API.md).
- **Physics oracle:** the JavaScript prototype `../voxel_threed_discrete`. Golden fixtures are
  exported from it by [`tools/fixture-export`](tools/fixture-export).

## Build and test

The WASM builds need Emscripten: run `source ~/emsdk/emsdk_env.sh` so that `EMSDK` is set.

```bash
# native: library, tools, tests
cmake --preset native-release && cmake --build --preset native-release -j
./build/native-release/tests/svx_tests            # doctest suites
SVX_T_BIG=1 ./build/native-release/tests/svx_tests -tc='collapse: a tower*'   # 10-storey collapse physics check (~5 min)
(cd build/native-release && ctest)                # + golden parity / outcome against the oracle

# browser module (pthreads; written to web/src/wasm/)
cmake --preset wasm-release-threads && cmake --build --preset wasm-release-threads -j --target svx_web
# WASM tests under Node (single-threaded and threaded builds)
cmake --preset wasm-release && cmake --build --preset wasm-release -j --target svx_tests
node build/wasm-release/tests/svx_tests.js

# x86-64 build (macOS: runs under Rosetta) for cross-architecture determinism checks
cmake --preset native-x86_64 && cmake --build --preset native-x86_64 -j
```

`scripts/fetch_freedoom.sh` downloads Freedoom (BSD-3-Clause) into `data/freedoom/`.

## Run the game

```bash
cd web && npm install
npm run dev -- --port 5190           # COOP/COEP headers are set (SharedArrayBuffer, pthreads)
open "http://localhost:5190/"          # the WASM engine by default (?engine=mock without it)
```

- **Worlds:**
  - `?world=rooms|city|tower&seed=N`. `city` is a streamed 1 km² city with a far render tier.
  - A Freedoom WAD via the panel. The map's doors, lifts, floors, platforms, crushers and stairs work.
- **Controls:**
  - WASD move, mouse look, Space jump, Shift run.
  - 1/2/3 or the wheel select pistol, shotgun, rocket launcher. Click fires.
  - E uses (doors, lift switches), V toggles noclip, R respawns.
  - G cycles debug views (utilization, bubble levels). H toggles the HUD with its job and
    budget timeline (the worker's last 4 s of ticks by part, against the 8 ms budget). Esc
    opens the menu.
- **Query options:**
  - `?persist=1` saves world edits to OPFS.
  - `?gpudisp=0` re-meshes bubble chunks on the CPU instead of using GPU displacement fields.
  - `?engine=mock` runs the TypeScript mock engine instead of the WASM one.
- **Checks:**
  - `npm run typecheck && npm test` for the front end.
  - `node scripts/smoke-wasm.mjs http://localhost:5190/` for the browser smoke test.
  - The production build (`npm run build`, `vite preview`) passes the same smoke test.
  - `node scripts/record.mjs --url URL --ready EXPR --action FILE.js --seconds S --out OUT.mp4`
    records a page to video (headless Chrome, DevTools screencast, ffmpeg); see
    `docs/phase5/recording/` for the feel comparison's action scripts.

## Tools (`build/native-release/tools/`)

| Tool | Purpose |
|---|---|
| `svx_engine_demo --world rooms\|city\|tower \| --wad F --map M [--rockets N] [--bullets N] [--threads T] [--gpu-displacement] [--no-verify] [--realtime]` | Headless engine run. Reports event and tick costs (percentiles; `--realtime` paces ticks at 60 Hz so background work gets real time), bubble step profile, render update costs and the session hash. |
| `svx_step_bench [--rockets N] [--R0 C]` | Bubble step cost on the rooms world in thread CPU time (robust on a loaded machine): event tick, steady steps, PCG per step. `SVX_BENCH_TICKS=1` prints every tick, `SVX_MG_INFO=1` the multigrid levels and kernel throughput. |
| `svx_replay record\|play --world W --seconds S [--out F \| --log F] [--threads T]` | Records and replays sessions from command logs. Checkpoint hashes every 10 s are the determinism gate (Phase 3) and the basis for deterministic lockstep. |
| `tools/replay/gate.sh OUTDIR` | The determinism gate: records a 10-minute rooms session on ARM (1 thread) and replays it on ARM 2/4/8 threads, x86-64 (Rosetta) 1/4 and WASM, diffing all 60 checkpoint hashes. |
| `svx_stream_bench [--speed M] [--seconds S] [--threads T]` | Flies through the streamed 1 km² city and reports the per-tick stream work on the simulation thread and the meshing cost (Phase 6 hitch gate). `SVX_STREAM_TRACE=1` splits slow ticks. |
| `svx_map_check [--threads T] [--movers] WAD...` | Imports, bakes and design-checks every map, then runs it idle (`--movers`: runs every move of every sector mover). Phase 5 gate. |
| `svx_fixture_compare [--outcome] [--feel \| --feel-grid] FIXTURES...` | Golden parity and outcomes against the oracle, and the feel spec against the XPBD captures. |
| `svx_sim_library [--pre]` | Static + DIF vs dynamics classification, and S invariance. Phase 1 gates. `--pre` compares the pre-event statics (secant, Anderson, load ramp). |
| `svx_composite_study`, `svx_bubble_study` | Composite and bubble accuracy and cost against full-fine truth (column removal; `SVX_NOMINATE_TRACE=1` shows nominations and marginal re-solves). Phase 2. |
| `svx_content_study`, `svx_wall_slenderness`, `svx_wad_textures`, `svx_bench_kernel` | Phase 0: Freedoom content and regime study, buckling margins, WAD graphics, kernel throughput. |

## Layout

```
core/include/svx, core/src
  base/    types, deterministic parallel pool, bundled deterministic math (dmath), work counting
  mech/    materials, bonds, reference and game laws
  solve/   lattice, multigrid (SIMD BSR)
  sim/     corotational kinematics, statics (closure), dynamics, blasts
  topo/    connectivity
  bubble/  composite partitions, event bubbles
  world/   voxel grid (chunks, overlays, deltas), regions, procgen, streaming sources,
           coarse connectivity of non-resident chunks
  mesh/    chunk mesher
  engine/  engine core, debris, sector movers, command logs (replay)
  doom/    WAD reader, voxelizer, textures, Doom world, specials and movers
  api/     flat C ABI for the WASM worker
tools/     command-line tools (native and WASM/Node)
tests/     doctest suites; fixtures/ from the prototype oracle
web/       TypeScript + Vite front end: worker host, WebGPU renderer, FPS sandbox
docs/      plan, API, status, phase reports and evidence
research/  Python methodology spike, WASM kernel benchmark
```
