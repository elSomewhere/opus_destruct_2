# structvox core: the destruction physics as a library

The physics of structvox is a self-contained C++20 library, `svx_core`, with no knowledge of
rendering, players, levels or any particular game. The prototype game in this repository is one
host of it; any other program (a different game, an engine plugin, a tool, a test rig) can be
another. This page is the guide for such hosts. The method itself is in
[`V2_DESIGN.md`](V2_DESIGN.md).

## 1. Layers

```
core/   svx_core   voxel grid, materials, fragments, structures, stress solver, rigid pieces,
                   streaming, persistence. Public API: svx::World (svx/world/world.hpp) and the
                   C API (svx/svx_core.h). Depends on the C++ standard library only.
mesh/   svx_mesh   voxel meshing for renderers: world chunks, pieces (in their shape frame),
                   coarse far tiles. Optional. Depends on svx_core.
game/   svx_game   the prototype harness: svx::Game (viewer, movers, triggers, command log,
                   piece meshes and poses, fading, far tier), procedural and city levels,
                   Doom WAD import, the web worker's C ABI. Depends on svx_core and svx_mesh.
web/               the TypeScript / WebGPU front end of the game (talks to svx_game's ABI).
```

The rule that keeps them apart: **code only reaches down**, and only through public headers.
The game harness uses nothing of the core but `World`'s public API (it has no friend access and
includes no private header), so the core can change freely behind that API and the harness can
be rewritten without touching the core. Each layer has its own tests:

- `svx_core_tests` links `svx_core` alone (stress, fragments, rigid bodies, the world API, the
  C API, audit regressions).
- `svx_game_tests` exercises the harness (collapse scenarios on game levels, streaming city,
  Doom maps and movers, replays).
- `examples/core_minimal` (C++) and `examples/c_api` (C) are complete hosts in ~80 lines.

## 2. Concepts

**Voxels.** A world is a grid of voxels of size `h` (0.125 m is what the defaults are tuned
for), stored in 32³ chunks. Voxel `p` (integers) is the cube centred at `h p`; z is up. A voxel
is one byte: 0 air, else `1 + material` in the low 7 bits and bit 7 **anchored**
(`make_vox(material, anchored)`).

- **Anchored** voxels are supports: bedrock, foundations, and kinematic parts a host moves
  itself. They never move and never break (carves can remove them unless their material is
  indestructible).
- **Free** voxels are structure: they are simulated.
- Every two face-adjacent solid voxels are **bonded**, unless both are anchored or the bond was
  broken (a bit per voxel face).

**Materials** live in a process-wide registry (`svx/material/material.hpp`): seven presets
(reinforced concrete, concrete, steel, masonry, soil, rock, indestructible bedrock), up to 127 in
all. A material has stiffness (E, G), density, interface strengths (tension, flexural tension,
crushing, Mohr–Coulomb cohesion and friction), a fracture energy, and a rubble size (the
fragment spacing per axis). Register or override materials at startup, before any world steps.

**Fragments** are the pre-scored rubble pieces the free voxels are grouped into (a jittered
Voronoi partition per material, within each chunk). Fragments never break; **bonds** between
fragments do.

**Structures** are connected sets of fragments that reach a support. They are extracted when
something happens to them, solved for equilibrium (K u = f: gravity, contact forces of pieces,
blast loads) under a per-tick work budget, and judged: the worst overloaded bonds break, parts
that lose their supports leave as pieces, and the rest is solved again.

**Pieces** are rigid bodies made of fragments, and they keep their bonds. After each contact
solve, a piece's stress is checked under its contact forces and inertia. If bonds break, it
splits (the step is solved again with the parts), and crushed material turns to dust.

**The design pass** (`bake`, and first touch for streamed chunks) solves every structure under
its own weight and strengthens members above `design_utilization`, so that a level stands as
built. This lets level authors (or generators) build freely without engineering every column.

## 3. Using a World (C++)

