# Arm control and contact continuity

The character foundry consumes these changes through its engine submodule. The
same `svx_anim` and `svx_core` code runs in the game and the WebAssembly preview.

## Living arms

`anim/src/body/humanoid.cpp` sets shoulder, elbow and wrist muscle frequencies to
16, 19 and 28 rad/s, with damping ratios 1.05, 1.05 and 1.15. The faster distal
response lets the wrist carry the hand through a turn or a shove.

`Behaviours::drive_pre` recruits support for raised hands, action effort and chest
turns. Recruitment rises at 24/s and releases at 4/s. The default resting arm tone
is 0.82. Regional injury, daze, shock and per-part stun still reduce this drive;
dead muscles still have zero stiffness.

`MotionPlan` smooths an outgoing action's local arm rotations with a critically
damped 10 rad/s return, retaining angular velocity when release starts. New
actions retain their authored timing. Behaviour arm tasks run afterwards so a
brace, wound hold or fall response can take over immediately. Weapon holds also
take priority. Placement clears the return state.

## Feet and walking

The gait table increases walking cadence without changing the running cadence
above 3 m/s. Both swing and stance use the same rolled sole-to-ankle transform.
Swing starts at the actual lift pitch, landing starts the contact roll clock, and
terrain clearance tapers toward touchdown. The early-swing hip reach clamp fades
before landing so the pelvis can accommodate the approaching foot.

The pelvis reserves 1.5% of leg length and the leg IK soft region is 0.5%. This
avoids pulling a reachable planted foot off its intended contact just to soften
the knee. A planted foot remains at its world position as the body advances.

Locomotion's physical foot attachment moves between the heel and ball of the foot.
Balance recovery continues to use its ankle support. `ArticulationControl` has an
optional `target_local` vector for changing a point target's body-local anchor.
The deep binding forwards it each tick. Changed anchors clear the accumulated
target impulses; invalid values are ignored. Session records store the current
anchor using the existing target record field, so the record format is unchanged.

The approach is informed by the contact continuity and soft extension discussion
in [Inverse Kinematics and Foot Locking](https://theorangeduck.com/page/inverse-kinematics-foot-locking).
This implementation uses the existing procedural gait and physical contacts.

### Standing and travelling stance

`GaitStyle::width` and `toe_out` describe standing. The new `move_width` is a
multiplier of that standing width, clamped to 0.60–1.50; `move_toe_out` is the
outward angle of each moving foot in radians, clamped to 0–20 degrees. Neutral
defaults are 0.75 and `2 * kDeg`. On the standard rig this reduces the distance
between the foot centres from 20 cm standing to 15 cm walking or running.
A wide swagger can use 1.20 and `12 * kDeg`. Existing seeded civilian, soldier
and thug styles retain different movement profiles.

The planner blends the travelling layout over 0.12–0.65 m/s and smooths targets
with critically damped 8 rad/s springs. It changes swing destinations, preserving
planted positions and headings. Stopping takes corrective steps back to the
standing layout; it then restores the usual turning dead zone. Placement clears
the smoothing and settling state. Guarded combat uses the standing spread and
toe angle; forced balance steps retain the controller's placement rules.

Narrower steps also exposed boot edges landing across rubble. Tread fitting and
swing clearance now sample the boot's width as well as its centre line.

## Regression checks

Build `svx_anim_tests` and `svx_core_tests` with `SVX_BUILD_ANIM=ON` and
`BUILD_TESTING=ON`, then run:

```sh
./build/tests/svx_anim_tests
./build/tests/svx_core_tests --test-case='articulations:*'
```

`tests/anim/test_motion_quality.cpp` checks post-action wrist tracking, mirrored
actions, rapid turns, pushes, stun/death, and rolling contacts. Planned gait
contacts are checked at eight speeds from 0.35 to 4 m/s and three height scales
from 0.8 to 1.15. Physical contact tests exercise both solvers. The articulation
test moves an attachment, rejects invalid input and restores it through a saved
session. Existing terrain, combat and recovery tests remain part of the suite.

`tests/anim/test_motion.cpp` also checks travel width and toe angles at five
speeds and three body scales, live edits without moving planted feet, quiet
standing after stopping, guarded movement and clearance beside a boot.

The foundry's `npm run engine:measure` records wrist tracking, hand speed, contact
error, touchdown motion and pelvis height range on both backends. Physical contact
is compliant; exact agreement between simulated and planned poses is not expected.
