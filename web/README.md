# structvox web front end

TypeScript + Vite + WebGPU front end for structvox (plan §B9, Phase 4/5). It runs today against a
pure-TypeScript **mock engine worker** that implements the engine protocol
([`docs/API.md`](../docs/API.md)). The real C++→WASM worker drops in behind the same messages.

## Run

```bash
cd web
npm install
npm run dev          # http://localhost:5173 (COOP/COEP headers on, see below)
npm run build        # typecheck + production bundle in dist/
npm run preview      # serve dist/ (also with COOP/COEP)
npm run typecheck    # tsc for app, workers, tests and vite config
npm test             # unit + end-to-end tests of the mock engine (node --test, no extra deps)
```

Browser smoke test (headless Chrome with WebGPU; fails on any console, page or WebGPU error):

```bash
npm i --no-save puppeteer-core
npm run dev -- --port 5190 &
npm run smoke -- http://localhost:5190/ smoke-out     # screenshots in smoke-out/
```

URL parameters: `?engine=mock|wasm` (default `mock`), `?world=rooms|city|tower`, `?seed=N`,
`?debug=none|utilization|bubbles`.

Controls: click the view to lock the pointer, WASD move, mouse look, Space jump, Shift run,
1/2/3 or wheel for pistol/shotgun/rocket launcher, left click fire, G cycles the debug view,
V noclip, R respawn, H toggles the HUD, Esc shows the settings panel.

### Cross-origin isolation

`vite.config.ts` sends `Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp` and `Cross-Origin-Resource-Policy: same-origin`
in dev and preview, so `crossOriginIsolated === true` and `SharedArrayBuffer` (WASM threads)
work. Production hosting must send the same headers. With `?engine=wasm` the app warns if the
page is not isolated.

## Architecture

```
src/
  main.ts               bootstrap: WebGPU, engine worker, game; graceful fatal screens
  engine/
    protocol.ts         the message contract as TS types + vertex-format constants (shared by
                        main thread and workers; no DOM/worker-only APIs)
    vertex.ts           28-byte vertex writer/reader, bounds (shared)
    client.ts           EngineClient: owns the Worker, typed send/on, promise raycast/collide by id
    select.ts           ?engine=mock|wasm -> worker; discovers src/worker/*-worker.ts by glob
  worker/
    host.ts             typed postToMain (with transfer lists) + serveCommands (errors -> 'error')
    mock-worker.ts      mock engine entry: command loop + fixed 60 Hz tick
    mock/               the mock engine (reference implementation of the protocol)
      engine.ts         commands, edits, support checks, bubbles/wobble, meshing queue, stats
      world.ts          32^3 chunked block storage, column tops
      procgen.ts        'rooms', 'city', 'tower' worlds
      mesher.ts         greedy / culled-face mesher with AO -> protocol vertices
      raycast.ts        Amanatides-Woo DDA
      collide.ts        axis-separated AABB sweep
      connectivity.ts   lowest-first flood fill -> detached islands
      blocks.ts, textures.ts, wad.ts   block palette, procedural textures, WAD directory check
  render/
    gpu.ts              adapter/device/context, device-lost + uncaptured-error reporting
    renderer.ts         pipelines, frame/object uniforms, 4x MSAA, reversed-Z depth32float
    chunks.ts           chunk buffers by key (reused when data fits), bounds, frustum culling
    atlas-pack.ts       texture atlas packing (wrapped gutters, 8-aligned, linear-space mips)
    atlas.ts            atlas upload (rgba8unorm-srgb 2D array + per-texture storage records)
    islands.ts          detached islands: ballistic motion + dithered 1.5 s fade
    particles.ts        CPU particles, instanced camera-facing sprites
    math.ts             column-major mat4, reversed-Z infinite projection, frustum planes
    shaders/*.wgsl      frame uniforms, world, sky, particles
  game/
    game.ts             message wiring + frame loop + debug handle (window.__structvox)
    player.ts           FPS controller (z up) on engine `collide`
    stepmove.ts         step-up built from plain `collide` sweeps
    weapons.ts          pistol/shotgun (raycast -> carve), rocket (look-ahead raycasts -> blast)
    effects.ts          particles for hits/cracks/impacts/detachments, flash light, camera shake
    input.ts            keyboard/mouse, pointer lock
  ui/                   HUD, settings panel (setParams, world + WAD loading), overlays
test/                   node --test suites (mock engine, step-up, atlas packing)
scripts/smoke.mjs       headless Chrome smoke test
```

Frame loop (main thread): input → player (`collide`, at most one move in flight; elapsed time is
folded into the next move) → weapons (`raycast`/`carve`/`blast`) → effects → render → HUD.
`viewer` is sent every second frame. The worker owns the simulation clock.

Rendering: one 4x MSAA pass into the sRGB view of the canvas (linear shading), reversed-Z
`depth32float` (clear 0, compare `greater`, infinite far plane). Sky first, then chunks
front-to-back after frustum and distance culling, then islands (per-object dynamic uniform
offsets), then particles (premultiplied alpha, additive when flagged).

## Protocol notes (how the front end reads docs/API.md)

* Messages are objects discriminated by a string `type` (`kind` is already used by
  `loadProcedural` and events). Bulk ArrayBuffers are transferred; `protocol.ts` exports
  `workerMessageTransferables` / `commandTransferables` (deduplicated transfer lists).
* **Vertex format** (28 B): the shader reads offset 24 as one `uint32`
  (`texture | light << 16 | debug << 24`, little-endian), because single-component `uint16` /
  `uint8` vertex formats are not universally available in WebGPU. Positions are world metres.
  Indices are `uint32`, CCW from outside; back faces are culled.
