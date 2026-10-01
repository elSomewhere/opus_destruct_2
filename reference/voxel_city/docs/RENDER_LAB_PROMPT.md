# Render lab: new meshers and a grittier look for voxel_city

You are working in `voxel_city` (`/Users/estebanlanter/Coding/voxel_city`), a procedural voxel
world engine in JavaScript with a Vite + React + Three.js viewer. Your job is to turn its viewer
into a **rendering lab**. Add new surface meshers (marching cubes and dual contouring first),
then experiment widely with the look. The goal is a realistic, gritty, bleak city that can also
be rainy or sunny, with a large set of options to explore.

Read this whole brief before you change anything.

---

## 1. The long-term goal, and what you must not touch

This procedural generator will eventually be merged into **structvox**, a C++20 structural
destruction physics engine with a WASM + TypeScript/WebGPU front end:
<https://github.com/elSomewhere/opus_destruct_2/tree/merge_anim_2> (branch `merge_anim_2`; the
merge guide in this repo is written against commit `a8cf8bd`). The final product will use
structvox's physics. This repository is where we iterate on how that product should *look*.
Techniques that win here will later be ported into structvox's mesher and shaders.

**Do not do anything in the `opus_destruct_2` repository.** Do not clone it into this
workspace, build it, branch it, commit to it, push to it, or open issues or pull requests there.
Do not run `scripts/svx-harness/run.sh`, which builds it. The merge is not happening now. If you
need a fact about its renderer, rely on §3 below. If something essential is missing, you may read
individual files from GitHub read-only, and you must tell the user that you did.

Also leave the merge contract in this repository alone. Do not change the structvox export or
its tests unless the user asks:

- `src/engine/svx/` (`source.js`, `materials.js`, `roads.js`, `worker.js`)
- `docs/MERGE_SVX.md`
- `test/svx.test.js`, `test/svxRoads.test.js`

---

## 2. This repository today

Read these first: `docs/ARCHITECTURE.md`, `docs/MERGE_SVX.md` (especially §1–§5 and §10), and
`ANGLED_WORLD_PLAN.md` §1–§4. Then read the render pipeline:

| File | Role |
| --- | --- |
| `src/engine/voxel/chunk.js` | chunks are 32³, stored padded to 34³ (a 1-voxel apron); voxels are 12.5 cm |
| `src/engine/voxel/materials.js` | ~500 materials: sRGB colour, emissive, glow, noise amplitude, opacity |
| `src/engine/voxel/mesher.js` | `meshChunk(data, opts)`: greedy face mesher with baked per-vertex AO, opaque + transparent passes, LOD skirts. Vertex: `Uint8` local position, axis normal, material RGB, aux = [ao, emissive, noise, opacity] |
| `src/engine/stream/worker.js` | builds a column tile's chunks at a LOD and meshes them (`meshChunk`); in parts mode, also meshes the oriented parts in their own lattices (`tileParts`) |
| `src/viewer/streamer.js` | LOD 0–9 quadtree tile streaming, mesh upload, collision bitsets, parts drawn with `partMatrix` |
| `src/viewer/voxelMaterial.js` | Three.js `MeshLambertMaterial` patched via `onBeforeCompile`: sRGB→linear, per-voxel value noise, AO, indoor dimming, night window glow |
| `src/viewer/ViewerEngine.js` | renderer, sun + hemisphere light, shadow map, exponential fog, time of day, `DISPLAY_DEFAULTS` (shadows, fog, resolution, fov, wireframe, debug tints), `getView` / `setView` |
| `src/viewer/App.jsx` | UI and presets; `?parts=grid` steps angled parts into the world grid instead of drawing them in their own lattices |
| `src/engine/config/presets.js` | world presets and viewer moods: `SEASON_ATMOSPHERE` (sky, fogDensity, sun, ambient, sunElevation, desaturate) and per-preset `viewer` settings |
| `src/viewer/controls.js` | walk / fly / orbit camera; the walker collides against voxel bitsets, not against meshes |
| `scripts/render-iso.js` and others | headless renders (raycast through chunks), useful for comparisons |

Key facts:

- **The generator is a pure function of `(config, structural key)`.** Same config, same voxels,
  in any worker, in any order. Rendering must never change generated voxels.
- **Angled world.** Angled presets set `world.angles.enabled`. Turned buildings, wings and
  pitched road runs are *oriented parts*: separate voxel lattices placed by an exact rotation
  matrix. Drawn in their own lattice, they appear as straight walls and planar slopes. The world
  grid (terrain, junctions, curves, highways, off-table grades) still shows 12.5 cm steps. Those
  stepped surfaces are the main thing a smooth mesher should fix.
- Water and glass are transparent voxels in the transparent pass. Flora is solid voxels here.
- The dev server is `npm run dev`; the user may already have it running.

---

