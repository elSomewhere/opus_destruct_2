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

Structures need not follow the voxel lattice. **Oriented grids** give a structure a lattice of
its own, at any position and rotation and of any voxel size: a building at an angle, a diagonal
brace, a tilted ramp. They are simulated like the world grid in every respect, bonded to it and to
each other where they meet, with no voxel stepping ([`docs/GRIDS.md`](docs/GRIDS.md)).

Things can move by design, and nothing moves that the physics does not move. **Joints** hold
pieces to structures (hinges, sliders, ropes, rods, welds) and give way; a hinge's or a slider's
**drive** moves it on a program. A machine - a lift's car, a turntable, a drawbridge, a crane's
jib - is pieces on driven joints: what rides on it is carried, its load goes into what holds it,
and when that is shot away it comes down; a wrecking ball swings on a crane's rope into a wall
([`docs/MOTION.md`](docs/MOTION.md)).

- **Video:** [`docs/media/tower_collapse.mp4`](docs/media/tower_collapse.mp4): the tower losing its
  two west rows of ground columns (`svx_engine_demo --world tower --scenario pillars`, CPU
  renderer).
- **The physics as a library:** [`docs/CORE.md`](docs/CORE.md). The destruction physics
  (`svx_core`: `svx::World`, and a C API in `svx/svx_core.h`) knows nothing of the game: no
  rendering, players or levels. It is reusable on its own, from C++, C (or any language with a C
  FFI) and JavaScript (a core-only WASM module). The game harness (`svx_game`) is one host of it,
  and the two iterate separately. Where it trades quality for time or memory, the trade is a
  tunable ([`docs/CORE.md`](docs/CORE.md) §9).
- **Method:** [`docs/V2_DESIGN.md`](docs/V2_DESIGN.md). In short:
  - Voxels form pre-scored rubble **fragments**, joined by **bonds**.
  - Every standing structure and every falling piece gets its stress from the same elastic
    equilibrium solve on its fragment graph (multigrid PCG).
  - Bonds fail by fibre-stress checks: tension, flexure, crushing, Mohr–Coulomb shear.
  - Falling pieces are rigid bodies that keep their bonds and break on impact, progressively,
    part by part.
- **Structures off the lattice:** [`docs/GRIDS.md`](docs/GRIDS.md): oriented grids, the
  junctions that bond them, priority and displacement, voxel sizes, their persistence,
  streaming and API.
- **Joints and machines:** [`docs/MOTION.md`](docs/MOTION.md); articulations (bodies of linked
  parts with anatomical joints and muscles: people, creatures, robots) in §6.
- **Characters:** [`docs/ANIM.md`](docs/ANIM.md). `svx_anim`: voxel people with a motion plan, a
  physical body and the behaviours between them (balance, stagger, bracing, falls, getting up,
  dying), simulated on their own (shallow) or as articulations of the world (deep: a car that
  hits one hits a body), or both by distance (hybrid). The drive city has pedestrians on its
  sidewalks, streamed with the city.
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
./build/native-release/tests/svx_anim_tests       # characters, on both of their physics paths
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

- **Worlds:** `?world=rooms|city|tower|yard|angles|machines&seed=N`, or a Freedoom WAD via the
  panel (doors, lifts, floors, platforms, crushers and stairs work).
  - `city`: a streamed 1 km² city, with timber floors in a third of its buildings, some
    buildings turned (in oriented grids), and ponds.
  - `yard`: one of each construction: a timber house, a stone tower, a greenhouse, a steel
    shed, a reinforced wall, a reservoir and a timber water tower.
  - `angles`: structures in oriented grids: a turned tower, a diagonal bridge deck, a ramp, a
    cross-braced steel portal, turned masonry walls, crates and a leaning monolith.
  - `machines`: machines on driven joints and hanging parts: a lift (ride it), a turntable with
    crates (ride it), a drawbridge, a crane swinging a wrecking ball into a wall, a pendulum, a
    chain, a hinged door - all held by structures you can shoot away.
