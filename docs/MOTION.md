# Motion: kinematic bodies and joints

The static world stands still and pieces fall. Between the two are the things that move by
design: a door on its hinge, a lift, a drawbridge, a crane swinging a wrecking ball, a chain.
The core has two means for them:

- **Kinematic bodies** (§1): rigid frames the host drives. Their grids move with them; pieces
  are pushed and carried by them; their structures are loaded by the motion and break like any.
- **Joints** (§2): constraints between two things (pieces, voxels of grids, kinematic bodies,
  the world): hinges, sliders, ropes, rods, welds. Their loads go into what they hold on to, and
  they give way.

Grids are in [`GRIDS.md`](GRIDS.md); the core's method in [`V2_DESIGN.md`](V2_DESIGN.md).

## 1. Kinematic bodies

- **A body** is a pose (`Pose {pos, rot}`) the host sets (`add_kinematic(pose, base)`), and the
  grids that belong to it (`GridDesc::body`: their frames are in its frame). Its anchored voxels
  are held by its drive: they are its supports.
- **Driving.** Each tick, a body moves steadily from where it was to where it is going:
  - `drive_kinematic(id, target)`: where it is at the end of the next tick (it stays there
    unless driven again);
  - `set_kinematic_velocity(id, v, w)`: a velocity it keeps from the next tick until driven or
    given another (zero: it stops).

  Its pose within the tick is interpolated (position linearly, rotation by nlerp), and its
  angular velocity is taken from the rotation between the poses with the bundled deterministic
  math, so a session is bit-identical on every platform and thread count.
- **Pieces meet it as a moving surface.** Its grids are placed at each substep's pose, and each
  contact with them has the surface's velocity there (`v + w × (X − c)`): a lift carries a crate
  up, a turntable turns what stands on it, a moving wall pushes a crate along the ground. The
  settling of resting pieces is relative to the surface they rest on (a crate on a conveyor is
  carried at its speed, not held back), and a piece on a moving surface never sleeps.
- **Its structures are solved in its frame.** Their geometry does not change as it moves; their
  body loads are gravity turned into the frame, less the frame's acceleration, and the Euler and
  centrifugal loads of its rotation: `R⁻¹(g − a) − α × c − ω × (ω × c)` per unit mass at the
  node's centre `c`. What rests on them, joints and blasts load them through the frame
  (`to_body`). A drawbridge's deck bends less as it rises (at 80°: a tenth of its bending flat);
  an arm spun up breaks off by its inertia, and what breaks off moves on with the body's motion
  where it was.
- **A sudden start is a hard load.** A drive that jumps to a speed in one tick is a large
  acceleration over that tick (the structure feels it): ramp a drive (the game's machines ease
  their motion).
- **Removing it.** `remove_kinematic(id)` removes it with its grids; `remove_kinematic(id,
  true)` releases them: they fall as one piece with its velocity field (it breaks where the
  pieces' checks find it in parts), and joints on their voxels hold on to that piece.
- **Queries.** `kinematic(id, &state)` (pose, velocity, angular velocity, grids),
  `kinematics()`, `grid_body(grid)`, `grid_velocity(grid, X)`. `collide` and `sweep` report the
  velocity of what a box stands on or touches: a character controller rides a lift by adding
  `ground_velocity × dt` to its next move.
- **Persistence.** A body of the level (`base`) is added again by the level, before
  `load_delta`; its pose and motion (the velocity it keeps, or the pose it is driven to) are
  saved. A body of the session is saved whole. A level's body removed is saved as removed.

## 2. Joints

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
- **Limits and motors.** A hinge's turn (b's reference turned about the axis from a's, from
  where it was made) and a slider's move (along a's axis) can be limited (`limited`, `lower`,
  `upper`) and driven by a motor (`motor_speed` rad/s or m/s, at most `motor_max` N m or N):
  `set_joint_motor`, `set_joint_limits`.
