# The city generator (`svx_city`)

`svx_city` is the world generator of the game's default worlds: a C++ port of
[voxel_city](../reference/voxel_city) (a JavaScript generator of cities with full interiors,
roads, highways, subways, sewers, rivers, lakes, terrain with 6 km mountains, biomes, forests,
islands, sites and an angled world of exact rational rotations). It reproduces the reference
**bit for bit**: the same configuration gives the same plans, the same voxels and the same
exports, natively and in WASM, on any thread count.

- **Reference**: `reference/voxel_city/` is a snapshot of `elSomewhere/voxel_city` at `4ed8e16`
  (`git archive`, without its viewer's public assets). It is vendored rather than a submodule
  because that repository is private and CI's token cannot fetch it. Its own checks pass there:
  `npm ci && node --test` (208 tests) and `node scripts/golden.js --check` (and `--angled`).
  The snapshot is never edited; porting a later change of the reference is a deliberate step
  (refresh the snapshot, re-record the stages it changes).
- **Code**: `level/city/src/<dir>/<file>.{hpp,cpp}` mirrors the reference's
  `src/engine/<dir>/<file>.js`, file for file, namespace `svx::city`; the public API is in
  `level/city/include/svx/city/`. The target `svx_city` depends on `svx_core` (for its
  deterministic math, `svx/base/dmath.hpp`) and on nothing else; never on the game.
- **Tests**: `tests/city/` (`svx_city_tests`), stage by stage against the reference (§3).

## 1. Status

| Stage | Reference | Port | Conformance |
| --- | --- | --- | --- |
| JS semantics | V8 | `core/js.hpp` | `Math.round`, `hypot`, `pow`, `log2`, `sin`/`cos`/`atan`/`atan2`/`exp`/`log` (the core's fdlibm ports), number formatting and `Array.prototype.sort` checked bit for bit against Node over millions of inputs |
| core | `core/*.js` | `core/` | stage `core` |
| config, presets | `config/*.js` | `config/` | stage `config` |
| material palette | `voxel/materials.js` | `voxel/materials.*` (generated: `tools/procgen_ref/gen_materials.mjs`) | stage `materials` |
| charts, wrap | `world/chart.js`, `world/wrap.js` | `world/chart.*`, `world/wrap.hpp` | stage `chart` |
| seasons | `world/season.js` | `world/season.*` | stage `season` |
| biomes | `nature/biomes.js` | `nature/biomes.*` | stage `biomes` |
| macro fields, island | `world/fields.js`, `world/island.js` | `world/fields.*`, `world/island.*` | stages `fields`, `island` |
| terrain, landforms | `terrain/*.js` | `terrain/*` | stage `terrain` (§6: nested calls isolated) |
| arterial grid, road classes | `network/arterials.js`, `network/roadClasses.js` | `network/arterials.*`, `network/roadClasses.*` | stage `arterials` |
| World (constructor) | `world/World.js` | `world/World.cpp` | tests (`test_world_ctor.cpp`) |
| chunk writer | `voxel/chunk.js` | `voxel/chunk.*` | stage `chunk` |
| export material classes, ids | `svx/materials.js`, `svx/ids.js` | `svx/materials.*`, `svx/ids.hpp` | stages `svxmat`, `ids` |
| districts, flavors, styles, civic table | `city/districts.js`, `city/flavors.js`, `buildings/styles.js`, `buildings/civic.js` | `city/districts.*`, `city/flavors.*`, `buildings/styles.*`, `buildings/civic.*`, `buildings/archetype_registry.*` | stage `registries` |
| building frames | `buildings/frame.js` | `buildings/frame.*` | stage `frames` |
| oriented parts | `world/parts.js` | `world/parts.*` (`parts_in`, `parts_homed_in` come with the cell plan) | stage `parts` |
| interior data | `buildings/interior/{prefabs,civicPrefabs,civicRules,common,stairs,grid}.js` | `buildings/interior/` | stages `prefabs`, `civicrules`, `floorgrid`, `stairs` |
| prop prefabs, industry | `city/{propPrefabs,industry}.js` | `city/propPrefabs.*`, `city/industry.*` | stages `propprefabs`, `industry` |
| polygon blocks, chamfers | `city/blockPoly.js`, `buildings/chamfer.js` | `city/blockPoly.*`, `buildings/chamfer.*` | stages `blockpoly`, `chamfer` |
| cell network (city stage 1), street patterns, diagonal boulevards; the road record | `city/cellNetwork.js`, `city/streets.js`, `city/diagonals.js`; `World.cellNet`; createWorld's island sea tests | `city/cellNetwork.*` (`World::cell_net`), `city/streets.*`, `city/diagonals.*`, `network/road.hpp`, `world/createWorld.cpp` (`sea_at`, `sea_hits_rect`, `sea_share`, `sea_hits_seg`) | stages `cellnet`, `streets`; tests (any order, 4 threads); on a World without lakes and highways (§6) |
| town plans | `city/townPlan.js` | `city/townPlan.*` (`Settlement::plan`) | stage `townplan`; tests (any order, 4 threads); without highways (§6) |
| land cover, farmland | `nature/landcover.js`, `nature/farmland.js` | `nature/landcover.*`, `nature/farmland.*` | stage `landcover` |
| rivers, lakes, port lakes | `nature/rivers.js`, `nature/lakes.js` | `nature/rivers.*`, `nature/lakes.*` | stage `water` (§6: port lakes) |
| createWorld (so far), harbour grading, water predicates | `world/createWorld.js` | `world/createWorld.cpp`: land cover, rivers, lakes, the terrain's `port_grade`, caves; `sea_at` ... `water_hits_rect`; marked places for the rest | stage `water`; `test_nature_threads.cpp` |
| caves, their feature source | `nature/caves.js` | `nature/caves.*`: `cave_z_range` and `cave_rasterize` over a view of the ground tile's columns (`CaveColumns`), for compose to wrap as a `FeatureSource` | stage `caves` (synthetic tiles) |
| road views, road surface | `network/roadView.js`, `network/roadSurface.js`; `World.roadView` | `network/roadView.*` (`World::road_view`: the 3 x 3 cell networks' roads, or a test's through `World::cell_roads`), `network/roadSurface.*` | stages `roadview`, `roadsurface` (scripted roads, and recorded ones: §6) |
| road levels | `network/roadLevel.js`; createWorld's `streetLevel` | `network/roadLevel.*` (`World::street_level`) | stage `roadlevel` (recorded roads: §6); tests (any order, views dropped and remade, 4 threads) |
| pitched road pieces | `network/roadParts.js` | `network/roadParts.*` | stage `roadparts` (recorded roads and waters) |
| highways, their feature source | `network/highways.js` | `network/highways.*` (`HighwayNetwork`, installed by `create_world`; `highway_z_range` and `rasterize_highways` over the ground tile's z, for compose to wrap as a `FeatureSource`); the town plans' corridor test (`city/townPlan.cpp`) | stage `highways` (recorded roads and waters); tests (4 threads) |
| building archetypes and envelopes | `buildings/archetypes.js` | `buildings/archetypes.*` (the 17 after civic's; `plan_building_envelope`, its finalize), `city/lots.hpp` (the lot record, what the archetypes read) | stages `archetypes`, `registries` |
| house, cabin and unit planners | `buildings/interior/{houses,cabins,units}.js`, plan.js's `PlanBuilder` | `buildings/interior/{houses,cabins,units}.*`, `buildings/interior/plan.*` (the builder's floors, grids and stairs: the rest of plan.js comes with the interior planners) | stages `houses`, `units` |
| facades | `buildings/facade.js` | `buildings/facade.*` | stage `facade` (§6: a look asked with two seeds) |
| sample buildings | `buildings/sample.js` | `buildings/sample.*` (`stage_archetype` reads a world through `StageWorld`, `StagedEnvelopes`, until the cell plan is ported) | stage `sample` |
| building shells (the coarse voxelizer), roof snow | `buildings/massing.js` | `buildings/massing.*` (`voxelize_massing`, `pitched_roof_only`, `roof_snow_cover`: `Envelope::snow`, `snow_cache`; `snow_at`) | stage `massing` (every archetype, season and snow cover, LOD 0 to 5); tests (4 threads) |
| wings, corner and canted bays, chamfers | `buildings/wings.js` | `buildings/wings.*` (`plan_wings`, `wing_placement`, `rasterize_wing`, `rasterize_wing_part`, `kEmbed`), `buildings/wing.hpp` (the record; `nearWing` is the interior planners'), `city/lots.hpp` (`block`, `frontages`, `whole`) | stage `wings` (scripted sites: lots on scripted streets served as a World's cell roads, slanted streets, turned lots) |
| the buildings' feature source | `buildings/source.js` | `buildings/source.*` (`building_z_range`, `rasterize_buildings` over the envelopes asked for: `World::envelopes_in` is the cell plan's) | stage `wings` (grid and parts mode) |
| garage ramps as pitched parts | `buildings/garageRamps.js` | `buildings/garageRamps.*` (`ramp_part`, `rasterize_ramp_part`) | stage `garageramps` |
| site grading | `city/grading.js` | `city/grading.*` (`SiteGrading`, `level_lot`, `kApron`; `Lot::underground`, `under_highway`) | stage `grading` |
| skybridges, their feature source | `city/skybridges.js` | `city/skybridges.*` (`plan_skybridges` sets `Envelope::sky_doors`; the source over the bridges near a chunk: `skybridges_in` per cell plan) | stage `skybridges` |
| island landmarks, their feature source | `world/landmarks.js`; createWorld's `landmarks` | `world/landmarks.*` (`Landmarks`, installed by `create_world` on an island; `landmark_z_range`, `rasterize_landmarks`) | stage `landmarks` (every island world, its open-ground answers recorded: §6; `free()` on scripted roads and plans); tests (4 threads) |

(The table grows with the port; §5 lists the order.)

## 2. Porting rules

The port is a transliteration: the same operations on the same values in the same order. Plans
and voxels are only reproduced if every number is.

### 2.1 Numbers

- **Every JS number is a `double`.** Use `int` only for loop counters, indices into arrays and
  values a typed array stores; never for a quantity JS divides, scales or compares with
  fractions. Coordinates, ids that are numbers, cell indices: `double`.
- **Operation order is evaluation order.** JS and C++ both evaluate `a + b + c` as
  `(a + b) + c`; keep every expression's shape (the build has `-ffp-contract=off`, so nothing is
  fused). Do not simplify `x * 2 / 2`, reorder sums or hoist subexpressions that change rounding.
- **Function arguments**: C++ evaluates them in an unspecified order (GCC right to left). Where
  JS evaluates calls with side effects in its arguments (`f(rng.next(), rng.next())`), sequence
  them into locals first. The same for initializer lists of objects (`{ x: rng.int(..), y: .. }`).
- Use `core/js.hpp` for everything JS defines differently from C++:

| JavaScript | C++ |
| --- | --- |
| `Math.round(x)` | `js::round(x)` (halves towards +infinity; **not** `std::round`) |
| `Math.hypot(a, b[, c])` | `js::hypot(a, b[, c])` (V8's algorithm; **not** `std::hypot` or `sqrt(a*a+b*b)`) |
| `Math.pow(x, y)`, `x ** y` | `js::pow(x, y)` (V8's port of fdlibm; not `std::pow`, not `dm::pow`) |
| `Math.sin/cos/atan/atan2/exp/log` | `js::sin` ... (= `svx::dm::`) |
| `Math.log2(x)` | `js::log2(x)` |
| `Math.sqrt`, `floor`, `ceil`, `abs`, `trunc` | `std::` (correctly rounded / exact) |
| `Math.max(a, b, ...)`, `Math.min` | `js::max`, `js::min` (NaN propagates; `std::max` does not) |
| `Math.sign(x)` | `js::sign(x)` |
| `x % y` | `std::fmod(x, y)` (dividend's sign); `((a % n) + n) % n` is `mod(a, n)` (`core/math.hpp`) |
| `x \| 0`, `x >>> 0`, `x >> n`, `x << n`, `Math.imul` | `js::to_int32`, `js::to_uint32`, `js::sar`, `js::shl`, `js::imul` |
| `a & b`, `a ^ b`, `~a` on numbers | on `js::to_int32(a)` (the result is an int32) |
| `x \|\| d` on numbers (0, NaN replaced) | `js::or_(x, d)` |
| `x ?? d` (only undefined / null) | an optional's `value_or`, or NaN as "undefined" when the code never holds a real NaN there |
| `String(x)`, `` `${x}` `` | `js::num(x)` / `js::cat(...)` (the shortest round-trip form; `-0` prints `0`) |
| `Number("12")` | `js::parse_number` |
| `arr.sort(cmp)` | `js::sort(vec, cmp)` (V8's TimSort; `std::sort` and even `std::stable_sort` differ when `cmp` is not consistent: NaN, booleans, random) |
| `arr.sort()` (no comparator: as strings!) | `js::sort_strings`, or `js::sort` with a comparator on `js::num` of each |
| `typedArray.sort()` (numeric) | `js::sort(vec, [](a, b) { return a - b; })` |
| `Float32Array` store | `float` (`js::f32`) - reading it back gives the rounded value |
| `Uint8Array`, `Uint16Array`, `Int32Array` store | `js::u8`, `js::u16`, `js::i32` (they wrap) |
| `Number.EPSILON`, `Infinity`, `NaN` | `2.220446049250313e-16`, `js::kInf`, `js::kNaN` |

- Comparisons with NaN are false in both languages; `x !== x` is `x != x`.
- `-0`: JS's `Math.round(-0.3)` is `-0`, and `1 / -0` is `-Infinity`; keep signs as JS does
  where a division or `Object.is` could see them (rare; `js::round` keeps them).

### 2.2 Values and objects

- **Records** (roads, lots, envelopes, plans, boxes ...) become structs with the fields JS gives
  them anywhere in the code (read the producers and the consumers). A field JS may leave
  undefined is a `std::optional`, a nullable pointer, or a documented sentinel. Keep JS's names in
  snake_case.
- **JS objects are references.** When JS mutates an object after it was stored in two places, or
  compares objects by identity (`a === b`, `Map`/`Set`/`WeakMap` keyed by objects), the C++ record
  must be shared (`std::shared_ptr`, or a pointer into storage that does not move) - a value copy
  breaks it. When a record is complete before anyone else sees it, a value is fine.
- **Products of caches** are immutable once made (`shared_ptr<const T>`). A pointer into one kept
  beyond the call that fetched it holds the product (an aliasing `shared_ptr<const Sub>(product,
  &product->sub)`): the cache may drop the product.
- **Lazy fields** (JS: `rec.model ??= make(rec)` on a record shared across calls) are
  `Lazy<T>` (`core/cache.hpp`): made once, safely from any thread.
- **`Map` / `Set` iteration is insertion order**; JS object keys too (except integer-like keys,
  which come first, ascending). Where code iterates a map, a set or `Object.keys/entries/values`,
  use an insertion-ordered container (a vector of keys beside a hash map); `std::map` /
  `std::unordered_map` only where nothing iterates it.
- **Strings**: `std::string`; ids are built with `js::cat` so numbers print as JS prints them.
  `String(part)` of a non-string seed part hashes "undefined", "null", "true"... - reproduce the
  string JS would hash.
- **Configuration** is a `Value` tree (`core/value.hpp`: JS semantics for undefined, null,
  truthiness, key order; `config["world"]["angles"]["enabled"].truthy()`,
  `cfg["lakes"]["scale"].num(1)` for `?? 1`). Read it once where a subsystem is made, never per
  voxel. `x !== false` on a config flag is `!(v.is_bool() && !v.truthy())`; `x === true` is
  `v.is_bool() && v.truthy()`.
- **Registries** (districts, archetypes, styles, prefabs, biomes, landforms, flavors, sites,
  feature sources ...): a vector in registration order plus an index by id. JS registers at
  module evaluation, in the import graph's depth-first post-order from `createWorld.js`; in C++
  a registry is filled by an explicit function called in that same order (registration order
  decides weighted picks). Never rely on C++ static initialisation order.
  The reference's order (the same from every entry point: `svx/source.js`, `createWorld.js`,
  `scripts/lib/golden.js`): BIOMES (`nature/biomes.js`), LANDFORMS (`terrain/landforms.js`),
  DISTRICTS (`city/districts.js`), FLAVORS (`city/flavors.js`), STYLES (`buildings/styles.js`),
  ARCHETYPES and STYLES (`buildings/civic.js`: one each, before archetypes.js's), ARCHETYPES
  (`buildings/archetypes.js`), COMPLEX_THEMES (`sites/complex.js`), DISTRICTS and SITES
  (`sites/militaryBase.js`, `sites/researchComplex.js`, `sites/mountainBase.js`, in that order),
  PRESETS (`config/presets.js`). `world/register_all.cpp` calls each module's `register_*()` in
  this order, once (`register_all()`, called by `create_world` and by tests).
- **Closures** become lambdas (`std::function` where stored). Classes become classes; a JS class
  holding a cache keeps a `MemoCache`.
- **Throwing**: the libraries have no exceptions. Where JS throws on a programming error (an
  unknown registry id), `SVX_FAIL`. Where JS catches, find what the catch does and do that.

### 2.3 Threads, caches and memory

`ChunkSource::generate` runs on several threads at once, all of them over one `World`.

- Nothing mutable is shared without a lock: no mutable statics, no mutable members in const
  methods except `MemoCache` and `Lazy`. Per-call scratch is local (or `thread_local`).
- JS's `LRU` caches become `MemoCache<K, V>`: the value made once by the first thread to ask,
  the others waiting for it, least recently used dropped beyond a count. Plans are pure functions,
  so a dropped plan made again is the same plan. Use numeric keys (`cell_key`, `tile_key` in
  `world/caches.hpp`) where JS's string keys are just coordinates.
- A JS object that queries mutate (`SpatialGrid`'s stamps) becomes one whose queries do not
  (`core/geom2d.hpp`'s `SpatialGrid` returns the same items in the same order).
- Keep every value JS keeps in a typed array in the same element type.

### 2.4 Performance

Port first, profile after conformance. The targets ([`PROCGEN_MERGE_PLAN.md`](PROCGEN_MERGE_PLAN.md) §7.6): at most 0.35 ms per non-empty
chunk on one native thread, near-linear scaling to 8 threads, WASM within twice native. Avoid
allocation per voxel; hoist configuration reads; keep JS's lazy structure (what JS computes only
when a LOD 0 chunk touches it, C++ does too).

## 3. Conformance

Each stage of the port is checked against the reference before the next is built on it.

- **Records**: a stage's output as text lines - numbers printed as JS prints them (`String(x)`,
  `js::num`), so equal text is equal bits. `tools/procgen_ref/stages/<stage>.mjs` writes the
  reference's (`node tools/procgen_ref/dump.mjs <stage>`); a test in `tests/city/` writes the
  port's with `tests/city/records.hpp` (`rec::Line`, `rec::Out`) in the same order.
- **Digests**: `tests/city/conformance.txt` holds the SHA-256 (16 hex digits) of the reference's
  records of every stage (`tools/procgen_ref/record.sh <stage>` writes it); the test compares
  its own records' digest with it. CI needs no Node for this.
- **Finding a difference**: `SVX_CITY_RECORDS=/tmp/rec build/native-release/tests/svx_city_tests
  -tc='*<stage>*'` writes the port's records, `tools/procgen_ref/diff.sh <stage> /tmp/rec` shows
  the first lines that differ (with their line numbers). Narrow a stage's samples to find the
  first wrong number; then compare intermediate values (a temporary record line in both).
- **Sampling**: stages draw their sample points from `samples(seed)` (an LCG, `rec::Samples`)
  and mirror the order of draws exactly (see §2.1 on argument order).
- **Golden**: the reference's own record (`reference/voxel_city/test/golden/presets.json`,
  `angled.json`: digests of ground tiles, chunks at LOD 0, 2, 5 and 8, parts and exports at fixed
  sample points of every preset) is the final acceptance of the whole generator.
- **Tolerance**: exact, always, for integer decisions, ids, plans and voxels. A divergence that
  resists (a V8-specific algorithm) is recorded in §6 with its extent; once the port is accepted,
  the C++ generator becomes its own reference (golden records natively and in WASM), and the
  JavaScript is frozen.

## 4. Layout

```
level/city/
  include/svx/city/   the public API (the generator as a world source: later stages)
  src/core/           js.hpp (JS semantics), hash, noise, placement, obb, geom2d, rect, math, value,
                      cache (MemoCache, Lazy)
  src/config/         defaults (DEFAULT_CONFIG, makeConfig), presets
  src/world/          World, createWorld, chart, wrap, island, fields, season, sites, parts, ...
  src/terrain/ nature/ network/ city/ buildings/ buildings/interior/ underground/ sites/ voxel/ svx/
tests/city/           svx_city_tests: a test per stage; records.hpp, sha256.hpp
tools/procgen_ref/    dump.mjs, stages/*.mjs, record.sh, diff.sh
reference/voxel_city/ the reference (pinned snapshot)
```

## 5. Order of the port

Each stage conformance-checked before the next ([`PROCGEN_MERGE_PLAN.md`](PROCGEN_MERGE_PLAN.md) §7.4):

1. core, config (done)
2. world: chart, wrap, season, island, fields; terrain: landforms, terrain; network: arterials,
   road classes. Data registries: voxel materials and the chunk writer, the export's material
   classes, styles, civic table, districts, flavors; interior data: prefabs, civic prefabs and
   rules, common, stairs, the floor grid; prop prefabs, chamfer, polygon blocks.
3. nature: biomes, land cover, rivers, lakes, caves, farmland, trees.
4. network: road surface, road view, road levels, road parts, highways; city stage 1: cell
   network, streets, diagonals, town plan; landmarks; sites.
5. buildings (archetypes, massing, facades, frames, wings, chamfers, garage ramps) and city
   stage 2 and 3 (lots, cell plan, grading, landscape, parks, industry, skybridges, dressing);
   parts.
6. buildings/interior; underground; forest, boulders, ground cover.
7. voxel: compose (ground tiles, chunks); part rasterizer; the export (`svx/`); golden digests.

## 6. Known differences

Where the reference's result depends on call order (and so on its cache sizes and, across its
workers, on timing), the port gives the order-independent result: the engine needs the same
voxels on any thread count. The conformance stages make the reference order-independent first,
so that it stays the oracle.

- **Terrain samples near a settlement: the reference's shared terrain context.** The reference's
  `Terrain` reuses one mutable context object (`this.ctx`) for every call, and `sample()` reads
  its `coast` and `rugged` back (and, on an island, the coast that grades the town's waterfront,
  so `h` too) after nested calls that run the landform stack again in that object: a settlement's
  base height made on first use (`settlementBase` in `cityMeters`), and the world's `portGrade`
  hook (lakes sampling the terrain when a port lake is first planned). So in the reference the
  first sample that makes a base height or a port lake takes values from the settlement's centre:
  up to 5 of the terrain stage's 2,000 samples per world (before the stages made the base heights
  first), and the island trunk roads, whose A* samples the terrain. The port runs every call in a
  context of its own (`terrain/terrain.hpp`), so a sample is a pure function of (world, x, y,
  arguments). The conformance stages make every base height a sample can read first, in a fixed
  order (`tools/procgen_ref/lib/worlds.mjs` `warmBasesAt` / `warmBasesIn`); the reference's
  samples are then the same in any order, and the port's. A stage that samples the terrain near
  settlements, directly or through any plan, must do the same, and plan the port lakes first once
  lakes are ported (`lakes.portLakeOf`). The reference's golden digests (`test/golden/*.json`)
  were recorded with the shared context: a golden sample that was such a first touch may differ
  (to be measured when the golden stage is ported).
- **The gullies' kernel cache** (`terrain/landforms.js`) is keyed `i * 1000003 + j` in the
  reference, which collides only for cells 40,000 km apart; the port keys it by the exact cell.
- **Road identity.** The reference compares roads as objects: the road view's maps by road,
  `roadLevel`'s `s.road === road` (a road is not its own through road) and `ownSeg` (a segment
  as its owner cell's view has it). A cell network dropped by its LRU and made again makes new
  objects for the same roads, so two road views made on either side of that hold different
  objects for one road, and those comparisons fail (`ownSeg` falls back to the asking segment):
  there, a road's levels depend on what the caches held. In the port a road is the same road as
  another when it has the same id and was made by the same cell (`same_road`,
  `network/road.hpp`: `id` alone repeats in each lap of a wrapping world); the port compares
  roads that way across views, and by address only within one road view (which holds one object
  per road): `throughAt` (a road found at its own end in another cell's view is a corner, its node
  level, not a through road's) and `ownSeg` (the owner cell's segment, cached by its index there:
  views are remade alike) give the reference's result when nothing is dropped, whatever the cache
  sizes. A stage of the road modules makes each cell network once while it runs (the reference's
  World caches 64: give it a cache that drops nothing, `tools/procgen_ref/lib/roads.mjs`
  `recordingWorld`), so the reference's objects stay one per road; `test_roads_threads.cpp` remakes
  the port's views from other road objects half way and gets the same levels.
- **Cell networks and town plans, on a World without lakes and highways.** The reference asks
  `world.lakes?.shoreNear` for a harbour district (`portNear`) and `world.highways` for the
  civic landmarks (a highway's corridor keeps them off a block), both installed by
  `createWorld`. Until the port installs them (`create_world`), its cell network and town plans
  are those of a World of `World.js` with createWorld's island sea tests (the stages:
  `lib/worlds.mjs` `withSeaTests` on a bare World; no lakes, highways or harbour grading of the
  terrain): `port_near` fails on a World that has lakes, the place to wire them (`resolve_town`
  asks the highways' corridors on a World that has them: `network/highways`). Their base heights are made first (`warmBasesIn`, `warmIsland`); measured
  on the `cellnet` cells, the reference's networks are the same planned cold and backwards (the
  country roads' slope test and the trunk roads' A* read heights, but none of these cells
  flips); a town plan's church (the highest of a few spots near the centre) compares terrain
  samples.
- **Port lakes and the harbour grading** (`createWorld.js`'s `portGrade` hook, `lakes.portLakeOf`).
  A lake is a pure function of its lattice cell, and a town's port lake of the town, once the
  terrain samples they read are (the entry above). The reference plans a town's port lake inside
  the first terrain sample near it, in the shared context (that sample's `rugged` and `coast` then
  come from the lakes' samples; on an island its `h` could), and keeps 512 lake cells (an LRU).
  Measured with the base heights (the entry above) over points of the stage `water` near towns,
  forward against backward on fresh worlds: the `rugged` of 13 of 390 points differs (cities), 29
  of 510 (desert), the `rugged` or `coast` of 4 of 300 (island:large); the heights and the water
  answers do not. The port plans in contexts of their own and caches the lakes (any size:
  `Lakes(world, capacity)`) and each town's port lake (`Settlement::port_lake`), whichever thread
  asks first. The stages make the reference pure with
  `tools/procgen_ref/lib/worlds.mjs` `pureTerrain(world)`: every terrain sample first makes, in a
  fixed order, the base heights and - where createWorld's hook will run (not raw, near a town) -
  the port lakes it reads (it runs the hook once first), and a terrain call nested in a sample
  throws, so a stage that passes nested none. A stage that samples the terrain through any plan
  (lakes, rivers, land cover, roads ...) calls it once after making the world (`landcover`,
  `water`, `caves` do). `test_nature_threads.cpp` checks the port: any order, a 16-cell lake
  cache, four threads.
- **Not differences, for the ports to come.** `Rivers.at` and `Lakes.at` return one shared object
  per instance that the next call overwrites; the port returns values (every caller reads its
  result before the next call: a port of one that keeps it across another call copies what
  JavaScript would read then). The reference's `hash32` / `hashFloat` read four arguments and
  ignore any more (`farmland.js`, `caves.js`, `sites/links.js`): the port's take four, so such a
  call is ported without its extra ones (a farm field's key is its strip's cut hash). A lot's
  `whole` is lots.js's `true` on a lot taking its block whole and cellPlan.js's rect (the lot as
  planned) on a lot fitLots trimmed to its block's slanted edges; planWings reads
  `lot.whole ?? lot.rect` as a rect, so on `whole: true` (no x0) its in-lot test fails everywhere:
  the port keeps both (`Lot::whole`, `whole_rect`), and `plan_wings` that failure.
- **The road network's stages run on recorded inputs.** The road network reads the cell networks
  (city stage 1) and the waters (`createWorld`'s `isWet`: rivers, lakes, the island's sea). Its
  stages were made while those were not ported, and check it on the reference's own inputs: they
  record what the reference reads - the roads of every cell a stage asks for, the questions `isWet`
  answered yes (any other is dry) - in `tools/procgen_ref/data/<stage>.json`, and the tests serve
  them to a World of `World.js` (no `create_world`) through two overrides: `World::cell_roads`
  (`World::road_view` reads it instead of `cell_net(i, j)->roads` where it is set) and
  `World::wet_source` (`World::is_wet` asks it first) (`tests/city/road_inputs.hpp`). The
  reference's world is `createWorld`'s with its terrain's port grading off (`terrain.portGrade =
  null`, as the port's World of `World.js` has none) and the base heights a stage reads made first
  (`warmAround`). Once the port's cell networks ask the lakes (`port_near`), the stages can run on
  a `create_world` World's own cell networks and waters instead, the port grading on (the
  reference's terrain made pure with `pureTerrain`); the recorded roads are those of a createWorld
  world whose terrain has no port grading, so a harbour town's may differ from them.
- **The landmarks' stage runs on recorded open-ground answers.** An island's landmarks
  (`world/landmarks.js`) keep to open natural ground (`free`: no road, lot, urban space or water),
  which asks the road views and the cell plans (`plan.lotAt`, `plan.spaceAt`), a later stage of the
  port. The stage `landmarks` records the reference's answers on every island world
  (`tools/procgen_ref/data/landmarks.json`; createWorld's world, its terrain made pure:
  `pureTerrain`) and the port replays them (`Landmarks::free_source`); everything else the plan reads
  (the island's coast, cliffs, harbour and places, the terrain, the props) is the port's own
  `create_world`'s. `free()` itself is checked on scripted roads and plans: its cell-plan half is
  `Landmarks::plan_occupied`, which the cell plan's port installs (until it does, `free()` fails:
  nothing in the port plans landmarks yet). Once it does, the stage can run on the port's own
  answers.
- **A building's look asked with two seeds** (`buildings/facade.js` `buildingLook`): the reference
  keeps the look per envelope (a WeakMap) whatever seed asks, so a second seed would get the first
  one's look; the port keeps it on the envelope too (`Envelope::look_cache`) and fails
  (`SVX_FAIL`) when another seed asks, so a look never depends on which seed asked first. An
  envelope belongs to one world: generation never asks twice.