- **Controls:** WASD move, mouse look, Space jump, Shift run; 1 to 5 or the wheel select pistol,
  shotgun, rocket launcher, flamethrower, water hose, click fires; E uses, V toggles noclip, R respawns; G cycles debug
  views (bond utilization, fragments); H toggles the HUD and its tick timeline; Esc opens the
  menu: fragility, impact and dynamic-factor sliders, gravity and the piece budget, and the
  environment (fire and its spread, wood's burn time, smoke and its lifetime, wind, water flow,
  pressure and buoyancy). Every engine and environment setting is also settable by name
  (`setEnv` / `setTunable`, recorded in replays).
- **Checks** (dev server running):
  - `npm run typecheck && npm test` for the front end.
  - `node scripts/smoke-wasm.mjs http://localhost:5190/`: the browser smoke test (with
    `people-wasm.mjs` and `drive-wasm.mjs`, CI's browser job: nightly, and for a push whose
    message says `[browser]`; `SMOKE_SWIFTSHADER=1` runs WebGPU in software without a GPU, as
    root in a container it does by itself).
  - `node scripts/tower-wasm.mjs http://localhost:5190/ OUT 25`: blasts the tower's ground
    columns in the browser and records screenshots and engine stats.
  - `node scripts/fire-wasm.mjs http://localhost:5190/ OUT 60`: sets the yard's timber house
    on fire, then hoses it down.
  - `node scripts/water-wasm.mjs http://localhost:5190/ OUT`: breaches the yard's reservoir.
  - `node scripts/angles-wasm.mjs http://localhost:5190/ OUT`: the `angles` world. It checks
    that the turned structures draw, that a box stops at the 45° wall (exactly where the turned
    cubes are) and the player stands on the ramp, then blasts them.
  - `node scripts/machines-wasm.mjs http://localhost:5190/ OUT`: the `machines` world. It checks
    that the pieces and the ropes draw and reach the client's collision, and that the player
    rides the lift and the turntable.

## Tools (`build/native-release/tools/`)

| Tool | Purpose |
|---|---|
| `svx_engine_demo --world tower\|rooms\|slab\|chimney\|bridge\|yard\|angles\|machines\|city\|drive [--scenario S] [--seconds T] [--threads N] [--turn DEG] [--turned-city] [--tune NAME=VALUE ...] [--frames DIR --fps F --res WxH --cam x,y,z --look x,y,z]` | Headless scenario run with a CPU renderer for frames. Tower scenarios: `pillars` (both west rows of ground columns), `side`, `core`, `all`, `rockets`. `--turn DEG` stands a procedural world's structure in a grid turned about the vertical; `--turned-city` streams the city with turned buildings; `drive` is the drive city with its traffic and people. `--tune` sets a world tunable before the load. Reports pieces, breaks, per-phase rigid costs, awake speeds, piece sizes and the session and world hashes (`SVX_TRACE_HASH=1`: each tick's; `SVX_TRACE_EVENTS=T`: the cracks and pieces from tick T). |
| `svx_replay record\|play --world W --seconds S [--out F \| --log F] [--threads T]` | Records and replays sessions from command logs; checkpoint hashes are the determinism check. |
| `svx_map_check [--threads T] [--movers] WAD...` | Imports, bakes and design-checks every map, then runs it idle. |
| `svx_soak [--world city\|drive\|tower\|rooms\|yard] [--wad F --map M] [--minutes M] [--archive-mb MB] [--forget-s S] [--no-shoot] [--no-env] [--people N] [--check]` | Long sessions and their memory: a streamed city crossed for minutes with continuous destruction, fires and water, the drive city with its traffic and people shot at (or a bounded level shot at), printing the world's memory by kind (the environment systems and characters included), the change archive, forgotten regions and the process's physical footprint. `--check`: exit status 1 if the second half of the session held more than the first half's peak allows. |
| `svx_env_bench [--scenario fire\|flood\|city\|all] [--threads T] [--repeat N] [--slow MS] [--tune NAME=VALUE ...] [--archive-mb MB] [--budget-ms MS]` | Deterministic environment scenarios, timed: the yard's timber house burning, the reservoir breached, the streamed city crossed with fires and water. Prints the tick cost (mean, 99th percentile, max), the environment's share and the session and world hashes (an optimization that changes no result keeps every hash); `--slow` breaks down the slow ticks by phase; `--budget-ms`: exit status 1 if a scenario's mean tick is slower (CI's performance gate). |
| `tools/baseline/golden.sh DIR`, `tools/baseline/compare.sh REF NEW [--parity]` | The behavioural baseline ([`docs/BASELINE.md`](docs/BASELINE.md)): pinned world hashes (the engine's, and the structural reference's reproduced with its switches); two builds compared scenario by scenario. |
| `svx_people_bench [--seconds S] [--speed M/S] [--counts 24,48,96] [--policies deep,shallow,hybrid] [--threads T]` | The drive city's people: the viewer drives through the city with its traffic; per way of simulating the bodies and crowd size, the tick and the characters' part (mean, 95th percentile, worst), the bodies' split, the people made and gone, the memory. |
| `svx_stream_bench`, `svx_wad_textures` | Streaming cost of the city; WAD graphics. |

