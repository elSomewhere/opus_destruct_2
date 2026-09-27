# structvox env: fire, smoke and water

`svx_env` is a library of **environment systems** built on the destruction core's public
extension points ([`CORE.md`](CORE.md) §5). It depends on `svx_core` only. The core knows
nothing about fire or water. The systems know nothing about rendering, players or levels.
Each system is a `WorldSystem`: added to a `World` once, then stepped with it every tick.

```
env/include/svx/env/fire.hpp    FireSystem   heat, ignition, burning, charring, burn-out, metal weakening
env/include/svx/env/smoke.hpp   SmokeSystem  a sparse coarse density field: rises, spreads, drifts, dissipates
env/include/svx/env/water.hpp   WaterSystem  a voxel fluid: falls, levels, rests, presses, floats, quenches
env/include/svx/env/env.hpp     Environment  the usual set, added to a world and linked together
```

```cpp
#include "svx/env/env.hpp"

svx::World world;
svx::Environment env;
env.attach(world);           // before the first load: registers the systems' layers
world.load(std::move(grid)); // a grid may carry a "water" layer (water at rest)
world.bake();                // designs structures for their weight and the water's pressure
env.fire()->ignite(world, pos, 0.4);
env.water()->pour(world, pos, 0.5);
for (;;) world.tick();       // the systems step inside tick(), after the mechanics
```

## 1. How the systems meet the core

Every interaction goes through `World`'s public API. None of it is special-cased in the core.

| Core extension point | Used by | For |
|---|---|---|
| Voxel layers (`add_layer`, `set_layer`, `layer`; pieces carry them), bound to the solid voxel (heat, burn, damage: gone with it) or to the air (water: gone when a solid takes its place) | all | state that streams, saves and moves with the voxels |
| The damage layer (`kDamageLayer`) | fire | charring and heat take strength from bond sections |
| `set_voxels`, `remove_piece_voxels` | fire | burnt-out voxels vanish, from the world and from pieces |
| Loads on the static world (`set_loads`) | water | hydrostatic pressure on walls and floors |
| Forces on pieces (`apply_force`, `wake_piece`) | water | buoyancy and drag |
| `WorldSystem` callbacks (`on_generated`, `on_evicted`, `on_voxels_changed`, `on_load`) | all | streaming, and noticing what changed around them |

The systems also meet each other only through layers:

- **Fire** looks at the `water` layer: a wet voxel, or one next to water, is quenched.
- **Smoke** follows the fire it is given (`SmokeSystem::follow`): each flame is a source.
- Blasts and dust reach the smoke through `emit` (the game harness does this).

`Environment::attach` adds the three in the order water, fire, smoke. Water moves first, fire
sees the new water, and smoke takes the flames of the same tick.

**Settings.** Every field of the three configs, and the fire properties of the main materials,
has a name (`env_param_*`: `"fire.flame_reach"`, `"fire.wood.burn_s"`, `"smoke.wind_x"`,
`"water.loads"`, ...), read and set through `Environment::get` / `set` (values brought into
their ranges). Each system also has an `enabled` switch. `configure` and `set_material` bring
any value into range (NaN included), so no setting can break a system.

**Determinism.** Everything a step does comes from its state and the world's, in a fixed order
(sorted keys, piece ids): runs are bit-for-bit the same on any thread count, native or WASM
(smoke's decay avoids the platform's `exp`). A load resets each system (its step count too), so
a session replayed from a load is the same whatever came before it; the systems' state hashes
(including their clocks) are part of `World::session_hash`.

**Stats.** Each system's `stats()` has its cost this tick (`step_ms`) and since the load
(`total_ms`), its step count, and its own counts.

## 2. Fire (`FireSystem`)

**State.** Two layers:

- `heat` (transient): the temperature above ambient, 4 °C per unit.
- `burn` (persistent): how much of a combustible voxel has burnt, 1 to 255.

The system also keeps a sorted list of the hot voxels it steps.

**Step.** It runs every `step_s` (0.1 s) over the hot voxels only:

