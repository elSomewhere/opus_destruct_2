# Merging the city into structvox

This guide is for the merge of this generator into the physics engine of
`elSomewhere/opus_destruct_2` (structvox), branch `merge_anim_2`, as of
commit `a8cf8bd`. The merge itself is not done here. What is here is the
city as structvox reads a world, in plain data, behind the same calls:
`src/engine/svx/`. The merge then comes down to an adapter plus the few
engine additions listed in §9.

| Module | What it is |
| --- | --- |
| `svx/source.js` | `createSvxSource(config, options)`: `ChunkSource` and `GameSource`, call for call |
| `svx/materials.js` | the physics classes, their `Material` records, the look, flora and water layers |
| `svx/roads.js` | `roadNetwork(world)`: the streets as a `RoadNetwork` (lanes, turns, signals, walkways, parking) |
| `svx/worker.js` | the same calls as a worker protocol (a browser engine beside it); `handle(state, msg)` is the protocol |
| `test/svx.test.js`, `test/svxRoads.test.js` | what the export promises, checked against the city's own voxels |
| `test/golden/*.json` | hashes of generated chunks, parts and exports: the conformance record for a port |

Everything is a pure function of the config and its arguments: any number
of sources over one config, in any worker, in any order, give the same
bytes. Nothing reads the clock or keeps state beyond caches.

## 1. The destination

These are the contracts on `merge_anim_2`, with the paths in that repository:

- `core/include/svx/world/source.hpp` defines `ChunkSource`:
  - `generate(chunk, out)`: kChunkVox `Vox`, index `(x * 32 + y) * 32 + z`. It must be a pure function, called from several threads.
  - `chunk_lo` / `chunk_hi`.
  - `region(chunk)`: the unit whose changes are archived and forgotten together.
  - `generate_layer(chunk, name, out)`.
  - `grids(chunk)`: `SourceGrid { id, origin, rot, voxel_size, priority }`, at home in one chunk.
  - `generate_grid(id, VoxelGrid&)`.
  - `joints(chunk)`.

  The same header defines `StreamConfig`: load radius 96 m, evict radius 128 m, and the change archive. It also says: "Generated structures should stand under their own weight; the world designs (strengthens) the members that do not when it first touches them."
- `game/include/svx/game/source.hpp` defines `GameSource`: `spawn_pos` / `spawn_dir`, `roads()` → `RoadNetwork*`, and `coarse(lo, n, factor, out)` for the far tier. It also defines `FarConfig`.
- `game/include/svx/game/roads.hpp` defines `RoadNetwork` (`Lane`, `Walk`, `ParkingSpot`), with one implementation, `procgen/src/drive_city.cpp`. The traffic (`game/src/traffic.cpp`) and the pedestrians (`game/src/pedestrians.cpp`) read it.
- `core/include/svx/material/material.hpp` holds `MaterialId`: the 12 standard presets, `kMaxMaterials` 127, `register_material`. The game's own materials are `svx/game/materials.hpp` (9, at 12..20).
- `core/include/svx/world/grid.hpp` holds `LayerSpec { name, persistent, bind }` and `add_layer`. `world.hpp` has `kEditIsolated`, and `WorldSystem::on_generated(world, chunks)` (a hook that runs once chunks are generated).
- The C API (`game/include/svx/game/api/svx_api.h`) loads built-in sources by name (`svx_load_procedural`: "city", "drive"). There is no way yet for a source outside the engine to feed it (§9).

## 2. The calls

