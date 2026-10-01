# Angled World — Implementation Plan

Relax this engine's 90-degree constraint to a quantized set of exactly-representable rotations
(yaw, pitch, roll), so streets turn and climb naturally, buildings sit at varied angles, and
vegetation reads organically — while keeping the **physics** cost of angles bounded, and keeping
the whole feature switchable back to today's axis-aligned output.

This document is self-contained. It states the context, the verified facts it rests on, the
design, the work in order, and the acceptance criteria for each step. Everything in §3 and §4 was
verified by reading the code or by measurement; the numbers are reproducible.

---

## 1. Context

### 1.1 This repository (`voxel_city`)

A JavaScript (ESM) procedural voxel world engine with a Vite + React + Three.js viewer.

- **Voxels are 12.5 cm** (`src/engine/core/units.js`, `vx()`); chunks are 32³, stored padded to
  34³ with a 1-voxel apron (`src/engine/voxel/chunk.js`, `P = PCHUNK = 34`).
- World frame: **x east, y south** (north is −y), **z up**.
- LOD 0–9 quadtree streaming (`src/engine/stream/`), workers each build an independent `World`.
- **Everything is a pure function of `(config, structural key)`.** No shared mutable state, no
  neighbour communication. Randomness only via `Rng.from(seed, id, purpose)`. Two runs, or two
  workers, produce identical voxels. *This property is load-bearing and must survive every change
  in this plan.*
- 502 materials (`src/engine/voxel/materials.js`), greedy mesher with baked per-vertex AO
  (`src/engine/voxel/mesher.js`).
- Commands: `npm test` (node:test, 17 suites in `test/`), `npm run lint`, `npm run dev`,
  and the renderers/auditors in `scripts/` (`audit-fit.js`, `render-iso.js`, `render-map.js`,
  `render-sat.js`, `render-plans.js`, `render-trees.js`, `render-overview.js`, `render-slice.js`).
- Architecture reference: `docs/ARCHITECTURE.md`.

### 1.2 The destination (`opus_destruct_2`, branch `cars`)

<https://github.com/elSomewhere/opus_destruct_2/tree/cars> — "structvox v2", a C++20 structural
destruction physics engine with a WASM + TypeScript/WebGPU front end. This engine is eventually
to become its procedural generation layer, so **every design decision here is also a decision
about that merge.**

What matters for this plan:

- Also **0.125 m voxels, 32³ chunks**. One byte per voxel: 0 = air, low 7 bits = `1 + material`,
  **bit 7 = anchored**.
- **Anchored voxels are supports**: never fragmented, never simulated, never in a structure
  solve. "A bond exists between every pair of face-adjacent solid voxels **unless both are
  anchored**" (`core/include/svx/world/grid.hpp`).
- **Fragments** are pre-scored jittered-Voronoi rubble pieces and never break; **bonds** between
  them break by fibre stress. **Structures** are connected fragment sets reaching an anchor,
  solved `K u = f` by smoothed-aggregation multigrid PCG. **Pieces** are rigid bodies that keep
  their bonds and break progressively on impact.
- **Oriented grids** (`docs/GRIDS.md`): voxel lattices with their own origin, rotation and voxel
  size, bonded to the world grid and to each other by **junctions** (3×3 samples per face,
  pushed `junction_reach` = 0.5 voxels into the other grid). *Grids stand still; what moves is
  pieces.*
- Generation contract: `ChunkSource` in `core/include/svx/world/source.hpp` — `generate(chunk)`,
  `chunk_lo/hi`, `region(chunk)`, `generate_layer`, `grids(chunk)`, `generate_grid(id, out)`,
  `joints(chunk)`. `generate()` is "a pure function of the chunk: called from several threads at
  once" — the same purity rule this engine already follows.
- Defaults that constrain us (`core/include/svx/world/world.hpp`, verified unchanged on `cars`):
  `max_bodies = 3000`, `structure_max_nodes = 60000`, `structure_max_radius = 60.0` m,
  `cluster_nodes = 2500`, `design_utilization = 0.45`, `load_radius = 96` m,
  `evict_radius = 128` m, `chunks_per_tick = 6`, `archive_mb = 64`.
