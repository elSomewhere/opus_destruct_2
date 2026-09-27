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
                   coarse far tiles, water surfaces. Optional. Depends on svx_core.
env/    svx_env    environment systems on the core's extension points (§5): fire, smoke,
                   water (docs/ENV.md). Optional. Depends on svx_core.
game/   svx_game   the prototype harness: svx::Game (viewer, movers, triggers, command log,
                   piece meshes and poses, fading, far tier, the environment), procedural and
                   city levels, Doom WAD import, the web worker's C ABI. Depends on svx_core,
                   svx_mesh and svx_env.
web/               the TypeScript / WebGPU front end of the game (talks to svx_game's ABI).
```

The rule that keeps them apart: **code only reaches down**, and only through public headers.
The game harness uses nothing of the core but `World`'s public API (it has no friend access and
includes no private header), so the core can change freely behind that API and the harness can
be rewritten without touching the core. Each layer has its own tests:

- `svx_core_tests` links `svx_core` alone (stress, fragments, rigid bodies, the world API, the
  C API, the extension points, audit regressions).
- `svx_env_tests` links `svx_env` and the core (fire, smoke, water).
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

**Materials** live in a process-wide registry (`svx/material/material.hpp`): eleven presets
(reinforced concrete, concrete, steel, masonry, soil, rock, indestructible bedrock, wood, stone,
glass, reinforcing bar), up to 127 in all. A material has stiffness (E, G), density, interface
strengths (tension, flexural tension, crushing, Mohr–Coulomb cohesion and friction), a fracture
energy, and a rubble size (the fragment spacing per axis). A bond's section takes its strengths
from the materials of its faces, so a composite section (concrete with bars in it) is as strong
as its parts. **Ductile** materials (steel, bars) bend rather than shatter: they are not
pulverized or carved by impacts. **Reinforcement** voxels (bars) join the fragments of the
material around them, tying a member together. Register or override materials at startup,
before any world steps.

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
- Chunks beyond `evict_radius` are evicted. Their changes go to the change archive and come
  back with them. The archive is bounded; see §4 (Memory).
- Chunks are never evicted under a moving piece. Sleeping rubble in chunks being evicted is
  unloaded with them (`PieceRemoved`, `Unloaded`), so rubble never pins the world.
- Structures reaching into chunks that are not resident are held there (the unknown world is a
  support).
- A generated structure is designed the first time something touches it.
- `ChunkSource::region(chunk)` names the unit a chunk's changes are remembered and forgotten
  with. The default is 8 × 8 chunk columns; the city generator uses its blocks, so a building
  never comes back in half.

A bounded level too large to keep resident streams the same way, from a source that reads the
level (a file, a compressed grid). It then usually keeps every change (`archive_mb = 0`), since
the level bounds them.

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
  (violent collapses step once per tick with fewer iterations). Memory is bounded as in §4.

## 4. Memory: bounded by construction

Every kind of state a world keeps has a bound, so a session never grows past a known size
however long it runs, and nothing lives on after what it belongs to:

| State | What bounds it | Beyond the bound |
|---|---|---|
| Resident voxels (the grid) | The level; for streamed worlds the radii and `max_resident_mb` | Chunks farthest from the focus are evicted |
| **Change archive** (streamed chunks changed and out of range) | `StreamConfig::archive_mb`: one arena allocated once, pages of 1 KB | Whole regions are forgotten, least recently seen first; they come back as generated |
| Fragment caches (derived from the grid) | `MemoryBudget::fragment_cache_mb` | Unheld ones dropped, least recently used first (rebuilt identically on demand) |
| Registered structures (graphs, matrices, preconditioners) | `MemoryBudget::structure_mb` | Idle ones dropped, longest idle first (extracted again when touched) |
| Rigid pieces | `max_bodies` and `MemoryBudget::piece_mb` | The smallest culled, sleeping first (`PieceRemoved`, `Culled`) |
| Warm starts, reference loads | `MemoryBudget::cache_mb`; streamed: resident chunks only | Only the registered structures' kept |
| Output the host does not take | `MemoryBudget::max_events`; changed chunks deduplicated | The oldest cosmetic events go |

### The change archive: remembering and forgetting

A streamed world is infinite, but the memory of what the player changed need not be.

- Changes of chunks out of range live in a fixed arena of `archive_mb`. Records are
  compressed per chunk and chained through 1 KB pages, so the arena neither grows nor
  fragments, and its pages are committed only as they are used.
- When the arena is full, the world forgets the region seen least recently, among those with no
  resident chunk. The region comes back from the generator exactly as generated: the world
  heals out of sight. Its structures are designed again when first touched.
- With `forget_after_s`, regions out of range for that long are forgotten even if there is room
  left.
- Each forgotten region is a `Forgotten` event (its centre and chunk count). Stats count
  forgotten regions and chunks, and report the arena's use.
- `archive_mb = 0` keeps every change (the arena grows). This is for bounded levels streamed
  from a file, whose changes their size bounds.

`save_delta()` saves what is remembered: the resident changes and the archive.

### Bounded levels (islands, Doom maps, Teardown-style scenes)

A level loaded whole (`load` + `bake`) holds its grid, which the level bounds. Destruction
does not grow it: emptied chunks are compacted, and chunk arrays are recycled through a bounded
pool. Everything derived from the grid is under the budgets above, and changes stay (they are
the level's state; `save_delta` persists them).

### Determinism under budgets

Budgets that bound derived data change no result: fragment caches and warm starts are rebuilt
identically, and events are output only. A test checks that a session gives the same hash under
tight and loose budgets. Culling pieces and forgetting regions do change the world. Both are
deterministic functions of the configuration and the commands, so replays and lockstep stay
exact as long as every peer uses the same configuration.

### Watching it

- `World::memory()` reports the bytes held by kind (grid, fragment caches, structures, pieces,
  archive, caches, queues).
- `stats()` counts what the budgets removed.
- `svx_soak` runs long sessions (a streamed city crossed for minutes with continuous
  destruction, or a bounded level) and prints both, together with the process's physical
  footprint.

## 5. Extension points: layers, damage, loads, forces, systems

The core simulates destruction and structural integrity, and nothing else. Other physics
(fire, fluids, weather, corrosion, a game's own rules) plug in through five public extension
points, without touching the core. `svx_env` ([`ENV.md`](ENV.md)) is built on them alone.

- **Voxel layers** (`add_layer(LayerSpec{name, persistent, bind})`, `set_layer`, `layer`,
  `take_layer_changes`): up to 8 named byte channels per voxel, stored sparsely per chunk.
  - A persistent layer is part of the chunk's changes: saved in deltas, archived when the
    chunk is evicted, forgotten with its region.
  - A transient one leaves with the chunk.
  - Its binding says what a value belongs to: `Solid` (damage, heat, char: cleared when the
    voxel is removed or replaced), `Air` (water, gas: cleared when a solid takes the place) or
    `Place` (kept). Adding a layer again returns its index; a different layer under the same
    name is refused (-1).
  - In a streamed world, writes to chunks not generated yet are dropped. Layer changes do not
    count as a player's for the design pass (only changed voxels and bonds do).
  - Layers are matched by name when a grid is loaded, so a level can carry them. A
    `ChunkSource` fills them for generated chunks (`generate_layer`).
  - Pieces carry their voxels' layer values (`piece_layer`, `set_piece_layer`).
- **Damage** (`kDamageLayer`, always present, bound to the solid voxel): 0 intact to 255 no
  strength left (a section at 255 fails under any load). It scales the strengths of every bond
  section it is in. Changing it re-measures the bonds of the voxels written (those of their
  nodes) and judges their structures again at the next tick, and extracts a structure nobody
  holds. On pieces it rebuilds their bond graph.
- **Loads on the static world** (`set_loads(group, loads)`): forces on voxels by group. They
  are replaced as a whole, stay until replaced, and are added to the structures' load cases
  every solve. New or changed loads extract the structures under them, and the design pass
  designs for them. (A producer writing its loads chunk by chunk makes them cheap to apply
  every tick.)
- **Forces on pieces** (`apply_force`, for the next tick only; `wake_piece`).
- **Systems** (`add_system(std::shared_ptr<WorldSystem>)`): objects stepped at the end of every
  tick, in the order added.
  - The world tells them what they could not see coming: `on_load`, `on_generated` and
    `on_evicted` (streaming), and `on_voxels_changed` (only chunks resident when they are
    told). Paused, they are told but not stepped.
  - From a system, `tick`, `load` and `load_delta` are refused (the world is inside its tick);
    a system added by a system is stepped from the next tick.
  - Their `memory_bytes` joins `MemoryReport::systems`, and their `state_hash` joins
    `session_hash`, so determinism checks cover them.
  - Two more hooks change the world from outside the core: `remove_piece_voxels` (burnt out,
    melted) and `set_voxels`.

## 6. The C API and WASM

`svx/svx_core.h` wraps `World` for hosts in other languages (C, C#, Rust, Python, Zig, ...):

- Worlds are handles (`svxc_world*`).
- Voxels come in as dense boxes (`svxc_load_box`), edits, or a streaming callback
  (`svxc_enable_streaming`).
- Every `WorldConfig` / `WorldParams` field is settable by name (`svxc_set(w, "rigid.gravity",
  9.81)`): the registry of `svx/world/tunables.hpp`, which C++ hosts use as well (settings UIs,
  command logs: an index is stable within a build; setup tunables are meant for before a load).
- Events, pieces (with their voxels), changed chunks and queries come out as plain structs and
  arrays.

- The extension points (§5) are there too: layers (`svxc_add_layer`, `svxc_set_layer`,
  `svxc_chunk_layer`, `svxc_poll_layer_changes`), piece layers and voxel removal, loads, piece
  forces, and systems with all their callbacks (`svxc_add_system_ex`).

`examples/c_api/main.c` is a complete C host.

The `svx_core_web` target (WASM builds) packages the core alone as an ES module
(`createSvxCore`) with the `svxc_*` functions exported, for JavaScript hosts.

## 7. Writing a harness

A harness turns a game's world into the core's terms and the core's output into the game's.
`svx::Game` is a worked example:

| Game concept | How it maps onto the core |
|---|---|
| Player / camera | `set_focus` (streaming); triggers are the harness's own business. |
| Doors, lifts, crushers | Anchored voxels written with `set_voxels(..., kEditUntracked \| kEditIsolated)` every time they move. A carve that hollows one is noticed after the tick by looking at its voxels. |
| Weapons | `carve` (bullets), `blast` (rockets), `apply_impulse` (pushes); fire and water through `svx_env`'s systems (`ignite`, `pour`). |
| Level loading | Build a `VoxelGrid` (procedural, WAD voxelizer, editor), `load`, `bake`; or a `ChunkSource` for streamed levels. |
| Replays, lockstep networking | Log the commands with their tick; replay them on the same level (`game/src/replay.cpp`). |
| Rendering | Chunk meshes from `take_changed_chunks`, piece meshes on `PieceAdded`, poses every frame, fades on `Culled`. |

Things a harness should not do:

- reach into `World`'s internals;
- write the grid other than through `set_voxels`;
- keep `Body` pointers across ticks (use ids).

## 8. Known limits

- `collide` sweeps against the world's voxels only. Pieces are obstacles for rays, not for box
  sweeps.
- Materials are process-wide, not per world.
- Pieces are not persisted in deltas, and sleeping rubble is unloaded with its chunks.
- Structures larger than `structure_max_nodes` (60,000 fragments or clusters) or
  `structure_max_radius` (60 m) around the event are solved in part, with their frontier held
  fixed.
