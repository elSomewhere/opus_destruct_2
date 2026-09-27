# svx_anim: voxel characters for structvox

`anim/` holds **svx_anim**, an animation engine for voxel characters: soldiers and civilians
built from voxels, animated procedurally, that bleed, lose limbs, turn into ragdolls and gibs.
It has a smooth modern presentation and a retro one (Voxel Doom style frames stepped in Doom
tics) that is computed from the modern animation.

Like the physics core (`svx_core`, [CORE.md](CORE.md)), it is a library with no knowledge of
the game:

- no physics engine;
- no renderer;
- no engine protocol;
- no DOM.

The game harness (`web/`) is one host of it. The two layers never meet inside svx_anim.

```
anim/                 svx_anim (TypeScript, zero dependencies, runs in node, browsers, workers)
  math/               vectors, quaternions, matrices, seeded random and noise
  core/               Skeleton, Pose, WorldPose, two-bone IK, springs
  locomotion/         gait parameters (walk -> run by speed, crouch)
  humanoid/           the humanoid rig, the HumanoidAnimator, the humanoid ragdoll layout
  voxel/              VoxelModel, sculpting (SDF shapes), meshing, damage (hits, wounds, severing)
  physics/            CollisionWorld (the only view of the world), particle ragdoll, gibs and blood
  retro/              re-voxelized frames of poses, Doom-style state sequences and playback
  characters/         procedural soldiers and civilians (many looks per geometry), weapons
  character.ts        Character: model + animator + ragdoll + wounds + retro, what hosts drive
web/src/render/characters.ts + shaders/character.wgsl   WebGPU drawing of svx_anim meshes
web/src/actors/       the game's actors: AI, navigation, combat, gore, crowds (the harness side)
web/src/lab/          the animation lab (lab.html): svx_anim without the physics engine
```

## 1. Conventions

- **Units and axes.** Metres. Model space is +x right, +y forward, +z up, with the origin on the
  ground between the feet. World space is the engine's (z up). Facing yaw 0 is +x.
- **Rest pose.** Every bone's rest rotation is the identity, so joint frames are aligned with
  model space. A positive x rotation swings a hanging limb forward and tips an upright bone
  backwards.
- **Voxel lattice.** A model's voxels sit on a lattice of pitch `s`; the default is 1/32 m,
  which is Doom's texel scale in this engine. Cell (i, j, k) is centred at
  ((i+½)s, (j+½)s, (k+½)s) in rest model space. The world's voxels are 1/8 m.
- **Palettes.** Voxels store a palette slot (skin, hair, top, top2, bottom, bottom2, shoes, gear,
  gearDark, metal, furniture, detail, accent, flesh, bone, blood) and a shade byte. Colours come
  from each instance's palette, so one geometry serves many looks (Doom's colour translation).
  Flesh and bone voxels are inside the body and only show through wounds.
- **Character vertex (20 bytes)**, the only contract with a renderer
  (`anim/src/voxel/mesh.ts`):

  | offset | type | meaning |
  |---|---|---|
  | 0 | float32x3 | position, rest model space |
  | 12 | snorm8x4 | normal xyz, w = ambient occlusion |
  | 16 | uint32 | bone (bits 0–7), palette slot (8–11), shade (12–19; 128 = 1.0) |

  A vertex is drawn at `skin[bone] × position`. This is rigid skinning, so voxels stay cubes.
  Skin matrices come from `WorldPose.writeSkin`: 16 floats per bone, column-major.

## 2. The world, as svx_anim sees it

svx_anim asks a host for three things (`physics/collision.ts`):

```ts
interface CollisionWorld {
  groundHeight(x, y, zTop, zBottom): number | null;   // feet
  sphere(center, r, out): boolean;                      // ragdolls, gibs, blood (push-out + normal)
  raycast(origin, dir, maxDist): number;                // blood drops, line of sight
}
```

`VoxelCollision(h, solid(i, j, k))` implements it over any voxel occupancy. The game uses the
engine's streamed chunk occupancy, the same bits the player collides with, so characters walk on
what the physics leaves standing. `FlatGround` is for tests and empty scenes.

## 3. Animation