Diagnostics of the core (printing only, never changing results; compiled out with
`SVX_NO_DIAGNOSTICS`): `SVX_DEBUG_BODY` (piece checks), `SVX_PROFILE`, `SVX_PROFILE_TICK`,
`SVX_PROFILE_COLLIDE`, `SVX_PROFILE_FRACTURE`, `SVX_DEBUG_DESIGN`, `SVX_AMG_INFO`.

## Layout

```
core/      svx_core: the destruction physics (docs/CORE.md). Depends on the standard library only.
  include/svx/world/world.hpp   svx::World, the public C++ API
  include/svx/svx_core.h        the C API
  base/      types, vectors, deterministic math and parallel pool, diagnostics
  material/  material tables (strengths, fracture energies, rubble sizes): the process's, a world's
  world/     voxel grids (the world grid, oriented grids and their junctions), chunk sources,
             World (structures, pieces and their fracture, joints and machines,
             streaming, persistence, queries)
  frag/      fragments (pre-scored rubble pieces per chunk)
  solve/     smoothed-aggregation multigrid, PCG
  stress/    fragment-graph stress problems, bond failure checks
  phys/      rigid voxel bodies: contacts, joints, solver, sleep
  capi/      the C API
mesh/      svx_mesh: chunk, piece, far-tile and water meshing for renderers
env/       svx_env: fire, smoke, water on the core's extension points (docs/ENV.md)
anim/      svx_anim: characters (docs/ANIM.md) - voxel models, the motion plan, the physical body
           (its own XPBD system, or an articulation of the world), behaviours, the character and
           its system, brawls
game/      svx_game: the prototype game harness
  game.hpp   svx::Game: viewer, movers, triggers, command log, piece meshes and poses, far tier,
             vehicles and traffic, pedestrians
  level.hpp  a level as the game loads it; materials (the game's smeared sections), paint,
             columns, replay; doom/ (WAD reader, voxelizer, textures, specials, movers)
  api/       svx_api: the web worker's flat C ABI (it picks the levels)
procgen/   svx_procgen: procedural generation, to be replaced - the test levels, the streamed
           city and the endless drive city (levels.hpp, city.hpp, drive_city.hpp)
examples/  minimal hosts of the core (C++, C)
tools/     command-line tools (game level runs, replays, map checks, benches), WASM modules
tests/     core/ (links svx_core only), env/ (svx_env), anim/ (svx_anim) and game/ doctest suites
web/       TypeScript + Vite front end: worker host, WebGPU renderer, FPS sandbox
docs/      the core guide, design, grids, motion and wheels, damage, environment, characters,
           vehicles, game API, the behavioural baseline, v1 history
```
