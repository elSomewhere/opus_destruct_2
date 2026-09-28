# svx_anim: voxel characters for structvox

`anim/` holds **svx_anim**, an animation engine for voxel characters: soldiers and civilians
built from voxels that move as physical bodies. They keep their balance, stagger, trip, reach
for walls, flinch, hold their wounds, fall, get up, fight, bleed, lose limbs and die. There is a
smooth modern presentation and a retro one (Voxel Doom style frames stepped in Doom tics),
computed from the modern animation.

The approach follows NaturalMotion's Euphoria. The character is a simulated body of rigid
parts driven by muscles, not a played animation. A **motion plan** says what the character
means to do: walk there, aim at that, throw a jab, sit down. A **physical body** of 16 rigid
parts with joints and muscles carries the plan out in a physics simulation. Between them sit
the **behaviours**, the character's motor intelligence. They read the body's senses (balance,
contacts, what hit it, what is within reach) and decide every frame how the muscles work: how
tense each region is, where the feet step, where the hands go and where the head turns. A hit
is not a canned reaction. It is an impulse on the body, and the body deals with it: it takes it
in its stride, staggers, reaches out, or falls.

Like the physics core (`svx_core`, [CORE.md](CORE.md)), it is a library with no knowledge of
the game:

- no physics engine (it has its own small one for bodies, gibs and blood);
- no renderer;
- no engine protocol;
- no DOM.

The game harness (`web/`) is one host of it. The two layers never meet inside svx_anim.

```
anim/                 svx_anim (TypeScript, zero dependencies, runs in node, browsers, workers)
  math/               vectors, quaternions, matrices, seeded random and noise
  core/               Skeleton, Pose, WorldPose, two-bone IK, springs, keyframe curves
  motion/             the motion plan: MotionPlan (what the character means to do), the foot
                      planner, gait parameters and personal styles, stances, actions, arm rig
                      and weapon holds
  body/               HumanoidBody: 16 rigid parts, anatomical joints, muscles, assists
  behaviour/          Behaviours (modes, balance, reflexes), senses (support polygon, walls
                      within reach), injuries
  physics/            CollisionWorld (the only view of the world), the rigid body solver
                      (XPBD), gibs and blood
  humanoid/           the humanoid rig (23 bones), fight choreography (Brawler)
  voxel/              VoxelModel, sculpting (SDF shapes), meshing, damage (hits, wounds, severing)
  retro/              re-voxelized frames of poses, Doom-style state sequences and playback
  characters/         procedural soldiers, civilians and thugs (many looks per geometry), weapons,
                      furniture (benches, chairs, desks, café tables)
  character.ts        Character: model + plan + body + behaviours + wounds + retro, what hosts
                      drive; gatherObstacles (bodies among bodies)
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
  sphere(center, r, out): boolean;                      // bodies, gibs, blood (push-out + normal)
  raycast(origin, dir, maxDist): number;                // blood drops, line of sight
}
```

`VoxelCollision(h, solid(i, j, k))` implements it over any voxel occupancy. The game uses the
engine's streamed chunk occupancy, the same bits the player collides with, so characters walk on
what the physics leaves standing. `FlatGround` is for tests and empty scenes.

## 3. Motion, body and behaviours

Every frame, `Character.update(dt)` runs three layers in turn:

1. **Behaviours, sensing.** Read the body: its centre of mass and velocity, the support
   polygon, the capture point, contacts, walls within reach. Pick the mode. Set the plan's
   controls: pelvis overrides, a held stance, reflex targets for the head and hands.
2. **The motion plan.** Compute the pose the character means to have this frame (a kinematic
   pose of the 23-bone rig) and the events (strikes, reloads).
3. **Behaviours, driving.** Turn that pose into muscle targets and tone per region, and set the
   assists (support, steering, feet, hands). Then step the physics and read the drawn pose back
   from the rigid bodies.

The character is moved by the host, with its own collision, via `setRoot(pos, yaw)`, and the
plan follows. When the body leads (staggering, falling, down, getting up, dying),
`controlled` is true: the host stops moving the root and follows `takeRootMotion()`.

### The motion plan (`motion/`)

