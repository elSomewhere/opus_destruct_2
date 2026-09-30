# Damage: crumpling, penetration, yielding, parts that come off

The structure's own damage - bonds that crack, crush and shear, pieces that break off, fall and
break again - is in [`V2_DESIGN.md`](V2_DESIGN.md). This document is the damage of bodies built
of *thin-walled* and *ductile* material, and of bodies assembled from parts: what makes a
vehicle, a machine, a steel frame or a crate damageable the way the real thing is. None of it
knows what it is used for. A host builds its things from voxels, materials and joints, and they
dent, fold, yield, lose parts and get holed by the same rules as everything else in the world
(the game's cars: [`VEHICLES.md`](VEHICLES.md)).

| Mechanism | Where | What |
| --- | --- | --- |
| Smeared sections | `core/src/material/material.cpp`, a host's materials | a member's real section in its voxels' material |
| Penetration | `World::penetrates` (`world.cpp`) | impacts remove material by energy density |
| Plastic hinges | `World::judge`, `plastic_hinge`, `piece_hinges` | ductile sections yield in bending |
| Crush patches | `core/src/phys/rigid.cpp` | a crumpling contact's force is capped |
| The crumple pass | `core/src/world/world_crumple.cpp` | the crumpling side folds out of what it hit |
| Punch-through | `World::punch` (`world_crumple.cpp`) | a wall pressed past its punching capacity breaks out |
| In-place edits | `split_body(..., in_place)`, `refresh_in_place` (`world_pieces.cpp`) | a damaged piece keeps its id |
| Parts on joints | `core/src/phys/joint.cpp`, `JointDesc::latch`, `break_*` | parts held on hinges and welds that give way |

## 1. Smeared sections

A voxel is 12.5 cm (a world) or 6.25 cm (a small body). A car's body is 1 mm steel sheet over a
hollow; a girder is thin-walled; a machine's housing is a shell. Voxels of solid steel would
weigh and resist ten to a hundred times too much. So a thin-walled member's real section is
*smeared* into the material of its voxels, as reinforced concrete smears its bars into the
concrete cell: its density, stiffness and strength are what the member has per unit of its
bounding volume.

The core's standard presets have one: `steel_section` (HEB 200 as 2 x 2 voxels: 980 kg/m3,
44 MPa smeared, ductile). The procedural levels' steel members (the yard's greenhouse and shed
frames, the angles world's portal and brace, the crane's jib) are `steel_section`; solid `steel`
is kept for what is solid (a wrecking ball, a pendulum's bob, bearing blocks). Fire weakens
both alike.

A host registers the sections its things are made of (`MaterialTable::set`, `svxc_material_set`)
at ids of its own after the standard presets (`kStandardMaterials`). The game's, for its cars
and roads (`game/src/materials.cpp`, [`VEHICLES.md`](VEHICLES.md)):

| Material | rho (kg/m3) | crush | penetration (J/m3) | notes |
| --- | --- | --- | --- | --- |
| `sheet` | 260 | 90 kPa | 2e4 | 1 mm body panels and their stiffeners |
| `car_frame` | 700 | 1.2 MPa | 1.5e5 | rails, floor pan, pillars (2 mm box sections) |
| `engine` | 1200 | 1.5 MPa | 4e5 | an engine block and gearbox (barely crumples) |
| `window`, `lamp` | 200, 400 | 20 kPa | 0 | 5 mm glazing: it shatters |
| `plastic` | 300 | 150 kPa | 1e4 | bumpers and trim over a beam |
| `tyre` | 265 | - | 3e4 | a wheel that came off |

Two material properties drive this damage:

- **`crush`** (Pa): the contact pressure at which a material folds where it is pressed. A
  contact with a crumpling side carries at most `crush x area` (§4). 0: it does not crumple
  (brittle material cracks and crushes by its bonds; a solid block or a steel plate is rigid).
- **`penetration`** (J/m3): the energy density an impact needs to remove the material (§2). 0:
  any impact removes it (brittle materials).

## 2. Penetration

A shot (`World::shoot`: a bullet's carve with its energy) and a blast's crater remove the
material in their sphere where the impact's energy density reaches its `penetration`
resistance: the energy over the sphere's volume, concentrated at its centre (1.5 x there, 0.5 x
at its edge - a bullet's hole is deepest where it strikes). Bullets hole sheet metal and shatter
glass; a rocket holes a steel section; nothing holes armour plate. A cut (`World::carve` with no
energy) removes everything but indestructible material.

`impact_penetration` (tunable, on): off, impacts never remove ductile material (steel and bars
stay in a crater, as in the structural reference: [`BASELINE.md`](BASELINE.md)).

## 3. Plastic hinges

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

**Loose pieces** yield the same way (`World::piece_hinges`): a piece's stress check that breaks a
ductile section in bending records its hinge (in the piece's frame), and when the piece comes
apart there, a hinge of the same kind joins the parts - anchored on the piece's voxels either
side of the section before it splits (the joint follows them into the parts), both ends at the
pivot. A loose steel plate (6 m of 12.5 cm steel section) on two supports with 15 t set on its
middle - half as much again as its section holds - yields and holds, bent a hundredth of a
radian; with 30 t it folds on its hinge and tears at its rotation capacity, letting the block
through; without hinges it snaps at once. For a piece to feel that bending at all, its stress
check spreads each partner's contacts on its own (`spread_per_partner`; [`V2_DESIGN.md`](V2_DESIGN.md) §5).

## 4. Crumpling

**Crush patches** (`core/src/phys/rigid.cpp`). The contacts of a body pair (or a body and a
static grid) are grouped per grid and facing into patches. For each patch a frontal voxel scan
finds the area pressed and the pressure its materials can take; the patch's force is capped at
`crush x area` of the softer side, spread over its contacts. A crushing contact does no position
correction: the bodies keep closing - a crumpling front goes *into* the wall - and the
collision's energy goes out over the distance it folds. What it hits feels that capped force,
not the rigid spike of a body stopped in one substep, so a crash loads a building like a crash
and breaks what it should.

**The crumple pass** (`World::crumple`, after each substep). The crushing side folds out of
what it hit: along its lattice axis nearest the push, column by column, each column whose front
is pressed in is pushed back until it is out; its neighbours are dragged along a cell less per
column (a dent has sloped sides: the panel around it bends, it does not shear). A column's front
moves back as a whole where there is room behind it (a panel over a hollow); where there is not
(a run of material along the push), what does not fit folds out sideways and upwards (crumpled
metal piles up in folds), or is compacted. When both sides crumple, each folds half the overlap.
Brittle, nearly free-to-crack material near a fold (glass) shatters (dust events).

With the game's sheet metal (90 kPa over a car's frontal area: a few hundred kN, some 20 g), a
1.2 t car at 50 km/h folds some 0.45 m of its front against a wall, over tens of milliseconds.

**Punch-through** (`World::punch`). A static structure pressed harder than it can hold around
the patch - its punching capacity, the patch's perimeter x the wall's thickness x its tensile
and cohesive strength - is broken through: the fragments in the body's way, through the wall's
thickness, become rubble thrown ahead with its speed, and the body slows as they take up their
share of its momentum. A crumpling front presses at a few hundred kN: glass gives way to it, a
brick wall to its stiffest part (an engine block) once the front has folded back to it,
reinforced concrete to neither. Slower failure - a wall bending over, its bonds overloaded - is
the structure's own solve. `WorldStats::punches` counts them.

**In-place edits** (`split_body(..., in_place)`, `refresh_in_place`). A crumpled piece keeps its
id, its place and its motion (its mass and contact samples are rebuilt); bits that no longer
hold on to it come off (dust, or pieces of their own). A piece that breaks in two keeps its id
on its largest part when it must stay identifiable: one on wheels, one kept by its host
(`keep_piece`), and one held by joints (`jointed_keep_identity`). Each changed piece is
announced once per tick (`PieceReshaped`: its host meshes it again). A crumpling piece is
checked for fracture on its own schedule (`crumple_check_gap` substeps apart while it crumples;
a first hit at once). `WorldStats::reshapes` counts the pieces folded.

## 5. Parts that come off

In a real crash the connections fail first: a door is torn off its hinges, a latch pops, a
mount shears. So a thing's detachable parts are **grids of their own** beside its body, held to
it by joints made before any of it comes loose (the joints hold on to their ends' voxels and go
with the pieces they become: [`MOTION.md`](MOTION.md) §1):

- **hinged parts** on a `Hinge` joint with a **latch** (`JointDesc::latch`, N m): latched, the
  hinge does not turn at all; the latch holds up to its strength about the axis and, knocked
  past it, gives way - what it could not hold passes on, and the part swings within its limits.
  A hinge turned past its stop, or pulled or twisted beyond its strength (what the latch held
  does not count), tears: the part is loose.
- **welded parts** on `Fixed` joints that shear beyond their strength (`break_force`,
  `break_torque`).

A part does not collide with its body while its joint holds (`JointDesc::collide = false`: a
door welded into its frame); once loose it is rubble like any piece - it collides with the body
it came off and is archived with its region.

**One latch, one hinge**: a second joint as the latch (a ball joint on the far edge of a door)
over-constrains the hinge: its stop and the ball's slop disagree by a millimetre, and the two
fight with growing impulses until something breaks. A latch on the hinge itself - its free turn
held as a stop until the torque passes the latch's strength - does not.

**Sleep**: a body at rest sleeps with its parts; a body woken (driven, hit, woken by one moving
near) wakes what is joined to it and what is joined to that (`RigidWorld::wake_jointed`, each
substep and in the solve) - a sleeping body is a static support to the solver, and a body woken
still would hang its weight on its sleeping parts. A driven wheel keeps the body it hangs from
awake before joined bodies' stillness is shared (`wheel_stillness`, then `joint_stillness`).

The registry needs nothing more: a body's parts are the pieces joined to it
(`World::joined_pieces`). A body archived out of range goes with its parts as one joint group,
and comes back with them, latches and all (sessions and the archive keep the joints' latches).

## 6. Tests

- `tests/core/test_crumple.cpp`: a crate-like car of sheet, frame and engine against a
  reinforced-concrete wall at 50 km/h (its front folds, the wall stands), faster folds further,
  at speed it goes through masonry where concrete stops it; a head-on crash is deterministic. A
  pistol's round holes sheet metal and neither steel section nor solid steel, a rocket's energy
  density holes the section too and never solid steel, a cut takes all three; with
  `impact_penetration` off, no impact removes ductile material.
- `tests/core/test_joints.cpp`: a steel arm bent past its strength folds down on a plastic
  hinge and tears off only once turned past its capacity; without hinges it snaps at once; a
  loose steel plate loaded past its strength between its supports yields on a hinge and holds,
  and under twice the load folds and tears; a latched door holds against a nudge, a hard knock
  opens its latch and it swings to its stop, and a session keeps its latch shut or open.
- `tests/core/test_wheels.cpp`: a jointed door sleeps and wakes with its body - woken still,
  the body does not hang its weight on its sleeping door; driven off, the door comes along.
- `tests/core/test_world.cpp`, `test_capi.cpp`: the material registry, a host's materials at
  its own ids (C++ and C).

## 7. Limits

- A plastic hinge forms where one breaks off: an impact must still pay for the cracks from the
  energy it takes out of the collision (steel's is large), so a blow bends a steel piece only
  when it has the energy to; a load resting on it is not limited.
- A part is held at one point of its seam (a door's two hinges are one hinge joint): a part
  crushed at that voxel comes off; one crushed elsewhere stays on, crumpled. Parts do not
  collide with their body while they hold: a door pushed in by a crash transfers the push
  through its hinge until it gives way.
- Crumpling folds along lattice axes: a side impact folds a side in, a frontal one the front; a
  very oblique blow folds along the axis nearest to it.
- Two crumpled bodies folded into each other can stay hooked (one that rode up onto the side it
  hit, its nose among the other's folds, may not back out), as real wrecks wedged in a crash.
