# structvox game ↔ front-end contract (v2)

This is the protocol of the prototype game: the game harness (`svx_game`, around the physics
core `svx_core`, see [`CORE.md`](CORE.md); its levels from `svx_procgen`; its flat C ABI
`svx_api`) runs in a **Web Worker**, compiled to WASM (SIMD128).
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
| `loadProcedural` | `seed, kind: 'city'\|'rooms'\|'tower'\|'yard'\|'angles'\|'machines'` | Test worlds. `city` is the streamed 1 km² city (some of its buildings turned, in oriented grids). `yard` has one construction of each kind (timber, stone, glass, steel, reinforced concrete, a reservoir, a water tower). `angles` has structures off the lattice, in oriented grids ([`GRIDS.md`](GRIDS.md)). `machines` has machines - pieces on driven joints held by structures - and hanging parts: a lift, a turntable, a drawbridge, a crane with a wrecking ball, a pendulum, a chain, a hinged door ([`MOTION.md`](MOTION.md)). |
| `loadWad` | `buffer: ArrayBuffer (transfer), map: string, options: {mode:'rock'\|'air', shellVoxels, bake:boolean}` | Doom level. |
| `viewer` | `pos:[x,y,z], dir:[x,y,z]` | Streaming, bake and LOD focus. Sent every frame or two, including while the player is not in control. |
| `blast` | `pos:[x,y,z], radius, energy` | Rocket or explosion. `energy` is in J. |
| `carve` | `pos:[x,y,z], radius` | Bullet impact. Removes the voxels inside the sphere. |
| `raycast` | `id, origin, dir (unit), maxDist, characters?` | Hitscan and picking. Never changes state. `characters` (**ext**): the people's bodies are seen too (the hit's `character` and `bone`). |
| `collide` | `id, min:[3], max:[3], move:[3]` | Player AABB sweep. The worker returns the move clipped against solid voxels (axis by axis in x, y, z order; an oriented grid's voxels as the turned cubes they are). `onGround` is set when a downward move was stopped. Never changes state. |
| `setParams` | `params: {fragility, impact, dif, debugView, paused}` | Tunables. Always the complete set. |
| `use` (**ext**) | `pos:[3], dir:[3]` | The player's use key (E). Operates a door in reach (2 m), or the lifts tagged by a switch line. Recorded in replays. |
| `ignite` (**ext**) | `pos:[3], radius` | Sets fire to what burns in the sphere; the rest heats up ([`ENV.md`](ENV.md)). Recorded in replays. |
| `extinguish` (**ext**) | `pos:[3], radius` | Puts out and cools the sphere. Recorded in replays. |
| `pour` (**ext**) | `pos:[3], radius` | Fills the air in the sphere with water. Recorded in replays. |
| `drain` (**ext**) | `pos:[3], radius` | Removes the water in the sphere. Recorded in replays. |
| `heat` (**ext**) | `pos:[3], radius, celsius` | Brings the solids in the sphere to (at least) that temperature. Recorded in replays. |
| `setEnv` (**ext**) | `name, value` | An environment setting by name (`fire.flame_reach`, `fire.wood.burn_s`, `smoke.wind_x`, `water.loads`, ...: the engine's `svx_env_param_*` list). Kept across loads; recorded in replays; unknown names are ignored. |
| `setTunable` (**ext**) | `name, value` | A world tunable by name (`rigid.gravity`, `max_bodies`, ...: `svx_tunable_*`). As `setEnv`. |
| `setPedestrians` (**ext**) | `pedestrians: {enabled, count, nearRadius, radius, bodies: 0 deep\|1 shallow\|2 hybrid, maxDeep}` | The people of a streamed world with walkways (the drive city; [`ANIM.md`](ANIM.md)). Kept across loads; recorded in replays. |
| `woundCharacter` (**ext**) | `id, pos:[3], radius, energy` | A round into a character, where a `raycast` with `characters` found it (a body that moved since is found along the same line); radius and energy as for the world's shot. Recorded in replays. |

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
| `loading` | `generation: number` | A load has begun: the engine counts them, and everything it sent before this belongs to the world being replaced. The host counts the loads it asked for and drops the old world's messages until the two agree, so a pose or a detached piece in flight when the world changed cannot enter the new one. The pose window ([Rigid debris](#rigid-debris)) starts over here on both sides. |
| `ready` | `info: {bounds:{min,max}, voxelCount, spawn:{pos,dir}, textures:boolean}` | After a load. `spawn.pos` is the player's **feet** position, standing on the floor. |
| `textures` | `list: [{id, name, width, height, rgba: ArrayBuffer}]` | Doom textures and flats (transfer). Sent before `ready` when `info.textures`. RGBA8, row 0 = top. |
| `chunkMeshes` | `meshes: [{key, origin:[3], vertices: ArrayBuffer, vertexCount, indices: ArrayBuffer, indexCount, grid?}], fields?` | New or changed chunk meshes (transfer). A mesh replaces the previous mesh with the same key. `fields` (**ext**): see [Displacement fields](#displacement-fields). An oriented grid's chunk (**ext**) has the key `g<grid>:<x>,<y>,<z>` and `grid` set; its vertices and origin are in the grid's lattice (metres), drawn with the grid's frame (`grids`). |
| `grids` (**ext**) | `frames: Float64Array (9 per grid: id, origin xyz, rotation xyzw, voxel size), removed: number[]` | The oriented grids that came or were placed anew, and the grids gone. See [Oriented grids](#oriented-grids-and-joints-ext). |
| `joints` (**ext**) | `joints: Float64Array (8 per joint: id, type, end a xyz, end b xyz)` | The joints after every tick while any exist, plus one empty set when the last is gone (type 4: a rope or rod, drawn between its ends). |
| `chunkRemoved` | `keys: string[]` | Evicted or emptied chunks. |
| `events` | `list: [...]` | See [Events](#events). |
| `debris` (**ext**) | `poses: Float64Array (15 per piece: id, pos xyz, rot xyzw, opacity, velocity xyz, angular velocity xyz)` | Rigid debris poses after every tick while any piece exists, plus one empty set when the last piece is gone. See [Rigid debris](#rigid-debris). |
| `occupancy` (**ext**) | `voxelSize, chunks: [{chunk:[3], state: 0\|1\|2, bits?: ArrayBuffer, grid?}]` | Solid occupancy of every chunk whose voxels changed, sent after the tick's meshes; an oriented grid's (`grid` set) in its lattice. See [Client-side collision](#client-side-collision). |
| `env` (**ext**) | `flames: Float32Array (x, y, z, °C per flame), smoke: Float32Array (x, y, z, density per cell)` | About 10 Hz while anything burns or smokes, plus one empty set when all is clear. At most 4096 of each: an even sample of the burning voxels, the densest smoke cells (4 voxels each). See [Environment](#environment). |
| `water` (**ext**) | `meshes: [...as chunkMeshes], removed: string[]` | Water surface meshes of chunks whose water changed (at most 10 Hz per chunk), and chunks whose water is gone. Drawn translucent after the opaque world. |
| `raycastResult` | `id, hit: null \| {pos:[3], normal:[3], distance, material, character?, bone?}` | `character` (**ext**): the character hit (its id; material -1). |
| `characterMeshes` (**ext**) | `meshes: [{id, vertices: ArrayBuffer, vertexCount, indices: ArrayBuffer, indexCount}], removed: number[], palettes: [{id, rgb: Float32Array(48)}]` | See [Characters](#characters-ext). |
| `characters` (**ext**) | `characters: Float64Array (12 each), skin: Float32Array (23 x 16 each), props?, seq` | See [Characters](#characters-ext). |
| `blood` (**ext**) | `drops: Float32Array (7 each), stains: Float32Array (8 each)` | See [Characters](#characters-ext). |
| `collideResult` | `id, move:[3], onGround:boolean, ground?, groundPiece?, groundVelocity?:[3]` | `ground`, `groundPiece` (**ext**): the grid (0 the world grid) or the piece stood on, and its velocity under the box (a lift's car, a turntable: the player rides it). |
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
- `{kind:'removed', id}` (**ext**): a `detached` piece is gone - the engine removed it (it broke,
  left the world, or a culled one finished fading out) - and its mesh and collision go at once,
  whether or not the page is receiving `debris` lists (they are held back while it is behind).
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
  - `velocity`, `angular`: its motion (what rides on it is carried so).
- A mesh vertex `p` is drawn at `pos + R(rot)·(p − centroid)`.
- A `detached` event carries the piece's voxels too (`occupancy`, **ext**: per shape its lattice
  at the event's pose and a bit per cell of its box), for the client's collision.
- A rigid piece missing from a `debris` list has been removed (engines that send `removed`
  events say so at once as well). A piece that breaks is removed,
  and its parts arrive as new `detached` events (meshes in world coordinates at that moment).
  A piece whose first pose has not come yet is the exception, and only for a few lists: every
  list holds every piece the engine has, so one absent from that many is gone (without that, a
  piece the engine never poses is drawn where it detached and stays there for good).
- Each list carries `seq`, and the host acknowledges the last it handled with `frameAck`. A host
  more than a few sequence numbers behind has its poses held back until it catches up - every
  list brings it fully up to date, so it never works through a backlog. The window starts over
  at `loading`: a load sends no poses while it bakes, and a window carried across it would leave
  the acknowledgement behind for good, holding the new world's poses with nothing left to ack.
- The web front end interpolates between the last two poses, one list interval behind - the
  interval measured, so an engine slowed down (ticks further apart) still moves things smoothly -
  by one clock for the pieces, the vehicles and the characters (a car's body, its wheels and the
  camera riding it are drawn at the same moment). It draws as many pieces as the engine keeps
  (`max_bodies`, plus the culled ones fading out); were it ever full, it gives up a fading or the
  smallest piece first, never one for its age (the oldest are parked cars' bodies).

Pieces load what they touch: contact impulses on a standing structure become impact load cases
(landings) or dead loads (resting rubble) of the fragments they touch, so rubble piling on a
floor can bring it down. Bedrock and ground absorb landings. A car's wheels load what they
stand on as a load that moves (a blow only beyond 2.5 x their share of its weight: a landing);
a large structure under a load that creeps - a car crossing a bridge - is solved again at most
every `load_trigger_gap` ticks (6), at once for a change of 4 x its trigger, an impact, or while
it is breaking.

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

- For every chunk it re-meshes or empties, it sends one bit per voxel: the world grid's chunks
  in the world, an oriented grid's (`grid` set) in its lattice.
  - `state 0`: all air, or not resident (or gone).
  - `state 1`: all solid.
  - `state 2`: mixed. `bits` is 4096 bytes: bit `v` of byte `v >> 3` is voxel
    `v = (x·32 + y)·32 + z` in local coordinates, where voxel = 32·chunk + local.
- The front end keeps these bits and runs the `collide` sweep locally, every frame, with
  exactly the engine's rules: the world grid's voxels layer by layer, an oriented grid's as
  turned cubes (the engine's separating-axis sweep), placed by the grid's frame (`grids`), and
  a piece's as turned cubes too (its `occupancy`, placed by its pose as it is drawn: one tick
  behind, interpolated). What the box lands on and its velocity there come with the result: the
  player rides a lift's car, a turntable, and is lifted out of a car that rose into their feet.
  - The browser smoke test checks 300 random sweeps against the worker's `collide`: they are
    identical.
- Engines that do not send `occupancy` keep the round-trip `collide`.

### Oriented grids and joints (**ext**)

- **Grids.** An oriented grid's chunk meshes are in its lattice; its frame comes in `grids`
  messages (when it comes, and when it is placed anew). The front end draws its chunks with the
  frame as their model matrix. A grid in `removed` is gone with its chunks.
- **Joints.** `joints` lists the joints' ends every tick while any exist; the front end draws
  the distance joints (ropes, rods) as thin tubes between their ends.

### Characters (**ext**)

The drive city's people ([`ANIM.md`](ANIM.md)) are voxel characters drawn by rigid skinning:

- `characterMeshes`: meshes new since the last one, in the svx_anim character vertex format (20
  bytes: position float32x3 in rest model space; normal snorm8x3 and ambient occlusion snorm8;
  uint32 bone | palette slot << 8 | shade << 12, 128 = 1.0), the meshes no character draws any
  more (apply those first), and palettes (16 slots' linear rgb). A wounded character gets a mesh of
  its own. A palette is sent once for the session: keep them across loads.
- `characters`: after every tick while any exist (and once empty after), a pose message like
  `debris` (`seq`, acknowledged with `frameAck`): 12 doubles each - id, mesh, palette, flags (1
  alive, 2 deep: its body an articulation of the world, 4 physical, 8 asleep, 16 down, 32 a gib),
  bounding sphere centre xyz and radius, hit flash 0..1, its prop's mesh (0: none), health 0..1, 1
  reserved - and its 23 skin matrices (column-major, rest model space to world; a gib's first
  alone counts). A vertex is drawn at skin[its bone] x its position. The root's matrix rides with
  the pelvis of a body the physics moves: the feet's and toes' (bones 16, 17, 20, 21) put a
  shadow on the ground under it.
- `blood`: the drops in flight (x, y, z, radius, linear rgb) and the stains they left on the
  world (x, y, z, the surface's normal xyz, radius, age in s), with the pose messages.

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
- queries: `svx_raycast`, `svx_collide` (9 values: the move, on ground, the grid stood on, the
  velocity there of what it stands on, the piece stood on);
- output: `svx_poll_meshes` (`svx_mesh_info` gives a mesh's grid, 0 for the world grid; a
  grid's mesh is in its lattice), `svx_poll_removed`, `svx_poll_removed_grid` (oriented grids'
  emptied chunks), `svx_chunk_occupancy` / `svx_grid_chunk_occupancy`, `svx_poll_grids` /
  `svx_grid_info` / `svx_poll_grids_removed` / `svx_grid_removed` (the grids' frames),
  `svx_poll_joints` / `svx_joint_info`, `svx_poll_events` (`svx_event_occupancy`: a piece's
  voxels), `svx_debris` (15 values a piece), `svx_stats`,
  `svx_poll_env` (flames and smoke), `svx_poll_water` / `svx_poll_water_removed`;
- characters (**ext**, [`ANIM.md`](ANIM.md)): `svx_set_pedestrians` (logged), meshes in the
  svx_anim character vertex format (`svx_poll_character_meshes` / `svx_character_mesh_info` /
  `_vertices` / `_indices`, `svx_poll_character_meshes_removed`), palettes
  (`svx_poll_character_palettes` / `svx_character_palette`), the characters after every tick
  (`svx_characters`: 12 values each - id, mesh, palette, flags, bounding sphere, flash, prop
  mesh, health - with `svx_characters_skin`: 23 skin matrices each, and `svx_characters_prop`),
  a shot's ray that sees them (`svx_raycast_shot`) and a round into one (`svx_wound_character`,
  logged); stats 51..56;
- persistence: `svx_save_delta` / `svx_load_delta`;
- determinism: `svx_state_hash`, a digest of the session including debris poses, identical
  across thread counts and between native and WASM builds.

A pure-TypeScript mock worker with the same protocol lets the front end run without the WASM
build.
