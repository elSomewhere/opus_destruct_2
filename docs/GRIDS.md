# Oriented grids: structures off the lattice

A voxel lattice steps everything that is not aligned with its axes. A wall at 30° becomes a
staircase of voxels, a diagonal brace a zigzag, and a ramp a flight of steps. Rasterized
members are also weaker than the members they stand for: a stepped bar is only as strong as its
thinnest step.

**Oriented grids** remove the stepping. A structure can have a lattice of its own, placed in
the world with any position and rotation, and it is simulated like the world grid in every
respect: fragments, bonds, stress, failure, pieces, carving, blasts, persistence and streaming.
Where the voxels of two grids meet, they are bonded by **junctions**, so a turned building
stands on the world grid's ground, and a diagonal brace carries load between world-grid columns.
A piece that breaks off may span several grids.

The method of the core is in [`V2_DESIGN.md`](V2_DESIGN.md); hosting the core is in
[`CORE.md`](CORE.md). This page covers what grids add.

## 1. The model

- **Grids.** A world has the **world grid** (id 0, the axes of the world; loaded or streamed)
  and any number of **oriented grids** (ids from 1). Each is a `VoxelGrid` with the world's voxel
  size `h`, placed by a `GridFrame {origin, rot}`: its voxel `p` is centred at
  `origin + R(rot) (h p)`.
- **Everything the world grid's voxels do, a grid's voxels do.**
  - Anchored voxels are supports; free voxels form fragments in the grid's own lattice (its
    rubble follows its axes); faces within a grid bond as in the world grid.
  - Carves, blasts, edits, layers (damage included), loads, the design pass, debug fields and
    probes all work per grid.
- **Structures span grids.** An extraction walks lattice faces within a grid and junctions
  between grids alike, so one structure can hold fragments of several grids, and one equilibrium
  solve covers them all.
- **Pieces span grids.** A piece is a set of **shapes**, one per grid it has voxels of, each
  with its voxels in its own lattice and a transform from that lattice to the piece's frame
  (`BodyShape::xf`). Its bonds are the lattice bonds within each shape plus the junctions between
  its shapes, so a piece made of a turned wall and the world-grid slab it carried breaks between
  them like anywhere else. A piece of one shape is placed in its own lattice's frame, as pieces
  always were.
- **Exactness.** The world grid's arithmetic is unchanged: an identity fast path keeps every
  world-grid computation what it was. A world without oriented grids gives bit-identical results
  to the engine before grids, which the regression hashes of the engine demo check. A structure
  in a grid turned about the vertical solves exactly as in the world grid (the same φ to six
  digits in the tests).

## 2. Junctions

A junction is the interface between the voxels of two grids.

- **Samples.** Each exposed face of a voxel is sampled at `S × S` points (`junction_samples`,
  default 3: the centres of an even subdivision of the face), pushed out along the face normal
  by `junction_reach` (default half a voxel). A sample that lands in a solid voxel of another
  grid joins the two voxels. A fragment's own faces give its "forward" samples; the samples of
  another grid's faces that land in it are its "reverse" samples.
- **Ownership.** Where two grids have voxels in the same space, the newer one (added later,
  higher id) owns it, as a member cast into an existing one does. So an interface is measured
  by the newer grid's faces alone, and each of its samples stands for its full `(h/S)²`.
  - Where the grids meet, the newer one always has faces at the interface or inside the older
    one's solid: flush against it, cast into it, at any angle.
  - The older grid's faces there sample nothing. Measured from both sides, a skewed interface
    gave slivers of the older grid's faces, half-counted, that carried more than their share.
  - A sample in a world-grid chunk that is not resident (the unknown world) holds the face, as
    for the world grid itself.
- **Bonds.**
  - Between two free fragments: one bond per fragment pair, taking all its samples. This is
    the world grid's rule for its own pair bonds, and it measured best against it (per-side
    junction bonds were tried; they were weaker than a member cast in whole).
  - To a support (anchored voxels, a fragment held fixed, the unknown world): one bond per side
    of the node's lattice and per grid, as the world grid's support bonds per axis and side.
- **Sections.** A sample is a small square at its point, with its face's normal. As for lattice
  faces:
  - the bond's normal is the area-weighted mean normal from a to b;
  - the section is the samples projected on the plane normal to it (each weighted by |n·nₖ|,
    at least 5%; the area at least a quarter of the total);
  - the section has principal axes, second moments and extreme fibres.

  Strengths come from both materials and their design classes.
