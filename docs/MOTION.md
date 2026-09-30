# Motion: joints and machines

The static world stands still and pieces fall. What moves by design - a door on its hinge, a
lift, a drawbridge, a turntable, a crane swinging a wrecking ball, a chain - is pieces too:
free parts held to structures by **joints** (§1), some of them **driven** (§2). Nothing moves
that the physics does not move: a machine's parts have mass, what they carry loads them, their
joints load the structures they hold on to, and when a structure gives way (shot, blasted,
burnt) what it held comes down with it.

Grids are in [`GRIDS.md`](GRIDS.md); the core's method in [`V2_DESIGN.md`](V2_DESIGN.md).

## 1. Joints

```cpp
JointDesc d;
d.type = JointType::Hinge;
d.a = {JointAnchor::Kind::Grid, kWorldGrid, V3{2.0, 0.0, 1.0}};  // a wall's voxel of the world grid
d.b = {JointAnchor::Kind::Grid, door_grid, V3{2.0, 0.0, 1.0}};   // the door's voxel there
d.axis = V3{0, 0, 1};
d.limited = true;
d.lower = -1.6;
d.upper = 1.6;
const JointId hinge = world.add_joint(d);
```

- **Types** (`JointType`):
  - `Ball`: the ends' points coincide;
  - `Hinge`: and their axes stay aligned (it turns about the axis);
  - `Slider`: the ends keep their relative rotation and b moves along a's axis only;
  - `Fixed`: the ends keep their relative place and rotation (a weld);
  - `Distance`: the points keep a distance in a range: a rope (`rope`, it only pulls: up to its
    `length`), a rod (the length both ways). A distance joint may stretch like a spring
    (`stiffness` N/m, `damping` N s/m): a crane's wire rope passes a shock on to the jib over the
    time it stretches, not in one substep.
- **Limits.** A hinge's turn (b's reference turned about the axis from a's, from where it was
  made) and a slider's move (along a's axis) can be limited (`limited`, `lower`, `upper`):
  `set_joint_limits`.
