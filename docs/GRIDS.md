# Oriented grids: structures off the lattice

A voxel lattice steps everything that is not aligned with its axes. A wall at 30° becomes a
staircase of voxels, a diagonal brace a zigzag, and a ramp a flight of steps. Rasterized
members are also weaker than the members they stand for: a stepped bar is only as strong as its
thinnest step.

**Oriented grids** remove the stepping. A structure can have a lattice of its own, placed in
the world with any position and rotation and of any voxel size, and it is simulated like the
world grid in every respect: fragments, bonds, stress, failure, pieces, carving, blasts, fire,
water, smoke, persistence and streaming. Where the voxels of two grids meet, they are bonded by
**junctions**, so a turned building stands on the world grid's ground, and a diagonal brace
carries load between world-grid columns. A piece that breaks off may span several grids. A grid
can be placed anew; grids stand still - what moves by design is pieces on driven joints (a door,
a lift, a drawbridge: [`MOTION.md`](MOTION.md)).

The method of the core is in [`V2_DESIGN.md`](V2_DESIGN.md); hosting the core is in
[`CORE.md`](CORE.md); joints and machines are in [`MOTION.md`](MOTION.md). This page covers
what grids add.

## 1. The model

- **Grids.** A world has the **world grid** (id 0, the axes of the world; loaded or streamed)
  and any number of **oriented grids** (ids from 1). Each is a `VoxelGrid`, described by a
  `GridDesc`:
  - `frame {origin, rot}`: its voxel `p` is centred at `origin + R(rot) (h p)` in the world;
  - `voxel_size` `h` (0: the world's; §4);
  - `priority` (§3);
  - `base`: part of the level, or a change of this session (§6).
- **Everything the world grid's voxels do, a grid's voxels do.**
  - Anchored voxels are supports; free voxels form fragments in the grid's own lattice (its
    rubble follows its axes); faces within a grid bond as in the world grid.
  - Carves, blasts, edits, layers (damage included), loads, the design pass, debug fields,
    probes, fire, water and smoke all work per grid.
- **Structures span grids.** An extraction walks lattice faces within a grid and junctions
  between grids alike, so one structure can hold fragments of several grids, and one equilibrium
  solve covers them all.
- **Pieces span grids.** A piece is a set of **shapes**, one per grid it has voxels of, each
  with its voxels in its own lattice, of its own voxel size, and a transform from that lattice to
  the piece's frame (`BodyShape::xf`, `BodyShape::h`). Its bonds are the lattice bonds within each
  shape plus the junctions between its shapes, so a piece made of a turned wall and the world-grid
  slab it carried breaks between them like anywhere else.
- **Exactness.** The world grid's arithmetic is unchanged: an identity fast path keeps every
  world-grid computation what it was. A world without oriented grids gives bit-identical results
  to the engine before grids, which the regression hashes of the engine demo check. A structure
  in a grid turned about the vertical solves exactly as in the world grid (the same φ to six
  digits in the tests).

## 2. Junctions

A junction is the interface between the voxels of two grids.

- **Samples.** Each exposed face of a voxel is sampled at `S × S` points (`junction_samples`,
  default 3: the centres of an even subdivision of the face), pushed out along the face normal
  by `junction_reach` voxels of the other grid (default half of one: where the owner displaced
  the other grid's voxels, §3, the other's surface is up to half of its voxel away). A sample that
  lands in a solid voxel of another grid joins the two voxels. A fragment's own
  faces give its "forward" samples; the samples of another grid's faces that land in it are its
  "reverse" samples.
- **Ownership.** Where two grids meet, the **owner** (§3) measures the interface: its faces
  alone, each sample standing for its full `(h/S)²`.
  - Where the grids meet, the owner always has faces at the interface, or inside the other's
    solid where it overlaps and did not displace it (voxels of other bodies, a level's older
    grids): flush against it, cast into it, at any angle.
  - The other grid's faces there sample nothing. Measured from both sides, a skewed interface
    gave slivers of the other grid's faces, half-counted, that carried more than their share.
  - A sample in a world-grid chunk that is not resident (the unknown world) holds the face, as
    for the world grid itself.
- **Bonds.**
  - Between two free fragments: one bond per fragment pair, taking all its samples. This is
    the world grid's rule for its own pair bonds, and it measured best against it (per-side
    junction bonds were tried; they were weaker than a member cast in whole).
  - To a support (anchored voxels, a fragment held fixed, the unknown world): one bond per side
    of the node's lattice and per grid, as the world grid's support bonds per axis and side.
