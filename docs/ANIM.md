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
  core/               Skeleton, Pose, WorldPose, two-bone IK, springs, keyframe curves
  locomotion/         gait parameters (walk -> run by speed, crouch)
  humanoid/           the humanoid rig, the HumanoidAnimator, gait styles, stances, actions,
                      hit reactions, fight choreography (Brawler), the humanoid ragdoll layout
  voxel/              VoxelModel, sculpting (SDF shapes), meshing, damage (hits, wounds, severing)
  physics/            CollisionWorld (the only view of the world), particle ragdoll, gibs and blood
  retro/              re-voxelized frames of poses, Doom-style state sequences and playback
  characters/         procedural soldiers and civilians (many looks per geometry), weapons,
                      furniture (benches, chairs, desks)
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
animation follows. Everything below is the smooth baseline; the retro presentation is baked
from it (section 6), so every improvement here shows in both.

| input | effect |
|---|---|
| `crouch` 0..1 | knees bend and the stance widens; crouch-walk takes short, careful strides |
| `stance` | `stand`, `kneel`, `prone` (crawls when moving), `sit` (on `seat`), `ground` (`groundVariant`: cross-legged, knees up, legs out), `down` (knocked down); transitions route through a stance graph (prone goes via kneeling) and take real time |
| `seat` | `{pos, backrest, deskHeight?, variant}`: upright, leaning back with an arm along the backrest, legs crossed, elbows on knees, or at a desk typing |
| `carry` | relaxed / ready / aim / hip; long guns and pistols have their own holds (below) |
| `aimAt` | the trunk turns and pitches to the target, spread over spine, chest and neck, bladed for long guns; the muzzle points exactly at it |
| `lookAt` | head and eyes turn to a point of interest (while walking too); idle heads look around |
| `lean` -1..1 | peeking round a corner: the trunk leans out sideways, the head stays upright |
| `talk` | `speak` (gestures, nods, head movement) or `listen` (nods, weight shifts, a hand on the chin) |
| `guard` | fighting stance: bladed, hands up, bouncing on the balls of the feet |
| `mood` | normal / panic (hands to the head while fleeing) / cower / surrender (hands up) |
| `airborne`, `idle` | legs tuck, landing dip; idle poses and fidgets come by themselves when idle |

Calls: `play(name, target?)` (one-shot actions), `fire()` (recoil), `hitAt({point, dir, force,
kind, bone})` (a hit reaction, returns the zone), `knockDown(back, seconds?)`,
`takeKnockback(dt)` (hosts move the root by it), `takeEvents()` (strikes landing, reloads
done).

**Locomotion** is a foot planter driven by a gait clock (`locomotion/gait.ts`):

- **Walk to run.** The walk (an inverted pendulum with double support) blends into a run (a
  spring-mass gait with a flight phase) between 2 and 3 m/s. Cadence, stride, stance fraction,
  lift, bob, sway, hip yaw and roll, lean and arm swing follow normal-gait data.
- **Personal style** (`humanoid/style.ts`, `GaitStyle`): stride, bounce, sway, arm swing, elbow
  bend, posture (upright or slouched), stance width, toe-out, heaviness, head stillness and
  fidgeting, drawn per character from archetypes (`randomStyle(seed, 'soldier' | 'civilian' |
  'civilianFemale')`), so a crowd doesn't walk in step.
- **Weight.** Footfalls load the body (the pelvis dips and the head and neck lag on springs);
  the trunk leans into acceleration with overshoot and banks into turns; soldiers aiming move
  in a rolling tactical walk; a wounded leg limps.
- **Planting.** A foot in stance stays where it landed in world space, so feet never slide at
  any speed, direction or turn rate. A swinging foot lands where the hip will be at mid-stance
  of the next step, on the ground found by `groundHeight`. That handles stairs, rubble and
  craters.
- **Foot roll.** Heel strike and toe-off roll the foot about the heel and the ball.
- **Standing.** Standing characters take corrective steps when they turn or drift. A foot left
  behind by a sudden start or a shove takes a quick catch-up step.
- **Legs.** Two-bone IK to the ankles, with the knees towards the feet. The pelvis sinks just
  enough for the planted feet to be reached.

**Actions** (`humanoid/actions.ts`) are short keyframed clips of channels (`core/curve.ts`:
Hermite tracks with eases) that steer the IK rather than replace it: hand and foot targets,
strike weights towards a target, elbow poles, trunk, head, pelvis, crouch, weapon offsets, and
events. A pose layer holds postures (guard, talking, idle poses); the action layer plays
one-shots on top and blends out. Mirrored versions (`.m`) are generated.