- **Ends** (`JointAnchor`):
  - `Grid`: a voxel of a grid (the world grid's too): its structure holds it (it takes the load),
    and it goes with the piece it breaks off in;
  - `Piece`: a voxel of a piece: it goes with the part it is in when the piece breaks;
  - `World`: a point fixed in the world (an indestructible hold: for tests and scripted scenes;
    a level's machines hold on to structures).
- **Following the voxels.** An end on a voxel follows it: into the piece it breaks off in (a
  hinge on a door leaf that comes loose, a lamp's rope when its beam falls), into the part of a
  piece it stays with when the piece splits. When the voxel is gone (carved, burnt, crushed) the
  joint lets go (`JointBroken` with strength 0).
- **Breaking.** Beyond `break_force` (N) or `break_torque` (N m) it gives way (`JointBroken`,
  its force). A rope of 5 kN under a 1 t block gives way. A hinge also gives way turned past
  `break_angle` from where it was made (a plastic hinge torn through, a door torn off).
- **Latches.** A hinge with a `latch` (N m) is held shut - it does not turn at all - until the
  torque about its axis passes the latch's strength: then the latch gives way, what it could
  not hold passes on (a door knocked open swings), and the hinge turns within its limits from
  then on. What the latch held does not count towards the hinge's `break_torque`
  (`JointState::latched`; sessions keep it). A car's doors and bonnet (docs/VEHICLES.md).
- **Collision.** `collide = false`: its two ends' pieces do not collide with each other while it
  holds (a part welded into its frame); once it gives way they do.
- **Loads.** A joint's force and torque load what its ends hold on to: the structure of a grid's
  voxel (a weight on a rope from a cantilever loads its root; a motor's reaction twists what it
  is mounted on), a piece's bonds (its stress checks). A joint whose pieces sleep keeps carrying
  what it carried when they fell asleep.
- **In the level.** A free part held by a joint (a door leaf, a pendulum's bob, a lift's car) is
  kept by the design pass, which removes free parts that stand on nothing, and comes loose as a
  piece in the first tick, on its joint. (A free part a voxel away from what it moves along
  bonds to nothing: junctions reach half a voxel.)
- **Solving.** Joints are solved with the contacts (sequential impulses, warm-started): after
  each sweep of the contacts, every joint in id order, the same on every thread count. A point
  and a lock of rotation are solved as 3 × 3 blocks, a hinge's two square turns and a slider's two
  square moves as 2 × 2 blocks, a drive and a limit as single clamped rows, a rope speculatively
  (it may tighten within a substep, not overshoot). Their position error is removed on pseudo
  velocities after the solve (split impulse: no energy is added), `rigid.joint_baumgarte` of it
  per substep beyond `rigid.joint_slop`. What hangs on a joint is held for sleep (it sleeps when
  still), but not settled like rubble on a floor (a pendulum swings on). Pieces joined sleep
  together, and wake together: a piece woken wakes the pieces joined to it, and theirs, before
  it is solved (asleep, a piece is a static support: a car woken still would hang on its
  sleeping door).
- **Queries.** `joints()` (the host's: an articulation's own joints are in its state, §6),
  `joint(id, &state)`, `joined_pieces(piece)`: the pieces joined to a piece by joints that hold
  (a car's parts still on it).
- **State.** `joint(id, &state)`: its ends in the world, the force and torque it carried in the
  last substep, its hinge angle, slider offset or rope length, the pieces its ends are on.

## 2. Drives and machines

A hinge's or a slider's **drive** (`JointDrive`, `JointDesc::drive`, `set_joint_drive`) is a
motor of limited strength (`max`: N m, N):

- `Speed`: at `speed` (rad/s, m/s);
- `Target`: to `target` (rad, m) and held there - a servo: it asks for `stiffness` times its error
  (1/s), at most `speed`;
- `Oscillate`: from `target` to `target2` and back every `period` s, eased (`phase` s in) - a
  machine's program on the **world's clock** (`World::time()`: simulated time since the level
  loaded; paused ticks do not count, a saved session's is restored). The servo follows the
  program's rate and position.

A drive weaker than its load stalls (a car too heavy for its winch sinks to its limit); one that
meets an obstacle pushes with at most its strength. A **machine** is pieces on driven joints held
by structures - the game's `machines` world (§4) has a lift, a turntable, a drawbridge and a crane:

- **What rides on it** is carried: it settles relative to the part it rests on (a crate on a
  turntable is not held back), never sleeps while the machine runs, and a machine at work wakes
  what it touches however slowly it moves. A machine at its target, still, sleeps like rubble.
- **Characters ride it.** `collide`, `sweep`, `overlaps` and `depenetrate` see the pieces'
  voxels (turned cubes, as the oriented grids'): `CollideResult::ground_piece` and
  `ground_velocity` (its velocity under the box: `v + w × (X − x)`) - a controller adds
  `ground_velocity × dt` to its next move, and lifts the box out of a car that rose into it
  (`depenetrate`) before it moves on.
- **It comes down with what holds it.** Its joints hold on to structure voxels: shoot the voxel
  and the joint lets go; cut the tower under the lift and the car goes with the falling part it
  hangs on. Its parts are pieces: they break like any (the drawbridge's deck, the crane's jib).
- **Its supports must carry it.** A joint loads the fragment its voxel is in: a turntable's motor
  spinning up twists its pedestal; a drawbridge's hinge on the corner of a concrete abutment tears
  it out; a jib's hinge seated in the top fragment of a concrete mast breaks that fragment out
  (it is a quarter of the mast's section); a steel mast standing on rock snaps at the rock (the
  joint is as strong as the rock in tension). The `machines` world's supports are engineered: an
  RC pedestal a metre square and a motor sized to the disc's inertia, steel seats (a cap on the
  crane's RC mast, a hinge seat set into the abutment) that spread a hinge's load over the
  section. (Bullets and craters do not remove steel: a support meant to be shot away is concrete,
  masonry or timber; steel gives way to the loads a blast puts on it.)
- **Kept.** A joint's pieces are never culled over `max_bodies` or the pieces' budget (nor are
  pieces the host keeps: `set_piece_keep`).
- **Saved.** Pieces, joints and the world's clock are in deltas: a machine comes back where it
  was, on its program (CORE.md §4).
- **Streamed.** A machine wholly out of range is archived with its joints and what it carries
  (the change archive's budget) and comes back, running its program, when its chunks are
  resident again; a chunk source places machines with `ChunkSource::joints` (CORE.md §4).

Measured (`tests/core/test_joints.cpp`, `test_machines.cpp`): a rod pendulum let go level keeps
its length to 5 mm through the bottom of its swing, and pulls its weight on average (678 N for 674
N); a hinged door stops at its limit (1.2003 rad for 1.2); a slider's drive holds 0.50 m/s; a
servo lifts 0.5 t to 1.1996 m for a target of 1.2 and follows a program within 1.1 mm; a drive of
2 kN under 4.9 kN stalls; a lift's car carries a crate 2 m up and down (the slider carries their
6066 N); a turntable turns a crate 1 m out at its speed (0.471 m/s for 0.471); a character rides
a car 3 m up and down; the anchor voxel shot away, the car falls; a session with machines is
bit-identical on 1 and 4 threads.

## 3. The C API

`svx/svx_core.h`: `svxc_joint_desc` (`svxc_joint_defaults` fills a ball joint, axis z, length −1:
the ends' distance, a rope, its drive off), `svxc_joint_drive`, `svxc_add_joint`,
`svxc_remove_joint`, `svxc_set_joint_drive`, `svxc_set_joint_limits`, `svxc_joint`
(`svxc_joint_state`), `svxc_joints`, `svxc_time`; the event `SVXC_JOINT_BROKEN`; riding:
`svxc_collide_ex` / `svxc_sweep_ex` (`ground_piece`, `piece`, their velocity), `svxc_overlaps`,
`svxc_depenetrate`; `svxc_set_piece_keep`.

## 4. The game

- **Drops.** `Game::add_drop` drops an object in when play starts (after the bake and a saved
  session's changes; a played session's drops are among its pieces, not dropped again).
- **Procedural worlds** carry grids, joints and drops: `load_procedural(game, make_procedural(kind,
  seed))`.
- **The `machines` world** (`?world=machines`, `svx_engine_demo --world machines`):
  - a lift beside a reinforced concrete tower: a timber car on a slider held by the tower's face,
    rising to its top and down again every 12 s;
  - a turntable: a timber disc on a hinge held by a pedestal under its centre, turning at 0.4
    rad/s, crates dropped on it;
  - a drawbridge: a timber deck on a hinge in a steel seat in its abutment, raised 70° and lowered
    every 16 s;
  - a crane: a steel jib on a hinge in the steel cap of a reinforced concrete mast, swinging 125°
    and back every 10 s, a 1 t steel ball on a 5 m wire rope from its tip, into a turned masonry
    wall;
  - a pendulum on a 3 m rod from a timber frame's beam, a chain of four wooden links from a timber
    gallows, a wooden door on a hinge in a brick wall.

  `tests/game/test_game.cpp` checks it: its machines follow their programs, its joints hold, the
  ball knocks the wall down, a session saved at 3 s comes back as it was, the session is
  bit-identical on 1 and 4 threads - and the machines come down with what holds them: the lift's
  slider shot off the tower (the car falls from 1.3 m to the ground), the pendulum's beam shot at
  its pivot (the bob falls), a rocket at the crane mast's foot (the jib comes down from 8.2 m), a
  rocket at the drawbridge's seat (the deck falls), a rocket on the turntable (its disc breaks).
- **The browser** draws the machines' parts as pieces and the ropes and rods (distance joints) as
  thin tubes (`joints` messages); the pieces' voxels come with their meshes (`occupancy`) and the
  player's collision sweeps them where they are drawn, riding what they stand on.
  `node web/scripts/machines-wasm.mjs http://localhost:5190/` checks it: the player dropped on the
  lift's car rides it up and down (0.31 to 4.80 m), and on the turntable goes round with it (1.65
  rad in 4 s at 0.4 rad/s).

## 5. Known limits

- **A `World` anchor is indestructible** (a point fixed in the world): a level's machines hold on
  to structures instead.
- **A rigid rope is rigid.** A ball stopped short by a wall pulls on what it hangs from within a
  substep: give ropes a `stiffness`.
- **A joint on a static voxel of a chunk that goes out of range is dropped** with its chunk (a
  source's is made again with its grids; one the host made is not).
- **A chunk source's joint holds on to the grids at home in its chunk** (or the world grid).
- **An articulation near an awake piece is solved at the world's substep** (1/120 s), with the
  world's iterations: its muscles and limits are stiffer there than in its own fine steps.

## 6. Articulations

A body made of parts that move on their own - a person, a creature, a robot, a rag doll - is an
**articulation** (`svx/world/articulation.hpp`): **links** (rigid bodies of no voxels that
collide as spheres: `BodySphere`s in their frames) held together by **joints** with the limits
of anatomy - a ball's elliptical cone and twist, a hinge's range - and moved by **muscles** (a
joint's drive to a relative rotation: stiffness, damping towards a target rate, a torque limit,
a feed-forward torque, the limb's inertia the damper works on) and **targets** (soft pulls of a
link to a point or a rotation of the world, each axis on or off, capped: `TargetDrive`).

```cpp
ArticulationDesc d;                          // links, joints, targets, the link pairs that collide
d.links = ...;                               // mass, inertia, pose, spheres, friction, tissue
d.joints = ...;                              // Ball / Hinge / Fixed between parent and child links
d.group = kMyGroup; d.tag = my_id; d.data = who_it_is;   // the host's: saved and archived with it
const ArticulationId id = world.add_articulation(d);
ArticulationControl* c = world.articulation_control(id);  // before each tick: muscles, targets,
c->muscles[k].target = ...;                  // forces, ghosts, anchors, spin cap, drag, sleep
world.tick();
ArticulationState st;
world.articulation_state(id, &st);           // where the links are, what each felt (contact, the
                                             // hardest touch's normal and point, impact, bumped)
```

- **One world.** Its links are bodies of the world's rigid world: they stand on the structures
  and load them, are knocked by what hits them (a car, a falling slab), push what they meet
  (debris, other bodies). The link pairs of its desc (`collide`) collide with each other
  (frictionless: a leg brushes past the other), the rest not at all.
- **Stepped finer on its own.** An articulation that touches no awake piece is an island of its
  own, stepped `link_substeps` times a substep (4: 1/480 s) with `link_iterations` velocity and
  `link_position_iterations` position passes: joints, targets, then contacts (what the ground
  holds up last is held up). Islands are stepped in parallel, the same on any thread count. Near
  an awake piece it is solved with it at the world's substep.
- **Supple limits.** Past a limit a joint turns back at most 0.025 rad a step, in its velocity and
  position passes alike: a body folded far past its range (a corpse landing on its back) comes out
  of it over a few steps instead of being flung.
- **Senses.** A link's senses are what the world did to it over the tick: the contacts of its own
  links with each other are not senses (a foot brushing the other leg has touched nothing).
- **Sleep, save, stream.** It sleeps when still if its host lets it (`can_sleep`: a corpse); a
  sleeping one is rubble at rest (it costs nothing). It is saved with sessions (`save_delta`);
  in a streamed world it is archived with its region when it rests out of range - or when it is
  wholly beyond the evict radius - host data and all, and comes back with the region
  (`ArticulationAdded` events; its host finds it by its group, tag and data).
- **Hosts.** svx_anim's characters are articulations on the deep path (docs/ANIM.md): a
  `CoreBinding` writes a character's body (links, joints, muscles, assists) as a desc, pushes its
  drives before each tick and pulls the links after it.