- **Conduction.** Heat flows to cooler solid neighbours at the smaller of the two materials'
  `conduct` rates.
- **Flames.** A burning voxel heats the exposed solids in its flame's reach: strongly up the
  wall above it, a third as much to the sides, hardly at all below. Fire therefore climbs
  walls, spreads slowly across them, and creeps down only by conduction.
- **Cooling.** A voxel loses a fraction of its excess heat to the air, and at least a few
  degrees a second, so the last degrees go too.
- **Ignition.** An exposed combustible voxel ignites above its ignition temperature. Each voxel
  jitters it (±15 %), and its burn time (±35 %), so fronts are ragged and burn-out is spread
  out.
- **Burning.** A burning voxel holds its flame temperature and advances `burn`. Its damage
  follows (`char_damage` × burn), so its sections weaken as it chars. At 255 it is gone
  (`set_voxels`): structures lose members and fall.
- **Metals and mineral materials.** Steel, rebar, concrete, stone, masonry and glass do not
  burn. Above `weaken_c` they take damage that grows with temperature, reaching full damage at
  `gone_c`. The damage is permanent.
- **Pieces.** The same rules apply in each piece's own voxel shape, with "up" taken as its
  axis nearest the world's. Burning world voxels heat the pieces in their flames and the
  reverse. Burnt-out piece voxels leave in batches about once a second, and each batch
  re-announces the piece's parts.

**Defaults.**

| Material | Behaviour |
|---|---|
| Wood | Burns: ignites at 300 °C, burns about 40 s per voxel, flames at 900 °C, chars to 10 % strength. |
| Steel | Weakens from 400 °C; nothing is left at 1000 °C. |
| Rebar | Weakens from 450 °C; nothing is left at 1100 °C. |
| Concrete | Weakens from 600 °C to 1400 °C. |
| Stone, masonry | Weaken from 900 °C to 1800 °C. |
| Glass | Weakens from 250 °C to 700 °C. |
| Rock, soil | Inert. |

Change them with `set_material`.

Damage is written when it has risen by `damage_quantum` (8 of 254 by default): each write
re-measures the bonds of the voxels written and judges their structures again, and a burning
piece is not kept from sleeping by rewrites that change nothing.

**Commands.** All three take effect at once, on a sphere of at most `kMaxReach` voxels, on the
world and the pieces in it:

- `ignite(pos, r)`: combustibles in the sphere catch fire; the rest heats up.
- `heat(pos, r, °C)`.
- `extinguish(pos, r)`.

**Queries.** `temperature(voxel)`, `burnt(voxel)`.

**Output.**

- `flames()`: the burning voxels (world position and temperature), for effects and smoke.
- `take_glow_changes()`: chunks where a voxel began or stopped glowing (`glow_c`), and
  `take_piece_changes()`: pieces whose charring or glow changed. A renderer re-meshes those,
  not every hot chunk every step.

**Bounds.** At most `max_hot` voxels are tracked (40,000 by default). Beyond that the coolest
non-burning ones go first, ties broken by a hash of the voxel rather than its place. Commands
admit their voxels nearest first, while there is room. Heat is transient: it leaves with evicted
chunks. Burns and damage are persistent: they come back charred. Heat a level or a source
comes with (a `heat` layer) is tracked from the load or the chunk's generation.

## 3. Smoke (`SmokeSystem`)

**State.** A density field in cells of 4³ voxels (0.5 m), grouped in blocks of one chunk each.
Blocks exist only where there is smoke, up to `max_blocks` (4 KB each). Beyond that, the
thinnest blocks go.

**Closed cells.** A cell is closed if it is mostly solid, or if it holds a wall: a voxel plane
across it (along any axis) that is at least ¾ solid. Thin walls, floors and roofs therefore
hold smoke, and windows let it through. The masks are cached per chunk and refreshed when the
chunk's voxels change.

**Step.** It runs every `step_s` (0.1 s). Each cell with smoke sends part of it:

- up (buoyancy, `rise`), or, when the cell above is closed, sideways towards thinner cells (a
  ceiling jet);
- to thinner open neighbours (diffusion);
- down the wind.