`HumanoidAnimator` animates the 23-bone humanoid rig procedurally from a few inputs. The
character itself is moved by the host, with its own collision, via `setRoot(pos, yaw)`; the
animation follows.

| input | effect |
|---|---|
| `crouch` 0..1 | knees bend and the stance widens; crouch-walk takes short strides |
| `carry` relaxed / ready / aim | the rifle hangs across the body, is at low ready, or is shouldered on the aim line; port arms while running |
| `aimAt` | the trunk turns and pitches to the target, spread over spine, chest and neck, into a bladed stance; the rifle points exactly at it |
| `lookAt` | head and eyes turn; when idle, the head looks around by itself |
| `mood` normal / panic / cower / surrender | hands to the head while fleeing, crouched with hands over the head, hands up |
| `airborne` | legs tuck; on landing the feet plant and the body dips |
| `fire()`, `hit(dir, strength, height)` | recoil springs; flinches on angular springs in the trunk and head, plus a stagger |

**Locomotion** is a foot planter driven by a gait clock (`locomotion/gait.ts`):

- **Walk to run.** The walk (an inverted pendulum with double support) blends into a run (a
  spring-mass gait with a flight phase) between 2 and 3 m/s. Cadence, stride, stance fraction,
  lift, bob, sway, hip yaw and roll, lean and arm swing follow normal-gait data.
- **Planting.** A foot in stance stays where it landed in world space, so feet never slide at
  any speed, direction or turn rate. A swinging foot lands where the hip will be at mid-stance
  of the next step, on the ground found by `groundHeight`. That handles stairs, rubble and
  craters.
- **Foot roll.** Heel strike and toe-off roll the foot about the heel and the ball.
- **Standing.** Standing characters take corrective steps when they turn or drift. A foot left
  behind by a sudden start or a shove takes a quick catch-up step.
- **Legs.** Two-bone IK to the ankles, with the knees towards the feet. The pelvis sinks just
  enough for the planted feet to be reached.

**Weapons** are separate one-bone prop models (`characters/props.ts`) placed at the rig's
`weapon` socket. Both hands are put on the prop by IK (grip and handguard).

## 4. Voxel characters and gore

`characters/humans.ts` sculpts humans from signed-distance shapes:

- **Build:** a body with flesh and bone inside, then dressed in trousers or shorts, shirts,
  jackets, hoodies, uniforms, shoes or boots, hair, helmets, caps, berets, balaclavas, plate
  carriers, belts, backpacks, knee pads and camouflage.
- **Soldier looks:** four camouflage schemes.
- **Civilian looks:** varied builds, clothes and hair.
- **Joints:** joint balls are copied into both parts that meet there, so rotating limbs overlap
  instead of opening gaps.

Damage (`voxel/damage.ts`, `character.ts`):

- **Hits.** `raycast` returns the voxel hit on the posed model, about 2 µs per ray.
- **Wounds.** `wound(hit, dir, damage)`:
  - carves an entry and an exit hole, exposing flesh and bone;
  - applies damage with hit-zone multipliers (head ×4, neck ×3, chest, spine, pelvis, limbs
    ×0.6);
  - flinches the living, or pushes the dead;
  - severs limbs and heads whose part was shot through or lost about half its voxels. The parts
    below go with them, so a severed forearm takes the hand.
- **Blasts.** `blast(center, radius)` does distance-scaled damage. Close to the centre the body
  is torn apart: every part becomes a gib.
- **Copy on write.** Models are shared between characters until the first wound, then copied.
  `geometryVersion` tells the host to re-mesh; only the parts that changed are re-meshed.

## 5. Ragdolls, gibs, blood

- **`Ragdoll`** (`physics/ragdoll.ts`) is a position-based particle solver with Verlet
  integration and fixed substeps. It has:
  - distance, limit and hinge constraints (knees and elbows bend one way);
  - sphere contacts against the CollisionWorld, with friction and a contact skin;
  - sleep, triggered when nothing moved more than 1.5 cm for half a second.
- **`HumanoidRagdoll`** maps the rig onto 21 particles:
  - it starts from the animated pose and its velocity, so a character shot mid-stride keeps its
    momentum;
  - it keeps muscle tone for a moment (soft targets that fade out), so the body slumps instead
    of dropping;
  - every frame it rebuilds all bone frames from the particles.