`MotionPlan` animates the rig procedurally from a few inputs (`motion.input`). It is what the
character means to do. The body carries it out as well as it can.

| input | effect |
|---|---|
| `crouch` 0..1 | knees bend and the stance widens; crouch-walk takes short, careful strides |
| `stance` | `stand`, `kneel`, `prone` (crawls when moving), `sit` (on `seat`), `ground` (`groundVariant`: cross-legged, knees up, legs out); transitions route through a stance graph (prone goes via kneeling) and take real time (dropping to a knee 0.7 s, sitting down 1.45 s) |
| `seat` | `{pos, backrest, deskHeight?, variant}`: upright, leaning back with an arm along the backrest, legs crossed, elbows on knees, or at a desk typing |
| `carry` | relaxed / ready / aim / hip; long guns and pistols have their own holds (below) |
| `aimAt` | the trunk turns and pitches to the target, spread over spine, chest and neck, bladed for long guns; the muzzle points exactly at it |
| `lookAt` | head and eyes turn to a point of interest (while walking too); idle heads look around |
| `lean` -1..1 | peeking round a corner: the trunk leans out sideways, the head stays upright |
| `talk` | `speak` (gestures, nods, head movement) or `listen` (nods, weight shifts, a hand on the chin) |
| `guard` | fighting stance: bladed, hands up, bouncing on the balls of the feet |
| `mood` | normal / panic (hands to the head while fleeing) / cower / surrender (hands up) |
| `airborne`, `idle` | legs tuck, landing dip; idle poses and fidgets come by themselves when idle |

Calls: `play(name, target?)` for one-shot actions, `fire()` for recoil, `takeEvents()` for
strikes landing and reloads done.

**Locomotion** is a foot planner (`motion/feet.ts`) driven by a gait clock (`motion/gait.ts`):

- **Walk to run.** The walk (an inverted pendulum with double support) blends into a run (a
  spring-mass gait with a flight phase) between 2 and 3 m/s. Cadence, stride, stance fraction,
  lift, bob, sway, hip yaw and roll, lean and arm swing follow normal-gait data.
- **Personal style** (`motion/style.ts`, `GaitStyle`): stride, bounce, sway, arm swing, elbow
  bend, posture (upright or slouched), stance width, toe-out, heaviness, head stillness and
  fidgeting, drawn per character from archetypes (`randomStyle(seed, 'soldier' | 'civilian' |
  'civilianFemale' | 'thug')`), so a crowd doesn't walk in step.
- **Weight.** Footfalls load the body; the trunk leans into acceleration and banks into turns;
  soldiers aiming move in a rolling tactical walk; a wounded leg limps.
- **Planting.** A planted foot stays where it landed in world space. A swinging foot lands
  where the hip will be at mid-stance of the next step, on the ground found by
  `groundHeight`, and clears what is in the way on its path: steps, rubble, a body on the
  ground. The feet stay half a cycle apart at every speed and through turns.
- **Stairs and ledges.** A foot lands flat on one tread, a little short of the next riser,
  never across an edge, and rises or drops at most about two stair steps per step (a shorter
  stride otherwise). Stepping up, the toes lift and the foot rises before the edge; the sole
  keeps above what is just ahead of it. A climb is leaned into on bent knees, and
  `motion.slope` tells hosts to slow down (the game and the lab do).
- **Forced steps.** The behaviours can take a step anywhere at any time. A stagger is a run of
  such steps, and a swing already under way is re-aimed.
- **Standing and turning.** The head and eyes lead, the trunk follows within the spine's twist,
  and the feet come round in unhurried steps.
- **Legs and arms.** Two-bone IK to the ankles (knees towards the feet) and to the hands
  (elbows hanging, as anatomy puts them).

**Actions** (`motion/actions.ts`) are short keyframed clips of channels (`core/curve.ts`:
Hermite tracks with eases) that steer the IK rather than replace it: hand and foot targets,
strike weights towards a target, elbow poles, trunk, head, pelvis, crouch, weapon offsets, and
events. A pose layer holds postures (guard, talking, idle poses); the action layer plays
one-shots on top and blends out. Mirrored versions (`.m`) are generated.