- **Strikes:** jab, cross, hook, uppercut, front kick, roundhouse; knife stab and slash; a rifle
  butt push. Targeted strikes step in to a target out of reach: the pelvis drives forward with
  the blow, a punch leans the trunk into it, a kick thrusts the hips and meets the target with
  the ball of the foot (push kick) or the instep (roundhouse).
- **Defence and weapons:** block, rifle and pistol reloads (with a `reloaded` event).
- **Everyday:** idle poses (arms crossed, hands in pockets, on the hips, behind the back,
  folded, looking at a phone), fidgets (checking the watch, scratching the head, stretching,
  rubbing the neck), waves, conversation gestures (open hands, pointing, shrugging, hand on
  the chest), nods, head shakes, laughing.

**Weapons** are separate one-bone prop models (`characters/props.ts`: rifle, SMG, light machine
gun, pistol, knife; small props on a 1/64 m lattice) placed at the rig's `weapon` socket, with
both hands put on them by IK:

- **Long guns:** relaxed (slung across the body), low ready, shouldered on the aim line (bladed
  stance), from the hip (stock under the arm, the trunk turned off the target so the support
  hand reaches the handguard; machine guns on the move), port arms when running. Recoil kicks
  back and up; walking fire is less precise.
- **Pistols:** lowered in the hand, low ready (two hands, muzzle down), two-handed at eye level
  (isosceles), or one-handed with the arm out.
- **Knife:** held in the right hand, blade forward; used by the stab and slash strikes.

**Hit reactions** (`humanoid/reactions.ts`) follow where and how hard a blow lands (`hitAt`
with a point, a direction, a force and a kind: bullet, blunt, blade, blast):

- The impulse's torque about the struck bone's joint (r × dir) turns damped springs of that
  bone and, weaker, the bones it hangs from, so the body answers the physics of the hit.
- Reflexes by zone: a head hit snaps the head (a cross to the jaw about 40°); a chest round
  rocks the trunk back about 20–25° with the head lagging; a gut hit doubles the body over and
  the hands go to the wound; an arm hit spins the shoulder and knocks the aim off; a leg hit
  drops the pelvis over the buckling knee and leaves a limp.
- Knockback: the body is shoved along the blow (the host moves the root; the feet stumble
  after it). Hard blows (a roundhouse to the head, blasts, heavy rounds in the legs) knock the
  body down; it falls backwards or forwards, lies a moment, and gets up through kneeling.
- Pain hunches the posture and slows the gait for a while.

**Fights** (`humanoid/brawl.ts`, `Brawler`) choreograph two characters: keeping range and
circling, choosing strikes by distance, combinations (jab, cross), blocking or stepping back
from what the opponent throws, backing off a downed opponent, and resolving each strike event
into a hit where the fist, foot or blade met the body (`Character.melee`, with the damage and
reaction by zone). Fists and feet knock people out rather than kill them (`knockedOut`: down
for several seconds); knives cut the voxel body open and can kill.

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
  - makes the living react by where the round hit (section 3), or pushes the dead;
  - severs limbs and heads whose part was shot through or lost about half its voxels. The parts
    below go with them, so a severed forearm takes the hand.
- **Melee.** `melee(point, dir, 'blunt' | 'blade', force)`: punches and kicks hurt by zone
  (the head most) and knock out rather than kill; a knife cuts a slice of the voxel body open
  and can sever and kill.
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

- **Baking.** For each state, a HumanoidAnimator is driven on flat ground until steady and a
  handful of poses are sampled; walk and run get four frames per cycle, Doom's A B C D. Each
  pose is re-voxelized into one whole-body voxel model by inverse mapping of every cell to its
  source part (no holes), with the held prop included. These are Voxel Doom's
  one-model-per-sprite-frame, generated.
- **States** (33): idle, walk, run, crouch, crouchWalk, aim, fire, pain, panic, cower,
  surrender; talk, listen; sit, sitDesk, ground; kneel, kneelFire, prone, proneFire, crawl,
  peek; guard, punch, kick, stab, block; reload, hipFire, pistolAim, pistolFire; down, getUp.
  Only the states that fit what the character holds are baked (a pistol set has pistolAim,
  a rifle set hipFire; unarmed sets have guard and block); missing states fall back to similar
  ones. A soldier's rifle set (28 states, 67 frames) bakes in about 160–200 ms at 1/32 m,
  a civilian's in about 50 ms; the game bakes one set per frame, on demand.