The field dissipates with `lifetime`. Smoke in a closed room therefore gathers under the
ceiling and fills the room from the top down, and it escapes through a hole in the roof.

**Sources.** A followed fire's flames, and `emit` / `emit_sphere` (the harness adds dust for
blasts).

A flame in a closed cell - on a wall or a floor, where the wall rule closes its cell - gives its
smoke to the first open cell next to it (up, the sides, down).

**Output.**

- `cells(max)`: the densest cells, with centre and density, for rendering.
- `density(pos)`: for gameplay (visibility, AI).

**Bounds.** At most `max_blocks` blocks after a step (and twice that within one, before the
trim); emits are capped per step (`kMaxPending`) and in radius (`kMaxEmitRadius`).

**Streaming.** Smoke is transient: it leaves with evicted chunks and is not saved.

## 4. Water (`WaterSystem`)

**State.** The persistent `water` layer: how full an air voxel is, 1 to 255 (full). Water
therefore streams, archives and saves like voxels do. A level or a `ChunkSource`
(`generate_layer("water")`) places water at rest: ponds, reservoirs, tanks.

**Flow.** It runs every `step_s` (1/30 s). Only moving water is stepped: the active set.

- **Order.** Voxels are taken bottom-up, with the sideways order alternating each step so
  nothing drifts.
- **Falling.** Water falls as a packet through empty voxels, up to `fall` voxels a step, then
  into partly filled water below.
- **Levelling.** Water on a floor, or on full water, levels with its lower open neighbours,
  taking them in rising order while they are below the running level.
- **Films.** Thin films (below `min_spread`) stay as puddles, and thinner ones still dry up.
- **Rest.** Water that did not change rests.
- **Waking.** Changed voxels wake the water next to them, and so do voxel changes nearby (a
  wall shot away, a voxel placed) and chunks generated next to it (water held by a chunk not
  generated yet flows on). Full water held on every side it could go to - below and around:
  solids, full water - is never woken: a lake stays at rest whatever happens at its shore.

Water is conserved except for drying films, water falling off the world, and water a solid
takes the place of (the layer is bound to the air). Pressure is not
propagated: water does not rise in a U-tube, and a tank drains through a hole in its side as
the water above falls into the water that left.

**Loads.** The system writes the hydrostatic pressure of its water on the free solids holding
it through `set_loads`:

- sideways, the mean pressure over the wet part of the face;
- downwards, the column's weight.

Loads are computed per chunk, only for water next to free voxels, and refreshed at most every
`load_s` for chunks whose water or voxels changed - and the chunks below them, whose columns'
depth counts their water. They are computed at load time, so `bake` designs structures to hold
the level's water; water poured into a closed cavity presses at once. Switched off (or moved to
another `load_group`), the system takes its loads back from the world.
In streamed worlds, water and other persistent layers do not keep a structure from its design
on first touch: only changed voxels or bonds (a player's) do. A weak wall (a thin dry-stone pane holding
2.7 m of water) bursts; a breach floods.

**Pieces.** Each tick, pieces near water get buoyancy (the water their sampled voxels
displace) and drag on the submerged part. The sample - at most `samples` voxels, picked by a
hash of the cell, with no stripes of a shape's layout - is kept while the piece keeps its
shape; drag is limited to what cannot overshoot. A sleeping piece wakes if it would float, and
one the water left falls. Wood floats half under, and stone sinks. A hard entry is reported as
a splash.

**Fire.** Wet voxels do not burn (§2).

**Commands** (on a sphere of at most `kMaxReach` voxels):

- `pour(pos, r)`: fills the air in the sphere.
- `drain(pos, r)`.

**Bounds.** At most `max_active` voxels are stepped per step (60,000 by default). Beyond that,
a rotating window of the active set moves each step, so the rest waits its turn; with water at
rest never stepped, a step's work follows the water that moves. Loads are capped at
`max_loads`. The water itself is voxel layer data, bounded by the grid and the change archive.

**Rendering** (`svx_mesh`): `mesh_water(grid, layer, chunk)` makes a chunk's water surfaces:

- tops at the water's height in each voxel, greedy-merged;
- sides where water stands higher than its neighbour;
- the undersides of falling sheets.

Vertices use texture `kWaterTexture` (0xFFFE).

## 5. The game harness and the web front end

`svx::Game` owns an `Environment`:

- **Commands.** `ignite`, `extinguish`, `heat`, `pour` and `drain`, and the settings
  (`set_env`, by name or index, and the world's `set_tunable`) are logged commands, so replays
  and lockstep reproduce fires, floods and changed settings bit for bit (`test_replay`). The
  environment itself is read only from outside (`env() const`): nothing changes it unlogged.
  Blasts raise dust into the smoke.
- **Output.**
  - `flames(max)` and `smoke(max)`.
  - Water meshes of changed chunks (`take_water_meshes`, at most every `water_remesh_s`) and
    removed ones.
  - Splash events.
  - Charring darkens chunk and piece meshes (the burn layer as light), and burning or red-hot
    voxels glow (texture 0xFE00 + material). Chunks whose charring or glow changed are meshed
    again at most every `char_remesh_s` (`decoration_only`: their occupancy is unchanged);
    pieces get a Remesh event with their new mesh.
- **Levels.**
  - The yard: a timber house, a stone tower with a timber floor, a masonry reservoir and a
    timber water tower (burn its legs and it falls and spills; the spilled water puts the fire
    out).
  - The streamed city: a third of its buildings have timber floors; some empty lots are ponds
    from `generate_layer("water")`.

The web front end draws:

- flames, embers and smoke puffs as particles at the engine's flames, with a flickering
  firelight;
- the smoke field as soft sprites, one per cell;
- the water as a translucent pass (fresnel sky reflection, ripples, sun glints, streaks on
  falling water).

Weapons 4 (flamethrower: `ignite`) and 5 (water hose: `pour` and `extinguish`) drive the
systems, and the settings panel's Environment section sets the main ones (fire and its spread,
wood's burn time, smoke and its lifetime, the wind, water flow, pressure and buoyancy).

Browser checks:

- `node scripts/fire-wasm.mjs`: sets the house on fire, then the hose.
- `node scripts/water-wasm.mjs`: breaches the reservoir.

## 6. Tests and budgets

`svx_env_tests` links `svx_env` and `svx_core` only.

- **Fire.** Wood burns, chars and falls; flames climb and burn out; steel weakens for good;
  water quenches and the extinguisher works; pieces burn; the budget holds and runs are
  deterministic; streamed heat leaves while burns stay.
- **Smoke.** It rises and thins; a closed room fills from the ceiling; a roof hole vents it;
  the budget holds; runs are deterministic.
- **Water.** A basin fills level and rests, and a breach drains it; tank pressure bursts a
  weak wall; wood floats and stone sinks; a hose puts out a fire; generated lakes and poured
  puddles stream, archive and save; runs are deterministic within the step budget.

`svx_soak` crosses the streamed city with fires and water added to the destruction and
reports each system's memory (`MemoryReport::systems`).

## 7. Known limits

- Water has no pressure propagation (no U-tubes, no fountains) and no velocity. It does not
  push pieces along, and pieces do not displace it.
- Smoke is one species: fire smoke and dust share a colour. It is 0.5 m coarse and does not
  block rays.
- Fire does not spread through air gaps wider than a flame's reach (two voxels to the sides,
  three up). Embers are visual only.
- The systems' per-voxel steps are single-threaded. They are bounded by `max_hot`,
  `max_blocks` and `max_active`.
- Water a host writes into the `water` layer directly (`World::set_layer`) is not woken; use
  `pour` / `drain`, or a level's layer (at rest from the load).
- Heat tops out at 1020 °C (a byte of 4 °C): materials' `gone_c` above that (concrete, stone)
  set how fast they weaken, not a temperature they reach.
- Burnt-out voxels leave a piece in batches (`piece_batch_steps`); each batch announces the
  piece's parts again with new ids (the core's `remove_piece_voxels`).
- The far render tier shows no water: distant ponds look like dry pits.