- **Strikes:** jab, cross, hook, uppercut, front kick, roundhouse; with a knife a stab, a
  backhand slash, a forehand slash and an underhand gut stab; a rifle butt push. A punch at a
  target out of reach steps in: the lead foot goes forward and the shoulders lean into the
  blow. A kick thrusts the hips and meets the target with the ball of the foot (push kick) or
  the instep (roundhouse).
- **Defence and weapons:** block, rifle and pistol reloads (with a `reloaded` event).
- **Everyday:** idle poses, fidgets, waves, conversation gestures, nods, head shakes, laughing.
- **A soldier's pauses:** catching a breath with the weapon lowered, a look round, setting the
  helmet straight, wiping the brow, rolling the shoulders, checking the weapon.

**Weapons** are separate one-bone prop models (`characters/props.ts`: rifle, SMG, light machine
gun, pistol, knife) at the rig's `weapon` socket, with both hands put on them by IK
(`motion/arms.ts`). Long guns: relaxed, low ready, shouldered on the aim line, from the hip,
port arms when running; recoil kicks back and up. Pistols: lowered, low ready, two-handed at
eye level, or one-handed. Knife: in the right hand, blade forward. In the body, a gripped prop
is held by the physical hand: a hit that knocks the arm knocks the aim.

### The physical body (`body/humanoid.ts`, `physics/rigid.ts`)

`HumanoidBody` builds 16 rigid bodies from the rig: pelvis, spine, chest, head, and upper
arm, forearm, hand, thigh, shin and foot on each side. They weigh 75 kg split by standard
segment fractions. Each part collides as a few spheres. The drawn neck is interpolated
between chest and head.

- **Joints** have anatomical limits: asymmetric elliptical swing cones (a hip swings 125°
  forward and 40° back, a shoulder 160° forward), twist ranges about the bone (swing-twist
  decomposition), and hinges for elbows and knees that bend one way.
- **Muscles** are PD drives towards the plan's joint rotations. Stiffness comes from a natural
  frequency per joint and the effective inertia it moves (the spine 15 rad/s, knees 18 rad/s),
  damping from a damping ratio. They also track the plan's joint velocities and feed its
  accelerations forward. **Tone** (per part, 0..1+) scales them: the behaviours make the body
  tense, slack, or anywhere between.
- **Gravity compensation** holds limbs up in proportion to tone, so an arm at ease hangs and
  swings while a tensed one holds its place.
- **Assists** ("hand of god" forces, as Euphoria has them, used sparingly): support holds the
  pelvis up, steering moves it along the plan, an upright orienter keeps the trunk upright,
  feet pins keep planted feet planted (stiff, not rigid), swing guides guide the feet, and hand
  grips hold weapons, a wall or a wound. Each is scaled by mode. A falling body has none.

`RigidSystem` is an XPBD solver (Müller et al. 2020): 1/480 s substeps, one pass per substep,
velocities derived from positions. It has contacts against the CollisionWorld with friction,
self-collision between non-adjacent parts, and limits corrected gradually and inelastically, so
a body folded against itself never gains energy. It also bounds velocity changes per substep,
damps twist on light limbs, and sleeps once nothing moved more than 2 cm for half a second.

**Bodies among bodies.** Each frame `gatherObstacles(characters, reach, extra)` gives every
body the collision spheres of the others nearby, plus debris. A body collides with them, and
the push it takes is handed back to the body it came from, spread over that body. A shove
into a bystander knocks the bystander, a runner's foot catches on a corpse, and steps clear
what they see. A limb that strikes passes through the body it hits, because the host deals
the blow.

### The behaviours (`behaviour/`)

`Behaviours` runs a mode machine:

- **animated:** the body tracks the plan closely, with its own secondary motion (arms swing at
  ease, the head lags). Blows it can take in its stride are absorbed. Knocked off the plan
  beyond what the plan itself intends (a lunge into a punch does not count), it reacts.
- **reacting:** balance takes over. The legs hold the body up but no longer steer it. The
  ground pushes back through the feet's centre of pressure (an inverted pendulum), with a hip
  strategy when that is not enough. When the capture point (ξ = c + v/ω₀) leaves the support
  polygon, a foot steps where it will be: quicker the worse it gets, re-aimed while swinging,
  on the side that suits. The arms go out to the shoulders. A wall within reach gets a hand on
  it that takes weight, and a shoulder leaning on a wall counts as support. Balanced and still
  again, it hands back to the plan. A fighter steady on its feet fights on.
