# Vehicles

Drivable, fully physical vehicles in the destructible voxel world: cars that drive on cast
wheels, crumple in crashes (car against car, car against wall), lose their wheels, doors,
bonnets and bumpers, break through walls at speed, and fill an endless procedural city with
traffic. They are the game's (`svx_game`): authored from the core's generic building blocks -
voxels of smeared-section materials, the wheel constraint, crumpling, joints with latches - which
know nothing of cars ([`DAMAGE.md`](DAMAGE.md), [`MOTION.md`](MOTION.md) §7). This document
describes how the game builds them on those, and how to use them.

| Layer | Where | What |
| --- | --- | --- |
| Materials | `game/src/materials.cpp` | the game's smeared sections: sheet, car frame, engine, window, tyre, plastic, lamp; asphalt, paint |
| Wheels | the core's (`World::add_wheel`: [`MOTION.md`](MOTION.md) §7) | suspension, bump stop, tyre, brake; breakage |
| Crash damage | the core's ([`DAMAGE.md`](DAMAGE.md) §4) | force-capped crumple contacts, the crumple pass, punch-through, in-place edits |
| Parts | `game/src/vehicle_models.cpp` on the core's joints ([`DAMAGE.md`](DAMAGE.md) §5) | doors, bonnet, boot, bumpers, cargo on latched hinges and fixed joints |
| Game | `game/src/vehicles.cpp`, `vehicle_models.cpp`, `traffic.cpp` | models, drivetrain, player driving, traffic |
| City | `procgen/src/drive_city.cpp` (`svx_procgen`) | the endless city with roads, lanes, signals, parking |
| C ABI | `game/src/api/svx_api.cpp` (the core's own: `core/src/capi/svx_core.cpp`) | vehicles, drive, shoot, traffic (wheels) |
| Front end | `web/src/game/driving.ts`, `vehicles.ts`, `vehicle-effects.ts`, `web/src/render/wheels.ts`, `skids.ts` | controls, cameras, wheels, skid marks, effects, HUD |

## Materials

A car's voxels are 6.25 cm; its body is 1 mm steel sheet over a hollow. Its materials are
**smeared sections** ([`DAMAGE.md`](DAMAGE.md) §1): what the real, thin-walled part has per unit
of its bounding volume. The game registers them when it starts (`register_game_materials`, in
`Game`'s constructor, before its world: `svx/game/materials.hpp`), at the ids after the core's
standard presets (`mat::Sheet` = `kStandardMaterials` + 0 ... `mat::Lamp` + 8, the ids they
always had: saves and replays read as before), and gives the fire module their fire properties
(`set_game_fire_materials`: sheet steel heats through fast, plastic and tyres burn).

| Material | rho (kg/m3) | notes |
| --- | --- | --- |
| `sheet` | 260 | a car's body panels; **crush 90 kPa**, penetration 2e4 J/m3 |
| `car_frame` | 700 | rails, floor pan, pillars; crush 1.2 MPa |
| `engine` | 1200 | engine block and gearbox; crush 1.5 MPa (barely crumples) |
| `window` | 200 | 5 mm glazing; shatters |
| `tyre` | 265 | a wheel that came off (its wheels' `WheelDesc::material`) |
| `plastic` | 300 | bumpers, trim; crush 150 kPa |
| `asphalt`, `paint` | 2300 | road surface and markings (tyre grip 1.0 / 0.9: `Material::grip`) |
| `lamp` | 400 | head and tail lamps; shatter |

90 kPa of `crush` over a car's frontal area is the few hundred kN (some 20 g) of a real front
structure: a 1.2 t car at 50 km/h folds some 0.45 m of its front against a wall. Bullets hole
sheet metal (`penetration`) and shatter glass; they do not hole the engine block.

## Wheels

A car's wheels are the core's wheel constraint ([`MOTION.md`](MOTION.md) §7), mounted on voxels
of its body's frame: each model's wheel slots give the mount, radius, suspension (rest, travel,
stiffness, damping), spin inertia and breaking strength, and `mat::Tyre` as what a wheel that
comes off is made of. Their host data is the game's registry: `group` is the vehicle's id, `tag`
its kind, paint, slot and flags - after a session is loaded, or a car comes back from the
streaming archive, the game finds its vehicles again from their wheels.

## Crash damage

A car crumples by the core's rules ([`DAMAGE.md`](DAMAGE.md) §4): its contacts carry at most its
materials' crush strength over the area pressed, the crumple pass folds its front (or its side)
out of what it hit, and a wall pressed past its punching capacity is broken through - at speed a
car goes through a brick wall where reinforced concrete stops it (and folds its front). A
crumpled car keeps its piece id, its place and its motion; a car that breaks in two keeps its id
on the part with its wheels; each changed piece is remeshed (`GameEvent::Remesh`).

**Damage** (`VehicleView::damage`, the HUD's): how much of the car is not as it was built -
its body's voxels gone or changed in its lattice, cells filled that were empty (the folds), and
the voxels of its parts no longer on it - with a fifth of it changed counting as a wreck. A
scrape shows a little, a crash into a wall at 60 km/h about two thirds (its bumper off).

## Parts that come off

In a real crash the connections fail first: a door is torn off its hinges, a bonnet's latch
pops, a bumper's mounts shear. So a car's doors, bonnet, boot lid, tailgate, bumpers and the
cargo strapped in a pickup's bed are **grids of their own** in the car's frame (`VehiclePart`,
`VehicleModel::parts`), their voxels beside the body's along their seams, held to it by the
core's joints ([`DAMAGE.md`](DAMAGE.md) §5: latches, one latch per hinge, parts that sleep and
wake with their body):

- **hinged parts** (doors on a vertical axis at their front edge, a van's and a lorry's rear
  doors at their outer edges, a bonnet at its rear, a boot lid at its front, a tailgate at its
  foot) are on a `Hinge` joint with a **latch**: latched, the hinge does not turn at all;
  knocked past the latch's strength, the part swings within its limits (a door out, never in; a
  bonnet up); turned past its stop, or pulled or twisted beyond its strength, it tears off.
- **bumpers** (and the cargo's straps) are on `Fixed` joints that shear beyond their strength (a
  bumper takes a car's deceleration times its share of the car; the first to meet a wall, it
  comes off).

A part does not collide with its car while its joint holds; once loose it is rubble like any
piece. The strengths are set so that driving (full throttle, a handbrake turn, an emergency
stop, the traffic's driving) shakes nothing loose, a knock pops latches, and a crash tears off
what it hits: a van at 40 km/h into a car's side takes its door off; a car into a wall at
60 km/h loses its bumper.

The registry needs nothing more: a car's parts are the pieces joined to its body
(`World::joined_pieces`, `Game::vehicle_parts`; `VehicleView::parts` and `parts0`). A car
removed (or gone out of range untouched) takes the parts still on it along; a car archived out
of range goes with its parts as one joint group, and comes back with them, latches and all.

## The game

**Models** (`game/src/vehicle_models.cpp`): a compact, a sedan, a van, a pickup and a truck,
built from 6.25 cm voxels - a sheet-metal shell (tumblehome, rounded corners, wheel arches)
over a car frame, an engine block, glass, bumpers, lamps and seats, painted in the `paint`
layer (`svx::Paint`). Each has its wheel slots (mount, radius, suspension), its tuning and its
parts (above): a sedan four doors, a bonnet, a boot lid and two bumpers; a compact two doors;
a van its cab's doors and two rear doors; a pickup a tailgate and two crates in its bed; a
lorry its cab's doors, its box's rear doors and a steel bumper. A part's voxels are marked as
the shell is made (the shell's alone: what is built inside it afterwards is the body's), and
where it is held is found when the model is done - the seam with the body nearest the point
asked for; the body stays one piece (a corner cut off by two parts goes with the one it
touches most).

**Driving** (`game/src/vehicles.cpp`). `Game::spawn_vehicle` drops the model in as a grid of
its own on its wheels (group = the vehicle's id, tag = kind, paint, slot, flags), its parts as
grids beside it on their joints. Each tick,
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

**The drive city** (`procgen/src/drive_city.cpp`, `make_drive_city`). Endless in every direction
(some 130 km each way): a grid of 56 m cells; streets (a lane each way, parking at the kerbs)
and every fourth an avenue (two lanes each way, a double yellow centre line); sidewalks with
lamps and trees; blocks of apartment buildings, shops with glass fronts, houses with gardens,
office towers, warehouses, parks and car parks - masonry, reinforced concrete, glass, timber,
steel, all destructible, painted. Its lanes, turns, signals (a 34 s cycle) and parking spots
are a `RoadNetwork`. The ground ahead of the player's car is made resident before it gets
there, whatever its speed, and the car itself is a focus of the streaming (and the centre of
the traffic) whatever the host's viewer says: a host that falls behind never sees the car it
drives archived out of range.

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
`svx_drive`, `svx_vehicles` / `svx_vehicles_data` (36 doubles each: the last two its parts on
and built), `svx_wheels` /
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
  crumples), its doors, bonnet and bumpers pieces of their own (painted, drawn like any piece);
  its **wheels** are drawn from the `vehicles` message - a tyre on a five-spoke
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
drives, slides, rams a van, gets out - screenshots, and it fails on any console error, or if
driving shakes a part off or the crash takes none. On a machine without a GPU (software WebGPU)
it draws one frame in six but for its screenshots (`renderer.drawEvery`): the software GPU
otherwise takes the cores the page and the engine need, and every look at the page waits
seconds to minutes (the whole check runs in two minutes instead of seven or more).

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

- **A car on a bridge** loads its deck through its wheels. A wheel rolling onto a fragment is a
  load that moves, not a blow (an impact load case only beyond 2.5 x its share of the car's
  weight: a landing, a kerb struck at speed), and a large structure (400 nodes or more) under a
  creeping load is solved again at most every `load_trigger_gap` ticks (0.1 s; at once for a
  change of 4 x its trigger, an impact, or while it breaks). A sedan crossing the 20 m concrete
  bridge: 69 solves of it instead of 220, 2.3 ms a tick mean instead of 4.7 (0.3 median).
- **Parts** make a car up to nine pieces on joints: the traffic of the drive city (some 18
  cars) ticks in 2.5 ms mean natively, 0.6 ms more than cars of one piece (the driving cars'
  parts are awake with them; a parked car sleeps with its parts).
- **Streaming while driving**: the chunk mesher finds a direction's visible faces with word
  operations on bit columns of the chunk's solidity and looks at those alone (the meshes are
  bit for bit the same): meshing what streams in at 80 km/h is ~2 ms a tick, not ~25 ms.

Measured natively on 4 threads in the drive city: a building's first touch is ~0.15 s
(median), ~0.55 s (p90), ~1.6 s for the largest office towers (34 x 34 x 56 m); later hits
~30 ms (median), ~0.2 s on a tower (its multigrid is rebuilt).

## Tests

- The core's mechanisms (crumpling, hinges, latches, wheels): [`DAMAGE.md`](DAMAGE.md) §6,
  [`MOTION.md`](MOTION.md) §7.
- `tests/game/test_vehicles.cpp`: every model is connected and heavy enough; every model's
  parts are grids of their own beside its body, held at their seams, the body and its parts
  one connected vehicle; a car settles, shifts up, steers, brakes and reverses; every kind
  drives off without tipping; a player's drive replays bit for bit and a saved session
  restores its vehicles with their parts latched; driven into a wall, its front crumples and
  its bumper comes off; a pickup driven hard keeps its parts and cargo, a van at 40 km/h into a
  car's side tears its door off (the car keeps its id), and a removed car takes the parts
  still on it along; at 100 km/h a car does not pass through a loose slab 12.5 cm thick; a car
  driven over a bridge has its deck solved again a few times a second, not at every fragment.
- `tests/game/test_drive_city.cpp`: lanes on asphalt between kerbs, markings, parking, turns
  that lead on; streamed traffic drives and parks, nothing falls through the road, and no
  driving car loses a part; the player's car stays in the world while the host's viewer lags
  far behind it; a car left behind is archived with its parts and comes back with them on.
- `web/test/vehicles.test.ts`: the front end's vehicle poses, the car to take, the wheel mesh.

## Limits

The core's mechanisms' limits (a plastic hinge's energy, a part held at one point of its seam,
folds along lattice axes, wrecks that stay hooked) are in [`DAMAGE.md`](DAMAGE.md) §7. The
game's:

- The first touch of a large building is a hitch (above): its design runs at once, on the
  simulation's threads.
- There is no sound yet.