- **`GibSystem`** (`physics/debris.ts`) handles gibs, blood drops and stains:
  - **Gibs** (severed limbs, heads, torso chunks, dropped rifles) are rigid voxel bodies with box
    inertia. About 64 surface sample points collide as spheres, using sequential impulses with
    restitution and friction, adaptive substeps and sleep.
  - **Blood drops** fly ballistically and become stains where they land, on floors and walls.
    Gibs bleed while they move.

## 6. Retro presentation

The retro look is a function of the modern animation, not separate content (`retro/`):

- **Baking.** For each state (idle, walk, run, crouch, crouchWalk, aim, fire, pain, panic, cower,
  surrender), a HumanoidAnimator is driven on flat ground until steady. A handful of poses are
  sampled; walk and run get four frames per cycle, Doom's A B C D. Each pose is re-voxelized
  into one whole-body voxel model by inverse mapping of every cell to its source part (no holes),
  with the held rifle included. These are Voxel Doom's one-model-per-sprite-frame, generated. A
  full set with 26 frames bakes in about 57 ms at 1/32 m and about 28 ms at 1/16 m ("chunky").
- **Playback.** `RetroPlayer` steps frames in Doom tics (35 Hz, several tics per frame), scaled
  by speed for gait cycles. The facing snaps to 8 directions.
- **Everything else.** Ragdolls, gibs and wounded characters (baked frames show no wounds) keep
  their real geometry, but their poses advance in the same 4-tic steps.

## 7. In the game (`web/src/actors`)

`ActorWorld` runs soldiers and civilians in the engine's worlds.

- **Population.** When a world loads, characters are placed on standable ground around the
  player:
  - city: 18 civilians, 8 soldiers;
  - tower: 12 civilians, 6 soldiers;
  - rooms: 5 civilians, 3 soldiers;
  - Doom maps: 8 civilians, 6 soldiers.

  `?civilians=N&soldiers=M` overrides the counts and `?actors=0` places none. The settings
  panel's Characters section adds more and clears them.
- **Navigation** (`nav.ts`) is A* over 0.25 m cells found on the fly from the occupancy (ground
  within a step, room above), with string-pulled paths. It is recomputed as the world breaks, at
  a budgeted number of searches per frame.
- **Soldiers** (`brain.ts`):
  - patrol their post;
  - gunfire, impacts, screams, shouts and bodies alert them and send them to look;
  - once they see the player they react, keep a combat range (advance, back off, strafe,
    crouch), and fire bursts whose spread tightens while they hold their aim;
  - they don't fire with a friend in the line;
  - they hunt the last known position when they lose sight of the player.
- **Civilians:**
  - wander and look around;
  - fear builds from what they hear;
  - frightened, they run from the danger, panicking when very scared and screaming, which
    spreads the fear;
  - they cower next to explosions and put their hands up when the player aims at them from
    close by.