- **falling:** the balance is lost for good: beyond reach for a moment, the trunk tipped past
  about 50°, thrown up off the feet, too many steps, legs that fail, a daze. The body goes
  loose (legs bent, trunk curled), the hands go out to break the fall and lock where they land,
  and the head is kept off the ground.
- **lying:** limp but alive, for a while (longer the more it hurts). Knocked out, until it
  comes to. Badly hurt, it writhes: knees drawn up, rolling from side to side, clutching
  the wound. Face down, a host that wants it away (`input.stance = 'prone'`) has it crawl
  off on its elbows. The plan follows the body if it rolls over.
- **rising:** gathers, then gets up through sitting or pushing up from the front, kneeling and
  standing, with the legs taking the weight back. It is brisk unhurt (about 2 s) and slower
  hurt.
- **dying:** the muscles fade, the legs first. The knees buckle, a hand goes to the wound, a
  last step or two, and the arms half catch the fall. Near a wall the body slumps against it.
- **dead:** no muscle, extra tissue damping. The body settles and sleeps.

Reflexes run on top of any mode:

- **Flinching** from what lands close (`perceive`: impacts, rounds whizzing past, blasts, blows
  coming): the head turns away and ducks, the shoulders come up, and a hand comes up between
  the face and the danger. Nerves build up over time and make the next flinch bigger.
- **Holding a wound:** a hand goes to it and stays for seconds (longer for the gut and chest).
  The posture of injuries follows: a limp, a weak arm, a hunch.
- **Stun and shock:** a struck part and its parent go slack for a moment, and the whole body
  dips with the shock of a hit.
- **Trips:** a swinging foot that touches something is lifted higher at once (the stumble
  reflex). Caught for longer than a careful step allows (hardly at all when running), it stops
  there: the body pitches forward over it and catches itself with quick steps, or goes down.
  Walkers pick their way across rubble; runners trip on it.

**What a hit does** (`hitAt`, via `wound`, `melee`, `blast`): an impulse where it lands. A
light part takes what it can and passes the rest up the limb. The struck part spins about its
joint along the lever arm: a round high in the chest rocks the trunk back, a shoulder hit
turns it, a fist on the jaw snaps the head round on the neck. A share goes to the whole body.
Then the balance deals with the rest. The results are graded rather than scripted:

- one rifle round in the chest rocks the trunk back 5–35° and does not fell;
- a jab snaps the head;
- a cross or an uppercut staggers the body a step or two;
- a push kick to the belly drives it back several steps;
- a gut wound folds the body over it;
- a leg wound buckles the knee and leaves a limp;
- a shove is taken in place when light, with steps when harder, and fells when hardest;
- a rocket 1.8–2.2 m away throws the body off its feet, and 2.6–3 m away staggers it 4–6
  steps;
- badly hurt (health below 30%, or a leg shot through), the body collapses and writhes.

**Fights** (`humanoid/brawl.ts`, `Brawler`) choreograph two characters: keeping range and
circling, choosing strikes by distance, combinations (jab, cross), blocking or stepping back,
backing off a downed opponent, and resolving each strike event into a hit where the physical
fist, foot or blade met the body (`Character.melee`). Fists and feet knock people down and
out rather than kill them. Knives cut the voxel body open and can kill.

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
  - hits the living body where the round hit (section 3), or pushes the dead;
  - severs limbs and heads whose part was shot through or lost about half its voxels. The parts
    below go with them, so a severed forearm takes the hand.
- **Melee.** `melee(point, dir, 'blunt' | 'blade', force)`: punches and kicks hurt by zone
  (the head most) and knock out rather than kill; a knife cuts a slice of the voxel body open
  and can sever and kill.
- **Blasts.** `blast(center, radius)` does distance-scaled damage and throws or staggers the
  body. Close to the centre the body is torn apart: every part becomes a gib.