- **Playback.** `retroStateOf` maps what the character does (stance, action, conversation,
  weapon, knockdown...) to a state; `RetroPlayer` steps frames in Doom tics (35 Hz, several
  tics per frame), scaled by speed for gait cycles. The facing snaps to 8 directions.
- **Everything else.** Ragdolls, gibs and wounded characters (baked frames show no wounds) keep
  their real geometry, but their poses advance in the same 4-tic steps.

## 7. In the game (`web/src/actors`)

`ActorWorld` runs soldiers and civilians in the engine's worlds.

- **Population.** When a world loads, characters are placed on standable ground around the
  player, with benches for the civilians:
  - city: 18 civilians, 8 soldiers;
  - tower: 12 civilians, 6 soldiers;
  - rooms: 5 civilians, 3 soldiers;
  - Doom maps: 8 civilians, 6 soldiers.

  `?civilians=N&soldiers=M` overrides the counts and `?actors=0` places none. The settings
  panel's Characters section adds more and clears them.
- **Looks and loadouts** (`cast.ts`): every character gets a geometry, a palette and a
  personal gait style. Soldiers carry a rifle (55%), an SMG, a light machine gun or a pistol;
  about one civilian in eight carries a pistol, a few an SMG or a knife (kept out of sight until
  needed). Weapons differ in damage, rate of fire, spread, magazine size and how much of the
  world a round carves.
- **Navigation** (`nav.ts`) is A* over 0.25 m cells found on the fly from the occupancy (ground
  within a step, room above), with string-pulled paths. A straight walkable line needs no
  search. Ground queries are cached across searches and forgotten per chunk column when the
  world changes; searches are budgeted per frame.
- **Civilians** (`brain.ts`) live a day in the city:
  - they stroll at their own pace, wait around (idle poses, fidgets, a glance at the phone),
    jog now and then, and look at what catches the eye while walking (people passing, the
    player, a fight);
  - they walk up to each other and talk, taking turns speaking and listening, facing each
    other at conversational distance; now and then a conversation turns into a fist fight (or
    a knife fight);
  - they sit on benches (walk up, turn, sit down, sit in their own way, get up and leave) and
    on the ground;
  - a fight draws a crowd: passers-by stop and watch, sitters and talkers look over; the winner
    walks off, the loser gets away once back on their feet, and a killer runs;
  - fear builds from gunfire, impacts, screams and bodies; frightened, they run, panicking and
    screaming when very scared, which spreads the fear. They cower next to explosions and put
    their hands up when the player aims at them from close by (not while the player
    spectates in noclip). Armed civilians may draw and shoot back at soldiers firing nearby
    (then soldiers treat them as hostile).
- **Soldiers:**
  - patrol their post; gunfire, impacts, screams, shouts and bodies alert them and send them
    to look;
  - once they see the player (or a hostile) they react, keep a combat range (advance, back
    off, strafe), and fight from positions: standing, kneeling, prone, crouched behind cover
    popping up, or leaning out from a corner to fire;
  - they fire bursts whose spread tightens while they hold their aim, wider when firing on the
    move or from the hip; machine gunners fire from the hip on the move; they reload (the
    magazine refills when the reload action says so) and don't fire with a friend in the line;
  - at arm's length they hit with the rifle butt (or punch, holding a pistol); they hunt the
    last known position when they lose sight of their target.
