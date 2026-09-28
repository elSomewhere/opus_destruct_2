# structvox game ↔ front-end contract (v2)

This is the protocol of the prototype game: the game harness (`svx_game`, around the physics
core `svx_core`, see [`CORE.md`](CORE.md)) runs in a **Web Worker**, compiled to WASM (SIMD128).
Hosts that want the physics alone use the core's own C API (`svx/svx_core.h`) instead. The main thread owns
input, UI and WebGPU rendering. The two sides talk through `postMessage` with transferable
`ArrayBuffer`s. SAB rings are a later optimization behind the same message shapes.

Conventions:

- All coordinates are metres, **z is up**, right-handed.
- The voxel pitch `h` defaults to 0.125 m.
- Every message is a plain object with a string **`type`** discriminator. Event objects
  inside an `events` message use **`kind`**.
- The TypeScript types live in `web/src/engine/protocol.ts`, which is the normative version
  of this page.
- *Extensions* (marked **ext**) are optional for engines. A worker that never sends them still
  works with the front end, and the front end tolerates their absence.

## Main → worker

| `type` | fields | notes |
|---|---|---|
| `init` | `config: {voxelSize, threads, memoryMB, params, persist?, gpuDisplacement?}` | First message. `persist` (**ext**): keep gameplay changes per world in OPFS (below). `gpuDisplacement` (**ext**): displacement fields (below; v2 engines send none). |
| `loadProcedural` | `seed, kind: 'city'\|'rooms'\|'tower'\|'yard'\|'angles'` | Test worlds. `city` is the streamed 1 km² city. `yard` has one construction of each kind (timber, stone, glass, steel, reinforced concrete, a reservoir, a water tower). `angles` has structures off the lattice, in oriented grids ([`GRIDS.md`](GRIDS.md)). |
| `loadWad` | `buffer: ArrayBuffer (transfer), map: string, options: {mode:'rock'\|'air', shellVoxels, bake:boolean}` | Doom level. |
| `viewer` | `pos:[x,y,z], dir:[x,y,z]` | Streaming, bake and LOD focus. Sent every frame or two, including while the player is not in control. |
| `blast` | `pos:[x,y,z], radius, energy` | Rocket or explosion. `energy` is in J. |
| `carve` | `pos:[x,y,z], radius` | Bullet impact. Removes the voxels inside the sphere. |
| `raycast` | `id, origin, dir (unit), maxDist` | Hitscan and picking. Never changes state. |
| `collide` | `id, min:[3], max:[3], move:[3]` | Player AABB sweep. The worker returns the move clipped against solid voxels (axis by axis in x, y, z order). `onGround` is set when a downward move was stopped. Never changes state. |
| `setParams` | `params: {fragility, impact, dif, debugView, paused}` | Tunables. Always the complete set. |
| `use` (**ext**) | `pos:[3], dir:[3]` | The player's use key (E). Operates a door in reach (2 m), or the lifts tagged by a switch line. Recorded in replays. |
| `ignite` (**ext**) | `pos:[3], radius` | Sets fire to what burns in the sphere; the rest heats up ([`ENV.md`](ENV.md)). Recorded in replays. |
| `extinguish` (**ext**) | `pos:[3], radius` | Puts out and cools the sphere. Recorded in replays. |
| `pour` (**ext**) | `pos:[3], radius` | Fills the air in the sphere with water. Recorded in replays. |
| `drain` (**ext**) | `pos:[3], radius` | Removes the water in the sphere. Recorded in replays. |
| `heat` (**ext**) | `pos:[3], radius, celsius` | Brings the solids in the sphere to (at least) that temperature. Recorded in replays. |
| `setEnv` (**ext**) | `name, value` | An environment setting by name (`fire.flame_reach`, `fire.wood.burn_s`, `smoke.wind_x`, `water.loads`, ...: the engine's `svx_env_param_*` list). Kept across loads; recorded in replays; unknown names are ignored. |
| `setTunable` (**ext**) | `name, value` | A world tunable by name (`rigid.gravity`, `max_bodies`, ...: `svx_tunable_*`). As `setEnv`. |

`params` fields:

- `fragility`: divides every bond strength (above 1: weaker bonds, more collapse).
- `impact`: scales the contact loads of pieces on structures and on each other (how hard
  landings hit).
- `dif`: dynamic increase factor of sudden load changes on structures (1 = quasi-static).
- `debugView`: 0 = none, 1 = bond utilization, 2 = fragments.
- `paused`: stops the simulation. Queries still work.

## Worker → main

| `type` | fields | notes |
|---|---|---|
| `ready` | `info: {bounds:{min,max}, voxelCount, spawn:{pos,dir}, textures:boolean}` | After a load. `spawn.pos` is the player's **feet** position, standing on the floor. |
| `textures` | `list: [{id, name, width, height, rgba: ArrayBuffer}]` | Doom textures and flats (transfer). Sent before `ready` when `info.textures`. RGBA8, row 0 = top. |
| `chunkMeshes` | `meshes: [{key, origin:[3], vertices: ArrayBuffer, vertexCount, indices: ArrayBuffer, indexCount}], fields?` | New or changed chunk meshes (transfer). A mesh replaces the previous mesh with the same key. `fields` (**ext**): see [Displacement fields](#displacement-fields). An oriented grid's chunk (**ext**) has the key `g<grid>:<x>,<y>,<z>`; its vertices are in the world like every chunk's. |
| `chunkRemoved` | `keys: string[]` | Evicted or emptied chunks. |
| `events` | `list: [...]` | See [Events](#events). |
| `debris` (**ext**) | `poses: [{id, pos:[3], rot:[x,y,z,w], opacity}]` | Rigid debris poses after every tick while any piece exists, plus one empty list when the last piece is gone. See [Rigid debris](#rigid-debris). |
| `occupancy` (**ext**) | `voxelSize, chunks: [{chunk:[3], state: 0\|1\|2, bits?: ArrayBuffer}]` | Solid occupancy of every chunk whose voxels changed, sent after the tick's meshes. See [Client-side collision](#client-side-collision). |
| `env` (**ext**) | `flames: Float32Array (x, y, z, °C per flame), smoke: Float32Array (x, y, z, density per cell)` | About 10 Hz while anything burns or smokes, plus one empty set when all is clear. At most 4096 of each: an even sample of the burning voxels, the densest smoke cells (4 voxels each). See [Environment](#environment). |
| `water` (**ext**) | `meshes: [...as chunkMeshes], removed: string[]` | Water surface meshes of chunks whose water changed (at most 10 Hz per chunk), and chunks whose water is gone. Drawn translucent after the opaque world. |
| `raycastResult` | `id, hit: null \| {pos:[3], normal:[3], distance, material}` | |
| `collideResult` | `id, move:[3], onGround:boolean` | |
| `stats` | `stats: {tickMs, structuralMs, rigidMs, voxels, chunks, memoryMB, events, pieces, awakePieces, contacts, bondsBroken, ...}` | About 4 Hz. `events` counts events since the previous stats message. The full set is `EngineStats` in `protocol.ts`; extra keys are shown generically by the HUD. |
| `error` (**ext**) | `message, fatal:boolean, command?` | `fatal`: the engine cannot continue. `command`: the command type that failed. |
| `progress` (**ext**) | `stage, done, total` | Load progress for the loading screen. |

### Chunk mesh vertex format: 28 bytes, interleaved

| offset | type | meaning |
|---|---|---|
| 0 | `float32x3` | position (world, metres) |
| 12 | `snorm8x4` | normal xyz; `w` = ambient occlusion (−1..1 maps to 0..1) |
| 16 | `float32x2` | texture coordinates in **texels**, 1 texel per Doom map unit (the shader wraps by texture size) |
| 24 | `uint16` | texture id (below) |
| 26 | `uint8` | light level 0..255 (Doom sector light) |
| 27 | `uint8` | debug value 0..255, per `debugView` |

Texture ids:

- Real textures use ids `0..n−1`, the index into `textures`.
- `0xFFFF` means untextured, in a neutral colour.
- `0xFF00 + m` (**ext**) means untextured, tinted with material `m`'s palette colour
  (m < 255).

Debug byte:

- View 1: bond utilization 0..1 as 0..255 (the last judged state of each structure).
- View 2: fragments: 1..255, a hash of the fragment (neighbouring fragments differ).

Indices are `uint32`, and triangles are counter-clockwise seen from outside.

Standing structures do not deform visibly in v2 (their stiffness only distributes load): chunk
meshes change only when voxels do.

### Displacement fields

A v1 extension, kept in the protocol and the renderer; v2 engines send no fields. With
`gpuDisplacement` (**ext**), a v1 engine skipped the per-tick re-sending of displaced meshes:

- When a bubble starts, each chunk it covers is sent once as a per-face mesh with static
  offsets only.
- Every tick, the `chunkMeshes` message carries `fields`: the complete set of displacement
  fields of the running bubbles. The previous set is replaced; when `fields` is absent, the
  set is unchanged.
- Each field is `{id, origin:[3], size:[3], voxelSize, maxDisp, data}`:
  - `data` is `rgba16float` texels, x fastest, texel `(0,0,0)` centred on `origin`.
  - Each texel holds `(w·dx, w·dy, w·dz, w)`, where `w = 1` for solid voxels and `0` for air.
    Displacements are metres, already amplified.
- The vertex shader samples each field trilinearly at the chunk vertex and adds `xyz / w`. The
  result is the displacement averaged over the solid voxels around the vertex, as the CPU
  mesher computes it.
- Chunk bounds overlapping a field are inflated by `maxDisp` for culling.
- When the bubble settles, its chunks' final static meshes and the reduced field set arrive in
  the same message, so there is no pop.

### Events

- `{kind:'detached', id, voxels, centroid:[3], velocity:[3], angular:[3], mesh:{vertices, vertexCount, indices, indexCount}, rigid?}`:
  a piece that lost support. The physics has already removed it from the world.
  - The mesh is in world coordinates at the moment of detachment.
  - `centroid` is the piece's centre of mass.
  - If `rigid` (**ext**) is true, the engine simulates the piece as rigid debris and the front
    end draws it at the poses of `debris` messages.
  - Otherwise, the front end animates it ballistically with no collision, fades it out and
    spawns dust.
- `{kind:'crack', pos:[3], normal:[3], strength, voxels?, velocity?:[3], radius?}`: bond ruptures
  (decals, particles). With `voxels` (**ext**): *dust*, material crushed, or broken off too small
  to be a piece (under 16 voxels), moving at `velocity`, of size `radius`; it has left the world.
- `{kind:'impact', pos:[3], energy}`: a blast, or a heavy landing of a piece (the energy its
  contacts dissipated, J, at the contacts' centre). Used for camera shake, dust and particles.
- `{kind:'splash', pos:[3], strength}` (**ext**): a piece hit the water hard; `strength` is its
  momentum into the water (kg m/s).
- A `detached` event with `remesh: true` (**ext**): a new mesh for a piece already shown (its
  charring or glow changed), in world coordinates at its pose now; it replaces the old one,
  without the effects of a detachment, and the `debris` poses that follow are relative to it.
- (v1 engines also sent `{kind:'bubble', ...}` debug events; v2 engines do not.)

### Environment

Fire, smoke and water are simulated by the engine ([`ENV.md`](ENV.md)). The front end only
draws them:

- **Flames.** Flame tongues, embers and smoke puffs as particles at the `env` message's
  burning voxels, and a flickering firelight.
- **Smoke.** The `env` message's smoke cells as soft sprites a little larger than a cell,
  jittered and drifting.
- **Water.** The `water` meshes in a translucent pass. Vertices have texture id `0xFFFE`, light
  255, and uv set to world x, y.
- **Charring.** Burnt voxels come darker in the ordinary chunk and piece meshes (the vertex
  light byte); burning and red-hot voxels glow (texture id `0xFE00 + material`: the material's
  colour and a flickering ember glow). Chunks re-meshed for that alone are flagged
  decoration-only in `svx_mesh_info` (their occupancy is unchanged: the worker does not send it
  again).

### Rigid debris

Detached pieces are rigid bodies that fall, collide with the world and each other, break, and
come to rest as rubble (docs/V2_DESIGN.md §4–5).

- The engine sends `{type:'debris', poses}` after every tick while pieces exist.
- Each pose carries:
  - `id`: the piece's `detached` event id.
  - `pos`: its current centre of mass.
  - `rot`: its rotation since detachment, as a unit quaternion `[x, y, z, w]`.
  - `opacity`: 1, falling to 0 while the piece fades out.
- A mesh vertex `p` is drawn at `pos + R(rot)·(p − centroid)`.
- A rigid piece missing from a `debris` list has been removed. A piece that breaks is removed,
  and its parts arrive as new `detached` events (meshes in world coordinates at that moment).
- The web front end interpolates between the last two poses, one tick behind, so motion is
  smooth at any display rate.

Pieces load what they touch: contact impulses on a standing structure become impact load cases
(landings) or dead loads (resting rubble) of the fragments they touch, so rubble piling on a
floor can bring it down. Bedrock and ground absorb landings.

### Sector movers (**ext**)

Doom maps get their moving sector planes as *movers*: doors, lifts, floors, platforms,
ceilings, crushers and stairs.

- A mover is a set of columns whose span holds a solid part some rows thick, hanging from the
  top (doors, ceilings, crushers) or standing on the bottom (lifts, floors, platforms). The solid
  part is anchored and bonded to nothing, so the structure never leans on it.
- A mover runs *moves*: go to a height and stay (floors, ceilings, stairs, open-only or
  close-only doors); go, wait and come back (doors, lifts); cycle between two heights with a
  wait at each end (perpetual platforms, crushers); stop a cycle where it is.
- Heights, speeds and waits follow the vanilla specials (`doom/specials.cpp`): doors of every
  kind (normal, open or close only, close for 30 s, blazing, locked), lifts, floors (to the
  lowest, highest or next neighbouring floor, by 24 or 512 units, raise-and-change, donut),
  platforms, ceilings, crushers with their stop lines, and both stair builders.
- Triggers:
  - `use` activates the mover whose span the ray hits (manual doors, lifts), or the movers
    tagged by a switch line it hits;
  - walk-over lines fire when the path between two consecutive `viewer` positions crosses them;
  - gunshot lines fire when a carve lands within a voxel of them;
  - once-only lines (S1 / W1 / G1) fire once.
- The player (the `viewer` box) is never crushed or trapped: a closing door goes back up, a
  descending ceiling or crusher waits, and a rising floor carries the player up but waits while
  the player would not fit under what is above.
- A door shot through stays as it is.
- Movers change voxels like any edit: meshes and occupancy follow, and the client pushes the
  player up out of a rising floor.

### Client-side collision

A large collapse can hold the worker for tens of milliseconds per tick. Player movement
must not wait for it, so the WASM worker streams **occupancy**:

- For every chunk it re-meshes or empties, and every chunk whose occupancy oriented grids
  changed, it sends one bit per voxel. A voxel is solid if the world grid's is, or if its centre
  lies in a solid voxel of an oriented grid.
  - `state 0`: all air, or not resident.
  - `state 1`: all solid.
  - `state 2`: mixed. `bits` is 4096 bytes: bit `v` of byte `v >> 3` is voxel
    `v = (x·32 + y)·32 + z` in local coordinates, where voxel = 32·chunk + local.
- The front end keeps these bits and runs the `collide` sweep locally, every frame, with
  exactly the engine's rules.
  - The browser smoke test checks 300 random sweeps against the worker's `collide`: they are
    identical.
- Engines that do not send `occupancy` keep the round-trip `collide`.

## Persistence (**ext**)

With `init.config.persist`, the WASM worker saves a binary delta of the world (changed chunks
only, `SVXD` v1, RLE) to OPFS every 5 s while the world is modified. It restores the delta when
the same world is loaded again. Worlds are identified by:

- procedural worlds: kind + seed;
- WADs: map name + a content fingerprint.

## C ABI (inside the worker)

`game/include/svx/game/api/svx_api.h` exposes the game's flat C API that the worker script
(`web/src/worker/wasm-worker.ts`) wraps into the messages above:

- lifecycle: `svx_create`, `svx_load_*`;
- simulation and commands: `svx_tick`, `svx_blast`, `svx_carve`, `svx_use`, `svx_ignite`,
  `svx_extinguish`, `svx_pour`;
- queries: `svx_raycast`, `svx_collide`;
- output: `svx_poll_meshes` (`svx_mesh_info` gives a mesh's grid, 0 for the world grid),
  `svx_poll_removed`, `svx_poll_removed_grid` (oriented grids' emptied chunks),
  `svx_poll_occupancy` (world chunks whose occupancy the grids changed), `svx_poll_events`,
  `svx_debris`, `svx_stats`, `svx_poll_env` (flames and smoke), `svx_poll_water` /
  `svx_poll_water_removed`;
- persistence: `svx_save_delta` / `svx_load_delta`;
- determinism: `svx_state_hash`, a digest of the session including debris poses, identical
  across thread counts and between native and WASM builds.

A pure-TypeScript mock worker with the same protocol lets the front end run without the WASM
build.