## 3. What structvox's renderer is like (do not edit; design with it in mind)

From `merge_anim_2` at `a8cf8bd`:

- **The mesher is C++, separate from physics:** `mesh/src/mesher.cpp`,
  `mesh/include/svx/mesh/mesher.hpp`. The physics core never meshes.
  - `mesh_chunk`: greedy faces with per-vertex AO for static chunks. A v1 "displaced" path emits
    one quad per face and moves each corner by the average displacement of the solid voxels
    sharing it. v2 sends no displacement, so the world renders as cubes.
  - `mesh_shape`: a falling rigid piece's voxels, in the piece's own frame.
  - `mesh_water`: a separate water surface from a fill-level layer.
  - `mesh_coarse`: greedy coarse blocks for the far tier (no AO, no textures).
- **Vertex contract (28 bytes, `docs/API.md`):** `f32` position (metres), `i8` normal xyz + AO,
  `f32` uv (texels), `u16` texture id (atlas, or `0xFF00 + material id` for plain colour),
  `u8` light, `u8` debug.
- **WebGPU front end:** `web/src/render/` (`renderer.ts`, `chunks.ts`, `grids.ts`,
  `shaders/world.wgsl`, `water.wgsl`, `sky.wgsl`, `particles.wgsl`, ...).
  - `ChunkStore` uploads and replaces chunk meshes.
  - `GridRenderer` draws each oriented grid's chunk meshes with that grid's model matrix.
  - The world shader samples a texture atlas and adds per-voxel variation in the object's own frame.
- **Meshes change all the time.** Destruction, cracks, fire charring and water remesh chunks
  and pieces while the game runs, so per-chunk mesh time is a real budget.
- **Its inputs are voxel bytes.** The city reaches structvox as per-chunk occupancy plus a
  physics class, a `look` layer (from which the city material is recovered), and `flora` and
  `water` layers. Oriented parts become separate grids, each meshed in its own lattice. A world
  chunk and a grid chunk are never meshed together.
- Collision and physics stay voxel-exact. A smooth render surface may sit up to about half a
  voxel off the collision surface.

---

## 4. Ground rules for this work

1. **Voxels stay bit-identical.** Before and after every change, run `npm test`, `npm run lint`,
   `node scripts/golden.js --check` and `node scripts/golden.js --angled --check`. If a golden
   check fails, you changed the generator; revert that part. A wider chunk apron is allowed only
   if it is additional sampling around the chunk and every interior voxel is unchanged.
2. **Everything is switchable, and today's look stays the default.** Each mesher and each look
   feature is an option (display settings, a look config, and URL parameters), so A/B comparison
   is always one toggle away. The current greedy mesher and current materials stay available,
   unchanged.
3. **Meshers are pure functions of chunk data.** Same shape as `meshChunk`: padded voxel data
   (plus optional per-chunk options) in, typed arrays out. They run in the stream workers, with
   no Three.js and no DOM. Keep them portable to C++: plain loops and integer or float maths,
   no JS-only tricks the algorithm depends on.
4. **Mesh per lattice.** Mesh the world grid and each oriented part separately, as structvox
   will. A technique that only looks right when a part and the ground are meshed together will
   not survive the merge. Make the seams between lattices look acceptable instead.
5. **Seamless across chunks and LODs.** Chunk borders must not crack or double surfaces. LOD
   transitions must not open gaps (Transvoxel-style transition cells, skirts, or another
   approach you can justify).
6. **Measure, don't guess.** Get baseline numbers before changing anything, and re-measure each
   change. Report the numbers, not impressions.
7. Write code that reads like the surrounding code: same naming, module style, comment density.
8. Work on a new local branch from the current one (currently `angled_1`), e.g. `render-lab`.
   Commit in small, coherent steps. Do not push unless the user asks.

---

## 5. Workstreams

Work roughly in this order, checking in with the user between them. Within each workstream,
favour many small switchable experiments over one big rewrite.

### A. Baseline and harness (do this first)

- Pick 6–8 fixed viewpoints with `getView` / `setView`. Include:
  - a dense street at eye level
  - a steep pitched street
  - an angled-preset junction where pitched slabs meet stepped ground
  - a turned building
  - a hillside with terrain
  - a highway ramp
  - a skyline at distance
  - an interior
- Make it easy to reproduce them: URL parameters or a small viewpoint list in the UI.
- Record per view: frame time (CPU and, if available, GPU timer queries), draw calls, triangles,
  GPU memory, and screenshots. Record per chunk: mesh time in the worker and vertex/triangle
  counts per LOD.
- Add an on-screen stats overlay and a screenshot / A-B capture tool that saves the same view
  under two settings.

### B. Pluggable meshers

- Put a small mesher interface in front of `meshChunk` and select it by option. Keep the vertex
  layout or extend it deliberately (for example, `f32` positions or smooth normals). If you
  extend it, note how it maps to structvox's 28-byte vertex.
