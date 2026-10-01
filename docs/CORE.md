# structvox core: the destruction physics as a library

The physics of structvox is a self-contained C++20 library, `svx_core`, with no knowledge of
rendering, players, levels or any particular game. The prototype game in this repository is one
host of it; any other program (a different game, an engine plugin, a tool, a test rig) can be
another. This page is the guide for such hosts. The method itself is in
[`V2_DESIGN.md`](V2_DESIGN.md).

## 1. Layers

```
core/   svx_core   voxel grids (the world grid and oriented grids), materials, fragments,
                   structures, stress solver, rigid pieces, streaming, persistence. Public API:
                   svx::World (svx/world/world.hpp) and the C API (svx/svx_core.h). Depends on
                   the C++ standard library only.
mesh/   svx_mesh   voxel meshing for renderers: world chunks, pieces (in their shape frame),
                   coarse far tiles, water surfaces. Optional. Depends on svx_core.
env/    svx_env    environment systems on the core's extension points (§5): fire, smoke,
                   water (docs/ENV.md). Optional. Depends on svx_core.
anim/   svx_anim   characters (docs/ANIM.md): voxel people, their motion plan, their bodies (an
                   articulation of the world, or their own), behaviours. Depends on svx_core.
game/   svx_game   the prototype harness: svx::Game (viewer, movers, triggers, command log,
                   piece meshes and poses, fading, far tier, the environment, vehicles and
                   traffic, pedestrians), the game's materials, levels as it loads them, Doom
                   WAD import. Depends on svx_core, svx_mesh, svx_env and svx_anim.
procgen/ svx_procgen  procedural generation (to be replaced): the test levels, the streamed
                   city, the endless drive city - Levels and chunk sources the game loads.
                   Depends on svx_game's level and source interfaces.
game/api svx_api   the web worker's C ABI (svx/game/api/svx_api.h): picks the levels.
web/               the TypeScript / WebGPU front end of the game (talks to svx_api).
```

The core knows nothing of what is built on it: a vehicle is voxels of a host's materials on the
core's wheels and joints, and it crumples, loses parts and is holed by the core's generic rules
([`DAMAGE.md`](DAMAGE.md), [`MOTION.md`](MOTION.md) §7); a person is an articulation.

The rule that keeps them apart: **code only reaches down**, and only through public headers.
The game harness uses nothing of the core but `World`'s public API (it has no friend access and
includes no private header), so the core can change freely behind that API and the harness can
be rewritten without touching the core. Each layer has its own tests:

- `svx_core_tests` links `svx_core` alone (stress, fragments, rigid bodies, the world API, the
  C API, the extension points, audit regressions).
- `svx_env_tests` links `svx_env` and the core (fire, smoke, water).
- `svx_anim_tests` links `svx_anim` and the core (motion, bodies on both physics paths,
  behaviours, voxel models).
- `svx_game_tests` exercises the harness (collapse scenarios on game levels, streaming city,
  Doom maps and movers, replays, vehicles, pedestrians, the C ABI).
- `tools/baseline/golden.sh` pins the engine's world hashes in fixed scenarios, and the
  structural reference's, reproduced bit for bit with its switches ([`BASELINE.md`](BASELINE.md)).
- `examples/core_minimal` (C++) and `examples/c_api` (C) are complete hosts in ~80 lines.

