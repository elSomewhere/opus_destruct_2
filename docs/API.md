# structvox engine ↔ front-end contract (v1.1)

The C++ core runs in a **Web Worker**, compiled to WASM (SIMD128). The main thread owns
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
| `init` | `config: {voxelSize, threads, memoryMB, params, persist?, gpuDisplacement?}` | First message. `persist` (**ext**): keep gameplay changes per world in OPFS (below). `gpuDisplacement` (**ext**, default on): move chunks inside physics bubbles with displacement fields (below). |
| `loadProcedural` | `seed, kind: 'city'\|'rooms'\|'tower'` | Test worlds. `city` is the streamed 1 km² city. |
| `loadWad` | `buffer: ArrayBuffer (transfer), map: string, options: {mode:'rock'\|'air', shellVoxels, bake:boolean}` | Doom level. |
| `viewer` | `pos:[x,y,z], dir:[x,y,z]` | Streaming, bake and LOD focus. Sent every frame or two, including while the player is not in control. |
| `blast` | `pos:[x,y,z], radius, energy` | Rocket or explosion. `energy` is in J. |
| `carve` | `pos:[x,y,z], radius` | Bullet impact. Removes the voxels inside the sphere. |
| `raycast` | `id, origin, dir (unit), maxDist` | Hitscan and picking. Never changes state. |
| `collide` | `id, min:[3], max:[3], move:[3]` | Player AABB sweep. The worker returns the move clipped against solid voxels (axis by axis in x, y, z order). `onGround` is set when a downward move was stopped. Never changes state. |
| `setParams` | `params: {compliance, amplification, fragility, damping, debugView, paused}` | Tunables (plan §B11). Always the complete set. |
| `use` (**ext**) | `pos:[3], dir:[3]` | The player's use key (E). Operates a door in reach (2 m), or the lifts tagged by a switch line. Recorded in replays. |

`params` fields:

- `compliance`: physical compliance S_p (plan §B3).
- `amplification`: render amplification A of event-induced displacement.
- `fragility`: capacity multiplier F.
- `damping`: fraction of critical damping, 0..1. The WASM engine maps it to Rayleigh
  α = 2ζ·2π·5 Hz.
- `debugView`: 0 = none, 1 = utilization / damage, 2 = bubble level.
- `paused`: stops the structural simulation. Queries still work.

## Worker → main

