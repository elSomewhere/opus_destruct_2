# structvox v2

A C++20 structural-destruction physics engine for large streaming voxel worlds, and a prototype
game built on it. The game runs in the browser as WebAssembly (a pthreads worker with a
TypeScript/WebGPU front end); everything also builds natively for tools and tests.

Structures stand under their own weight, crack and collapse when they lose support, and what
comes loose keeps breaking: a tower that loses its ground columns fails at its base, comes down
storey by storey, breaks up in the air and where it lands, and ends as a pile of slab plates,
wall panels and blocks that settles and sleeps.

Materials include concrete (plain and reinforced, with explicit bars in thick members), steel,
masonry, stone, wood and glass. On top of the destruction core, the environment systems add
three more things, all interacting with the structures:

- **Fire** chars wood until members give way, and weakens steel.
- **Smoke** rises and fills rooms.
- **Water** flows, presses on the walls that hold it until they burst, floats pieces and puts
  out fires.

All of it works in streamed and bounded worlds alike.

- **Video:** [`docs/media/tower_collapse.mp4`](docs/media/tower_collapse.mp4): the tower losing its
  two west rows of ground columns (`svx_engine_demo --world tower --scenario pillars`, CPU
  renderer).
- **The physics as a library:** [`docs/CORE.md`](docs/CORE.md). The destruction physics
  (`svx_core`: `svx::World`, and a C API in `svx/svx_core.h`) knows nothing of the game: no
  rendering, players or levels. It is reusable on its own, from C++, C (or any language with a C
  FFI) and JavaScript (a core-only WASM module). The game harness (`svx_game`) is one host of it,
  and the two iterate separately.
- **Method:** [`docs/V2_DESIGN.md`](docs/V2_DESIGN.md). In short:
  - Voxels form pre-scored rubble **fragments**, joined by **bonds**.
  - Every standing structure and every falling piece gets its stress from the same elastic
    equilibrium solve on its fragment graph (multigrid PCG).
  - Bonds fail by fibre-stress checks: tension, flexure, crushing, Mohr–Coulomb shear.
  - Falling pieces are rigid bodies that keep their bonds and break on impact, progressively,
    part by part.
- **Fire, smoke and water:** [`docs/ENV.md`](docs/ENV.md). `svx_env` is built on the core's
  public extension points (voxel layers, damage, loads, piece forces, systems), so the core
  stays a clean destruction and structural-integrity engine.
- **Game ↔ front-end contract:** [`docs/API.md`](docs/API.md).
- **v1** (the bubble/lattice engine this version replaces): [`docs/v1/`](docs/v1) (plan, status,
  phase reports).

## Build and test

The WASM builds need Emscripten: run `source ~/emsdk/emsdk_env.sh` so that `EMSDK` is set.

```bash
# native: library, tools, tests
cmake --preset native-release && cmake --build --preset native-release -j
./build/native-release/tests/svx_core_tests       # the physics core alone (stress, fragments, rigid, world)
./build/native-release/tests/svx_env_tests        # fire, smoke, water on the core
./build/native-release/tests/svx_game_tests       # the game harness (collapse, game, movers, replay, doom, ...)

./build/native-release/examples/svx_core_minimal # the core from C++ (and svx_core_c: from C)

# browser module of the game (pthreads; written to web/src/wasm/)
cmake --preset wasm-release-threads && cmake --build --preset wasm-release-threads -j --target svx_web
# the physics core alone for JavaScript hosts (build/.../tools/svx_core_web.js)
cmake --build --preset wasm-release-threads -j --target svx_core_web
```

`scripts/fetch_freedoom.sh` downloads Freedoom (BSD-3-Clause) into `data/freedoom/`.

## Run the game

```bash
cd web && npm install
npm run dev -- --port 5190           # COOP/COEP headers are set (SharedArrayBuffer, pthreads)
open "http://localhost:5190/?world=tower"   # the WASM engine by default (?engine=mock without it)
```

- **Worlds:** `?world=rooms|city|tower|yard&seed=N` (`city` is a streamed 1 km² city with timber
  floors in a third of its buildings and ponds; `yard` has one of each construction: a timber
  house, a stone tower, a greenhouse, a steel shed, a reinforced wall, a reservoir and a timber
  water tower), or a Freedoom WAD via the panel (doors, lifts, floors, platforms, crushers and
  stairs work).