- `kMaxMaterials = 127`, `kStandardMaterials = 21` → **106 free material ids**.
- Determinism: bit-identical across thread counts, native/WASM and ARM/x86, with "no
  transcendental functions on the simulation path".

### 1.3 Goals

- Streets that turn, curve and climb at natural angles instead of only on the cardinal axes.
- Buildings placed at varied angles, optionally with angled wings, chamfers and canted bays.
- Forests, trees and scattered nature that read organically rather than on a visible lattice.
- Inclined road surfaces smooth enough to drive on — **no 12.5 cm stepping on graded roads**.
- A **bounded, measurable physics cost**.
- A config switch restoring today's axis-aligned output bit-for-bit.

### 1.4 Non-goals

- Arbitrary continuous rotation. A quantized set is deliberate: it makes the arithmetic exact
  (§4.1) and the cost predictable (§3).
- Curved surfaces. Everything stays a voxel lattice; only *placement* changes.
- Renderer optimisation. Angled geometry costs triangles (§6); the renderer is iterated
  separately and is expected to absorb it.

---

## 2. What is already angle-capable

Verified by reading this repository. Roughly half the work is already done, which is why the
phase order in §7 front-loads the cheap wins.

| Subsystem | Status | Evidence |
| --- | --- | --- |
| Road representation | **already arbitrary angle** | `road.pts` is a general polyline; `buildSegments` derives real unit directions `dx, dy` (`src/engine/network/roadView.js`) |
| Road rasterization | **already arbitrary angle** | per-column SDF, circular smooth-min carriageway union, sharp-min right-of-way, markings resolved in the dominant road's own frame (`src/engine/network/roadSurface.js`, `sampleRoadSurface`) |
| Bent roads | **already implemented** | `wobble()` in `src/engine/city/cellNetwork.js`, driven by `flavor.mainRoadWobble`; `edgeInfo` returns `wob` so blocks keep clear |
| Block clearance vs roads | **already generic** | `clearOfStreets` walks centrelines as `s.ax + s.dx * t`, sampling every 8 voxels (`src/engine/city/cellPlan.js`) |
| Highways | **already splines** | Catmull-Rom, resampled (`src/engine/network/highways.js`) |
| Vertical profiles / inclines | **already implemented** | grade-limited cut and fill, junction blending, embankments (`src/engine/network/roadLevel.js`); grades arterial 8%, collector 10%, local 12%, village 13%, alley 14%, lane 16% |
| Terraced pads and easing | **already implemented** | `src/engine/city/grading.js` (`APRON = vx(1.5)`, `EASE = 2.3`, `levelLot`) |
| Trees | **already off-lattice** | leaning, forking trunks from tapered capsules; `Math.cos(la) * lean`, arbitrary fork/elev/spread per species (`src/engine/nature/trees.js`) |
| Building frame | **locked to 4 rotations** | `src/engine/buildings/frame.js` — a `switch (this.front)` over `N/S/E/W` |
| Arterial lattice | **axis-aligned by construction** | `x = X(i)`, `y = Y(j)` (`src/engine/network/arterials.js`) |
| Blocks, lots, pads | **axis-aligned rects** | `src/engine/city/lots.js`, `src/engine/city/cellPlan.js`, `src/engine/city/grading.js` |

**The chokepoint is narrow.** `Frame` (106 lines) hard-codes four proper rotations in `toWorld`,
`fromWorld`, `dirToWorld`, `rectToWorld`, `rectFromWorld`, `SIDE_MAP`, `CSIDE_DIR`, `COPP`.
There are 73 frame-transform references in the engine (7 of them `frame.js`'s own definitions, so
66 consumer call sites), of which only **15 are `rectToWorld`**, and **all** canonical-box
emission funnels through one function:

```24:27:src/engine/buildings/interior/fixtures.js
function toWorld(frame, b) {
  const r = frame.rectToWorld(b);
  return { x0: r.x0, y0: r.y0, z0: b.z0, x1: r.x1, y1: r.y1, z1: b.z1, m: matId(b.m), mode: b.mode ?? 0 };
}
```