- **Marching cubes** on a density field derived from occupancy. Binary occupancy gives terraced,
  blobby results, so experiment with fields: filtered occupancy, a distance field over a wider
  apron, or per-material iso-levels. Handle ambiguous cases consistently.
- **Dual contouring** with Hermite data and a QEF solve (or a cheaper variant such as surface
  nets as a stepping stone). The city needs **sharp features**: walls, corners and kerbs must stay
  crisp while terrain, rubble, slopes and steps smooth out. Keep the solve deterministic and
  clamped to the cell.
- **Hybrid strategies**: smooth for terrain, roads, rubble and natural materials; greedy or
  sharp-feature DC for buildings. Decide per material or material class, without chunk-level
  seams between the two.
- Carry material and colour through. Rethink AO (baked AO assumes axis-aligned faces) and the
  per-voxel noise (it assumes voxel cells; consider triplanar noise in world or lattice space).
- Cover the transparent pass, oriented parts (`tileParts`), LOD tiles and skirts.
- Every mesher must also work on small, arbitrary voxel sets. In structvox, rubble pieces are
  meshed on their own (`mesh_shape`).
- Report, per mesher: per-chunk mesh time, triangle count versus greedy, visual quality at each
  viewpoint, crack-free borders, and estimated porting effort into `mesh/src/mesher.cpp`.

### C. The look: realistic, gritty, bleak, with weather

Build a **look system**: a look config (JSON) separate from world presets, combining weather,
time of day, grade and surface treatment. It should load from presets, a UI panel with sliders,
URL parameters, and saved files. Ideas to explore (not a checklist; prioritize what moves the
image most):

- **Lighting and tone:**
  - physically based materials (roughness/metalness per material class)
  - a proper tone mapper (AgX or ACES) with exposure
  - a colour-grading LUT and white balance
  - cascaded shadow maps or better shadow filtering
  - SSAO/GTAO on top of the baked AO
  - a better physically based sky with sun and moon
- **Atmosphere:**
  - height fog
  - aerial perspective
  - volumetric light shafts
  - low cloud and haze
  - overcast diffuse light
- **Weather:**
  - **rain:** particles or streaks, wet surfaces (darker albedo, lower roughness, specular
    highlights), puddles on flat up-facing surfaces, ripples, wet sheen ramping in and drying out
  - **sunny:** hard shadows, warm bounce, heat haze
  - fog, snow (the generator already has `snowCover`), dusk, night with sodium lamps and lit
    windows
- **Grit and wear:**
  - procedural grime driven by AO, height, up-facing and edge masks
  - streaks under windows and ledges
  - moss and damp at street level
  - soot and rust
  - cracked and stained concrete
  - triplanar detail and normal noise per material class
- **Post:**
  - bloom on emissives
  - film grain
  - vignette
  - subtle chromatic aberration
  - desaturated "bleak" grades
  - optional depth of field for screenshots

Create a set of named looks to compare, for example "bleak overcast", "rain at dusk", "hard
noon", "fog", "wet night", "winter grey", plus today's look as "classic".

For each look feature, note in the log whether it is plain shader maths (portable to
`world.wgsl`) or depends on Three.js machinery (post-processing passes, built-in shadow
pipeline). Three.js is fine for experimenting. The note is for the eventual port.

### D. Performance

As the smooth meshers and effects land, keep frame time in check:

- worker mesh time and parallelism
- triangle budgets per LOD (simplify smooth meshes at distance)
- shadow-map cost
- draw-call batching
- frustum and occlusion culling
- post-processing resolution scaling

Report before and after numbers at the fixed viewpoints.

---

## 6. Deliverables

- The meshers and look features in code, all switchable, with today's look still the default.
- A viewer panel and URL parameters for choosing mesher and look, a stats overlay, and a
  viewpoint/screenshot capture for A/B comparisons.
- Tests for each new mesher in the style of `test/core.test.js`:
  - empty and full chunks
  - deterministic output
  - no cracks along chunk borders (matching border vertices across neighbours)
  - watertight or bounded error against the voxel surface
- **`docs/RENDERING.md`**: a running log, updated as you go:
  - every technique tried, with screenshots and numbers at the fixed viewpoints
  - what worked and what did not
  - the recommended direction
  - a **merge section** listing, per adopted technique, what porting it to structvox would take
    (`mesh/src/mesher.cpp` and `mesh_shape` / `mesh_coarse` for pieces and the far tier, the
    28-byte vertex, `world.wgsl`), and any inputs it needs that structvox's chunk data
    (occupancy, physics class, `look`, `flora`, `water` layers, oriented grids) would not provide
- Passing `npm test`, `npm run lint`, and both golden checks at every commit.

Start with workstream A, show the user the baseline numbers and screenshots, then propose your
order for B and C before building them.
