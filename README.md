# structvox v2

A C++20 structural-destruction engine for large streaming voxel worlds. It runs in the browser
as WebAssembly (a pthreads worker with a TypeScript/WebGPU front end) and natively for tools and
tests.

Structures stand under their own weight, crack and collapse when they lose support, and what
comes loose keeps breaking: a tower that loses its ground columns fails at its base, comes down
storey by storey, breaks up in the air and where it lands, and ends as a pile of slab plates,
wall panels and blocks that settles and sleeps.

- **Video:** [`docs/media/tower_collapse.mp4`](docs/media/tower_collapse.mp4): the tower losing its
  two west rows of ground columns (`svx_engine_demo --world tower --scenario pillars`, CPU
  renderer).
- **Method:** [`docs/V2_DESIGN.md`](docs/V2_DESIGN.md). In short:
  - Voxels form pre-scored rubble **fragments**, joined by **bonds**.
  - Every standing structure and every falling piece gets its stress from the same elastic
    equilibrium solve on its fragment graph (multigrid PCG).
  - Bonds fail by fibre-stress checks: tension, flexure, crushing, Mohr–Coulomb shear.
  - Falling pieces are rigid bodies that keep their bonds and break on impact, progressively,
    part by part.
- **Engine ↔ front-end contract:** [`docs/API.md`](docs/API.md).
- **v1** (the bubble/lattice engine this version replaces): [`docs/v1/`](docs/v1) (plan, status,
  phase reports).

## Build and test

The WASM builds need Emscripten: run `source ~/emsdk/emsdk_env.sh` so that `EMSDK` is set.

```bash
# native: library, tools, tests
cmake --preset native-release && cmake --build --preset native-release -j
./build/native-release/tests/svx_tests            # doctest suites (stress, fragments, rigid, collapse, engine, doom, ...)

# browser module (pthreads; written to web/src/wasm/)
cmake --preset wasm-release-threads && cmake --build --preset wasm-release-threads -j --target svx_web
```

`scripts/fetch_freedoom.sh` downloads Freedoom (BSD-3-Clause) into `data/freedoom/`.

## Run the game

```bash
cd web && npm install
npm run dev -- --port 5190           # COOP/COEP headers are set (SharedArrayBuffer, pthreads)
open "http://localhost:5190/?world=tower"   # the WASM engine by default (?engine=mock without it)
```

- **Worlds:** `?world=rooms|city|tower&seed=N` (`city` is a streamed 1 km² city), or a Freedoom
  WAD via the panel (doors, lifts, floors, platforms, crushers and stairs work).
- **Controls:** WASD move, mouse look, Space jump, Shift run; 1/2/3 or the wheel select pistol,
  shotgun, rocket launcher, click fires; E uses, V toggles noclip, R respawns; G cycles debug
  views (bond utilization, fragments); H toggles the HUD and its tick timeline; Esc opens the
  menu (fragility, impact and dynamic-factor sliders).
- **Checks** (dev server running):
  - `npm run typecheck && npm test` for the front end.
  - `node scripts/smoke-wasm.mjs http://localhost:5190/`: the browser smoke test.
  - `node scripts/tower-wasm.mjs http://localhost:5190/ OUT 25`: blasts the tower's ground
    columns in the browser and records screenshots and engine stats.

## Tools (`build/native-release/tools/`)

| Tool | Purpose |
|---|---|
| `svx_engine_demo --world tower\|rooms\|slab\|chimney\|bridge\|city [--scenario S] [--seconds T] [--threads N] [--frames DIR --fps F --res WxH --cam x,y,z --look x,y,z]` | Headless scenario run with a CPU renderer for frames. Tower scenarios: `pillars` (both west rows of ground columns), `side`, `core`, `all`, `rockets`. Reports pieces, breaks, per-phase rigid costs, awake speeds, piece sizes and the session hash. |
| `svx_replay record\|play --world W --seconds S [--out F \| --log F] [--threads T]` | Records and replays sessions from command logs; checkpoint hashes are the determinism check. |
| `svx_map_check [--threads T] [--movers] WAD...` | Imports, bakes and design-checks every map, then runs it idle. |
| `svx_stream_bench`, `svx_wad_textures` | Streaming cost of the city; WAD graphics. |

Debug environment variables of the engine: `SVX_DEBUG_BODY` (piece checks), `SVX_PROFILE`,
`SVX_PROFILE_TICK`, `SVX_PROFILE_COLLIDE`.

## Layout

```
core/include/svx, core/src
  base/    types, vectors, deterministic parallel pool, deterministic math
  mech/    material table (strengths, fracture energies, fragment sizes)
  frag/    fragments (pre-scored rubble pieces per chunk)
  solve/   smoothed-aggregation multigrid, PCG
  stress/  fragment-graph stress problems, bond failure checks
  phys/    rigid voxel bodies: contacts, solver, sleep
  world/   voxel grid (chunks, broken faces), procedural worlds, streaming sources
  mesh/    chunk mesher
  engine/  structures, pieces and their fracture, events, persistence, movers, replay
  doom/    WAD reader, voxelizer, textures, Doom world, specials and movers
  api/     flat C ABI for the WASM worker
tools/     command-line tools
tests/     doctest suites
web/       TypeScript + Vite front end: worker host, WebGPU renderer, FPS sandbox
docs/      design, API, v1 history
```