`Frame` has a single construction point (`frameOf(env)` in `src/engine/buildings/massing.js`),
memoized on a `WeakMap`. And both voxelizers are **already inverse-transform rasterizers** —
they iterate world columns and map back into canonical space, which is exactly the shape a
rotated lattice needs:

```109:113:src/engine/buildings/massing.js
  for (let j = j0; j <= j1; j += 1) {
    const y = chunk.wy(j);
    for (let i = i0; i <= i1; i += 1) {
      const x = chunk.wx(i);
      const [u, v] = frame.fromWorld(x, y);
```

`src/engine/buildings/interior/voxelize.js` does the same and already guards
`if (u < 0 || v < 0 || u >= env.U || v >= env.V) continue;`, which handles a rotated footprint's
out-of-bounds columns for free. `src/engine/buildings/interior/units.js` (≈line 266) already
nests a `Frame` inside canonical space, so the abstraction is already hierarchical.

---

## 3. The governing constraint: what physics actually costs

This is the core of the plan. **Physics cost is not proportional to the number of distinct
angles.** From `docs/GRIDS.md` and the sources in `core/src/world/world_grids.cpp`:

1. **Junction samples** are "computed only for chunks whose box meets another grid's box, once
   per extraction, so a world-grid-only world pays nothing." There is an explicit fast path:
   `i32 oriented_ = 0; // oriented grids there are (none: no junctions anywhere)`.
2. **`collide` / `sweep` / wheel-cast** cost scales with the grids overlapping the query box.
3. **Structure solve** cost is per structure, independent of its frame.
4. But `World::near_grids(g, chunk)` is a **linear scan over every resident grid**, cached per
   `(grid, chunk)` and invalidated globally whenever any grid is added, removed or grows its
   bounds (`grids_changed()` bumps `grid_epoch_`). Under streaming, grids are added and removed
   continuously, so **the resident grid count is a global cost, not just a local one.** Slots are
   reused deterministically (lowest free slot) but `grids_` only shrinks from the back, so a
   transient spike in grid count leaves a permanently larger scan.

So there are two quantities to minimise, and the size of the angle set is neither of them:

> **Rule A — partition, never overlap.** One oriented part per coherent object; parts must not
> occupy the same space. Deliberate overlaps (a canopy cast into a facade, a chamfer meeting a
> main mass) go through structvox's `priority`/displacement, which exists for exactly that.

> **Rule B — keep the resident oriented-part count small.** Because of `near_grids`, this is a
> budget in its own right, independent of where the parts are.

> **Rule C — anchored angles are free.** Two face-adjacent anchored voxels are never bonded, so
> an anchored pitched road part against anchored ground produces **zero junction bonds** and
> zero fragments. Ground, terrain, roads and pads can be angled at no structural cost at all.

A fourth fact makes streaming safe: "A sample in a world-grid chunk that is not resident (the
unknown world) holds the face." An oriented part at the edge of the resident area is held up by
the unloaded world rather than collapsing.

### 3.1 Rotated is *measured* better than stepped

This inverts the intuition, and it is not speculation — these are rows of
`tests/core/test_grids.cpp`, reproduced in `docs/GRIDS.md`:

| Configuration | Oriented grid (junctions) | Stepped into the world grid |
| --- | --- | --- |
| Cantilever in a grid turned about z, on its own pier | **φ 1.31903** | φ 1.31903 (identical) |
| Cantilever cast 2 voxels into an anchored pier, turned 10° / 30° / 45° | φ 0.898 / 0.893 / 1.13 | 1.09 / 1.09 / 0.98 |
| **Concrete strut making a 4 m cantilever a truss** | **stands, φ 0.65** | **breaks at its steps** |

Three conclusions:

- A **self-contained** turned part on its own pier is *bit-identically* as strong as the
  monolithic world-grid equivalent. That is Rule A's case, and it costs nothing in accuracy.