- **Controls:** WASD move, mouse look, Space jump, Shift run; 1 to 5 or the wheel select pistol,
  shotgun, rocket launcher, flamethrower, water hose, click fires; E uses, V toggles noclip, R respawns; G cycles debug
  views (bond utilization, fragments); H toggles the HUD and its tick timeline; Esc opens the
  menu (fragility, impact and dynamic-factor sliders).
- **Checks** (dev server running):
  - `npm run typecheck && npm test` for the front end.
  - `node scripts/smoke-wasm.mjs http://localhost:5190/`: the browser smoke test.
  - `node scripts/tower-wasm.mjs http://localhost:5190/ OUT 25`: blasts the tower's ground
    columns in the browser and records screenshots and engine stats.
  - `node scripts/fire-wasm.mjs http://localhost:5190/ OUT 60`: sets the yard's timber house
    on fire, then hoses it down.
  - `node scripts/water-wasm.mjs http://localhost:5190/ OUT`: breaches the yard's reservoir.

## Tools (`build/native-release/tools/`)

| Tool | Purpose |
|---|---|
| `svx_engine_demo --world tower\|rooms\|slab\|chimney\|bridge\|city [--scenario S] [--seconds T] [--threads N] [--frames DIR --fps F --res WxH --cam x,y,z --look x,y,z]` | Headless scenario run with a CPU renderer for frames. Tower scenarios: `pillars` (both west rows of ground columns), `side`, `core`, `all`, `rockets`. Reports pieces, breaks, per-phase rigid costs, awake speeds, piece sizes and the session hash. |
| `svx_replay record\|play --world W --seconds S [--out F \| --log F] [--threads T]` | Records and replays sessions from command logs; checkpoint hashes are the determinism check. |
| `svx_map_check [--threads T] [--movers] WAD...` | Imports, bakes and design-checks every map, then runs it idle. |
| `svx_soak [--world city\|tower\|rooms\|yard] [--wad F --map M] [--minutes M] [--archive-mb MB] [--forget-s S] [--no-shoot] [--no-env]` | Long sessions and their memory: a streamed city crossed for minutes with continuous destruction, fires and water (or a bounded level shot at), printing the world's memory by kind (the environment systems included), the change archive, forgotten regions and the process's physical footprint. |
| `svx_stream_bench`, `svx_wad_textures` | Streaming cost of the city; WAD graphics. |

Diagnostics of the core (printing only, never changing results; compiled out with
`SVX_NO_DIAGNOSTICS`): `SVX_DEBUG_BODY` (piece checks), `SVX_PROFILE`, `SVX_PROFILE_TICK`,
`SVX_PROFILE_COLLIDE`, `SVX_PROFILE_FRACTURE`, `SVX_DEBUG_DESIGN`, `SVX_AMG_INFO`.

## Layout

```
core/      svx_core: the destruction physics (docs/CORE.md). Depends on the standard library only.
  include/svx/world/world.hpp   svx::World, the public C++ API
  include/svx/svx_core.h        the C API
  base/      types, vectors, deterministic parallel pool, diagnostics
  material/  the material registry (strengths, fracture energies, rubble sizes)
  world/     voxel grid, chunk sources, World (structures, pieces and their fracture, streaming,
             persistence, queries)
  frag/      fragments (pre-scored rubble pieces per chunk)
  solve/     smoothed-aggregation multigrid, PCG
  stress/    fragment-graph stress problems, bond failure checks
  phys/      rigid voxel bodies: contacts, solver, sleep
  capi/      the C API
mesh/      svx_mesh: chunk, piece, far-tile and water meshing for renderers
env/       svx_env: fire, smoke, water on the core's extension points (docs/ENV.md)
game/      svx_game: the prototype game harness
  game.hpp   svx::Game: viewer, movers, triggers, command log, piece meshes and poses, far tier
  procgen, city, columns, dmath, replay; doom/ (WAD reader, voxelizer, textures, specials,
  movers); api/ (the web worker's flat C ABI)
examples/  minimal hosts of the core (C++, C)
tools/     command-line tools (game level runs, replays, map checks, benches), WASM modules
tests/     core/ (links svx_core only), env/ (svx_env) and game/ doctest suites
web/       TypeScript + Vite front end: worker host, WebGPU renderer, FPS sandbox
docs/      the core guide, design, game API, v1 history
```