| structvox | the export (`createSvxSource`) |
| --- | --- |
| `chunk_lo()` / `chunk_hi()` | `chunkLo()` / `chunkHi()`: the island's or planet's extent, z from 600 m below sea level to 800 m above the highest peak |
| `generate(chunk, out)` | `generate(cx, cy, cz)` → `{ any, vox }` (`Uint8Array(32768)`, structvox's index order) |
| `generate_layer(chunk, name, out)` | `generateLayer(cx, cy, cz, name)`: `"look"`, `"flora"` or `"water"` → `Uint8Array(32768)` or null |
| `grids(chunk)` | `grids(cx, cy, cz)` → `[{ id, origin [m], rot {x,y,z,w}, voxelSize, priority, anchored, kind, key }]` |
| `generate_grid(id, out)` | `generateGrid(id)` → `{ h, chunks: [{ cx, cy, cz, vox, look, flora }] }` in the grid's lattice, or null |
| `region(chunk)` | `region(cx, cy)` → a key below 2^53: the city block of the building most of the column holds |
| `GameSource::coarse(lo, n, factor, out)` | `coarse(lo, n, factor)` → `Uint8Array` (index `(x * n1 + y) * n2 + z`): the world grid at that LOD |
| `spawn_pos()` / `spawn_dir()` | `spawn()` → `{ pos: [x, y, z] (m, feet on the ground at the origin), dir: [1, 0, 0] }` |
| `GameSource::roads()` | `roadNetwork(source.world)` (§8) |
| (after generation) | `isolated(cx, cy, cz)` → flat `[index, vox, look, ...]`: props and furniture (§6) |
| (materials) | `materials()` → `{ cityBase, register, classes, looks, flora, palette }` (§4) |

`createSvxSource(config, { flora = "layer", props = "isolated" })` forces
parts mode (`world.angles.partsMode: "separate"`). Everything a part holds
is left out of the world grid (the plan's Rule A), and each part is a grid
of its own.

## 3. Conventions

- **Units and voxels.** The city's voxel `(x, y, z)` is structvox's voxel
  `(x, y, z)`. structvox's voxel `p` is the cube `h (p - 1/2) .. h (p + 1/2)`
  (h = 0.125 m), so a city point `q` (in voxel units, the corner of voxel 0
  at 0) lies at `h (q - 1/2)` metres. Chunks are 32³ in both.
- **Axes.** The same numbers: z up, x and y as they are. The city calls +y
  south; structvox is right-handed. So in compass terms the city reaches
  structvox mirrored, but every rotation stays proper (det +1). Nothing is
  mirrored in coordinates, and every rotation the export hands over is a
  rotation. Only "which side is right" is structvox's, which matters for
  traffic (§8).
- **Oriented grids.** A part's local cell `p` is the grid's voxel `p`,
  centred at `origin + R(rot) (h p)`. `gridFrame` gives the origin as the
  centre of local cell 0. The quaternion is taken from the exact integer
  matrix, each component a single correctly rounded `sqrt`, so it is the
  same bits everywhere. Every table rotation and every composed yaw
  (`yawProduct`: a bay on a turned building) has legs of opposite parity.
  A voxel centre therefore never lands on a cell face, and the float map
  agrees with the integer one: `test/svx.test.js` finds 0 mismatches over
  1.3 million samples.
- **Reach.** `kVoxelLimit` (2^20 − 4096) bounds the extent. A part reaches
  at most 4 chunks from its home chunk (`Game::far_mesh`'s margin).
- **Ids.**
  - Grid ids are the part ids: a u32, never 0, from the canonical cell and
    the grant order (`world/parts.js partId`).
  - Region keys are below 2^53.
  - Road ids are 52-bit (§8).

## 4. Materials and layers

A city has some 400 looks; a physics engine needs a few classes. Each look
maps to a physics class (`svx/materials.js` `RULES`, which throws on an
unclassified material). What it looks like travels in a layer:

| Byte / layer | Content |
| --- | --- |
| `vox` | `(1 + class id) \| 0x80` where anchored, 0 air |
| `look` (persistent, bound to Solid) | the look's index within its class: class + index give back the city material, for colour |
| `flora` (bound to Air) | decorative voxels (leaves, flowers, grass, crops): air to the physics, `1 + index` into `FLORA` |
| `water` (bound to Air) | 255 where water, sewage or sludge stands |

- **Class ids.** The core's presets keep theirs: rc, concrete, steel,
  masonry, soil, rock, bedrock, wood, stone, glass, steel section (rebar
  is unused). So do the game's: sheet, window, tyre, plastic, asphalt,
  paint, lamp. The city registers its own from `CITY_BASE` = 21: roofing
  21, partition 22, soft 23, ice 24, snow 25, foliage 26. `register` lists
  complete `Material` records (E, G, rho, ft, fb, fc, cohesion, friction,
  Gf, fragment sizes) to pass to `register_material` in that order, at
  those ids.
- **Anchoring.** A voxel is anchored where the ground holds it: the column
  tile's fill (terrain, road surfaces, pavements) still standing when every
  feature is drawn. A bridge's deck, piers and railings are the exception;
  they are structure. The road slab parts are anchored, since they are
  ground (Rule C).
- **A decision for the merge.** Flora is air to the physics until the core
  has a `decorative` flag: a voxel that renders and collides for the
  camera but is neither structure nor support. With such a flag, flora
  becomes the `foliage` class (`flora: "solid"`). Without it, millions of
  leaves would either fragment (as free structure) or hold up walls (as
  anchored). `flora: "none"` leaves them out.

## 5. Oriented parts → SourceGrids

A part (`world/parts.js`) is one coherent object in a lattice of its own:

- a turned building, or a row of them sharing one lattice (`members`);
- a pitched road piece (anchored);
- a wing, a corner bay or a canted bay;
- a chamfer's facade slab;
- a garage's ramp.

`grids(chunk)` lists the parts at home in the chunk. `generateGrid(id)`
returns a part's voxels by chunks of its own lattice, and a road slab's
voxels are anchored.

- **Priority** (`partPriority`) decides who owns an overlap:
  - A building owns what it shares with its wings.
  - Wings own what they share with a road piece or a ramp.
  - All of them sit above the world grid (0), except parts cast into
    something the world grid holds. Those yield to it with a negative
    priority: a wing of a building square to the grid, a garage's ramp
    between its decks.
  - A chamfer's slab owns the stepped wall behind it.
- **The budget** (Rule B, `near_grids` scans every resident grid):
  - About one part per 3,600 m² of city.
  - One part at home per chunk.
  - At most `angles.maxResident` (8) at home in any disc of
    `angles.residentRadius`, exactly, across cell borders too.
  - `residentRadius` defaults to the 96 m load radius. structvox keeps
    chunks until the 128 m evict radius (hysteresis), so a host that wants
    the bound to hold for a player roaming back and forth should set 128.
  - `scripts/audit-angles.js` reports all of it.
- **The far tier.** `coarse()` is the world grid alone. The engine's far
  tier gathers a tile's grids from home chunks within 4 chunks and splats
  them itself, which is why the reach limit holds.

## 6. Props and furniture

Street furniture and the furniture inside buildings (benches, lamps,
tables, beds) are not structure. `isolated(cx, cy, cz)` gives their voxels
apart from the chunk: its voxel bytes leave them out. A host writes them
after generation with `kEditIsolated`, in `WorldSystem::on_generated`, so
they bond to nothing and fall or are pushed as loose bodies.
`props: "solid"` keeps them in the chunk as structure instead.

## 7. Regions

`region(cx, cy)` is the city block of the building that most of the column
holds, else the block of the column's centre, else 8 × 8 columns. A row
of turned buildings shares one block, so no building comes back from the
change archive in half.

## 8. Roads → RoadNetwork

`roadNetwork(world)` follows the conventions of structvox's drive city, so
the engine's traffic and pedestrians behave as they do there.

- **Junctions.** A junction is every road that meets at one place, as each
  of them sees it. The city cuts its roads at cell borders, so an
  arterial's two halves meet only through the roads they both cross; the
  export closes over those meetings, so every road of a junction agrees on
  its ways on and its signal. Two junctions too close for a lane between
  their kerbs (a jog, a side road just past a crossing) are one. A road
  whose end lies in another's carriageway within 2 voxels of its level
  meets it there too, even where the road network (`network/roadView.js`)
  has no junction for the pair (an end that also meets a third road there
  keeps that junction's level in the voxels).
- **Lanes.** Traffic keeps to the right in structvox's frame (heading
  `(dx, dy)`, right is `(dy, -dx)`). A lane is a straight run one way: a
  road is cut into links at its junctions and into pieces at its bends.
  Lane k counts outwards from the centre line or median. A lane stops at
  the kerb line of the road it meets, and the turn through the junction is
  the host's, from one lane's `b` to the next one's `a`. `z` is the lane's
  own road's surface. A piece is cut in two wherever its straight line
  would stray more than a voxel from the road's profile (a landing, a
  change of grade), so a car set on a lane stands on the road, never in
  it.
- **Which roads carry traffic.** Roads with a lane each way or more.
  Alleys and single-track lanes (one lane, 3.5–5 m) carry people only:
  structvox's traffic keeps its distance to anything within 2.4 m to the
  side and has no passing places, so two cars meeting on one track would
  stop for each other for good.
- **Turns.** `next(id)` returns `[[id, turn]]`, where turn is −1 left,
  0 straight on, 1 right (clockwise).
  - Straight on keeps the lane, onto the same road or the one beyond the
    junction.
  - A right turn goes from the kerb lane to the kerb lane, a left turn from
    the inner lane to the inner lane. This is the realistic way round;
    `drive_city.cpp` turns right from its inner lane. Where there is no
    straight on (a T), any lane may turn.
  - There are no U-turns, except where a street ends or meets only roads
    without traffic, so no car is ever stuck.
- **Signals.** A junction with two roads of collector rank or more among
  its roads is signalled, and every road there follows its signal. Its
  roads fall into phases by heading: a road joins the first phase (in the
  order of their first roads by id) whose first road runs within 22.5° of
  it. Each phase has 17 s: green for the first 14 s, then 3 s all red. The
  offset is per junction. A crossing over a road is open for the first 4 s
  of the next phase. A plain crossing thus has a 34 s cycle, the first
  road's phase green for u < 14 s and the other's for 17 ≤ u < 31; a
  junction of three headings has a 51 s cycle. `signal(id)` gives `{
  cycle, offset, from, to }` (green while `(t + offset) mod cycle` lies in
  `[from, to)`), and `green(id, t)` and `walkOpen(id, t)` evaluate it.
- **Walkways.** Each walk has a `kind`:
  - `sidewalk`: along each side of a link, on the middle of the sidewalk,
    from corner to corner. A corner is where two walking lines cross,
    worked out alike from either road, so walks meet exactly. `inset`
    keeps people a fifth of the sidewalk towards the buildings.
  - `corner`: between the corners of two roads a junction puts on one side
    (a side road's mouth just past a crossing).
  - `crossing`: over a road at its crosswalks, and over a street an alley
    runs across. Signalled with the junction.
  - `middle`: down the middle of an alley or a single-track lane (people
    walk the carriageway there). The street's sidewalk stops at their
    mouths, so the two meet.

  `walkNext(id, end)` lists the walks of the junction's roads whose ends
  lie within 3 voxels of that end. It is a pure function of the junction,
  the same whatever was asked before.
- **Parking.** A place every 6 m in a road's parking strips, from 9 m past
  the kerb a link starts at. It is given as a position and a heading (unit
  vector; the host takes its yaw).
- **Queries.** `lanesIn`, `walksIn` and `parkingIn` take boxes in metres
  and return records in id order. Ids are stable 52-bit integers from
  structural keys (road id, link, piece, direction, lane). `lane(id)` and
  `walk(id)` answer for any id a query or `next` has returned.
- **The region payload.** `region(lo, hi)` (worker: `{ type: "roads", lo,
  hi }`) returns everything a host's own `RoadNetwork` needs for a box:
  lanes with their `next` and `signal`, walks with the walks at both ends,
  and parking. A C++ holder answers the engine's calls from it without
  calling back:

  ```cpp
  class PushedRoads : public RoadNetwork {  // filled per region from the worker's payload
    std::unordered_map<u64, Lane> lanes; std::unordered_map<u64, std::vector<std::pair<u64,int>>> next;
    std::unordered_map<u64, Signal> signals; /* walks, walk ends, parking likewise, in a grid by region */
    bool green(u64 id, f64 t) const override { auto s = signals.find(id); return s == signals.end() || s->second.open(t); }
    // lanes_in / walks_in / parking_in: the records whose box meets, sorted by id (the export's order)
  };
  ```
- **Coverage.** Within 250 m of the origin (`test/svxRoads.test.js`
  checks the rules):
  - Every lane has a way on. A lane turns back only at a true dead end:
    no lane of another road leaves within 5 m of its end.
  - Two ways straight through a signalled junction that cross are never
    green together, and a signalled way never crosses an unsignalled one.
  - Walks joined to others at both ends: 100% in the grid city, 95% in the
    angled grid city, 99% in the old harbour town, 98% in the angled one.
    The rest end at dead ends, where the engine's pedestrians turn back.
  - The highways' lanes are in it (below).
- **Highways** (`svx/highwayLanes.js`). The highways' lanes are listed
  with the streets', in the same conventions.
  - **Decks.** Each carriageway has `lanesPerSide` lanes (3), counted from
    the median outwards, at 27.8 m/s. They are cut into links where a ramp
    leaves or joins that side, and into pieces wherever a chord would
    stray more than 2 voxels from the curve or 1 from the deck's level.
  - **Nodes.** Where two highways meet, the lanes run on, each keeping
    its lane. At a junction of three or four, the lanes stop short of the
    plateau and turn across it by the streets' rules: straight on keeps
    its lane, right from the kerb lane, left from the inner lane. The
    junction is signalled with one phase per heading, as a street's is. At
    a terminus (a route's last node) the lanes turn back.
  - **Ramps.** A ramp is one lane, one way, at 13.9 m/s. An off-ramp
    leaves the deck's kerb lane where a link ends (a right turn) and runs
    down to its landing. An on-ramp runs up from its landing and joins
    the kerb lane of the link that starts there. Each landing is a node of
    its arterial, so the arterial's lanes end and leave there: an on-ramp
    is one of the ways on from them, and an off-ramp turns onto the
    arterial's lanes leaving it, either way.

## 9. Two ways in

**A. A C++ port** (`VoxelCitySource : GameSource`, beside
`procgen/src/drive_city.cpp`). The generator is plain modules with no DOM
and no clock, and integer maths where it matters (placements, the budget,
chamfers). Port them in dependency order: core, network, city,
buildings, nature, voxel, svx. The golden records are the conformance
suite. `scripts/lib/golden.js` digests generated chunks, parts, exports
and the angled world's samples, and a port reproducing its digest
reproduces this generator. It is the larger job, and the result runs
wherever the engine does, streams at native speed, and needs no engine
additions.

**B. The generator beside the engine** (the browser build: the engine's
WASM in its worker, this in another). This is quicker to reach, but the
engine needs a door, which `merge_anim_2` does not have:

1. **An external source**: e.g. `svx_load_external(extent, spawn, voxel
   size)` and a `PushedSource : GameSource` whose `generate` serves chunks
   pushed by `svx_source_chunk(cx, cy, cz, vox, look, flora, water)`.
   `generate` is synchronous and pure, and "false" means air, so the
   streamer needs a *not yet* answer. A chunk the host has not received is
   skipped this tick and asked for again (`stream_update` keeps it in
   `want`). The host learns what to generate from the focus
   (`svx_set_focus`), in the engine's order: nearest columns first, within
   the load radius.
2. **Grids**: `svx_source_grids(cx, cy, cz, list)` pushed with their home
   chunk, and `svx_source_grid_chunk(id, cx, cy, cz, vox, look, flora)`.
   `grids(chunk)` and `generate_grid(id)` answer from them.
3. **Isolated voxels**: pushed with their chunk, and written by a
   `WorldSystem` in `on_generated` with `kEditIsolated`.
4. **Roads**: `svx_source_roads(lo, hi, payload)` fills the `PushedRoads`
   of §8.
5. **The far tier**: `svx_source_coarse(lo, n, factor, vox)`, asked for by
   tile.
6. **Materials**: `svx_register_material(record)` for each of `register`,
   in order, before loading.

The worker protocol (`svx/worker.js`) already speaks all of it:

| Message | Reply |
| --- | --- |
| `init` | extent, spawn, materials |
| `generate` | vox, layers, isolated, transferred |
| `grids` | the grids at home in the chunk |
| `grid` | a grid's voxels |
| `region` | the region key |
| `coarse` | the far tier's voxels |
| `roads` | the region payload of §8 |

## 10. Decisions and known limits

- **Highways stay in the world grid.** The plan's §4.3 lists bridge decks
  and ramps as one anchored part per 16 m, but two things rule that out:
  - Their decks follow splines in every direction, at grades of 5% at
    most, which the tables do not hold (the gentlest pitch is 8%, the yaws
    some 5° apart).
  - A highway through a 96 m disc would take a dozen parts, more than the
    whole disc's resident cap.

  Their decks step a voxel every 2.5 m at most (ramps every 1.8 m), so a
  car drives them as it does the streets. Their lanes are in
  `roadNetwork` (§8), and the harness loads a junction of four decks on
  their piers in structvox itself, which stands (§11).
- **Partial interchanges.** A ramp is left out where a street below would
  pass under it with too little headroom, or a junction plateau is too
  near; about two in three candidate ramps in a dense grid. So some
  arterial crossings have one to three ramps, and some none. Highways
  meet each other only at grade-level junction plateaus (no flyovers),
  and the single route that ends at a town ends at grade.
- **Single-track roads carry no traffic** (§8): structvox's traffic would
  deadlock on them. A host with passing places can add one lane each way
  down the middle from `world.roadView` segments of classes `lane` and
  `alley`.
- **Garage ramps are not anchored.** The plan's S4 text anchors them, but
  an anchored ramp would hold up the decks it touches whatever became of
  the garage (Rule C's free case is an anchored part against anchored
  ground). They yield to the decks and are left to the engine's design
  pass like any member. To follow the plan's version instead, change two
  things: `makePart`'s `anchored: true` in `garageRamps.js`, and a
  non-negative priority in `cellPlan.js`.
- **Budget calibration.** `partArea` (3,600 m²), `partCluster` (4),
  `maxResident` (8) and `residentRadius` (96 m) were derived from the
  engine's turned city. They are config: measure `near_grids` in the
  merged build and tune.
- **Walkability.**
  - Hillside buildings are 93–95% walkable: a street door is raised to its
    street, or stepped from it (`interior/plan.js streetLevels`).
  - The rest are doors onto front gardens below the floor, and streets that
    climb more than a storey along one facade (the plan has no
    split-level buildings).
  - Turned buildings are 100% walkable.
- **What this repository's viewer does and the engine will not need.** The
  viewer draws parts in their lattices (parts mode) or stepped into the
  world grid (grid mode), with its own mesher and LODs. An engine port
  keeps only the export's data.

## 11. Checking an export

- `npm test` runs `test/svx.test.js` and `test/svxRoads.test.js`:
  - the index order and the byte layout;
  - the classes, their ids and their records;
  - anchoring (the ground, road slabs);
  - isolated props;
  - the grids' frames against the integer placements;
  - regions and the far tier;
  - the worker protocol;
  - roads against the voxels they run on.
- `node scripts/golden.js --check` and `--angled --check` compare every
  generated sample with the record. A port reproduces them.
- **In structvox itself**: `scripts/svx-harness/run.sh <opus_destruct_2
  checkout>` builds the engine's core and game libraries (cmake, a C++20
  compiler) and a harness (`harness.cpp`). The harness serves an export's
  dump (`dump.js`: materials, a district's chunks, every part reaching
  into it, its lanes) through a `ChunkSource`. It registers the game's
  materials and then the city's, streams the district round a focus,
  ticks it for 4 s, and checks:
  - the city's materials get the ids the export gave them;
  - every part is a grid of the world at exactly its exported frame;
  - nothing comes down: the engine's design pass holds the district as
    generated;
  - points every 2 m along the lanes lie on a surface within 0.3 m (the
    world grid or a road slab);
  - with touch points (a district with highway decks): a car's weight
    (15 kN) set on the decks every 24 m. The engine extracts the
    structure there and designs it as it is first touched; it must still
    stand.

  On `merge_anim_2` (`a8cf8bd`) it passes in four 100 m districts, each of
  12 to 44 million voxels:

  | District | Parts | Touched | Max utilization |
  | --- | --- | --- | --- |
  | Nordic fjord town | 4 pitched road slabs | | 0.44 |
  | Angled old harbour town | turned buildings and wings | | 0.56 |
  | Angled grid city | corner bays | 4 points on a highway | 0.73 |
  | Highway junction (`infiniteCity` at 428, −573 m) | | 9 points on 4 decks | 0.58 |

  In all four no piece falls, and every lane point checked (94, 138, 246
  and 371, the last on the highway lanes and ramps) lies on a surface.
  The harness is also the seed of the `PushedSource` of §9.