| `type` | fields | notes |
|---|---|---|
| `ready` | `info: {bounds:{min,max}, voxelCount, spawn:{pos,dir}, textures:boolean}` | After a load. `spawn.pos` is the player's **feet** position, standing on the floor. |
| `textures` | `list: [{id, name, width, height, rgba: ArrayBuffer}]` | Doom textures and flats (transfer). Sent before `ready` when `info.textures`. RGBA8, row 0 = top. |
| `chunkMeshes` | `meshes: [{key, origin:[3], vertices: ArrayBuffer, vertexCount, indices: ArrayBuffer, indexCount}], fields?` | New or changed chunk meshes (transfer). A mesh replaces the previous mesh with the same key. `fields` (**ext**): see [Displacement fields](#displacement-fields). |
| `chunkRemoved` | `keys: string[]` | Evicted or emptied chunks. |
| `events` | `list: [...]` | See [Events](#events). |
| `debris` (**ext**) | `poses: [{id, pos:[3], rot:[x,y,z,w], opacity}]` | Rigid debris poses after every tick while any piece exists, plus one empty list when the last piece is gone. See [Rigid debris](#rigid-debris). |
| `occupancy` (**ext**) | `voxelSize, chunks: [{chunk:[3], state: 0\|1\|2, bits?: ArrayBuffer}]` | Solid occupancy of every chunk whose voxels changed, sent after the tick's meshes. See [Client-side collision](#client-side-collision). |
| `raycastResult` | `id, hit: null \| {pos:[3], normal:[3], distance, material}` | |
| `collideResult` | `id, move:[3], onGround:boolean` | |
| `stats` | `stats: {tickMs, structuralMs, activeBubbles, activeNodes, voxels, chunks, memoryMB, events, ...}` | About 4 Hz. `events` counts events since the previous stats message. Extra numeric or string keys are engine-specific and shown generically by the HUD. |
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

- View 1: utilization 0..1 as 0..255.
- View 2: bubble level, where 0 = not in a bubble and 1 + L is level L (fine = 0).

Indices are `uint32`, and triangles are counter-clockwise seen from outside.

Displacements: chunks inside an active physics bubble are re-sent as `chunkMeshes` with
displaced positions, already amplified by `amplification`, at most once per tick. When the
bubble settles, the persistent rest offsets are baked into the static meshes.

### Displacement fields

With `gpuDisplacement` (**ext**), the engine skips the per-tick re-sending of displaced meshes:

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

Measured on the rooms world with 3 rockets (native, M-series): mean per-tick render work while
bubbles run drops from 15.9 ms (re-meshing 8.5 chunks per tick) to 3.5 ms, with 28 KB of field
data per tick.

### Events

- `{kind:'detached', id, voxels, centroid:[3], velocity:[3], angular:[3], mesh:{vertices, vertexCount, indices, indexCount}, rigid?}`:
  a piece that lost support. The physics has already removed it from the world.
  - The mesh is in world coordinates at the moment of detachment.
  - `centroid` is the piece's centre of mass.
  - If `rigid` (**ext**) is true, the engine simulates the piece as rigid debris and the front
    end draws it at the poses of `debris` messages.
  - Otherwise, the front end animates it ballistically with no collision, fades it out and
    spawns dust.
- `{kind:'crack', pos:[3], normal:[3], strength}`: bond ruptures (decals, particles).
- `{kind:'impact', pos:[3], energy}`: a blast, or a piece of debris landing. `energy` is in J
  (the landing kinetic energy for debris). Used for camera shake, dust and particles.
- `{kind:'bubble', id, center:[3], radius, level}`: debug. An active structural bubble.

### Rigid debris

With rigid debris, detached pieces fall, bounce, slide and come to rest on the voxel world
instead of vanishing (plan Phase 7).

- The engine sends `{type:'debris', poses}` after every tick while pieces exist.
- Each pose carries:
  - `id`: the piece's `detached` event id.
  - `pos`: its current centre of mass.
  - `rot`: its rotation since detachment, as a unit quaternion `[x, y, z, w]`.
  - `opacity`: 1, falling to 0 while the piece fades out.
- A mesh vertex `p` is drawn at `pos + R(rot)·(p − centroid)`.
- A rigid piece missing from a `debris` list has been removed.
- The web front end interpolates between the last two poses, one tick behind, so motion is
  smooth at any display rate.

Pieces that land on free structure faster than 2.5 m/s load it:

- The landing impulse is triaged statically as J / 0.05 s.
- If the landing ruptures bonds, it becomes a dynamic bubble. A landing inside a running
  bubble goes straight into that bubble.
- This is plan §B7's virtual impact, now at the real landing point, so pancake-style
  progressive collapse emerges.
- Bedrock and ground absorb landings.

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

A structural bubble can hold the worker for tens of milliseconds per tick. Player movement
must not wait for it, so the WASM worker streams **occupancy**:

- For every chunk it re-meshes or empties, it sends one bit per voxel.
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

`core/include/svx/api/svx_api.h` exposes a flat C API that the worker script
(`web/src/worker/wasm-worker.ts`) wraps into the messages above:

- lifecycle: `svx_create`, `svx_load_*`;
- simulation and commands: `svx_tick`, `svx_blast`, `svx_carve`;
- queries: `svx_raycast`, `svx_collide`;
- output: `svx_poll_meshes`, `svx_poll_events`, `svx_debris`, `svx_stats`;
- persistence: `svx_save_delta` / `svx_load_delta`;
- determinism: `svx_state_hash`, a digest of the session including debris poses, identical
  across thread counts and between native and WASM builds.

A pure-TypeScript mock worker with the same protocol lets the front end run without the WASM
build.