- A skewed *interface* is slightly conservative (1.13 vs 0.98 at 45°), well inside the lattice's
  own variation (the reference bar's utilization ranges 0.30–1.14 over lattice offsets).
- A **diagonal member stepped into the world grid simply fails.** "A stepped bar is only as
  strong as its thinnest step": the design pass over-strengthens it, it cracks at the steps, thin
  stepped necks are near-mechanisms that get pushed onto the multigrid solver permanently, and it
  needs extra thickness to avoid pinholes, which adds volume and therefore fragments.

**So for any angled structural member, the oriented part is both the cheaper and the correct
choice. Stepping is the expensive option.**

### 3.2 The validated budget

`game/src/city.cpp` in `opus_destruct_2` contains a working, shipping precedent: `CitySource`
with `turned_` enabled does exactly what this plan proposes. Its calibration is our budget.

- Block pitch `kPitch = 192` voxels = **24 m**.
- `turned_lot()` is `!empty_lot && ((lot_traits >> 8) & 7) == 3` → **1 building in 8** is an
  oriented grid; the rest stay in the world grid.
- `grids()` returns one `SourceGrid` per turned lot, at home in the chunk of the lot centre,
  `g.id = bx * 4096 + by + 1` (stable), `g.origin` at the lot centre, `g.rot` a yaw-only
  quaternion; `generate_grid` rasterizes the building **about its centre in its own lattice**.
- `generate()` **skips** turned lots (`if (!turned_lot(bx, by)) building(bx, by, fill);`) — the
  world grid and the oriented grids partition space. That is Rule A, in their code.

At the default `load_radius = 96` m, a 96 m disc is ≈28,950 m²; at a 24 m block pitch that is
≈50 blocks, of which ≈1 in 8 is turned → **≈6–8 resident oriented grids**. Equivalently:

> **Budget: about one oriented part per 3,600 m² of city footprint.**

That is the operating point the reference implementation was built and tested at, and it is what
`angles.maxPartsPerChunk` (§5.4) must be calibrated to reproduce.

### 3.3 Part size limit (answers the open question)

`generate_grid(u32 id, VoxelGrid& out)` fills a **whole grid in one call**, and per
`source.hpp`: "the grids at home in a chunk — generated whole when that chunk is, evicted with
it… A grid is at home in one chunk only." So a part's lifetime is its home chunk's lifetime.

The binding limit is in the far-render tier. `Game::far_mesh` (`game/src/game.cpp` ≈line 950)
gathers a tile's oriented grids by scanning home chunks with `constexpr i32 kMargin = 4;`
chunks — "the grids that reach into it". The core therefore assumes **a grid reaches at most
about 4 chunks (16 m) beyond its home chunk**; anything further is dropped from the far tier and
will pop.

> **A part must not extend more than ~4 chunks (16 m) from its home chunk.**

Consequences: a building is fine as one part. **A long road run is not** — it must be split into
segments of at most ~16 m reach, each with its own home chunk and stable id. Plan for this from
the start in S1/S4; it is not a late detail.

---

## 4. Design decisions

### 4.1 Quantize every rotation to a Pythagorean triple

Use integer triples `p² + q² = r²`, so `cos = q/r` and `sin = p/r`.

**Why.** IEEE 754 requires `+ − × ÷ sqrt` to be correctly rounded; it does **not** require it of
`sin`, `cos` or `atan2`. A rotation built from triples uses no transcendentals and is bit-identical
across x86, ARM, native, WASM, JS and C++. That matters specifically because *this* generator will
run in two languages — JS in the viewer, C++ in the engine — and feed `state_hash` and replays.
(`CitySource` uses `std::sin`/`std::cos` for its yaw, which is fine inside one binary; our
cross-language case is exactly where libm implementations diverge.)

Better still, **both mapping directions stay in pure integer arithmetic**:

- World voxel → local cell: `u = floor((q·dx + p·dy) / r)`, `v = floor((−p·dx + q·dy) / r)`, with
  integer `dx, dy` relative to the part origin. No floats at all — this is the rasterizer path.
- Local cell → scaled world: `X = q·u − p·v`, `Y = p·u + q·v`, exact integers at `1/r` granularity.

Membership tests likewise stay integral by scaling (`q·dx + p·dy` compared against `r·extent`),
so no tolerance constants are needed anywhere.

**Verified** (`node --input-type=module`, reproducible): over an 801 × 801 lattice the scaled
forward map and its integer inverse round-trip with **0 mismatches**, and
`|cos² + sin² − 1| ≤ 1 ulp (2.22e-16)` across 400 triples.

#### Yaw table — primitive triples, `r ≤ 100`, angle ≥ 8°

| triple | angle | triple | angle |
| --- | --- | --- | --- |
| 13, 84, 85 | 8.80° | 8, 15, 17 | 28.07° |
| 11, 60, 61 | 10.39° | 33, 56, 65 | 30.51° |
| 9, 40, 41 | 12.68° | 28, 45, 53 | 31.89° |
| 16, 63, 65 | 14.25° | 3, 4, 5 | 36.87° |
| 7, 24, 25 | 16.26° | 48, 55, 73 | 41.11° |
| 12, 35, 37 | 18.92° | 65, 72, 97 | 42.08° |
| 5, 12, 13 | 22.62° | 20, 21, 29 | 43.60° |
| 36, 77, 85 | 25.06° | (45° exact by symmetry) | 45.00° |
| 39, 80, 89 | 25.99° | | |

Sixteen angles from 8.80° to 43.60° with 1–4° gaps; mirrored across the eight octants that is
**~132 distinct yaws** — far more than enough to break up a grid. Note the reference
implementation gets a convincing result from just 10°–35°.

#### Pitch table — the family `(2n, n² − 1, n² + 1)`

This family lands almost exactly on the road-class grade limits already in `roadLevel.js`:

| n | triple | grade | road class |
| --- | --- | --- | --- |
| 25 | 50, 624, 626 | 8.01% | arterial 8% |
| 20 | 40, 399, 401 | 10.03% | collector 10% |
| 16 | 32, 255, 257 | 12.55% | local 12% |
| 14 | 28, 195, 197 | 14.36% | village 13% / alley 14% |
| 12 | 24, 143, 145 | 16.78% | lane 16% |
| 10 | 20, 99, 101 | 20.20% | steepest |

All verified exact. Roof slopes draw from the same table.

**Roll** uses the yaw table and is needed only for leaning monoliths, fallen logs and tilted
boulders — low instance counts, no structural role.

Products of rational rotations are rational, so yaw × pitch × roll composes exactly. The
quaternion handed to structvox is derived once via `sqrt`, which is IEEE-exact.

### 4.2 One primitive: `Placement`

New module `src/engine/core/placement.js`:

```js
Placement {
  origin: { x, y, z },   // integer world voxels
  yaw, pitch, roll,      // indices into the triple tables (0 = identity)
  h,                     // voxel size (the world's, or finer for a detail part)
  priority,              // for deliberate overlaps
  anchored,              // hint: this part is ground/support (Rule C)
}
```

API: integer `toLocal(x, y, z)` / `toWorld(u, v, w)`, `localBoundsToWorldAABB(extent)`,
`worldAABB()`, `toQuat()`, and an **identity fast path** so today's four proper rotations stay
integer-exact and bit-identical. Internally the rotation is materialised as an exact integer
matrix over a common denominator.

The record is designed to serialise **directly** into structvox's
`SourceGrid { id, origin, rot, voxel_size, priority }`.

`src/engine/buildings/frame.js` becomes a thin wrapper over `Placement`, so S0 changes no
behaviour and no call site.

### 4.3 One unit of orientation: parts

Promote the generator's output to explicit **parts**: `{ id, placement, extent, content }`. One
part becomes either one oriented grid (when rotated) or a direct world-grid rasterization (when
axis-aligned). Part ids must be **stable and derived from structural keys**, exactly as
`CitySource` does (`bx * 4096 + by + 1`).

| Emitter | Parts |
| --- | --- |
| Terrain, ground tile, groundcover | world grid, always axis-aligned |
| Trees, boulders, props, deadwood | world grid (angled SDFs rasterized in place) |
| Road run | one anchored part per ≤16 m segment, yaw + pitch |
| Building | one part; two or three with wings |
| Bridge deck, ramp | one anchored part per ≤16 m segment |

Parts must partition space (Rule A) and respect the ~16 m reach limit (§3.3).

### 4.4 Do not touch the arterial lattice

`src/engine/network/arterials.js` builds strictly axis-aligned lines `x = X(i)`, `y = Y(j)` with
per-line jitter, and its own comment states why: "each cell depends only on its index, any cell
can be planned without its neighbours and all shared edges agree exactly." That per-index
independence is what makes the whole engine's purity work. **Angled roads are added as additional
line families and polyline geometry on top of it, never by skewing it.**

### 4.5 Foliage must become non-structural

Independent of angles, but it interacts: leaves, grass and groundcover must be *neither* free
structure (they would form fragments and structures by the million) *nor* anchored (an anchored
voxel beside a wall becomes a **support** for that wall — grass would hold up buildings).

structvox has `indestructible`, `ductile`, `reinforcement`, `crush`, `penetration`, `grip` — but
**no non-structural flag**. This is a small, clean core addition to propose: a `decorative`
material flag meaning *not fragmented, not bonded, still carveable*. Note it in the merge notes;
it does not block anything here.

### 4.6 Props need no core change

Worth recording, because an earlier assumption was wrong. `World::set_voxels` accepts
`kEditIsolated = 1u << 1` — "the written solid voxels bond to nothing" — and
`WorldSystem::on_generated(World&, chunks)` tells a host system exactly which chunks were just
generated. So furniture and props can be written as isolated free voxels right after generation
by a host `WorldSystem`: destructible, fallable and pushable, deterministic, participating in
`state_hash`, with no engine modification. `loosen_grid(GridId)` exists; a `loosen(bb)` is not
needed.

Props stay **axis-aligned in the world grid** and therefore need no parts and cost no grid slots.

---

## 5. Controls and enforcement

### 5.1 The switch

```js
world.angles: {
  enabled: false,             // false => identity only: bit-identical to today
  yawSet:   "triples100",     // or "cardinal"
  pitchSet: "grades",
  maxPartsPerChunk: N,        // calibrated to §3.2
  features: { roads: true, buildings: true, ramps: true, wings: false },
}
```

Add to `src/engine/config/defaults.js` alongside `streaming`. With `enabled: false` the triple
tables collapse to identity plus the four proper rotations and output is **bit-identical to
today** — which keeps the existing 17 test suites valid as a regression oracle and makes every
phase reviewable as a diff against a known-good baseline.

Existing presets in `src/engine/config/presets.js` (`cities`, `infiniteCity`, `wrapWorld`,
`island`, `nordicIsland` + `skerry`/`fjord`/`forest` variants, `nordicTown`, `oldHarbourTown`,
`whiteSeaTown`, `planetEquator`, `planetNorth`) keep `enabled: false`. Introduce angled variants
as **new** preset ids so the oracle survives.

### 5.2 The physics budget, enforced in the planner

`angles.maxPartsPerChunk` is a hard cap. Where oriented-part density already sits at budget, the
planner falls back to an axis-aligned placement for the next candidate. This is deterministic
because it is evaluated in a fixed order per cell, and it must not require neighbour
communication — derive it from the cell's own part list.

### 5.3 The auditor

Add `scripts/audit-angles.js` alongside `scripts/audit-fit.js`, reporting:

- oriented parts per chunk, and the maximum;
- resident parts within a 96 m disc (the number that must land near 6–8, §3.2);
- count of interface chunks (chunks where two parts' boxes meet);
- fraction of world volume touched by more than one part (must be ~0 — Rule A);
- maximum part reach from its home chunk in chunks (must be ≤ 4 — §3.3);
- the distribution of angles actually used.

Add a test pinning the budget on the densest downtown preset and the thickest forest preset.

---

## 6. Renderer consequence (measured, and recoverable)

Same walled box, same volume, rasterized at different angles into one chunk and greedy-meshed
with `src/engine/voxel/mesher.js`:

| angle | triangles | vs axis-aligned |
| --- | --- | --- |
| 0° | 74 | 1.0× |
| 1:4 (14.0°) | 558 | 7.5× |
| 1:3 (18.4°) | 678 | 9.2× |
| 1:2 (26.6°) | 900 | 12.2× |
| 2:3 (33.7°) | 952 | 12.9× |
| 45° | 958 | 13.0× |

Greedy meshing is what degrades: a flat wall merges to one quad, a staircase merges to nothing.
CPU mesh time stayed flat (~0.8 ms/chunk); the cost is vertex count, buffer memory and upload.
Cost tracks **steepness**, not rationality — shallow angles are meaningfully cheaper, which is a
reason to prefer the low end of the yaw table for large masses.

**This cost is recovered for parts.** `svx_mesh` meshes each grid's chunks in that grid's own
lattice — "an oriented grid's mesh is placed in the world by its host" — where the walls are flat
again. The sanctioned pattern is **per-part chunk meshes plus a model transform**, and this
repository's renderer should adopt the same. Two things to work out when it does: it conflicts
with purely chunk-keyed streaming, and AO must be handled across the part/ground seam.

Also note `GameSource::coarse(lo, n, factor, out)` — structvox's far render tier, with a
conservative rule ("a cell is solid where any solid covers part of it; air only where it covers
the cell"). This maps directly onto this engine's existing LOD/overview path, so the LOD system
is an asset for the merge, not a thing to discard.

---

## 7. Phases

Ordered so the two **zero-physics-cost** phases land first: they carry most of the visual payoff.

### S0 — `Placement` and the rotation tables

- Add `src/engine/core/placement.js`: both triple tables, integer maps, identity fast path,
  `toQuat`.
- `src/engine/buildings/frame.js` delegates to it. No call site changes.
- Add `world.angles` to `src/engine/config/defaults.js` with `enabled: false`.
- Tests: exact integer round-trip over a large lattice; orthonormality within 1 ulp; quaternion
  bit-identity; `enabled: false` reproduces current golden output.

**Acceptance:** `npm test` and `npm run lint` pass unchanged; no rendered-output diff from
`scripts/render-iso.js`, `render-map.js`, `render-sat.js`.

### S1 — Angled and curved streets (zero physics cost)

- Let the `organic` pattern cut at angled lines, not only axis-parallel ones.
- Add a **diagonal boulevard line family**: global lines at a fixed yaw indexed by `i` with
  position `f(i)`, as deterministic and neighbour-agnostic as the arterial lines. Leave
  `arterials.js` itself alone (§4.4).
- Curve rural and village roads more freely via the existing `wobble()`.
- Pitch road vertical profiles to the exact grade table (§4.1).
- Fix part granularity here: **≤16 m reach per segment** (§3.3), stable ids per segment.

**Acceptance:** `render-map.js` and `render-sat.js` show non-orthogonal street networks;
`test/roads.test.js` grade and continuity invariants still hold; `scripts/audit-fit.js` reports no
new ground steps; `test/streaming.test.js` still passes (cross-chunk agreement).

### S2 — Organic vegetation (zero physics cost)

- Widen lean and fork ranges in `src/engine/nature/trees.js`; vary crown asymmetry; jitter the
  forest lattice harder.
- Tilt boulders, deadwood and fallen logs using the roll table.

**Acceptance:** `scripts/render-trees.js` gallery and a forest `render-iso.js` read less regular;
`test/vegetation.test.js` and `test/nature.test.js` pass; no change to feature-source budgets.

### S3 — Building yaw

- Generalize the 15 `rectToWorld` sites and the single `toWorld(frame, b)` in
  `src/engine/buildings/interior/fixtures.js` into a **canonical-box rasterizer** that
  inverse-maps, as the two voxelizers already do. `buildingBoxes` caches on `plan._boxes`; keep
  that.
- `env.bounds` becomes the AABB of the rotated footprint.
- Oriented lots and pads: lot-vs-lot and lot-vs-road clearance become oriented-box tests;
  `SiteGrading.at` needs an oriented distance; `city/grading.js` pads (built at ≈lines 50/55 via
  `tierRects(env, 0).map((r) => frame.rectToWorld(r))`) need OBB treatment.
- Gate placement on `angles.maxPartsPerChunk`, calibrated to ≈1 part per 3,600 m² (§3.2) — i.e.
  expect to rotate roughly **1 building in 8**, not all of them.

**Acceptance:** `src/engine/validate/walkability.js` passes on rotated buildings (0.5 m × 1.75 m
walker, 2-voxel steps — expect to widen doorways, with yaw-aware door-width rules);
`test/interiors.test.js`, `test/fit.test.js`, `test/cities.test.js`, `test/towns.test.js` pass;
`scripts/audit-angles.js` within budget.

### S4 — Smooth ramps and inclined surfaces

- Inclined road surfaces and garage ramps as **anchored** pitched parts (Rule C: no fragments, no
  bonds), one per ≤16 m segment, so wheels roll rather than judder on 12.5 cm steps.

**Acceptance:** no visible stepping on a 12.5% street in `render-iso.js`/`render-slice.js`; in the
destination engine, no periodic wheel judder driving that street.

### S5 — Wings and chamfers

- Angled wings, chamfers and canted bays as additional parts on one envelope, joined with
  structvox `priority`/displacement rather than by merely touching.
- Default `angles.features.wings = false` until the budget audit shows headroom.

---

## 8. Merge notes for `opus_destruct_2`

- `Placement` → `SourceGrid { id, origin, rot, voxel_size, priority }` is a direct projection, so
  `ChunkSource::grids(chunk)` becomes a filter of the part list by home chunk, and
  `generate_grid(id, out)` becomes the part's content rasterized in its own lattice — exactly the
  `CitySource` shape (`game/src/city.cpp`).
- `ChunkSource::generate(chunk)` must **skip** anything emitted as a part, or Rule A is violated
  and the two representations will fight through displacement.
- `region(chunk)` should group a city block so buildings come back whole, as `CitySource` does.
- `GameSource::coarse()` is fed by this engine's existing LOD path.
- Material mapping: 502 → at most 106 free ids. Plan is ~25 physical classes plus **one
  `Solid`-bound persistent voxel layer** carrying visual identity (up to 8 layers exist; pieces
  carry layer values via `piece_layer`, and `svx_mesh`'s colour/texture provider reads them).
- Two core additions to propose, both small: the `decorative` material flag (§4.5), and nothing
  else — props are covered by `kEditIsolated` + `WorldSystem::on_generated` (§4.6).
- Density reality check for the physics radius: this engine is ~69% ground/anchored, ~22% building
  fabric, ~8% furniture, with ~435 vegetation voxels/m² in forest. Memory, not CPU, is the binding
  constraint; expect the simulated radius to sit near **48–64 m** rather than the default 96 m.

---

## 9. Risks

- **Oriented-box clearance is the real work in S3.** Lot packing, road right-of-way and pad
  easing all currently lean on cheap axis-aligned rect intersection (`src/engine/core/rect.js`,
  imported by 10 engine files). Budget for it properly.
- **Walkability regressions on rotated buildings.** Stepped interior partitions narrow doorways
  along world axes. The test will catch it; the fix is yaw-aware door-width rules.
- **Part reach vs. streaming.** The ~16 m limit (§3.3) is easy to violate with roads and bridges
  and the symptom is far-tier popping, not a crash. Audit it (§5.3) rather than trusting it.
- **Resident grid count creep.** `near_grids` is a linear scan whose cost is set by the
  high-water mark of grid slots. Enforce the budget; do not let "one part per building" slip in.
- **Test churn.** Every golden-output test moves the moment `angles.enabled` is switched on for an
  existing preset. Keep `enabled: false` on all current presets and add angled variants as new
  preset ids.
- **Sliver junctions are *not* a risk** — worth recording, since it looks like one. The core
  resolves skewed interfaces by one-sided **ownership**: the higher-priority grid measures the
  interface with its faces alone, precisely because "measured from both sides, a skewed interface
  gave slivers of the other grid's faces, half-counted, that carried more than their share." The
  remaining care needed is correct `priority` ordering, not tolerance tuning.