* **Textures:** ids index the `textures` list; texcoords are texels and wrap per texture in
  the fragment shader. Textures are packed into a multi-page atlas with wrapped gutters so
  mipmapped filtering is seamless (`lodMaxClamp = 3`, nearest magnification for the Doom
  look). `rgba` is RGBA8 sRGB, row 0 at the top. Id `0xFFFF` renders the untextured default
  colour. Meshes may arrive before `textures`; they render untextured until it does.
* **Light** (Doom sector light) scales brightness roughly as `level^2` with Doom-like
  diminishing in dark sectors, plus hemisphere/sun terms, AO (`normal.w`) and exponential fog.
* **Debug byte:** `debugView` 0 = none, 1 = utilization (0..255 heat map),
  2 = bubble level (0 = no bubble, 1 + L for level L). Changing `debugView` relies on the engine
  re-sending meshes.
* **Detached events:** the mesh is in world space at the moment of detachment; the island
  rotates about `centroid` with `angular` (rad/s, world frame) and falls with `velocity` and
  g = 9.81 m/s² (no collision), fading out over 1.5 s with dust.
* **Spawn:** `ready.info.spawn.pos` is the player's feet position; the eye is 1.6 m above.
* **Collide:** the player box is 0.6 × 0.6 × 1.75 m. Step-up (0.55 m) is done client-side
  with extra plain `collide` sweeps (up, across, down), so it needs nothing beyond the API.
* **setParams** always carries the full parameter set.
* **Blast energy:** joules; the rocket sends 1e6 J with a 1 m radius. Pistol carve 0.15 m,
  shotgun 8 pellets × 0.12 m.

Optional front-end extensions (an engine that never uses them works unchanged):

| extension | direction | purpose |
|---|---|---|
| `{type:'error', message, fatal, command?}` | worker → main | toasts; `fatal` stops the game and shows a fatal screen |
| `{type:'progress', stage, done, total}` | worker → main | loading bar |
| texture ids `0xFF00 + material` | vertices | untextured with that material's palette colour (the mock uses this; `0xFFFF` stays the default) |

## What the mock does vs. the real engine

| | mock (`src/worker/mock`) | real engine (C++ core → WASM) |
|---|---|---|
| worlds | procedural rooms / city / tower, 0.5–1.7 M voxels | procedural + voxelized Doom maps (`loadWad`) |
| structure | anchored bedrock; flood fill after each edit detaches unsupported pieces | RBSM lattice, damage law, composite telescoping bubbles, hierarchical connectivity |
| meshes | greedy per 32³ chunk with AO; culled-face while displaced | greedy chunk meshes, crack remesh, displaced chunks |
| displacement | fake damped sag/sway in a sphere around each blast, re-sent at 20 Hz | amplified Δu from bubble solves, at most once per tick |
| utilization / bubbles | heuristics (load above × slenderness, crater damage; spherical shells) | bond utilization; real bubble levels |
| cracks / impacts | crater-rim cracks; blast impact; virtual landing impact of islands | bond ruptures; blast + virtual debris impacts |
| `loadWad` | validates the WAD and map, sends an `error`, loads 'rooms' instead | full WAD pipeline (plan §B10) |
| stats | protocol fields + extras (`meshQueue`, `islands`, ...) | protocol fields + engine extras |

## WASM worker integration

* Create **`src/worker/wasm-worker.ts`**. Any `src/worker/<name>-worker.ts` is picked up by the
  glob in `engine/select.ts` and bundled as a module worker; `?engine=<name>` selects it. While the
  file is missing, `?engine=wasm` shows "The WASM engine is not built yet".
* Implement exactly the messages of docs/API.md using the types in `engine/protocol.ts`, and use
  `worker/host.ts`: `serveCommands(handler)` validates commands and reports exceptions as
  `error` messages; `postToMain(msg)` transfers mesh/texture/event buffers.
* Reply to every `raycast` / `collide` with the same `id` (the client's promises wait for it),
  and after `loadProcedural` / `loadWad` send `textures` (if any), `ready`, then `chunkMeshes`.
  When a new world replaces an old one, send `chunkRemoved` for the old chunk keys (the client
  also clears its chunk store when it requests a load).
* If the Emscripten module cannot be loaded, post `{type:'error', fatal:true, message}` (or let
  the worker fail to load; the client turns a worker `error` event into a fatal screen).
* Loading the module: an `-sEXPORT_ES6 -sMODULARIZE` build can be imported from the worker. The
  dev server allows files from the repo root (`server.fs.allow: ['..']`), so importing from
  `../build/...` works in development; for production copy the `.js/.wasm` into `web/public/`.
  Workers are ES modules (`worker.format: 'es'`), which pthread builds need.
* Vertex data from `svx_poll_meshes` can be posted as-is if it follows the 28-byte layout.

## Known gaps

* Displacement is applied by re-sent meshes only; the planned shader-side interpolation of node
  displacements (plan §B9) is not implemented.
* No crack decals yet (cracks spawn particles), no weapon view model, no audio.
* Draw calls are one per visible chunk; render bundles or multi-draw would help at 10⁷ voxels.
* The mock never streams (`viewer` only orders meshing) and does not voxelize WADs.
* Pointer lock needs a real user click; the automated checks drive the game through
  `window.__structvox` (`fire`, `select`, `look`, `teleport`, `setDebugView`, `load`, `state`).