- **Sections.** A sample is a small square at its point, with its face's normal and the side of
  its own lattice's voxel. As for lattice faces:
  - the bond's normal is the area-weighted mean normal from a to b;
  - the section is the samples projected on the plane normal to it (each weighted by |n·nₖ|,
    at least 5%; the area at least a quarter of the total);
  - the section has principal axes, second moments and extreme fibres.

  Strengths come from both materials and their design classes.
- **Breaking.** A junction breaks sample by sample. Each broken sample is kept on the voxel
  whose face it samples (the owner's: `Chunk::jbroken`, or `BodyShape::jbrk` in a piece), so a
  failed interface stays failed, is saved in deltas, and goes along with pieces.
- **Cost.** Samples are computed only for chunks whose box meets another grid's box, once per
  extraction, so a world-grid-only world pays nothing.

**How close junctions come to monolithic members.** Measured values; every row is a test in
`tests/core/test_grids.cpp`:

| Configuration | Junctions | Monolithic |
|---|---|---|
| Cantilever in a grid turned about z, on its own pier | φ 1.31903 | φ 1.31903 (the world grid) |
| Cantilever cast 2 voxels into an anchored pier, square to it (mean over 8 placements) | φ 0.897 | φ 0.899 (the world grid) |
| The same, turned 10°, 30°, 45° (the root skewed across the pier's face) | φ 0.898, 0.893, 1.13 | stepped into the world grid: 1.09, 1.09, 0.98 |
| Steel bar cast into a free concrete block (mean over 12 lattice offsets) | 0.93 × | 1 (the world grid) |
| The same bar set flush in a hole of the block | 0.93 × | 1 |
| Concrete strut making a 4 m cantilever a truss (the prop removed) | stands, φ 0.65 | stepped strut: breaks at its steps |
| The pier's cantilever in a grid of half the voxel size (mean over 12 placements in its lattice) | 1.06 × (square), 1.19 × (20°) | 1: the grid of the world's voxel size |

The world grid's own utilization for the bar in the block ranges from 0.30 to 1.14 over those
offsets (the fragment layout changes with the lattice offset), so the junction values are well
within the variation of the lattice itself. A single placement is no measure: compare means.

## 3. Overlaps: priority and displacement

Two grids may be given voxels in the same space: a steel bar in a column cast around it, a door
in a wall, a turned beam cast into a pier.

- **The owner.** Of two grids, the one of higher `GridDesc::priority` owns their overlap; of
  equal priority, the newer (higher id). The world grid has priority 0 and id 0: a grid of
  negative priority yields to it. A bar placed first, then a column cast around it: give the bar
  the higher priority and the column is cast around the bar, its voxels removed where the bar is.
- **Displacement.** The other grid's voxels whose centres lie in the owner's solid are
  **removed**. Each space holds one material,
  so its mass counts once, and the owner's surface meets the other grid's where the member was
  cast. It is done when a grid is added, placed anew, when voxels are written into a grid, and
  when streamed chunks and grids are generated.
  - A level's overlaps (grids added with `base`, a source's) are resolved as part of the level:
    not a change, not saved.
  - A session's (a grid of the session, one placed anew, an edit) are changes, saved like any.
- **Measured.** A steel bar cast into a concrete block (overlapping it) now carries its load as
  the same bar set flush in a hole of the block (§2): 0.93 × the monolithic bar, both.

## 4. Voxel size

A grid may have a voxel size of its own (`GridDesc::voxel_size`): a finer grid has finer surfaces
(a turned railing, a pipe) and a coarser one fewer voxels.

- **Its rubble keeps the world's size in metres.** The fragments of a grid are laid out with
  the materials' rubble sizes scaled by the world's voxel size over the grid's
  (`FragParams::scale`), and small components merge by the same measure, so a structure breaks
  into pieces of the same size at any resolution, and its bonds are of the same size.
- **Everything else is per grid:** sections (a face's side is its lattice's voxel), junction
  samples and their reach (voxels of the other grid), the mass and inertia of fragments and
  pieces, contacts (a piece's shape samples of its own voxel size), rays, sweeps, carves.
- **Measured** (§2): the same cantilever at half the voxel size is 1.06 × as utilized at 0°,
  1.19 × at 20° (means over 12 placements of the beam in its lattice), and its mass is the same.

## 5. Placing a grid anew

`set_grid_frame(id, frame)` moves a grid and keeps its voxels, changes and design. What it was
bonded to lets go (structures there are extracted again: a slab resting on it falls), it bonds to
what it meets where it is now, and where it overlaps other grids, the lower priority's voxels are
displaced (§3). Its broken junction samples are forgotten (a new interface), and its fragments'
identities change with it. `GridMoved` reports it; `save_delta` keeps the new frame. (Grids
stand still between such placements: what moves by design is pieces on driven joints,
[`MOTION.md`](MOTION.md).)

## 6. Using grids (C++)

```cpp
#include "svx/world/world.hpp"
using namespace svx;

World world;
world.load(std::move(level));                 // the world grid (ground, anything on the axes)

VoxelGrid wall;                               // a wall in its own coordinates
wall.h = world.voxel_size();
for (i32 x = -32; x < 32; ++x)
  for (i32 y = -1; y < 2; ++y) wall.fill_column(x, y, 0, 24, make_vox(MaterialId::Masonry, false));
const f64 t = 0.5 * 30.0 * 3.14159265358979323846 / 180.0;  // 30 degrees about z
const GridId id = world.add_grid(GridFrame{V3{12.0, 8.0, 0.0}, Quat{0, 0, std::sin(t), std::cos(t)}},
                                 std::move(wall));
world.bake();                                 // designs every grid's structures

world.blast(world.grid_to_world(id, V3{0.0, 0.0, 1.0}), 0.8, 5e5);
for (;;) {
  world.tick();
  for (const GridChunk& c : world.take_changed_grid_chunks()) { /* re-mesh c.chunk of grid c.grid */ }
  // ... the world grid's chunks, events and pieces as before
}
```

| Call | What it does |
|---|---|
| `add_grid(desc, voxels)`, `add_grid(frame, voxels, base = true)` | Adds a grid; returns its id (0: refused, e.g. inside a tick, with a non-finite frame or a bad voxel size). `base`: part of the level (the level adds its grids again, in the same order, before `load_delta`; their changes are saved like the world grid's); else a change of this session, saved whole. After the design pass, a new grid is designed when first touched. Free voxels touching nothing fall as a piece at the next tick. |
| `remove_grid(id)` | Removes a grid's voxels (pieces that broke off it stay). What it held through junctions is extracted again: a slab resting on it falls. |
| `set_grid_frame(id, frame)` | Places a grid anew (§5). |
| `grids()`, `grid(id)`, `grid_frame(id, &f)` | The oriented grids (ascending ids), a grid's voxels, its frame in the world now. |
| `grid_priority(id)` | Its priority (§3). |
| `grid_to_world(id, p)`, `world_to_grid(id, X)` | Points between a grid's coordinates (metres: voxel `p`'s centre is `h p`) and the world. |
| `grid_solids(chunk)`, `grid_solid(voxel)`, `grid_voxel_at(X, &grid, &voxel)` | The grids in the world grid's voxels (a voxel whose centre lies in a solid voxel of one), per world chunk and cached, for systems of the world's lattice (§8). |
| `set_voxels(grid, edits, flags)` | Edits in a grid's coordinates. |
| `take_changed_grid_chunks()` | The oriented grids' chunks whose voxels changed, by grid, then chunk. |
| `layer(grid, L, p)`, `set_layer(grid, L, edits)`, `take_layer_changes(grid, L)` | Layers of a grid's voxels. |
| `set_loads(group, loads)` | `VoxelLoad::grid` names the voxel's grid. |
| `carve`, `blast` | Act on every grid the sphere reaches (the sphere in each lattice). |
| `raycast` | Walks each grid's lattice; `RayHit::grid` and `voxel` say what it hit, `shape` which shape of a piece. |
| `collide`, `sweep`, `overlaps`, `depenetrate` | `collide` moves a box axis by axis against the world grid and against the grids' and the pieces' voxels (swept separating-axis tests against each voxel's cube), and reports what it lands on with its velocity there (`CollideResult::ground`, `ground_piece`, `ground_velocity`: a lift's car carries its rider). `sweep` moves a box in any direction and returns the normal of what stopped it (a controller slides along a turned wall) and that surface's velocity. `overlaps` and `depenetrate` find a box stuck in solid voxels, and how far up it is free. |
| `debug_field(grid, chunk, field, out)`, `probe_utilization(grid, voxel)` | Per grid. |
| `piece(id)->shapes[k]` | A piece's shapes: `grid` (where its voxels came from), `h`, `xf` (its lattice in the piece's frame), voxels in lattice coordinates. A lattice point `s` of shape `k` is at `Body::lattice_to_world(k, s)`. |
| `piece_layer`, `set_piece_layer`, `remove_piece_voxels` | Take a shape index (0: the first). |

- **Events:** `GridAdded` (a streamed grid came, or `load_delta` made one: its id, origin and
  rotation), `GridRemoved` (removed, evicted with its home chunk, or by `load`) and `GridMoved`
  (placed anew, or by `load_delta`).
- **Stats:** `WorldStats::grids`. `state_hash` includes the grids, their frames, voxel sizes,
  priorities and junction breaks (only when there are grids: a world-grid-only world hashes as
  before).

### Rendering

- **A grid's chunks.** Mesh them in the grid's lattice (`mesh_chunk(*world.grid(id), chunk,
  opts)`: positions in the lattice, in metres) and draw them with the grid's frame as their
  model transform. A grid placed anew is not meshed again for it: only its transform changes.
  The game harness sends exactly that (`Game::take_meshes`, `ChunkMesh::grid`; the frames with
  `Game::take_grid_views`).
- **Pieces.** Mesh each shape with `mesh_shape` (vertices in the shape's lattice), map them
  into the piece's frame with `shapes[k].xf.to(v)`, then draw at the piece's pose as usual. A
  shape whose `xf` is the identity (the first) is meshed exactly as before.

## 7. Persistence

- **Chunk records** (delta format version 4) end with the chunk's junction breaks. Version 3
  records load with none.
- **The grids' part.** An optional trailer after the world grid's records:
  - magic `SVXG` (0x47585653), version 5 (4: the session has the wheels and the joints'
    collide flags and break angles; 5: the joints' latches);
  - the ids of the level's grids that were removed;
  - per grid: its id, flags (the level's; moved), its frame (origin xyz and rotation xyzw as f64),
    voxel size and priority, and its chunk records. A level's grid saves its changed chunks (and
    its frame, if it was placed anew); a session's grid saves all of its chunks;
  - the session ([`CORE.md`](CORE.md) §4): the world's clock, the pieces, the joints, the
    sleeping pieces' dead loads, and a streamed world's pieces archived out of range.
  - A world of the world grid alone, with no pieces or joints, saves exactly as before, with no
    trailer. Version 1 trailers (grids of the world's voxel size) and version 2 trailers (of the
    kinematic bodies there were: read if they had none) still load, as do 3 and 4 (joints
    without latches, sessions without wheels).
- **Loading.**
  - Load the level (the world grid, then its grids in the same order, then its joints), `bake`,
    `load_delta`: the session's pieces and joints take the place of the level's joints.
  - The whole delta is checked first. A malformed trailer, or a level's grid that is missing
    (unless streamed), refuses it, and nothing is applied.
  - A session's grid is made again with its id.

## 8. The environment

Fire, smoke and water ([`ENV.md`](ENV.md)) act on the grids too.

- **Fire** burns in each grid's own lattice, as in the world grid (its "up" is the lattice axis
  nearest the world's): heat, burning, charring, the damage it does, voxels burnt away.
  Flames reach across lattices through their world points: a burning turned wall sets the world
  grid's wall beside it alight, a burning floor a turned crate on it. The harness meshes a grid's
  chunks again as they char and glow.
- **Water and smoke** live in the world grid's lattice. They see the grids by `grid_solids`: a world voxel whose centre lies in a grid's solid voxel is solid to them. Water
  is held back by a turned wall and presses on its voxels (its structure takes the load); a
  turned roof holds smoke. A grid's changes (voxels, removal, a new place) wake them where it is.

## 9. Streaming

- `ChunkSource::grids(chunk)` lists the `SourceGrid {id, origin, rot, voxel_size, priority}` at
  home in a chunk, and `generate_grid(id, out)` gives a grid's voxels. A grid is generated right
  after its home chunk and evicted with it. Ids are the source's: unique, stable, not 0.
- An evicted grid's changes are archived (under key `2^63 | id`) and come back with it. They
  are forgotten with its home chunk's region, and saved from the archive by `save_delta`.
- A streamed grid is designed on first touch, like a streamed chunk.
- Eviction does not make what rests on a grid fall (the world out of range is unknown, and
  holds); `remove_grid` does.
- The streamed city of the game can turn about one lot in eight's building in a grid of its own
  (`make_city_source(seed, extent, h, true)`: the browser's `city`).

## 10. The C API

`svx/svx_core.h`:

- **Grids:** `svxc_add_grid` (a dense box of voxels, origin and rotation, base flag) and
  `svxc_add_grid_desc` (`svxc_grid_desc`: frame, voxel size, priority, base),
  `svxc_remove_grid`, `svxc_set_grid_frame`, `svxc_grids`, `svxc_grid_frame`,
  `svxc_grid_voxel_size`, `svxc_grid_priority`.
- **Voxels, layers and loads per grid:** `svxc_set_grid_voxels`, `svxc_grid_chunk_voxels`,
  `svxc_poll_changed_grid_chunks` (4 ints each: grid, chunk x, y, z), `svxc_set_grid_layer`,
  `svxc_grid_layer`, `svxc_set_grid_loads`.
- **Pieces:** `svxc_piece_shape_count` and `svxc_piece_shape` (voxels, box, the shape's
  lattice in the piece's frame, its grid).
- **Queries and events:** `svxc_sweep`, `svxc_sweep_ex` and `svxc_collide_ex` (with the touched
  or stood-on grid or piece and its velocity), `svxc_overlaps`, `svxc_depenetrate`; `svxc_hit`
  carries `grid` and `shape`; the events `SVXC_GRID_ADDED`, `SVXC_GRID_REMOVED` and
  `SVXC_GRID_MOVED`; `svxc_stats::grids`.
- Joints and machines: [`MOTION.md`](MOTION.md).

## 11. The game and the browser

- **`Game`.**
  - Meshes the grids' changed chunks in their lattices (`ChunkMesh::grid` set), and drops
    them when a grid goes (`take_removed_grid_chunks`).
  - `take_grid_views()` gives the grids that came or were placed anew (frame, voxel size);
    `take_removed_grids()` the grids gone.
  - The client's collision: `chunk_occupancy(chunk)` of the world grid, and
    `grid_chunk_occupancy(grid, chunk)` of a grid's chunk in its lattice.
  - The far render tier of streamed worlds splats the source's grids into its coarse cells:
    a turned building shows in the distance where it stands.
- **The worker's ABI** (`svx_api.h`).
  - `svx_mesh_info` gives the grid id in `out[9]`; a grid's mesh (and its origin) is in its
    lattice.
  - `svx_poll_removed_grid` / `svx_removed_grid_chunk` list emptied grid chunks.
  - `svx_poll_grids` / `svx_grid_info` (9 doubles: id, origin, rotation, voxel size) and
    `svx_poll_grids_removed` / `svx_grid_removed`.
  - `svx_grid_chunk_occupancy`; `svx_collide` (9 doubles: the move, on ground, the grid stood
    on, the velocity there of what it stands on, the piece stood on); `svx_event_occupancy` (a
    piece's voxels, with its detached or remeshed mesh).
  - The worker keys a grid's chunk meshes `g<grid>:<x>,<y>,<z>` and sends the frames in `grids`
    messages (`web/src/engine/protocol.ts`).
- **The browser.**
  - The renderer draws each grid's chunks with its frame (an object slot per grid).
  - The client's collision (`web/src/game/occupancy.ts`) holds the grids' occupancy in their
    lattices and the pieces' voxels (`web/src/engine/pieces.ts`, placed as they are drawn), and
    sweeps the player against them as turned cubes, exactly as the engine's `collide`
    (separating axes). The player rides what it stands on: a lift's car, a turntable.
- **Levels** carry grids, joints and drops: a `Level` (`svx/game/level.hpp`; the procedural
  generators of `svx_procgen` make them), loaded with `load_level(game, std::move(level))`.
- **The `angles` world** (`?world=angles`, `svx_engine_demo --world angles`) has:
  - a frame building turned 30°;
  - a 17 m bridge deck at 35° cast into two world-grid piers;
  - a ramp pitched 15° onto a block;
  - a steel portal with a cross brace of two diagonal bars;
  - masonry walls at 20° (with a doorway) and 45°;
  - timber crates at their own yaws, one stacked on another;
  - a stone monolith leaning 12°.

  The engine demo's scenario blasts them.
- **The `machines` world** (`?world=machines`) has machines on driven joints and hanging parts:
  [`MOTION.md`](MOTION.md) §4.
- **Checks.**
  - `svx_engine_demo --turn DEG` runs any procedural world's structure in a grid turned about
    the vertical, on the world grid's ground; `--turned-city` streams the city with turned
    buildings.
  - `node web/scripts/angles-wasm.mjs http://localhost:5190/` checks the `angles` world in
    the browser: it draws, a box moving into the 45° wall stops at it, the player stands on the
    ramp, and the blasts run without errors.
  - `web/test/occupancy.test.ts`: the client's collision against a turned wall and a moving
    piece.

## 12. Known limits

- **Minimum contact.** A contact smaller than a sample (a third of a voxel at the default
  `junction_samples`) may not bond, and a member held by a few samples is nearly a hinge. Its
  structure is solved with the multigrid (see V2_DESIGN.md §2).
- **Water and smoke see grids at the world's resolution.** A world voxel is solid to them if its
  centre lies in a grid's solid voxel: a turned wall thinner than a world voxel may let water
  through in places.
- **The far render tier** samples a grid's voxel centres into its coarse cells (a thin turned
  wall is a row of coarse cells).
