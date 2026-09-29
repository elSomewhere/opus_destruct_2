# Vehicles

Drivable, fully physical vehicles in the destructible voxel world: cars that drive on cast
wheels, crumple in crashes (car against car, car against wall), lose their wheels, break
through walls at speed, and fill an endless procedural city with traffic. This document
describes how it works, layer by layer, and how to use it.

| Layer | Where | What |
| --- | --- | --- |
| Materials | `core/src/material/material.cpp` | smeared sections: sheet, car frame, engine, window, tyre, plastic, lamp; `crush` and `penetration` |
| Wheels | `core/src/phys/wheel.cpp`, `core/src/world/world_wheels.cpp` | the cast-wheel constraint: suspension, bump stop, tyre, brake; breakage |
| Crash damage | `core/src/phys/rigid.cpp` (crush patches), `core/src/world/world_crumple.cpp` | force-capped crumple contacts, the crumple pass, punch-through |
| In-place edits | `core/src/world/world_pieces.cpp` | a car keeps its id through crumpling and damage |
| Game | `game/src/vehicles.cpp`, `vehicle_models.cpp`, `traffic.cpp` | models, drivetrain, player driving, traffic |
| City | `game/src/drive_city.cpp` | the endless city with roads, lanes, signals, parking |
| C ABI | `game/src/api/svx_api.cpp`, `core/src/capi/svx_core.cpp` | vehicles, wheels, drive, shoot, traffic |
| Front end | `web/src/game/driving.ts`, `vehicles.ts`, `vehicle-effects.ts`, `web/src/render/wheels.ts`, `skids.ts` | controls, cameras, wheels, skid marks, effects, HUD |

## Materials: smeared sections

A voxel is 12.5 cm (the world) or 6.25 cm (a car). A car body is 1 mm steel sheet over a
hollow; a girder is thin-walled. Voxels of solid steel would weigh and resist ten to a
hundred times too much. So a thin-walled member's real section is *smeared* into the
material of its voxels, as reinforced concrete smears its bars into the concrete cell: its
density, stiffness and strength are what the member has per unit of its bounding volume.

| Material | rho (kg/m3) | notes |
| --- | --- | --- |
| `steel_section` | 980 | HEB 200 as 2 x 2 voxels: 44 MPa smeared |
| `sheet` | 260 | a car's body panels; **crush 90 kPa** |
| `car_frame` | 700 | rails, floor pan, pillars; crush 1.2 MPa |
| `engine` | 1200 | engine block and gearbox; crush 1.5 MPa (barely crumples) |
| `window` | 200 | 5 mm glazing; shatters |
| `tyre` | 265 | a wheel that came off |
| `plastic` | 300 | bumpers, trim; crush 150 kPa |
| `asphalt`, `paint` | 2300 | road surface and markings (tyre grip 1.0 / 0.9) |
| `lamp` | 400 | head and tail lamps; shatter |

Two properties drive vehicle damage:

- **`crush`** (Pa): the contact pressure at which a material folds. A contact with a crumpling
  side carries at most `crush x area`. 90 kPa over a car's frontal area is the few hundred kN
  (some 20 g) of a real front structure: a 1.2 t car at 50 km/h folds some 0.45 m of its front
  against a wall.
- **`penetration`** (J/m3): the energy density an impact (a bullet, a blast crater) needs to
  remove the material. Bullets hole sheet metal and shatter glass; they do not hole armour
  plate. Brittle materials (0) are removed by any impact.