```cpp
#include "svx/world/world.hpp"
using namespace svx;

VoxelGrid g;
g.h = 0.125;
g.fill_column(x, y, z0, z1, make_vox(MaterialId::Concrete, false));  // ... build, then
g.compact();

World world;
world.configure(WorldConfig{});   // optional: dt, budgets, rigid parameters, ...
world.load(std::move(g));
world.bake();                     // design pass

world.blast({2.0, 1.0, 0.5}, 0.8, 3e5);   // commands are queued for the next tick
for (;;) {
  world.tick();                            // 1/60 s by default
  for (const WorldEvent& e : world.take_events()) { /* pieces added / removed, cracks, ... */ }
  for (u64 key : world.take_changed_chunks()) { /* re-mesh chunk unkey3(key) */ }
  for (const PieceState& p : world.pieces()) { /* draw piece p.id at p.pos, p.rot */ }
}
```

### Commands

| Call | Effect |
|---|---|
| `carve(pos, radius)` | Removes the voxels in a sphere at the next tick. Indestructible materials are left. |
| `blast(pos, radius, energy)` | Carves, throws the fragments around the crater as pieces, and loads the structures near it (J). |
| `set_voxels(edits, flags)` | Writes voxels now. Structures there are extracted again and pieces near are woken. `kEditIsolated`: the written voxels bond to nothing (doors, lifts). `kEditUntracked`: not in the persistence delta (parts the host rebuilds on load). Free voxels written into the air become a falling piece, which is how to drop or spawn objects. |
| `apply_impulse(piece, point, J)` | Pushes a piece (N s at a world point): throws, explosions, a character's push. |
| `remove_piece(piece)` | Removes a piece (`PieceRemoved`, `Removed`). |
| `set_params(...)` | Runtime knobs: `fragility` (divides every strength), `impact` (scales contact loads), `dif` (dynamic increase factor), `paused`. |

### Output

- **Events** (`take_events`), in order:
  - `PieceAdded`: `id`, the `parent` piece it broke from (0 for the static world), pose,
    velocity and voxel count.
  - `PieceRemoved`: `id` and the reason (`end`):
    - `Split`: it broke, or changed shape; its parts are new pieces with this one as parent.
    - `Culled`: over `max_bodies`, the smallest sleeping pieces go first.
    - `OutOfWorld`: it fell below the world.
    - `Removed`: removed by the host, or by `load`.
  - `Crack`: a bond broke (position, normal, utilization).
  - `Impact`: a blast or a heavy landing (energy).
  - `Dust`: crushed material, or a shard too small to be a piece.

  Cosmetic events (cracks, dust, landings) are budgeted per tick. Piece events never are.
- **Pieces** (`pieces()`, `piece(id)`): pose (centre of mass `pos`, rotation `rot` from the shape
  frame), velocities, mass, sleep state. `piece(id)->shape` holds the voxels in the shape frame
  (the grid coordinates the piece had when it was made). A shape point `s` is at
  `pos + rot (s - com)`.
- **Chunks**: `take_changed_chunks()` lists the chunks whose voxels changed (carves, detachments,
  edits, streaming). `take_evicted_chunks()` lists the ones no longer resident.
- **Queries**:
  - `raycast` hits the world and the pieces.
  - `collide` moves a box as far as the world's voxels let it (a character controller's sweep;
    pieces are not obstacles to it).
  - `debug_field` gives per-voxel utilization or fragment colours of a chunk.
  - `probe_utilization` solves the structure holding a voxel now.