- **Breaking.** A junction breaks sample by sample. Each broken sample is kept on the voxel
  whose face it samples (the newer grid's: `Chunk::jbroken`, or `BodyShape::jbrk` in a piece),
  so a failed interface stays failed, is saved in deltas, and goes along with pieces.
- **Cost.** Samples are computed only for chunks whose box meets another grid's box, once per
  extraction, so a world-grid-only world pays nothing.

**How close junctions come to monolithic members.** Measured values; the rows marked † are
tests in `tests/core/test_grids.cpp`:

| Configuration | Junctions | Monolithic |
|---|---|---|
| † Cantilever in a grid turned about z, on its own pier | φ 1.31903 | φ 1.31903 (the world grid) |
| † Cantilever cast 2 voxels into an anchored pier, square to it (mean over 8 placements) | φ 0.897 | φ 0.899 (the world grid) |
| † The same, turned 10°, 30°, 45° (the root skewed across the pier's face) | φ 0.896, 0.890, 1.12 | stepped into the world grid: 1.09, 1.09, 0.98 |
| † Steel bar cast into a free concrete block (mean over 12 lattice offsets) | 1.24 × | 1 (the world grid) |
| † The same bar set flush in a hole of the block | 0.93 × | 1 |
| † Concrete strut making a 4 m cantilever a truss (the prop removed) | stands, φ 0.66 | stepped strut: breaks at its steps |

The world grid's own utilization for the bar in the block ranges from 0.30 to 1.14 over those
offsets (the fragment layout changes with the lattice offset), so the junction values are well
within the variation of the lattice itself.

## 3. Using grids (C++)

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
| `add_grid(frame, voxels, base = true)` | Adds a grid; returns its id (0: refused, e.g. inside a tick or with a non-finite frame). `base`: part of the level (the level adds its grids again, in the same order, before `load_delta`; their changes are saved like the world grid's); else a change of this session, saved whole. After the design pass, a new grid is designed when first touched. Free voxels touching nothing fall as a piece at the next tick. |
| `remove_grid(id)` | Removes a grid's voxels (pieces that broke off it stay). What it held through junctions is extracted again: a slab resting on it falls. |
| `grids()`, `grid(id)`, `grid_frame(id, &f)` | The oriented grids (ascending ids), a grid's voxels, its frame. |
| `grid_to_world(id, p)`, `world_to_grid(id, X)` | Points between a grid's coordinates (metres: voxel `p`'s centre is `h p`) and the world. |
| `set_voxels(grid, edits, flags)` | Edits in a grid's coordinates. |
| `take_changed_grid_chunks()` | The oriented grids' chunks whose voxels changed, by grid, then chunk. |
| `layer(grid, L, p)`, `set_layer(grid, L, edits)` | Layers of a grid's voxels. |
| `set_loads(group, loads)` | `VoxelLoad::grid` names the voxel's grid. |
| `carve`, `blast` | Act on every grid the sphere reaches (the sphere in each lattice). |
| `raycast` | Walks each grid's lattice; `RayHit::grid` and `voxel` say what it hit, `shape` which shape of a piece. |
| `collide`, `sweep` | `collide` moves a box axis by axis against the world grid and against the grids (swept separating-axis tests against each grid voxel's cube). `sweep` moves a box in any direction and returns the normal of what stopped it, so a controller can slide along a turned wall. |
| `debug_field(grid, chunk, field, out)`, `probe_utilization(grid, voxel)` | Per grid. |
| `piece(id)->shapes[k]` | A piece's shapes: `grid` (where its voxels came from), `xf` (its lattice in the piece's frame), voxels in lattice coordinates. A lattice point `s` of shape `k` is at `Body::lattice_to_world(k, s)`. |
| `piece_layer`, `set_piece_layer`, `remove_piece_voxels` | Take a shape index (0: the first). |

- **Events:** `GridAdded` (a streamed grid came, or `load_delta` made one: its id, origin and
  rotation) and `GridRemoved` (removed, evicted with its home chunk, or by `load`).
- **Stats:** `WorldStats::grids`. `state_hash` includes the grids, their frames and junction
  breaks (only when there are grids: a world-grid-only world hashes as before).

### Rendering

- **A grid's chunks.** Mesh them in the grid's lattice (`mesh_chunk(*world.grid(id), chunk,
  opts)`: positions in the lattice, in metres), then place the vertices and normals with the
  frame: `X = origin + R v`. Key the meshes by grid and chunk. The game harness does exactly
  this (`Game::take_meshes`, `ChunkMesh::grid`).
- **Pieces.** Mesh each shape with `mesh_shape` (vertices in the shape's lattice), map them
  into the piece's frame with `shapes[k].xf.to(v)`, then draw at the piece's pose as usual. A
  shape whose `xf` is the identity (the first) is meshed exactly as before.

## 4. Persistence

- **Chunk records** (delta format version 4) end with the chunk's junction breaks. Version 3
  records load with none.
- **The grids' part.** An optional trailer after the world grid's records:
  - magic `SVXG` (0x47585653), version 1;
  - the ids of the level's grids that were removed;
  - per grid: its id, whether it is the level's, its frame (origin xyz and rotation xyzw as
    f64), and its chunk records. A level's grid saves its changed chunks; a session's grid
    saves all of its chunks.
  - A world of the world grid alone saves exactly as before, with no trailer.
- **Loading.**
  - Load the level (the world grid, then its grids in the same order), `bake`, `load_delta`.
  - The whole delta is checked first. A malformed trailer, or a level's grid that is missing
    (unless streamed), refuses it, and nothing is applied.
  - A session's grid is made again with its id.

## 5. Streaming

- `ChunkSource::grids(chunk)` lists the `SourceGrid {id, origin, rot}` at home in a chunk, and
  `generate_grid(id, out)` gives a grid's voxels. A grid is generated right after its home chunk
  and evicted with it. Ids are the source's: unique, stable, not 0.
- An evicted grid's changes are archived (under key `2^63 | id`) and come back with it. They
  are forgotten with its home chunk's region, and saved from the archive by `save_delta`.
- A streamed grid is designed on first touch, like a streamed chunk.
- Eviction does not make what rests on a grid fall (the world out of range is unknown, and
  holds); `remove_grid` does.

## 6. The C API

`svx/svx_core.h`:

- **Grids:** `svxc_add_grid` (a dense box of voxels, origin and rotation, base flag),
  `svxc_remove_grid`, `svxc_grids`, `svxc_grid_frame`.
- **Voxels, layers and loads per grid:** `svxc_set_grid_voxels`, `svxc_grid_chunk_voxels`,
  `svxc_poll_changed_grid_chunks` (4 ints each: grid, chunk x, y, z), `svxc_set_grid_layer`,
  `svxc_grid_layer`, `svxc_set_grid_loads`.
- **Pieces:** `svxc_piece_shape_count` and `svxc_piece_shape` (voxels, box, the shape's
  lattice in the piece's frame, its grid).
- **Queries and events:** `svxc_sweep`; `svxc_hit` carries `grid` and `shape`; the events
  `SVXC_GRID_ADDED` and `SVXC_GRID_REMOVED`; `svxc_stats::grids`.

## 7. The game and the browser

- **`Game`.**
  - Meshes the grids' changed chunks in world coordinates (`ChunkMesh::grid` set), and drops
    them when a grid goes (`take_removed_grid_chunks`).
  - Keeps the client's collision in step: `chunk_occupancy(chunk)` has a world voxel solid if
    its centre lies in a grid's solid voxel, and `take_occupancy_changed` lists the world chunks
    whose occupancy the grids changed.
- **The worker's ABI.**
  - `svx_mesh_info` gives the grid id in `out[9]`.
  - `svx_poll_removed_grid` / `svx_removed_grid_chunk` list emptied grid chunks.
  - `svx_poll_occupancy` / `svx_occupancy_chunk` list the world chunks whose occupancy the
    grids changed.
  - The worker keys a grid's chunk meshes `g<grid>:<x>,<y>,<z>`.
- **Procedural worlds** carry grids: `ProcWorld::grids`, added after the world grid with
  `add_grids(world, std::move(w.grids))`.
- **The `angles` world** (`?world=angles`, `svx_engine_demo --world angles`) has:
  - a frame building turned 30°;
  - a 17 m bridge deck at 35° cast into two world-grid piers;
  - a ramp pitched 15° onto a block;
  - a steel portal with a cross brace of two diagonal bars;
  - masonry walls at 20° (with a doorway) and 45°;
  - timber crates at their own yaws, one stacked on another;
  - a stone monolith leaning 12°.

  The engine demo's scenario blasts them.
- **Checks.**
  - `svx_engine_demo --turn DEG` runs any procedural world's structure in a grid turned about
    the vertical, on the world grid's ground.
  - `node web/scripts/angles-wasm.mjs http://localhost:5190/` checks the `angles` world in
    the browser: it draws, a box moving into the 45° wall stops at it, the player stands on the
    ramp, and the blasts run without errors.

## 8. Known limits

- **Fire, smoke and water act on the world grid's voxels only.** An oriented grid's static
  voxels do not burn, and water flows through them. Pieces burn and float whatever grid they
  came from.
- **Grids do not move.** A grid's frame is fixed; to move one, remove it and add it again, and
  its structural state goes with it.
- **Voxel size.** All grids have the world's voxel size.
- **Overlaps count twice for mass.** Where members overlap, both grids' voxels count for mass.
  A member cast in is joined by its surface, not fused. Its joint is on average about 20% more
  utilized than the member cast in whole, which is within the lattice's own variation (§2).
- **Ownership is by age.** The newer grid owns an overlap. A member added before the one it
  is cast into (a bar placed first, a column cast around it) is joined only where it leaves the
  newer member, not along its buried surface.
- **Minimum contact.** A contact smaller than a sample (a third of a voxel at the default
  `junction_samples`) may not bond, and a member held by a few samples is nearly a hinge. Its
  structure is solved with the multigrid (see V2_DESIGN.md §2).
- **The browser's collision is stepped.** The web client's collision samples the grids at
  world voxel centres, so collision against a turned wall is stepped by up to a voxel. The
  engine's `collide` and `sweep` test the grids' voxels themselves.
- **The far render tier** of streamed worlds draws the world grid only.
