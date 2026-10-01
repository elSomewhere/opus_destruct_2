# Rendering lab

This is the running measurement and decision log for renderer experiments. The generated world
is an input to this lab, not an output: renderer changes must leave all voxel goldens unchanged.

## A. Baseline and capture harness

### Fixed viewpoints

The viewer's **Places → Render lab viewpoints** section exposes eight stable links. Each link
contains its preset, seed and complete `getView` camera in the URL. The cameras live in
`src/viewer/renderLab.js`, in metres and radians, rather than depending on a generated point-of-
interest search.

| ID | Preset | Subject |
| --- | --- | --- |
| `dense-street` | angled infinite city | downtown facades at eye level |
| `pitched-street` | angled old harbour | planar pitched road part against the terrain |
| `angled-junction` | angled old harbour | pitched slabs, kerbs and stepped world-grid ground |
| `turned-building` | angled cities | a building in its own oriented lattice |
| `hillside` | angled Nordic fjord | terrain terraces, vegetation and a pitched street |
| `highway-ramp` | angled cities | highway deck, ramp and ground transitions |
| `skyline` | angled infinite city | distant LODs, fog and skyline |
| `interior` | angled cities | near geometry, baked AO and indoor dimming |

### Measurements

The always-visible HUD now reports CPU frame time, GPU frame time when
`EXT_disjoint_timer_query_webgl2` is available, draw calls and visible triangles. The Debug tab
also reports Three.js geometry count, approximate bytes in visible vertex/index buffers, and
worker mesh time, vertices and triangles per chunk at every LOD. The approximation deliberately
does not claim texture, render-target or driver allocation as measured GPU memory.

The A/B tool stores the current display controls into slots A and B. **Capture A + B** renders
the same camera twice, downloads both canvas PNGs, then downloads JSON containing the URL, camera,
settings and renderer snapshots. The previous live settings are restored afterward. Classic
greedy rendering and the existing display defaults remain unchanged.

### Baseline run status (2026-10-01)

The instrumentation, cameras and capture path are in place. This build container has no Chrome,
Chromium or Firefox executable. Downloading Playwright/Chromium is blocked by the environment's
HTTP 403 policy, so honest WebGL frame/GPU numbers and viewer screenshots cannot be recorded here.
Run each fixed link on the target GPU after its queues reach zero and use **Capture A + B**; paste
the resulting JSON rows and PNGs into this section. Headless generator timings are not substituted
for GPU results because they would be misleading.

## Proposed work order

1. **Mesher selection contract.** Put the current greedy mesher behind a worker-safe selector,
   with `greedy` as the default and URL/UI controls. Extend measurement records with the mesher id.
2. **Surface nets as the dual-contour scaffold.** Establish float positions, smooth normals,
   deterministic cell ownership, material selection, chunk-border tests and oriented-part support
   before adding the more delicate QEF solve.
3. **Sharp-feature dual contouring.** Add Hermite intersections and a clamped deterministic QEF;
   preserve hard building/kerb features by material class, and retain greedy faces where the
   smooth/sharp boundary cannot be made watertight.
4. **Marching cubes.** Implement a consistent ambiguity policy and compare binary, filtered and
   distance-derived fields. This follows DC because MC is expected to round architectural corners
   more aggressively, but remains valuable for terrain/rubble and as an independent reference.
5. **LOD transitions and transparent surfaces.** Add skirts or transition cells, then cover water,
   glass and small arbitrary part meshes before judging a mesher complete.
6. **Look foundation.** Add a separate, serializable look config with `classic` as the default;
   start with tone mapping/exposure, PBR roughness classes, height fog and triplanar detail because
   each maps directly to `world.wgsl` shader maths.
7. **Highest-value named looks.** Build `bleak overcast`, `rain at dusk`, `hard noon`, `fog`,
   `wet night` and `winter grey`; add wetness/puddles and rain first, then AO/post effects and
   optional screenshot-only depth of field.

For the eventual structvox port, mesher work targets `mesh/src/mesher.cpp`, including shape and
coarse paths, and must map deliberately to the 28-byte vertex contract. Shader-only look work maps
to `world.wgsl`; Three.js post-processing and shadow machinery will require separate WebGPU passes.

## B. Pluggable meshers

The world panel offers `greedy`, `marching-cubes`, and `dual-contour`; `?mesher=` records the
selection. Greedy remains the default. All implementations are pure worker functions over the
padded chunk and use no Three.js or DOM APIs. Smooth opaque payloads deliberately use `f32`
positions with the existing normalized `i8` normal, colour, aux, and index attributes. Structvox's
28-byte vertex already has `f32` positions and `i8` normals, so this representation ports without
a position conversion.

`marching-cubes` currently uses the standard six-tetrahedra decomposition of each binary cube.
That makes ambiguous faces deterministic without a large case table. It is watertight and useful
as a deliberately rounded reference, but emits substantially more triangles than classic greedy
faces and should not be the architectural default.

`dual-contour` is the surface-nets stepping stone: it averages Hermite edge crossings into one
clamped vertex per active cell, then joins the four cells around every sign-changing edge. The
next quality step is a bounded QEF and material-class feature planes; until that lands, it rounds
kerbs and thin walls too readily. Transparent water/glass retain the proven greedy pass. LOD 1+
retains greedy skirts, providing a conservative transition closure while smooth extraction is
evaluated at the near tier. Oriented parts pass through the same selector in their own lattices.

Tests cover empty/full chunks, deterministic typed-array output, bounded vertices and ownership
at a shared chunk edge. These are algorithmic checks; visual crack and error measurements still
belong in the fixed-view capture run on a browser-capable target.

## C. Look presets

The View panel and `?look=` select seven independent looks. `classic` is byte-for-byte the old
display path: no tone mapper, no grade, no wetness, no grit, and no particles. The comparison set
is `bleak overcast`, `rain at dusk`, `hard noon`, `dense fog`, `wet night`, and `winter grey`.
The A/B slots now include the look id as well as display options.

The look shader adds switchable desaturation, deterministic lattice-space grime, wet darkening,
and fine film grain. AgX/ACES exposure uses Three.js tone mapping. Fog density and the sun/ambient
balance are preset parameters. Rain presets add a deterministic camera-centred particle field;
wetness supplies the surface response. The grade, grime, wetness and fog maths map directly to
`world.wgsl`; tone mapping and particles need WebGPU render-pipeline equivalents. No look value is
read by generation, collision, or physics.

Deferred high-cost experiments are cascaded shadows, screen-space AO, volumetric shafts, cloud
rendering, puddle geometry/ripples, bloom, LUT loading, depth of field, and chromatic aberration.
They should be added only after the baseline run identifies enough GPU headroom; bundling all of
them before measuring would violate the lab's measure-first rule.