- **Stats and hashes**: `stats()`; `state_hash()` (voxels and broken bonds);
  `session_hash()` (plus the pieces' poses).

### Rendering

The core never meshes. A host can:

- Mesh changed chunks with `svx_mesh` (`mesh_chunk`: greedy, AO, texture/light/debug providers),
  or with its own mesher (read `grid().chunk(...)`).
- Mesh a piece once, when its `PieceAdded` event arrives: `mesh_shape(piece(id)->shape, h, opts)`
  gives vertices in the shape frame. Draw it every frame at `pos + rot (s - com)`. A piece never
  changes shape: a new shape comes as a new piece (`Split`).
- Use `Crack`, `Dust` and `Impact` for particles, decals, sound and camera shake.

The game harness (`game/src/game.cpp`) does exactly this. It also fades out culled pieces.

### Streaming

Large worlds are generated on demand. Implement `ChunkSource`:

- `generate(chunk, out)` is a pure function of the chunk, called from several threads.
- `chunk_lo()` and `chunk_hi()` give the extent in chunks.

Then call `enable_streaming(source, StreamConfig)`. `set_focus(points)` tells the world where it
must be resident: one point per player, camera or AI of interest.

- Chunks within `load_radius` are generated, a budgeted number per tick.
- Chunks beyond `evict_radius` are evicted. Their changes are archived and come back with them.
- Chunks are never evicted under a piece.
- Structures reaching into chunks that are not resident are held there (the unknown world is a
  support).
- A generated structure is designed the first time something touches it.

### Persistence

`save_delta()` is a binary delta of the changed chunks (voxels, broken bonds, design classes)
against the regenerable base world. `load_delta()` applies one:

- Load the same base world, `bake()`, then `load_delta()`.
- A malformed delta is refused whole: nothing is applied.

Pieces in flight are not part of a delta.

### Determinism

Given the same world, configuration and commands at the same ticks, a session is bit-identical:

- on any thread count;
- natively and in WASM, on ARM and x86 (no transcendental functions on the simulation path at the
  default substeps of whole multiples of 1/120 s).

This is what lockstep networking and replays need (see `game/include/svx/game/replay.hpp`).
Things that break it:

- changing the material registry while worlds step;
- changing `WorldConfig` at different ticks;
- feeding commands in a different order.

### Threading and limits

- A `World` is not thread-safe: call it from one thread.
- It runs its work on a shared deterministic pool (`set_num_threads`). Worlds stepped on several
  threads take turns on the pool.
- Inputs are validated. Non-finite or out-of-range positions (beyond about ±2²⁰ voxels), radii,
  energies, rays, sweeps, edits and parameters are refused or clamped. Carves and blasts are
  clamped to `max_event_radius`.
- Work per tick is bounded by `stress_work` (structures) and the busy mode of the rigid solver
  (violent collapses step once per tick with fewer iterations). Memory is bounded by
  `max_bodies` and the streaming radii / `max_resident_mb`.

## 4. The C API and WASM

`svx/svx_core.h` wraps `World` for hosts in other languages (C, C#, Rust, Python, Zig, ...):

- Worlds are handles (`svxc_world*`).
- Voxels come in as dense boxes (`svxc_load_box`), edits, or a streaming callback
  (`svxc_enable_streaming`).
- Every `WorldConfig` / `WorldParams` field is settable by name (`svxc_set(w, "rigid.gravity",
  9.81)`).
- Events, pieces (with their voxels), changed chunks and queries come out as plain structs and
  arrays.

`examples/c_api/main.c` is a complete C host.

The `svx_core_web` target (WASM builds) packages the core alone as an ES module
(`createSvxCore`) with the `svxc_*` functions exported, for JavaScript hosts.

## 5. Writing a harness

A harness turns a game's world into the core's terms and the core's output into the game's.
`svx::Game` is a worked example:

| Game concept | How it maps onto the core |
|---|---|
| Player / camera | `set_focus` (streaming); triggers are the harness's own business. |
| Doors, lifts, crushers | Anchored voxels written with `set_voxels(..., kEditUntracked \| kEditIsolated)` every time they move. A carve that hollows one is noticed after the tick by looking at its voxels. |
| Weapons | `carve` (bullets), `blast` (rockets), `apply_impulse` (pushes). |
| Level loading | Build a `VoxelGrid` (procedural, WAD voxelizer, editor), `load`, `bake`; or a `ChunkSource` for streamed levels. |
| Replays, lockstep networking | Log the commands with their tick; replay them on the same level (`game/src/replay.cpp`). |
| Rendering | Chunk meshes from `take_changed_chunks`, piece meshes on `PieceAdded`, poses every frame, fades on `Culled`. |

Things a harness should not do:

- reach into `World`'s internals;
- write the grid other than through `set_voxels`;
- keep `Body` pointers across ticks (use ids).

## 6. Known limits

- `collide` sweeps against the world's voxels only. Pieces are obstacles for rays, not for box
  sweeps.
- Materials are process-wide, not per world.
- Pieces are not persisted in deltas.
- Structures larger than `structure_max_nodes` (60,000 fragments or clusters) or
  `structure_max_radius` (60 m) around the event are solved in part, with their frontier held
  fixed.