**Inside the core.** `World` is its public API and nothing else: `svx/world/world.hpp` declares
what hosts call and holds its implementation, `World::Impl` (`core/src/world/world_impl.hpp`,
private to the core's sources), which every call forwards to - a change behind the API
recompiles no host. The implementation is organised by subsystem, a source file each: the tick,
structures and loads (`world.cpp`); grids and their junctions (`world_grids.cpp`); pieces and
their fracture (`world_pieces.cpp`); crumpling (`world_crumple.cpp`); joints
(`world_joints.cpp`); wheels (`world_wheels.cpp`); articulations (`world_articulations.cpp`);
the design pass, persistence, streaming and queries (`world_io.cpp`); sessions and the change
archive's records (`world_session.cpp`); the extension points (`world_ext.cpp`). The state a
subsystem owns is grouped with it (`Impl::att_`: the joints' and wheels' anchors; `strm_`:
streaming and the archive; `ext_`: layers, systems, host loads and change tracking; `pw_`: a
tick's piece work).

## 2. Concepts

**Voxels.** A world is a grid of voxels of size `h` (0.125 m is what the defaults are tuned
for), stored in 32³ chunks. Voxel `p` (integers) is the cube centred at `h p`; z is up. A voxel
is one byte: 0 air, else `1 + material` in the low 7 bits and bit 7 **anchored**
(`make_vox(material, anchored)`).

- **Anchored** voxels are supports: bedrock, foundations (and a Doom level's movers, which the
  harness writes itself). They never move and never break (carves can remove them unless their
  material is indestructible).
- **Free** voxels are structure: they are simulated.
- Every two face-adjacent solid voxels are **bonded**, unless both are anchored or the bond was
  broken (a bit per voxel face).

**Grids.** Besides the world grid (its axes the world's), a world can hold **oriented grids**:
voxel lattices placed with a position and rotation of their own and a voxel size of their own (a
building at an angle, a diagonal brace, a ramp, a fine railing), simulated like the world grid in
every respect. Where the voxels of two grids meet they are bonded by **junctions**, so structures
and pieces span grids; where they overlap, the grid of higher priority keeps its voxels. A grid
can be placed anew. See [`GRIDS.md`](GRIDS.md).

**Motion.** What moves by design is pieces on **joints**: joints hold pieces, grids' voxels and
the world together (hinges, sliders, ropes, rods, welds; limits, a breaking strength) and load
what they hold on to; a hinge's or a slider's **drive** moves it at a speed, to a target, or on a
program of the world's clock. A machine - a lift's car, a turntable, a drawbridge, a crane's jib -
is pieces on driven joints held by structures: what rides on it is carried, and it comes down
with what holds it. **Articulations** are bodies of linked parts - links that collide as
spheres, on joints with anatomical limits and muscles, pulled by targets - that their host drives
every tick: a person, a creature, a robot, a rag doll. Their links are bodies of the world like
the pieces (a car that hits one hits it; it stands on the structures and loads them), stepped
finer on their own while no awake piece is near; they sleep, save and stream with their host's
data. See [`MOTION.md`](MOTION.md) (§6) and, for the characters built on them,
[`ANIM.md`](ANIM.md).

**Materials** live in tables (`svx/material/material.hpp`): every world has its own
(`World::materials`, `set_material`, `register_material`), made from the process's
(`default_materials`: what a host sets up at startup) when the world is made, so worlds may
differ in their materials. Eleven presets (reinforced concrete, concrete, steel, masonry, soil,
rock, indestructible bedrock, wood, stone, glass, reinforcing bar), up to 127 in all. A material has stiffness (E, G), density, interface
strengths (tension, flexural tension, crushing, Mohr–Coulomb cohesion and friction), a fracture
energy, and a rubble size (the fragment spacing per axis). A bond's section takes its strengths
from the materials of its faces, so a composite section (concrete with bars in it) is as strong
as its parts. **Ductile** materials (steel, bars) bend rather than shatter: they are not
pulverized or carved by impacts. **Reinforcement** voxels (bars) join the fragments of the
material around them, tying a member together. Register or override a world's materials before
it loads (what it builds from them - fragments, structures, pieces - keeps what it was built
with).

**Fragments** are the pre-scored rubble pieces the free voxels are grouped into (a jittered
Voronoi partition per material, within each chunk). Fragments never break; **bonds** between
fragments do.

**Structures** are connected sets of fragments that reach a support. They are extracted when
something happens to them, solved for equilibrium (K u = f: gravity, contact forces of pieces,
blast loads) under a per-tick work budget, and judged: the worst overloaded bonds break, parts
that lose their supports leave as pieces, and the rest is solved again. What happens later to a
registered structure patches it: only the chunks that changed are fragmented and bonded again (a
hole in a tower is a few chunks' work). A structure cut short by its reach
(`structure_max_radius`) is extracted again about an event near its frontier instead, so that no
artificial support is next to what happens.

**Pieces** are rigid bodies made of fragments, and they keep their bonds. After each contact
solve, a piece's stress is checked under its contact forces and inertia. If bonds break, it
splits (the step is solved again with the parts), and crushed material turns to dust. A piece
that may move more than half a voxel in a substep looks along its motion: the nearest faces its
samples would reach become speculative contacts (`rigid.speculative`), so a fast piece stops at
a thin wall instead of passing through it. What a blast's load breaks off a structure moves off
with the momentum the blast gave its fragments.

**The design pass** (`bake`, and first touch for streamed chunks) solves every structure under
its own weight and strengthens members above `design_utilization`, so that a level stands as
built. This lets level authors (or generators) build freely without engineering every column.
A structure designed on first touch takes its new strengths in place (its bonds are not
extracted again) and starts its solve from the design state.

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
| `blast(pos, radius, energy)` | Carves, throws the fragments around the crater as pieces, and loads the structures near it (J): what the load breaks off moves off with the blast's momentum. |
| `set_piece_keep(piece, keep)` | A piece the host keeps: never culled over the pieces' budget (a joint's pieces are kept anyway). |
| `set_voxels(edits, flags)` | Writes voxels now. Structures there are extracted again and pieces near are woken. `kEditIsolated`: the written voxels bond to nothing (a level's movers). `kEditUntracked`: not in the persistence delta (parts the host rebuilds on load). Free voxels written into the air become a falling piece, which is how to drop or spawn objects. |
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
  - `GridAdded`, `GridRemoved`, `GridMoved`: an oriented grid came, went or was placed anew
    (streamed, removed, moved, loaded).
  - `JointBroken`: a joint gave way (its force), or lost its hold (its voxel is gone).

  Cosmetic events (cracks, dust, landings) are budgeted per tick. Piece events never are.
- **Pieces** (`pieces()`, `piece(id)`): pose (centre of mass `pos`, rotation `rot` from the shape
  frame), velocities, mass, sleep state. `piece(id)->shapes` hold the voxels, one shape per grid
  the piece came from, in that grid's coordinates. The first shape's lattice is the piece's shape
  frame: a point `s` of it is at `pos + rot (s - com)`. The others carry their lattice's place in
  that frame (`shapes[k].xf`; `Body::lattice_to_world(k, s)`).
- **Chunks**: `take_changed_chunks()` lists the world grid's chunks whose voxels changed
  (carves, detachments, edits, streaming), `take_changed_grid_chunks()` the oriented grids'.
  `take_evicted_chunks()` lists the ones no longer resident.
- **Queries**:
  - `raycast` hits the grids and the pieces.
  - `collide` moves a box as far as the grids' voxels let it (a character controller's sweep;
    pieces are not obstacles to it), and reports what it stands on and that surface's velocity
    (a lift carries its rider). `sweep` moves a box in any direction and returns the normal and
    velocity of what stopped it (sliding along a turned wall).
  - `debug_field` gives per-voxel utilization or fragment colours of a chunk.
  - `probe_utilization` solves the structure holding a voxel now.
- **Stats and hashes**: `stats()`; `state_hash()` (voxels and broken bonds);
  `session_hash()` (plus the pieces' poses).

### Rendering

The core never meshes. A host can:

- Mesh changed chunks with `svx_mesh` (`mesh_chunk`: greedy, AO, texture/light/debug providers),
  or with its own mesher (read `grid().chunk(...)`).
- Mesh a piece once, when its `PieceAdded` event arrives: `mesh_shape(piece(id)->shapes[k], h,
  opts)` gives each shape's vertices in its lattice; `shapes[k].xf.to(v)` puts them in the shape
  frame (the identity for the first). Draw it every frame at `pos + rot (s - com)`. A piece never
  changes shape: a new shape comes as a new piece (`Split`).
- Mesh an oriented grid's changed chunks in its lattice and draw them with its frame
  ([`GRIDS.md`](GRIDS.md) §6).
- Draw joints from `joint(id, &state)`: a rope between its ends.
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
- Chunks are never evicted under a moving piece. Sleeping rubble in chunks being evicted goes
  with them into the change archive (`PieceRemoved`, `Unloaded`), so rubble never pins the world,
  and comes back where it lay when its chunks do (`PieceAdded`, the same ids). What is wholly out
  of range - a machine running there (with its joints and what it carries), debris flying off -
  is archived as it is, and comes back as it was, a machine's drive on its program. Pieces are
  grouped by their joints (and, out of range, what touches them): a group goes and comes back
  whole, with its region's changes, in the archive's budget, and is forgotten with its region.
  What rests on a piece that goes is not woken: it goes too, or the piece comes back under it.
- A source's grid changed, placed anew (`set_grid_frame`) or removed (`remove_grid`) in play stays
  so: it comes back changed and where it was put, or not at all.
- A source places joints with `ChunkSource::joints(chunk)` (`SourceJoint`: a stable id and a
  `JointDesc` on the grids at home in the chunk, or the world grid's voxels): made when the
  chunk's grids are, not while the machine is archived, again when it was forgotten.
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

`save_delta()` is a binary delta of the changed chunks (voxels, broken bonds and junctions,
design classes) against the regenerable base world, of the oriented grids (the level's changed,
the session's whole), and of the session: the world's clock (`World::time`), the pieces (all of
each: its shapes, voxels, fragments, bonds and layers, its pose, motion and sleep; kept with
their ids), the joints (their anchors, settings, drives), the sleeping pieces' dead loads, and a
streamed world's pieces archived out of range. `load_delta()` applies one:

- Load the same base world (its oriented grids in the same order, its joints), `bake()`, then
  `load_delta()`: the session's pieces and joints take the place of the ones there are (the
  level's joints, made again).
- A malformed delta is refused whole: nothing is applied.
- A saved session goes on as it would have: its pieces where they were, a machine on its
  program. (The solver's warm starts are not saved: close, not the same bits. The environment's
  transient state - heat, smoke - is not either.) In a streamed world, the chunks its pieces lie
  in and its joints hold on to are generated as it loads (a piece would fall through ground not
  there yet); what is out of range goes back to the archive at the next eviction scan.

### Determinism

Given the same world, configuration and commands at the same ticks, a session is bit-identical:

- on any thread count;
- natively and in WASM, on ARM and x86 (no transcendental functions on the simulation path at the
  default substeps of whole multiples of 1/120 s).

This is what lockstep networking and replays need (see `game/include/svx/game/replay.hpp`).
Things that break it:

- changing a world's materials while it steps;
- changing `WorldConfig` at different ticks;
- feeding commands in a different order.

### Threading and limits

- A `World` is not thread-safe: call it from one thread.
- It runs its work on a shared deterministic pool (`set_num_threads`). Worlds stepped on several
  threads take turns on the pool.
- Inputs are validated. Non-finite or out-of-range positions (beyond about ±2²⁰ voxels), radii,
  energies, rays, sweeps, edits and parameters are refused or clamped. Carves and blasts are
  clamped to `max_event_radius`.
- So is the configuration (`configure`, `enable_streaming`, `load`). A NaN knob takes its
  default. Counts that drive loops are held where a tick stays bounded: at most 64 substeps and
  256 solver iterations, 4096 chunks generated a tick, 256 threads. Memory budgets of any value
  are byte counts, and an infinite one is no bound. A source's extent is held within the key
  range, never inverted, and at most `kMaxColumnChunks` (1024) chunks tall; the load radius is
  at most 256 chunks, the change archive at most 4096 MB (1024 on a 32-bit build), and the
  world's voxel size 1 mm to 100 m. Callbacks (`ChunkSource`, `WorldSystem`) must not throw:
  the core is built without exceptions and does not contain one.
- Work per tick is bounded by `stress_work` (structures: solver iterations, and assemblies at
  the cost of the products and the factorization that build their multigrids) and the busy mode
  of the rigid solver (violent collapses step once per tick with fewer iterations). A solve
  slow to converge rebuilds its preconditioner only when it is stale (bonds broke since): a
  large structure is never assembled again tick after tick. Memory is bounded as in §4.

## 4. Memory: bounded by construction

Every kind of state a world keeps has a bound, so a session never grows past a known size
however long it runs, and nothing lives on after what it belongs to:

| State | What bounds it | Beyond the bound |
|---|---|---|
| Resident voxels (the grid) | The level; for streamed worlds the radii and `max_resident_mb` | Chunks farthest from the focus are evicted |
| **Change archive** (streamed chunks changed and out of range, and the pieces out of range with them) | `StreamConfig::archive_mb`: one arena allocated once, pages of 1 KB | Whole regions are forgotten, least recently seen first, their pieces with them; they come back as generated |
| Fragment caches (derived from the grid) | `MemoryBudget::fragment_cache_mb` | Unheld ones dropped, least recently used first (rebuilt identically on demand) |
| Registered structures (graphs, matrices, preconditioners) | `MemoryBudget::structure_mb` | Idle ones dropped, longest idle first (extracted again when touched) |
| Rigid pieces | `max_bodies` and `MemoryBudget::piece_mb` | Over the bytes: first the awake pieces' fracture solvers released, the largest first (a sleeping piece holds none: assembled again at its next check); then, as over the count, the smallest culled, sleeping first (`PieceRemoved`, `Culled`); never a joint's, nor one the host keeps |
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
identically, and events are output only. Every budget weighs what it bounds by what it uses -
its elements and a fixed size per record (`Bytes::Used`, `svx/base/mem.hpp`), never a
container's capacity or a hash table's buckets, which differ between standard libraries - so
its decisions are the same on every platform; `memory()` reports what is held (`Bytes::Held`).
A test checks that a session gives the same hash under tight and loose budgets. Culling pieces
and forgetting regions do change the world. Both are
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
  tick, in the order added (and, before the mechanics, `pre_step`: what they drive - an
  articulation's muscles and targets - is set there; they may add and remove articulations in
  both).
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
  command logs: an index is stable within a build, an id - `tunable_id`, a hash of the name -
  across builds, and logs record the id; setup tunables are meant for before a load).
- Events, pieces (with their voxels), changed chunks and queries come out as plain structs and
  arrays.

- The extension points (§5) are there too: layers (`svxc_add_layer`, `svxc_set_layer`,
  `svxc_chunk_layer`, `svxc_poll_layer_changes`), piece layers and voxel removal, loads, piece
  forces, and systems with all their callbacks (`svxc_add_system_ex`).
- Oriented grids (`svxc_add_grid`, `svxc_add_grid_desc`, `svxc_set_grid_frame`,
  `svxc_remove_grid`, voxels, layers and loads per grid, pieces' shapes, `svxc_sweep`,
  `svxc_collide_ex`): [`GRIDS.md`](GRIDS.md) §10.
- Joints and their drives (`svxc_add_joint`, `svxc_set_joint_drive`, ...), riding pieces
  (`svxc_collide_ex`, `svxc_overlaps`, `svxc_depenetrate`): [`MOTION.md`](MOTION.md) §3.
- Materials: the process's (`svxc_material_set`, `_get`, `_find`: what a world starts with) and
  each world's own (`svxc_world_material_set`, `_get`, `_find`).

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
| Rendering | Chunk meshes from `take_changed_chunks` (and `take_changed_grid_chunks`, placed by the grids' frames), piece meshes on `PieceAdded`, poses every frame, fades on `Culled`. |
| Structures off the lattice | Oriented grids (`add_grid`): a level's grids added after its world grid, in order ([`GRIDS.md`](GRIDS.md)). |

Things a harness should not do:

- reach into `World`'s internals;
- write the grid other than through `set_voxels`;
- keep `Body` pointers across ticks (use ids).

## 8. Known limits

- Continuous collision (speculative contacts) is along a piece's motion in the substep, against
  the grids and between pieces (a pair that may close more than half a voxel looks along its
  relative motion: a car at 100 km/h stops at a loose slab 12.5 cm thick, and at another car's
  one-voxel panels). It stops a fast piece at the face it would reach, without the restitution
  of a slower impact; a piece that spins fast about a far axis can still step over a thin part.
- A saved session goes on close to how it would have, not in the same bits (the solver's warm
  starts are not saved); the environment's transient state (heat, smoke) is not saved.
- Water and smoke see the grids at the world grid's resolution. More limits of grids are in
  [`GRIDS.md`](GRIDS.md) §12, of joints and machines in [`MOTION.md`](MOTION.md) §5.
- Structures larger than `structure_max_nodes` (60,000 fragments or clusters) or
  `structure_max_radius` (60 m) around the event are solved in part, with their frontier held
  fixed.
- The memory budgets (`MemoryBudget`, `StreamConfig::archive_mb`) cull pieces and forget regions
  by bytes: which ones go, and when, follows from how much each weighs - which changes with the
  engine's own data (a piece's crumpling areas, a record's new fields: [`BASELINE.md`](BASELINE.md)).
  They are the lossy knobs of a long session; raise them where the memory is there.

## 9. Cost and quality: the knobs

Where the engine trades quality for time or memory, the trade is a knob (a world tunable:
`set_tunable`, `--tune NAME=VALUE`, the web worker's settings), and the other side of it is
there for stronger hardware. The defaults are the engine's; the structural reference's own
choices, where they differ, are in [`BASELINE.md`](BASELINE.md) §2.

| What is traded | Default | Back towards quality |
| --- | --- | --- |
| A collapse's violent part: more than `rigid.busy_bodies` (150) pieces faster than `rigid.busy_speed` (2 m/s), or more than `rigid.busy_contacts` (6000) contacts, is stepped once a tick with `rigid.busy_iterations` (6) velocity and 2 position iterations - decided once a tick and held until both are under two thirds (`rigid.busy_hold`), the solver's warm starts scaled to the substep's length across the switch (`rigid.warm_to_step`) | busy | raise `rigid.busy_bodies` / `rigid.busy_contacts` (never busy), or `rigid.busy_iterations` |
| The pieces' step: `rigid.substeps` (2 a tick: 1/120 s), `rigid.iterations` (10), `rigid.position_iterations` (4) | 2, 10, 4 | raise them |
| An articulation solved with the pieces (it touches an awake one): their substep, not its fine steps - its muscles damped more than they were tuned for ([`MOTION.md`](MOTION.md) §5) | `rigid.mixed_substeps` 0 | 8: such a tick at the fine steps' rate, everything in it |
| An articulation on its own: `rigid.link_substeps` fine steps a substep (4: 1/480 s) of `rigid.link_iterations` (2) and `rigid.link_position_iterations` (1) passes | 4, 2, 1 | raise them |
| A creeping load on a large structure (more than `load_trigger_gap_nodes`, 400) is solved again at most every `load_trigger_gap` ticks | 6 | 0: at once, always |
| A piece crumpling in place is re-checked by collisions every `crumple_check_gap` substeps; a steady contact every `body_check_ticks` | 8, 12 | 2, lower |
| Structure solves: to `stress_rtol` (3e-3), at most `stress_work` block operations a tick; a structure beyond `structure_max_nodes` (60,000) or `structure_max_radius` (60 m) is solved in part; clustered beyond `cluster_nodes` (2500; pieces `body_cluster_nodes`, 400) | as listed | tighter, more, larger |
| Pieces' stress checks: `body_stress_maxit` (60) iterations to `body_stress_rtol` (1e-2) | 60, 1e-2 | more, tighter |
| Streaming's eviction scan runs every `evict_scan_ticks` (and at once when a focus moved 8 m) | 10 | 1 |
| Memory: `memory.*_mb` (§4), `max_bodies` (3000 pieces), `StreamConfig::archive_mb` (64) and `forget_after_s` - what goes beyond them is rebuilt or forgotten | as listed | raise them (§8) |
| The smallest rubble: pieces under `min_body_voxels` (16) turn to dust; pieces of fewer than `min_fracture_frags` (8) fragments never break further | 16, 8 | lower |
| Cosmetic events: `crack_events_per_tick` (24), `impact_events_per_tick` (6) | 24, 6 | raise them |
| The characters (svx_anim, [`ANIM.md`](ANIM.md) §6): deep (articulations of the world) within `deep_radius` (14 m) of a focus or `piece_radius` (3.5 m) of an awake piece, at most `max_deep` (24); shallow further; the plan alone beyond `physics_radius` (45 m) | `BodyPolicy::Hybrid` | `BodyPolicy::Deep`, or larger radii |