- **Ends** (`JointAnchor`):
  - `World`: a point fixed in the world;
  - `Grid`: a voxel of a grid (the world grid's too): its structure holds it (it takes the load),
    it moves with its kinematic body, and it goes with the piece it breaks off in;
  - `Piece`: a voxel of a piece: it goes with the part it is in when the piece breaks;
  - `Kinematic`: a kinematic body's frame (its drive holds it; not a voxel of it).
- **Following the voxels.** An end on a voxel follows it: into the piece it breaks off in (a
  hinge on a door leaf that comes loose, a lamp's rope when its beam falls), into the part of a
  piece it stays with when the piece splits. When the voxel is gone (carved, burnt, crushed) the
  joint lets go (`JointBroken` with strength 0).
- **Breaking.** Beyond `break_force` (N) or `break_torque` (N m) it gives way (`JointBroken`,
  its force). A rope of 5 kN under a 1 t block gives way.
- **Loads.** A joint's force loads what its ends hold on to: the structure of a grid's voxel (a
  weight on a rope from a cantilever loads its root; a kinematic body's structures in its frame),
  a piece's bonds (its stress checks). A sleeping piece keeps pulling on what it hangs from (a
  dead load).
- **In the level.** A free part held by a joint (a door leaf, a pendulum's bob, a crane's
  ball) is kept by the design pass, which removes free parts that stand on nothing, and comes
  loose as a piece in the first tick, hanging on its joint.
- **Solving.** Joints are solved with the contacts (sequential impulses, warm-started): after
  each sweep of the contacts, every joint in id order, the same on every thread count. A point
  and a lock of rotation are solved as 3 × 3 blocks, a hinge's two square turns and a slider's two
  square moves as 2 × 2 blocks, a motor and a limit as single clamped rows, a rope speculatively
  (it may tighten within a substep, not overshoot). Their position error is removed on pseudo
  velocities after the solve (split impulse: no energy is added), `rigid.joint_baumgarte` of it
  per substep beyond `rigid.joint_slop`. What hangs on a joint is held for sleep (it sleeps when
  still), but not settled like rubble on a floor (a pendulum swings on).
- **State.** `joint(id, &state)`: its ends in the world, the force and torque it carried in the
  last substep, its hinge angle, slider offset or rope length, the pieces its ends are on.
- **Joints are of the session.** Like pieces, they are not saved in deltas: a level makes its
  joints again when it loads.

Measured (`tests/core/test_joints.cpp`): a rod pendulum let go level keeps its length to 5 mm
through the bottom of its swing, and pulls its weight on average (678 N for 674 N); a hinged door stops at its
limit (1.2003 rad for 1.2) and does not sag; a slider's motor holds 0.50 m/s; a joint session is
bit-identical on 1 and 4 threads.

## 3. The C API

`svx/svx_core.h`:

- **Kinematic bodies:** `svxc_add_kinematic`, `svxc_remove_kinematic`, `svxc_drive_kinematic`,
  `svxc_set_kinematic_velocity`, `svxc_kinematic`, `svxc_kinematics`; grids join a body through
  `svxc_add_grid_desc` (`svxc_grid_desc::body`); `svxc_grid_body`, `svxc_grid_velocity`,
  `svxc_collide_ex` / `svxc_sweep_ex` (the velocity of what a box stands on or touches).
- **Joints:** `svxc_joint_desc` (`svxc_joint_defaults` fills a ball joint, axis z, length −1: the
  ends' distance, a rope), `svxc_add_joint`, `svxc_remove_joint`, `svxc_set_joint_motor`,
  `svxc_set_joint_limits`, `svxc_joint` (`svxc_joint_state`), `svxc_joints`; the event
  `SVXC_JOINT_BROKEN`.

## 4. The game

- **Machines.** `Game::add_machine(body, drive)` drives a kinematic body every tick with a
  `MachineDrive`, a function of time (the same in every session and replay):
  - `Oscillate`: along an axis, out by the amplitude (m) and back, eased;
  - `Spin`: about an axis at the amplitude (rad/s);
  - `Swing`: about an axis, out by the amplitude (rad) and back, eased.
- **Drops.** `Game::add_drop` drops an object in when play starts (after the bake and a saved
  session's changes): crates on a turntable.
- **Procedural worlds** carry kinematic bodies (`ProcBody`: pose, grids, drive), joints and drops:
  `load_procedural(game, make_procedural(kind, seed))`.
- **The `machines` world** (`?world=machines`, `svx_engine_demo --world machines`):
  - a lift beside a concrete tower, rising to its top and down again every 12 s;
  - a turntable with crates dropped on it;
  - a drawbridge raised 70° and lowered every 16 s;
  - a crane: a jib on a mast swinging a 3 t steel ball on a 5 m wire rope into a turned masonry
    wall;
  - a pendulum on a 3 m rod from a steel frame;
  - a chain of four wooden links on rods from a gallows;
  - a wooden door on a hinge in a brick wall.

  `tests/game/test_game.cpp` checks it: its machines move, its free parts hang on their joints,
  the ball knocks the wall down, and the session is bit-identical on 1 and 4 threads.
- **The browser** draws a kinematic body's grids with their frames, interpolated like pieces, and
  the ropes and rods (distance joints) as thin tubes (`joints` messages); the player rides what
  they stand on. `node web/scripts/machines-wasm.mjs http://localhost:5190/` checks it: the grids
  and ropes draw, and the player dropped on the lift rides it up and down (0.44 to 4.92 m).

## 5. Known limits

- **A kinematic body passes through the static world** (and other bodies): the host drives it
  where it wants. Pieces caught between it and the static world are squeezed out.
- **Joints are not saved** in deltas (a level makes them again; a joint of the session is lost,
  like the pieces it held).
- **Water and smoke** do not see a kinematic body's grids (they move); fire burns in them, but
  only their own flames heat them.
- **A rigid rope is rigid.** A ball stopped short by a wall pulls on what it hangs from within a
  substep: give ropes a `stiffness`.
- **Kinematic bodies come from the host,** not from streamed sources.