- **Copy on write.** Models are shared between characters until the first wound, then copied.
  `geometryVersion` tells the host to re-mesh; only the parts that changed are re-meshed.

## 5. Dead bodies, gibs, blood

- **Dead bodies are the same physical body** with the muscles gone. Nothing is swapped at the
  moment of death, so a character shot mid-stride keeps its momentum and its last step, and a
  dying body keeps some tone for a moment. Death (`die(point, dv, collapse)`) starts the dying
  mode: the killing impulse where it hit, then the muscles fade, legs first (`collapse` sets how
  fast the knees give; a head shot drops the body at once). Shot from the front a body goes
  down backwards, from behind forwards.
- **Stability.** Limits are corrected gradually and inelastically, pair contacts inside a body
  are speed-capped, velocity changes per substep are bounded, and dead bodies get extra tissue
  damping and drag. A folded body whose constraints fight never blows up, and nothing bounces
  back up. Limbs never flip about their length, and the head rests within the neck's range
  (face down, on a cheek). Settled corpses stop colliding with themselves and sleep, and
  sleeping bodies cost nothing.
- **Level of detail.** Physics costs time, so a calm character can run on its plan alone
  (`physics = false`, the host's choice, blended over a few frames). Anything that needs the
  body (a hit, a push, a trip, a fall, death) wakes it where the plan has it.
- **`GibSystem`** (`physics/debris.ts`) handles gibs, blood drops and stains:
  - **Gibs** (severed limbs, heads, torso chunks, dropped rifles) are rigid voxel bodies with box
    inertia. About 64 surface sample points collide as spheres, using sequential impulses with
    restitution and friction, adaptive substeps and sleep. Hosts may pass small gibs to
    `gatherObstacles` so feet catch on them.
  - **Blood drops** fly ballistically and become stains where they land, on floors and walls.
    Gibs bleed while they move.

## 6. Retro presentation

The retro look is a function of the modern animation, not separate content (`retro/`):

- **Baking.** For each state, a MotionPlan is driven on flat ground until steady and a
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
- **Everything else.** Physical bodies away from their plan (staggering, falling, dead), gibs
  and wounded characters (baked frames show no wounds) keep their real geometry, but their
  poses advance in the same 4-tic steps.

## 7. In the game (`web/src/actors`)

`ActorWorld` runs soldiers and civilians in the engine's worlds.

- **Population.** When a world loads, characters are placed on standable ground around the
  player, with benches and café tables (a chair, a laptop, a cup) for the civilians:
  - city: 18 civilians, 8 soldiers, 3 thugs;
  - tower: 12 civilians, 6 soldiers, 2 thugs;
  - rooms: 5 civilians, 3 soldiers, 1 thug;
  - Doom maps: 8 civilians, 6 soldiers, 2 thugs.

  `?civilians=N&soldiers=M&thugs=K` overrides the counts and `?actors=0` places none. The
  settings panel's Characters section adds more and clears them.
- **Looks and loadouts** (`cast.ts`): every character gets a geometry, a palette and a
  personal gait style. Soldiers carry a rifle (55%), an SMG, a light machine gun or a pistol;
  about one civilian in eight carries a pistol, a few an SMG or a knife (kept out of sight until
  needed). Thugs (heavier set, hoodies and dark jackets, beanies, caps, balaclavas; a swagger)
  carry a knife most of the time, otherwise they use their fists. Weapons differ in damage, rate of fire, spread, magazine size and how much of the
  world a round carves.
- **Navigation** (`nav.ts`) is A* over 0.25 m cells found on the fly from the occupancy (ground
  within a step, room above), with string-pulled paths. A straight walkable line needs no
  search. Ground queries are cached across searches and forgotten per chunk column when the
  world changes; searches are budgeted per frame.
- **Movement with weight** (`steer.ts`): bodies follow a point a little ahead on their path (the
  corners are rounded, not turned on the spot), speed up in about a second and brake into their
  goal, change heading at a limited rate (a walker turns within half a metre, a runner takes
  metres) and slow down for sharp turns; setting off away from where they face, they turn
  first. The facing turns with angular momentum (speeding up and braking into the new
  direction). Standing and aiming, the trunk takes small corrections and the body turns only
  for larger ones. Everyone walks at a personal pace. Runners face where they run and keep the
  target with their head; soldiers change positions every few seconds, not constantly, and
  scan with their eyes and trunk before turning the body.
- **Civilians** (`brain.ts`) live a day in the city:
  - they stroll at their own pace, wait around (idle poses, fidgets, a glance at the phone),
    jog now and then, and look at what catches the eye while walking (people passing, the
    player, a fight);
  - they walk up to each other and talk, taking turns speaking and listening, facing each
    other at conversational distance; now and then a conversation turns into a fist fight (or
    a knife fight);
  - they sit on benches (walk up, turn, sit down, sit in their own way, get up and leave), at
    café tables working on a laptop, and on the ground;
  - a fight draws a crowd: passers-by stop and watch, sitters and talkers look over; the winner
    walks off, the loser gets away once back on their feet, and a killer runs;
  - fear builds from gunfire, impacts, screams and bodies; frightened, they run, panicking and
    screaming when very scared, which spreads the fear. Badly hurt and down on their front,
    they crawl away, then lie where they got to. They cower next to explosions and put
    their hands up when the player aims at them from close by (not while the player
    spectates in noclip). Armed civilians may draw and shoot back at soldiers firing nearby
    (then soldiers treat them as hostile).
- **Thugs** (`thug.ts`): they loiter with a swagger, eyeing people, and pick a victim: the
  player when close and in sight, civilians, now and then a soldier. They walk up, draw the
  knife and shout (civilians close by run), then charge. Against a character they fight through
  the brawls (guard, footwork, strikes, knife attacks, blocks); against the player they keep
  close, circling, and strike and cut. Badly hurt, or under fire from soldiers, they run.
  Soldiers engage thugs they see (the nearer threat before a player further off).
- **Soldiers:**
  - patrol their post; gunfire, impacts, screams, shouts and bodies alert them and send them
    to look;
  - once they see the player (or a hostile) they react, keep a combat range (advance, back
    off, strafe), and fight from positions: standing, kneeling, prone, crouched behind cover
    popping up, or leaning out from a corner to fire. Rounds coming close rattle them (their
    nerves): they flinch harder, crouch, and pick lower positions;
  - they fire bursts whose spread tightens while they hold their aim, wider when firing on the
    move or from the hip; machine gunners fire from the hip on the move; they reload (the
    magazine refills when the reload action says so) and don't fire with a friend in the line;
  - at arm's length they hit with the rifle butt (or punch, holding a pistol); they hunt the
    last known position when they lose sight of their target;
  - between bursts now and then, or with the target out of sight, they lower the weapon a
    moment (a deep breath, a look round) and top up a half-empty magazine; standing easy they
    fidget (helmet, brow, shoulders, weapon).
- **Bodies with balance:** the twelve living characters nearest the player within 32 m run as
  physical bodies. The rest run on their plans until something happens to them. A round
  smacking in within 3.5 m, or whizzing past within about a metre of the head, makes people
  flinch. A blast throws those close off their feet, staggers those further out, and makes
  everyone around flinch. Now and then a hurrying foot catches: more often running, most when
  panicking, rarely a soldier. Characters collide with each other, with corpses and with small
  debris. A body that leads (staggering, down, getting up) moves the actor, and AI waits for
  it to be back on its feet. The player's body counts too: people walked into give way with
  a stumble, and those run into go down.
- **Combat.** A round (theirs or the player's) hits the first thing on its line:
  - a character, voxel-exact: a wound, and a reaction by where it hit;
  - the player's capsule (health, a red vignette, respawn after death);
  - or the world, where it sends `carve` to the engine (about one world voxel per round).

  The player has a knife too (key 4): a cut into whoever is within reach in front, carving the
  voxel body open like the thugs' knives.

  The battle destroys the level: rounds break bonds and bring down storeys. Rockets tear
  characters apart and throw bodies. Falling rigid debris crushes characters, and a long fall
  kills. Blows push the player.
- **Drawing.** Characters, their weapons, benches, gibs, blood drops (instanced cubes), stains,
  blood pools and blob shadows (decals) are drawn in the main pass with the world's lighting:
  hemisphere, sun, sector light, fog and muzzle flashes.

Controls and parameters, in addition to the game's:

| | |
|---|---|
| `?civilians=N&soldiers=M&thugs=K`, `?actors=0` | initial population (city: 18, 8, 3) |
| `?anim=smooth\|retro\|retro-chunky` | animation presentation (also in the panel) |
| `?god=1` | soldiers' rounds don't hurt the player (also in the panel) |
| panel "Characters" | +6 civilians, +4 soldiers, +3 thugs, clear, animation style, AI on/off, god mode |
| key 4 | the knife |
| `window.__structvox` | `spawn('civilian' \| 'soldier' \| 'thug', n)`, `spawnAt(kind, x, y, z)`, `actors`, `characters({ai, god, style})`, `actorWorld()`, `brawl(idA, idB)` |

**The lab** (`/lab.html`) runs svx_anim without the physics engine: a test course (stairs,
ramp, rubble, beams, walls, benches, a desk) and a scripted cast in four groups, each with a
camera bookmark. The HUD shows the followed character's mode, balance and steps.
`window.__lab` (`step`, `setTimeScale`, `shoot`, `rocket`, `byName`, `follow`, `cam`) drives it
from scripts, frame by frame:

- **city:** a three-way conversation, bench sitters in four styles, a desk worker, ground
  sitters, people waiting (idle poses, fidgets), a stroller, a commuter, a jogger, a runner,
  stairs;
- **soldiers:** kneeling fire, prone, crawling, peeking round the corner, pistol walk-and-fire,
  one-handed pistol, machine gun from the hip, reloading, crouch-walk, patrol, sprint;
- **fights:** two brawlers, a knife against an unarmed fighter, a thug with a knife on a
  civilian, a thug with his fists on a rifleman (they reset after a while);
- **reactions:** a runner barging through a pedestrian, a line-up to shoot at, a stumbler (pushed by pretend blasts, felled every third
  time), a tripper (walking to and fro, now and then falling), beams and a kerb across a
  running lane that catch feet, a wall bracer (shoved towards a wall: a hand takes the weight),
  a wounded walker limping along a wall, a soldier shot dead against a wall (he slumps down
  it), a lone subject to try things on, a soldier at ease (fidgets, breathers, reloads,
  flinches).

Click a character to shoot it, shift-click the ground to fire a rocket; switch smooth / retro
/ chunky, change time scale (slow motion) or follow a character.

## 8. Using svx_anim in another host

```ts
import { Character, VoxelCollision, gatherObstacles, makeSoldier, makeRifle, ModelMesher, GibSystem } from 'svx-anim';

const world = new VoxelCollision(0.125, (i, j, k) => myVoxels.solid(i, j, k));
const look = makeSoldier(7);                        // geometry + palette
const soldier = new Character({ model: look.model, palette: look.palette, collision: world, weapon: makeRifle() });
soldier.place([x, y, z], yaw);
const mesh = new ModelMesher().mesh(soldier.model); // upload once (20-byte vertices)

// every frame
gatherObstacles(allCharacters, 2.2, smallDebrisSpheres); // bodies among bodies
const rm = soldier.takeRootMotion();
if (soldier.controlled) myController.feet = vadd(myController.feet, rm); // the body leads
else soldier.setRoot(myController.feet, myController.yaw);             // the host leads
soldier.motion.input.carry = 'aim';
soldier.motion.input.aimAt = target;
soldier.update(dt);
draw(mesh, soldier.skin /* 23 bone matrices */, look.palette);

// a bullet: a wound, an impulse on the body where it hit; the behaviours do the rest
const hit = soldier.raycast(origin, dir, 200);
if (hit) for (const g of soldier.wound(hit, dir, 34).gibs) gibs.spawn(g.part, g.voxelSize, g.bonePos, g.boneRot, g.boneRestHead, g.vel, g.ang);

// the world around it
soldier.perceive({ point: impact, strength: 1, kind: 'impact' }); // a flinch
soldier.push(awayFromBlast, 2.0);                                    // a shove (m/s)
soldier.trip();                                                      // a foot catches
soldier.physics = nearPlayer;                                        // level of detail

// a punch or a knife cut; strikes come from motion.play('jab', target) and their events
soldier.melee(point, dir, 'blunt', 1.2);

// everyday life
soldier.motion.input.stance = 'sit';
soldier.motion.input.seat = { pos: benchSeat, backrest: true, variant: 'leanBack' };
civilian.motion.input.talk = 'speak';
civilian.motion.input.lookAt = otherPerson.eyes();
```

Hand-to-hand fights: give each fighter a `Brawler` (`opponent`), call `update(dt)`, move the
fighters by its `move` and `yaw` (or by root motion while `controlled`), and pass their
animation events to `resolve`, which returns the blows that landed.

Other calls: `hitAt(info)` (a blow without a wound), `impulse(point, dv)`, `knockOut(s)`,
`addInjury(bone, severity)`, `die(point, dv, collapse)`, `blast(center, radius)`, and
`down`, `writhing`, `asleep`, `knockedOut` for the AI.

## 9. Tests and checks

```bash
cd anim && npm install && npm test          # 83 unit tests: rigid joints, the body, tracking, hits,
                                            # balance, trips, flinches, bracing, knockouts, dying,
                                            # bodies among bodies, motion, strikes, brawls, damage,
                                            # gibs, retro
cd web && npm run typecheck && npm test     # includes typechecking anim/
node scripts/smoke-actors.mjs http://localhost:5190/   # browser: population, fighting, kills, gibs, retro,
                                            # city life (talking, benches), a brawl, a thug going
                                            # for a civilian, the player's knife, MAP01, lab
```

The tests measure behaviour. Some examples:

- `anim/test/body.test.ts`: a joint holds its anchors while energy does not grow; a hinge
  stays in its range; segments weigh what bodies weigh; a body without muscle collapses and
  sleeps without sinking in; muscles and assists hold a standing pose.
- `anim/test/behaviour.test.ts`: walking and running bodies follow their plans and stay up; a
  calm body rests on its plan and a hit wakes it; a chest round rocks the trunk 5–35° without
  felling it; a gut wound folds the body and a hand goes to it; a leg wound limps on that side;
  a kick shoves the body and it steps and recovers; shoves are graded (in place, steps, a fall,
  getting up); a caught foot pitches the body forward; running over a beam catches a foot; a
  close round turns the head away with a hand up; shoved towards a wall a hand holds on; a
  knockout stays down, then gets up; the dying go down and come to rest; a body shoved into a
  bystander moves it and they stay apart; a runner trips over a body.
- `anim/test/death.test.ts`: limbs never flip, bodies rest without bouncing, a shot body
  crumples over half a second and a head shot drops it, bodies fall the way the shot pushes
  them, heads rest within the neck's range.
- `anim/test/motion.test.ts` and `plan.test.ts`: feet planted without sliding, the feet
  alternating, stances, weapons on target, strikes reaching (a jab steps in to a head 0.95 m
  away), turning on the spot.

Measured on an M5 Pro:

- **Node:** a physical body costs about 90 µs per frame (plan, behaviours and 8 substeps of
  16 rigid bodies), a character on its plan alone about 10 µs.
- **Browser (Chrome, WebGPU):** 56 characters in the streamed city, about 20 of them physical,
  in full combat: the actors' update averages 3.5–5.5 ms per frame at 60 fps.

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
  connections, and nav paths do not use doors or lifts. The AI does not steer round each
  other's bodies (they bump). The city generator doesn't furnish interiors yet.
- **Fights.** Brawls are one on one; a third person doesn't join in. Soldiers fight hand to hand
  only with the rifle butt. Fighters do not clinch or grab.
- **Bodies.** Other bodies are obstacles pushed back one frame late, not bodies solved
  together, so a pile of bodies is soft. Hands grip walls, weapons and wounds, not other
  people. Bodies do not collide with the
  engine's rigid debris pieces (only with small gibs and the voxel world).
- **Retro.** Frames are baked per geometry and held item. Staggers and deaths in retro mode are
  stepped physics, not baked sequences.
- **More content** would come as more rigs (quadrupeds, Doom-like monsters) on the same core: the
  foot planner, IK, actions, the rigid body solver, the behaviours' balance and the retro
  baking do not assume a humanoid beyond their rig tables.