The procedural levels' steel members (the yard's greenhouse and shed frames, the angles
world's portal and brace, the crane's jib) are `steel_section`; solid `steel` is kept for what
is solid (a wrecking ball, a pendulum's bob, bearing blocks). Fire weakens both alike.

## Plastic hinges

Steel yields: a member bent past its strength does not snap like glass, it folds. When a
structure's bond fails and its section is ductile on both sides (steel, steel sections, rebar)
and bending is what failed it (at least 1.5 times its tension, compression or shear share), the
part that comes loose there does not drop: it stays on a **plastic hinge** (`World::judge`,
`plastic_hinge`, `detach_unsupported`) -

- a `Hinge` joint about the axis it bends, found from the solved rotation of the two sides
  (no sign conventions to trust), pivoting at the section's compression edge so the parts fold
  about it rather than grind into each other;
- holding the section's plastic moment while it turns: a drive at speed 0 whose strength is
  `hinge_shape` (1.3) x the section's elastic moment - a friction hinge that absorbs moment x
  angle, the plastic work of a real hinge;
- tearing once turned past its rotation capacity (`break_angle` = `hinge_rotation`, 0.35 rad),
  or pulled harder than its section's tension capacity;
- anchored on the voxels either side of the section before they become a piece (the joint goes
  with them), one per pair of parts, the sections' moments summed.

A steel arm overloaded at its root folds down on its hinge, hangs bent if the load eases (a
weight that comes to rest on the ground), or tears off once turned too far. `plastic_hinges`
(tunable) turns it off; `WorldStats::plastic_hinges` counts them.

## Wheels

A wheel is not voxels: it is a constraint cast from the chassis (`World::add_wheel`,
`WheelDesc` in `core/include/svx/world/joint_desc.hpp`). Its mount is a voxel of the chassis
(a grid dropped in, or a piece) at the top of its suspension.

Each substep the tyre is **cast** along the suspension axis: samples on its lower arc (0, 15,
30, 45 degrees fore and aft) and across its width, against the static grids and the other
bodies, find the ground (a kerb is met by the front of the arc before the axle gets there).
Its rows are solved with the contacts and joints (sequential impulses, warm-started):

- **suspension**: a soft row (spring and damper as a constraint's softness: stable at any
  stiffness), from full droop (`rest`) to the **bump stop** (`travel`): a hard row beyond;
- **tyre**: longitudinal and lateral rows in the contact plane with a slip-dependent grip
  (peak at 8 % slip ratio, 0.1 rad slip angle; 72 % of it when sliding), limited to the
  **friction ellipse** of the load the suspension carries x the surface's grip; rigid below
  0.6 m/s (a car holds on a slope);
- **drive and brake**: the wheel's spin is a degree of freedom of its own (`inertia`); drive
  torque turns it, the brake row stops it (a locked wheel skids), rolling resistance slows it.

A wheel **comes off** when its mount voxel is gone (crushed, shot away) or its force passes
`break_force` (a hard crash on it): it becomes a wheel-shaped piece of `tyre` voxels
(`WheelDetached`), tumbling away. Wheels are saved with sessions and archived with their
chassis when it streams out; their `group` and `tag` (host data) let the game find its
vehicles again after a load.

## Crash damage

**Crush patches** (`core/src/phys/rigid.cpp`). The contacts of a body pair (or a body and a
static grid) are grouped per grid and facing into patches. For each patch a frontal voxel scan
finds the area pressed and the pressure its materials can take; the patch's force is capped at
`crush x area` of the softer side, spread over its contacts. A crushing contact does no
position correction: the bodies keep closing - the car's front goes *into* the wall - and the
collision's energy goes out over the distance it folds, as in a real crash. The wall feels
those few hundred kN, not the rigid spike of a car stopped in one substep (so a crash loads a
building like a crash, and breaks what it should).

**The crumple pass** (`World::crumple`, after each substep). The crushing side folds out of
what it hit: along its lattice axis nearest the push, column by column, each column whose front
is pressed in is pushed back until it is out; its neighbours are dragged along a cell less per
column (a dent has sloped sides). A column's front moves back as a whole where there is room
(a panel over a hollow); where there is not (a rail, a fender), what does not fit folds out
sideways and upwards (crumpled metal piles up in folds), or is compacted. Between two cars,
each folds half the overlap. Glass near a fold shatters (glittering dust events).

**Punch-through**. A wall pressed harder than it can hold around the patch - its punching
capacity, perimeter x thickness x its tensile and cohesive strength - is broken through: the
voxels in the car's way become rubble thrown ahead with its speed, and the car's excess
impulse is refunded. At speed a car goes through a brick wall where reinforced concrete stops
it (and folds its front).

**In-place edits** (`split_body(..., in_place)`, `refresh_in_place`). A crumpled car keeps its
piece id, its place and its motion (mass and contact samples are rebuilt); a car that breaks
in two keeps its id on the part with its wheels. Each changed piece is announced once per tick
(`PieceReshaped`; the game remeshes it: `GameEvent::Remesh`).

## The game

**Models** (`game/src/vehicle_models.cpp`): a compact, a sedan, a van, a pickup and a truck,
built from 6.25 cm voxels - a sheet-metal shell (tumblehome, rounded corners, wheel arches)
over a car frame, an engine block, glass, bumpers, lamps and seats, painted in the `paint`
layer (`svx::Paint`). Each has its wheel slots (mount, radius, suspension) and its tuning.

**Driving** (`game/src/vehicles.cpp`). `Game::spawn_vehicle` drops the model in as a grid of
its own on its wheels (group = the vehicle's id, tag = kind, paint, slot, flags). Each tick,
before the physics: the controls (the player's `Game::drive`, or a driver's) become wheel
inputs - an engine with a torque curve through an automatic gearbox (it shifts on road speed;
traction control; a launch clutch; engine braking), brakes with a front bias, a handbrake on
the rear, speed-sensitive steering with Ackermann geometry, anti-roll bars, air drag. After
the physics, the registry is rebuilt from the wheels (a vehicle whose wheels are all gone is
gone; one back from the archive is found again).

Everything is deterministic and logged (`replay.hpp`: `Vehicle` spawn/remove/enter/exit,
`Drive`, `Shoot`, `Traffic`): a replay drives the same cars and crashes them the same way,
bit for bit, on any number of threads.

**Traffic** (`game/src/traffic.cpp`). Around the viewer, on a world whose source has a
`RoadNetwork` (`GameSource::roads`): drivers follow their lanes (pure pursuit through the
junctions onto the lane they take), at the limit (less in turns), stop at red lights, keep
their distance (raycasts ahead: cars, rubble, walls), and become wrecks after a hard hit, a
lost wheel or a roll. Parked cars wait at the kerbs. Cars out of range that nobody touched go
(and come again); wrecks stay (archived with their region). The player can take any car.

**The drive city** (`game/src/drive_city.cpp`, `make_drive_city`). Endless in every direction
(some 130 km each way): a grid of 56 m cells; streets (a lane each way, parking at the kerbs)
and every fourth an avenue (two lanes each way, a double yellow centre line); sidewalks with
lamps and trees; blocks of apartment buildings, shops with glass fronts, houses with gardens,
office towers, warehouses, parks and car parks - masonry, reinforced concrete, glass, timber,
steel, all destructible, painted. Its lanes, turns, signals (a 34 s cycle) and parking spots
are a `RoadNetwork`. The ground ahead of the player's car is made resident before it gets
there, whatever its speed.

## Using it

C++ (`svx::Game`):

```cpp
game.load_streaming(make_drive_city(seed, 0.125), 0.125, stream_config);
const u32 car = game.spawn_vehicle({VehicleKind::Sedan, Paint::Red}, pos, yaw);
game.enter_vehicle(car);
game.drive({/*throttle*/ 1.0, /*brake*/ 0.0, /*steer*/ 0.2, /*handbrake*/ false});
for (const VehicleView& v : game.vehicles()) { /* v.speed, v.gear, v.rpm, v.damage ... */ }
for (const WheelView& w : game.wheel_views()) { /* draw a tyre at w.centre, w.rot */ }
```

C ABI (`svx_api.h`): `svx_spawn_vehicle`, `svx_enter_vehicle`, `svx_exit_vehicle`,
`svx_drive`, `svx_vehicles` / `svx_vehicles_data` (34 doubles each), `svx_wheels` /
`svx_wheels_data` (15 doubles each), `svx_vehicle_near`, `svx_shoot`, `svx_set_traffic`, and
the `drive` level of `svx_load_procedural`. The core's own C API (`svx_core.h`) has the wheels
(`svxc_add_wheel`, `svxc_wheel_state`, ...).

## The web front end

`?engine=wasm` loads the drive city by default (`?world=drive`).

| Key | On foot | Driving |
| --- | --- | --- |
| E | take the car near by (the prompt names it) / use | get out |
| W / S | walk | throttle / brake, then reverse |
| A / D | strafe | steer |
| Space | jump | handbrake |
| C | (noclip down) | camera: chase, far, roof |
| B | drop a car ahead | - |
| mouse | look | look around (swings back behind the car) |

A gamepad drives too (RT/LT, left stick, A handbrake, Y in/out, right stick looks).

- A car's body is its chassis piece (a detached event, debris poses; remeshed when it
  crumples); its **wheels** are drawn from the `vehicles` message - a tyre on a five-spoke
  rim, turned and spun, the spokes blurred when fast - interpolated one engine tick behind
  like the pieces.
- **Cameras** (`driving.ts`): the chase camera lags the car's heading and shows part of a
  slide, widens its field of view with speed, is kept out of walls (a ray through the
  occupancy) - where the way back is short (a wall, a parked car behind) it rises over it
  rather than closing in on the roof - and shakes on a crash.
- **Effects** (`vehicle-effects.ts`): skid marks (translucent quads on the road, a ring of
  6000), tyre smoke on hard surfaces, dust on soil, sparks where a crash hits (from the sudden
  change of the car's velocity), glass shards and metal sparks from the engine's dust events.
- **Look**: car paint has a clear coat (the sky mirrored by Fresnel and the sun's highlight),
  glass and lamps are glossy.
- **HUD** (`ui/drivehud.ts`): speed, gear, revs, damage, wheels on, the handbrake light.
- The settings panel has the traffic (on/off, cars driving and parked, their speed).
- The worker sends poses (debris, vehicles) with a sequence number the page acknowledges each
  frame; while the page is far behind (a slow machine) it holds poses back, so the page gets
  the latest state rather than a backlog.

Browser check: `node scripts/drive-wasm.mjs` (dev server running) takes the wheel of a car,
drives, slides, rams a van, gets out - screenshots, and it fails on any console error.

## Performance

What a crash costs is kept to what changed:

- **Spawning** a car is a few milliseconds: its model's fragments are made once per kind (the
  world keeps the fragments of session grids' chunks by content), it comes loose whole as one
  piece at once (`World::loosen_grid`: no structure is solved for it), and its mesh is its
  kind's, re-tinted to its paint (`Game::shape_mesh`); the models are made when a world with
  roads loads.
- **A crumpling car** is checked for fracture on its own schedule (`crumple_check_gap`
  substeps apart while it crumples; a first hit at once), and its mesh is built chunk by chunk
  in parallel. A car scraping along a building at 50 km/h: ticks of 2.6 ms median, 18 ms p90.
- **A hit on a building** patches its structure: only the changed chunks are fragmented and
  bonded again (see [`CORE.md`](CORE.md)), not the whole building.
- **The first touch** of a streamed building designs it once: the chunks it can reach are
  fragmented in parallel (the flood crosses into a chunk only where free voxels meet), the
  fragmenter reads each cell's Voronoi seed from a table instead of hashing it for every
  voxel, and the structure takes its new strengths in place.

- **Streaming while driving**: the chunk mesher finds a direction's visible faces with word
  operations on bit columns of the chunk's solidity and looks at those alone (the meshes are
  bit for bit the same): meshing what streams in at 80 km/h is ~2 ms a tick, not ~25 ms.

Measured natively on 4 threads in the drive city: a building's first touch is ~0.15 s
(median), ~0.55 s (p90), ~1.6 s for the largest office towers (34 x 34 x 56 m); later hits
~30 ms (median), ~0.2 s on a tower (its multigrid is rebuilt).

## Tests

- `tests/core/test_crumple.cpp`: a crate-like car against a reinforced-concrete wall at
  50 km/h (its front folds, the wall stands), faster folds further, at speed it goes through
  masonry where concrete stops it; a head-on crash is deterministic.
- `tests/core/test_capi.cpp`: a chassis on four wheels settles, drives, loses a wheel.
- `tests/core/test_joints.cpp`: a steel arm bent past its strength folds down on a plastic
  hinge and tears off only once turned past its capacity; without hinges it snaps at once.
- `tests/game/test_vehicles.cpp`: every model is connected and heavy enough; a car settles,
  shifts up, steers, brakes and reverses; every kind drives off without tipping; a player's
  drive replays bit for bit and a saved session restores its vehicles; driven into a wall, its
  front crumples.
- `tests/game/test_drive_city.cpp`: lanes on asphalt between kerbs, markings, parking, turns
  that lead on; streamed traffic drives and parks, nothing falls through the road.
- `web/test/vehicles.test.ts`: the front end's vehicle poses, the car to take, the wheel mesh.

## Limits

- Plastic hinges form in static structures; a steel piece (already loose) that is overloaded
  in bending breaks at its bonds.
- Car bodies are one grid: panels come off by fracture (a split keeps the car's id on the part
  with its wheels), not on hinges of their own.
- Crumpling folds along lattice axes: a side impact folds a door in, a frontal one the front;
  a very oblique blow folds along the axis nearest to it.
- The first touch of a large building is a hitch (above): its design runs at once, on the
  simulation's threads.
- There is no sound yet.