- **Combat.** A round (theirs or the player's) hits the first thing on its line:
  - a character, voxel-exact: a wound, and a reaction by where it hit;
  - the player's capsule (health, a red vignette, respawn after death);
  - or the world, where it sends `carve` to the engine (about one world voxel per round).

  The battle destroys the level: rounds break bonds and bring down storeys. Rockets tear
  characters apart and throw bodies. Falling rigid debris crushes characters, and a long fall
  kills. Blows push the player.
- **Drawing.** Characters, their weapons, benches, gibs, blood drops (instanced cubes), stains,
  blood pools and blob shadows (decals) are drawn in the main pass with the world's lighting:
  hemisphere, sun, sector light, fog and muzzle flashes.

Controls and parameters, in addition to the game's:

| | |
|---|---|
| `?civilians=N&soldiers=M`, `?actors=0` | initial population |
| `?anim=smooth\|retro\|retro-chunky` | animation presentation (also in the panel) |
| `?god=1` | soldiers' rounds don't hurt the player (also in the panel) |
| panel "Characters" | +6 civilians, +4 soldiers, clear, animation style, AI on/off, god mode |
| `window.__structvox` | `spawn`, `spawnAt`, `actors`, `characters({ai, god, style})`, `actorWorld()`, `brawl(idA, idB)` |

**The lab** (`/lab.html`) runs svx_anim without the physics engine: a test course (stairs,
ramp, rubble, a wall corner, benches, a desk) and a scripted cast in four groups, each with a
camera bookmark:

- **city:** a three-way conversation, bench sitters in four styles, a desk worker, ground
  sitters, people waiting (idle poses, fidgets), a stroller, a commuter, a jogger, a runner,
  stairs;
- **soldiers:** kneeling fire, prone, crawling, peeking round the corner, pistol walk-and-fire,
  one-handed pistol, machine gun from the hip, reloading, crouch-walk, patrol, sprint;
- **fights:** two brawlers, a knife against an unarmed fighter (they reset after a while);
- **reactions:** a line-up to shoot at.

Click a character to shoot it, shift-click the ground to fire a rocket; switch smooth / retro
/ chunky, change time scale (slow motion) or follow a character.

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

// a bullet: a wound and a reaction by where it hit; move the root by the knockback
const hit = soldier.raycast(origin, dir, 200);
if (hit) for (const g of soldier.wound(hit, dir, 34).gibs) gibs.spawn(g.part, g.voxelSize, g.bonePos, g.boneRot, g.boneRestHead, g.vel, g.ang);
myController.feet = vadd(myController.feet, soldier.animator.takeKnockback(dt));

// a punch or a knife cut; strikes come from animator.play('jab', target) and its events
soldier.melee(point, dir, 'blunt', 1.2);

// everyday life
soldier.animator.input.stance = 'sit';
soldier.animator.input.seat = { pos: benchSeat, backrest: true, variant: 'leanBack' };
civilian.animator.input.talk = 'speak';
civilian.animator.input.lookAt = otherPerson.animator.eyes();
```

Hand-to-hand fights: give each fighter a `Brawler` (`opponent`), call `update(dt)`, move the
fighters by its `move` and `yaw`, and pass their animation events to `resolve`, which returns
the blows that landed.

## 9. Tests and checks

```bash
cd anim && npm install && npm test          # 54 unit tests: models, animator, IK, stances, actions,
                                            # reactions, weapons, strikes, brawls, ragdoll, damage, gibs, retro
cd web && npm run typecheck && npm test     # includes typechecking anim/
node scripts/smoke-actors.mjs http://localhost:5190/   # browser: population, fighting, kills, gibs, retro,
                                            # city life (talking, benches), a brawl ending in a knockout, MAP01, lab
```

`anim/test/behaviour.test.ts` checks the behaviours measurably, for example: stances reach
their heights and come back; a knockdown gets up; a chest round tips the trunk 10–35°; a gut
hit folds it; a leg hit drops the pelvis and limps on that side only; knockback follows the
shot; an aimed pistol points within 8° with both hands on the grip; the machine gun's support
hand is on the handguard; a jab steps in to reach a head 0.95 m away and a push kick a belly
1.1 m away, without the support foot sliding; reloads end with their event; two walkers differ;
a fist fight lands blows on both.

Measured on an M5 Pro:

- **Browser (Chrome, WebGPU):** 70 characters in the streamed city in full combat run at
  60 fps; the actors' update (brains, movement, paths, animation, reactions) averages about
  3 ms per frame.
- **Node:** a character's animation update costs about 20 µs per frame. A human model sculpts
  in about 13 ms and meshes in about 8 ms (~10k triangles).

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
  connections, and nav paths do not use doors or lifts. Benches are placed on open ground; the
  city generator doesn't place furniture or desks indoors yet.
- **Fights.** Brawls are one on one; a third person doesn't join in. Soldiers fight hand to hand
  only with the rifle butt.
- **Hits.** Rounds hit the static world via occupancy, not the rigid debris pieces. Characters
  are not obstacles for the engine's pieces. Debris crushes characters, but it doesn't bury
  them.
- **Ragdoll collisions.** Ragdolls collide with the world but not with each other.
- **Retro.** Frames are baked per geometry and held item. Deaths in retro mode are stepped
  ragdolls, not baked death sequences. A sprite-billboard mode, rendering the baked frames from 8
  angles into sprites, would be the next step towards classic Doom.
- **More content** would come as more rigs (quadrupeds, Doom-like monsters) on the same core: the
  foot planter, IK, actions, reactions, ragdoll and retro baking are rig-agnostic. Keyframed
  clips such as mocap can be added as actions (channels) or layered through `Pose.blend`.