- **Combat.** A round (theirs or the player's) hits the first thing on its line:
  - a character, voxel-exact (a wound);
  - the player's capsule (health, a red vignette, respawn after death);
  - or the world, where it sends `carve` to the engine.

  The battle destroys the level: a 25 s firefight in the city broke about 7,000 bonds and brought
  down storeys. Rockets tear characters apart and throw bodies. Falling rigid debris crushes
  characters, and a long fall kills.
- **Drawing.** Characters, rifles, gibs, blood drops (instanced cubes), stains, blood pools and
  blob shadows (decals) are drawn in the main pass with the world's lighting: hemisphere, sun,
  sector light, fog and muzzle flashes.

Controls and parameters, in addition to the game's:

| | |
|---|---|
| `?civilians=N&soldiers=M`, `?actors=0` | initial population |
| `?anim=smooth\|retro\|retro-chunky` | animation presentation (also in the panel) |
| `?god=1` | soldiers' rounds don't hurt the player (also in the panel) |
| panel "Characters" | +6 civilians, +4 soldiers, clear, animation style, AI on/off, god mode |
| `window.__structvox` | `spawn`, `spawnAt`, `actors`, `characters({ai, god, style})`, `actorWorld()` |

**The lab** (`/lab.html`) runs svx_anim without the physics engine: a test course (stairs,
ramp, rubble, cover), an orbit camera and a scripted cast. The cast covers patrol, sprint,
strafe-and-fire, crouch-walk, stairs, panic, cower, surrender, turning on the spot, and
aim-tracking. In the lab:

- click a character to shoot it;
- shift-click the ground to fire a rocket;
- use the controls to switch smooth / retro / chunky and change time scale (slow motion).

## 8. Using svx_anim in another host

```ts
import { Character, VoxelCollision, makeSoldier, makeRifle, ModelMesher, GibSystem } from 'svx-anim';

const world = new VoxelCollision(0.125, (i, j, k) => myVoxels.solid(i, j, k));
const look = makeSoldier(7);                        // geometry + palette
const soldier = new Character({ model: look.model, palette: look.palette, collision: world, weapon: makeRifle() });
soldier.place([x, y, z], yaw);
const mesh = new ModelMesher().mesh(soldier.model); // upload once (20-byte vertices)

// every frame
soldier.setRoot(myController.feet, myController.yaw);   // the host moves it
soldier.animator.input.carry = 'aim';
soldier.animator.input.aimAt = target;
soldier.update(dt);
draw(mesh, soldier.skin /* 23 bone matrices */, look.palette);

// a bullet
const hit = soldier.raycast(origin, dir, 200);
if (hit) for (const g of soldier.wound(hit, dir, 34).gibs) gibs.spawn(g.part, g.voxelSize, g.bonePos, g.boneRot, g.boneRestHead, g.vel, g.ang);
```

## 9. Tests and checks

```bash
cd anim && npm install && npm test          # 22 unit tests: models, animator, IK, ragdoll, damage, gibs, retro, Character
cd web && npm run typecheck && npm test     # includes typechecking anim/
node scripts/smoke-actors.mjs http://localhost:5190/   # browser: population, fighting, kills, gibs, retro, MAP01, lab
```

Measured on an M5 Pro:

- **Browser (Chrome, WebGPU):** 65 characters fighting in the streamed city run at 57–60 fps.
- **Node:** a character's animation update (gait, IK, weapon, skin matrices) costs about 11 µs
  per frame. A human model sculpts in about 13 ms and meshes in about 8 ms (~10k triangles).
  A retro set bakes in 30–60 ms, one per frame when retro mode is turned on.

## 10. Decoupling and merging

- svx_anim depends on nothing, and nothing in `core/`, `mesh/` or `game/` (C++) was changed. The
  physics branch can merge with this one without conflicts in the engine.
- On the web side, the characters touch the harness only in a few places:
  - `game/game.ts`: wiring, player health, population;
  - `game/weapons.ts`: `ShotTargets`, so rounds and rockets can hit characters;
  - `render/renderer.ts`: the character pass;
  - `ui/hud.ts`: health and counters.
- A different renderer needs only the 20-byte vertex format and skin matrices. A different world
  needs only a CollisionWorld.

## 11. Limits and next steps

- **Sector light.** Characters use one light level per world, not the sector light of where they
  stand (Doom maps are drawn at 0.8).
- **Movement.** Actors move on one level. They do not climb between floors without walkable
  connections, and nav paths do not use doors or lifts.
- **Hits.** Rounds hit the static world via occupancy, not the rigid debris pieces. Characters
  are not obstacles for the engine's pieces. Debris crushes characters, but it doesn't bury
  them.
- **Ragdoll collisions.** Ragdolls collide with the world but not with each other.
- **Retro.** Frames are baked per geometry and weapon state. Deaths in retro mode are stepped
  ragdolls, not baked death sequences. A sprite-billboard mode, rendering the baked frames from 8
  angles into sprites, would be the next step towards classic Doom.
- **More content** would come as more rigs (quadrupeds, Doom-like monsters) on the same core: the
  foot planter, IK, ragdoll and retro baking are rig-agnostic. Keyframed clips, such as mocap
  imported as local rotations, can be layered through `Pose.blend` and additive rotations.
