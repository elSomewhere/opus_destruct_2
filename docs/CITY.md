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
| site complexes | `sites/complex.js`, `sites/kit.js` (`box`, `finishStructure`) | `sites/complex.*`, `sites/kit.*` (the kit's surface structures and `planGate` come with the site kinds) | stage `complex` |
| site layer | `world/sites.js` | `world/sites.*` (SITES, the layer, pads, the ground override, the site source's z range and rasterizer; the kinds, and the highway and water tests of a site's placement, come later) | stage `sites` (stand-in kinds: `tools/procgen_ref/lib/sitekinds.mjs`, `tests/city/site_kinds.hpp`) |

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
  lakes are ported (`lakes.portLakeOf`). (The stages `sites` and `sitelinks` warm every base over
  the lattice cells they probe and the sites and links those reach, `warmBasesIn`; a stage of the
  real site kinds must reach as far as their placement samples: a stronghold's service road runs
  up to 5 km from its apron.) The reference's golden digests (`test/golden/*.json`)
  were recorded with the shared context: a golden sample that was such a first touch may differ
  (to be measured when the golden stage is ported).
- **The gullies' kernel cache** (`terrain/landforms.js`) is keyed `i * 1000003 + j` in the
  reference, which collides only for cells 40,000 km apart; the port keys it by the exact cell.
