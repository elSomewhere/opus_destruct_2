# Characters: svx_anim

Voxel people that move like people and fall like bodies: the character library of the
"euphoria_3" prototype (TypeScript), ported to C++ as the module `svx_anim` on top of the core.
A character is

- a **voxel model** (`voxel/`: sculpted humans, props, furniture, palettes; meshed for any
  renderer in a documented vertex format; damaged voxel by voxel - holes, severed limbs, gibs),
- a **motion plan** (`motion/`: gait and styles, stances - kneel, prone, sit, on the ground,
  down - actions and strikes, arms and weapon holds, a foot planner that finds the ground): what
  it means to do, from its host's inputs,
- a **physical body** (`body/humanoid`: 16 rigid parts on 15 joints with anatomical limits and
  muscles, pulled by assists: support, steer, upright, feet, hands),
- **behaviours** (`behaviour/`: the motor intelligence between plan and body - balance by the
  capture point, stepping, staggering, flinching and bracing, holding wounds, tripping, falling,
  lying, crawling, getting up, dying, knockouts),
- a `Character` that ties them together with health, hit zones and wounds, and a
  `CharacterSystem` that owns characters in a core `World`. `Brawler` choreographs fist and knife
  fights between two characters; `GibSystem` (`physics/debris`) moves what comes off them - limbs
  shot off, a body a blast tore apart, a dropped weapon - and their blood (drops that stain the
  surfaces they hit).

The port is faithful: the same constants, algorithms, order of operations and random draws as
the original; its tests are ported with their thresholds (`tests/anim/`).

## 1. Two ways to simulate a body

A body can be simulated two ways, and moved between them at any frame (`set_backend`: its
state goes with it):

- **Shallow** - on its own: `physics/rigid` is the original's XPBD rigid system (1/480 s
  substeps, one pass, velocity clamps), stepped by the character. It meets the world through a
  `CollisionWorld` (a `WorldCollision` reads a core World's voxels) and other bodies through
  obstacle spheres (`gather_obstacles`). Cheap, and independent of the world's tick.
- **Deep** - as an **articulation of the World** (docs/MOTION.md §6): a `CoreBinding` writes the
  body as links, joints (their limits and muscles) and targets (the assists), pushes the drives
  before each tick and pulls the links after it. The body is one of the world's bodies: it
  stands on the structures and loads them, a car or a falling slab that hits it hits it, it pushes
  what it meets. Unified with everything else, and stepped by the world (in parallel islands).

The deep body behaves as the original: the behaviour tests run on both paths with the same
thresholds (walking and running on the plan, hits, balance, stairs, rubble, trips over bodies,
reflexes, bracing, knockouts, lying and crawling, dying, limbs lost, brawls). What the deep path
needed to match XPBD: its own links' contacts are not senses and slide (frictionless); a target's
spring and damper are capped separately; joints, targets, then contacts in its fine steps; limits
that turn back at most 0.025 rad a step (the original's `LIMIT_STEP`: a corpse on its back is not
flung); the character's frame split in two about the world's tick (below).

## 2. The frame

A character's frame is `update(dt)` on the shallow path. A body bound to a world moves with its
ticks, so the frame comes in two halves about the tick - the order the shallow path keeps inside
`update`:

- `begin(dt)`: timers and the level of detail (a body woken or put to rest: its articulation
  added or removed), then the plan and the drives (`begin_body`), then the drives pushed to the
  core (`begin_push`);
- the world's tick;
- `end()`: what the tick made of the body - the pose, root motion, trips, the dead settling.

`CharacterSystem` (a `WorldSystem`) calls them: `pre_step` does the level of detail and the
obstacles, the characters' starts one at a time, their bodies **in parallel** (each reads the
world and steps its own; one at a time when the world has oriented grids, whose solids cache
fills as it is asked), their pushes one at a time; `step` ends them. The host sets roots and
inputs between ticks.

## 3. Level of detail: deep, shallow, hybrid

`CharacterSystemConfig::policy`:

| policy | bodies | for |
|---|---|---|
| `Deep` | every physical body an articulation | everything physical and two-way, at a cost |
| `Shallow` | every physical body its own | many people, the world one-way |
| `Hybrid` (default) | deep within `deep_radius` (14 m) of a focus point (the player) and within `piece_radius` (3.5 m) of any awake piece (a car coming, debris flying) - at most `max_deep`, the nearest; shallow within `physics_radius` (45 m); on the plan alone beyond (calm) | a city |

Whatever the policy, a body that needs physics (hit, falling, dying) gets it, and **the dead are
always deep**: a body at rest sleeps in the world and costs nothing, and the world keeps it
(below). Changes of path use a hysteresis (2 m).

## 4. Streaming and memory

A character whose ground goes out of range goes with it (`on_evicted`). The living are the
host's to make again (a population). The dead are the world's: each bound body's articulation
carries a record of what it is (the system's kind, alive, health, the host's data - a look -
and the damage it took: the limbs lost and the model's voxels gone, as runs against the look's
whole model, a few hundred bytes); the world archives a sleeping body with its region (or any
body wholly beyond the evict radius) and gives it back with it, and the system makes the
character again - the host's `restore` callback turns the record into a model and palette, the
damage is cut into a copy of it - and adopts the body where it lies, wounds and all.

Models are shared between characters until the first wound (then copied); meshes are made per
model and geometry version. The system's memory is in the world's report (`systems`).

## 5. The city's people (svx_game)

`Game` has **pedestrians** in a streamed world whose `RoadNetwork` has walkways (the drive city):
`PedestrianConfig` (logged: replays and lockstep have the same people doing the same) - how many
about the viewer, spawned out of sight (beyond `near_radius`) within `radius`, the bodies'
policy and `max_deep`.

- **Walkways**: the drive city's sidewalks from street corner to street corner (their walking
  lines clear of the lamps and trees) and its zebra crossings, which open with the signals.
- **Walkers** are the original's civilians, as much as a street asks: walking corner to corner,
  waiting at the kerb for the lights and crossing, stopping a while, jogging, stopping to talk
  with someone they pass (facing each other, speaking and listening in turn); afraid of shots,
  blasts, crashes, screams and the dead - running away along the walkways, cowering near blasts;
  jumping out of the way of a car coming at them. They move as the original's actor world moved
  people: steering with weight, keeping apart, a box swept against the world with a step up
  kerbs, and following the body while it leads (staggering, down, getting up).
- **Blows**: a deep body knows what hit it (its links' `bumped`): the change of speed it took
  hurts it - a car at 40 km/h kills - and the physics knocks it down. Drivers brake for people in
  their path. A shot's ray sees the characters (`raycast_shot`); a round into one is a logged
  command (`wound_character`); a blast throws, hurts, kills, tears apart. What comes off - limbs,
  pieces, blood - is the game's `GibSystem`'s: gibs are drawn as characters of one matrix, the
  blood as drops and stains (`Game::blood`).
- **Population**: every half second, as the traffic: the living out of range go, new people come
  on resident sidewalks out of sight; the dead in range stay (the longest dead beyond ten go),
  and out of range are the world's.

The front end gets the characters' meshes, palettes and per-tick skin matrices through the C
API (`svx_set_pedestrians`, `svx_poll_character_meshes`, `svx_characters`, `svx_raycast_shot`,
`svx_wound_character`, stats 51..56; docs in `svx_api.h`), and draws them with rigid skinning.

## 6. Cost

A body's step, single-threaded (20 standing, `link_substeps` 4): **deep** 0.22 ms a tick
(its fine steps 1.3 ms and its fine collision 0.33 ms per 20 bodies a substep), **shallow**
0.13 ms. The deep bodies' islands and the characters' own updates run in parallel; the plan
alone (a calm body far away) costs a small fraction of either.

The middle paths:
- **hybrid** (the default): deep only where it matters - near the player, near moving pieces -
  shallow in sight, on the plan beyond; the dead deep (asleep: free).
- **coarser deep bodies**: `RigidParams::link_substeps` 2 (1/240 s) halves the deep bodies'
  fine steps; the original's behaviour tests still pass on it but one (a corpse that settles
  and sleeps later than its window).
- **the plan alone**: `physics = false` (the level of detail) for the calm; anything that needs
  the body wakes it at once.

`svx_people_bench` drives the viewer through the drive city (8 m/s, traffic on, streaming) and
reports, per policy and crowd, the whole tick and the characters' own part (the deep bodies'
solve is in the world's), the bodies' split on average and the memory. On a 4-core container
(other work running; ms a tick):

| bodies | people | tick | 95th pct | characters | deep / shallow / plan | characters' memory |
|---|---|---|---|---|---|---|
| hybrid | 24 | 9.9 | 19.8 | 0.8 | 2.5 / 8.3 / 13.0 | 1.0 MB |
| hybrid | 48 | 11.2 | 22.6 | 1.2 | 4.8 / 15.1 / 26.1 | 1.8 MB |
| hybrid | 96 | 14.5 | 27.6 | 1.9 | 9.7 / 25.7 / 50.1 | 3.5 MB |
| shallow | 24 | 8.7 | 18.3 | 0.8 | 0 / 9.7 / 14.1 | 1.0 MB |
| shallow | 96 | 11.2 | 24.6 | 1.9 | 0 / 30.6 / 54.9 | 3.5 MB |
| deep | 24 | 15.9 | 37.0 | 0.8 | 9.7 / 0 / 14.2 | 0.9 MB |
| deep | 96 | 31.2 | 47.9 | 2.0 | 30.4 / 0 / 55.1 | 3.2 MB |

The hybrid costs little over all-shallow and keeps the bodies near the player and near moving
pieces bodies of the world; all-deep costs some 0.65 ms a deep body in the city (its fine steps
and its collision with the streets' voxels). Each character is some 40 KB; meshes are shared by
look until a wound.

## 7. Tests

`svx_anim_tests`: the original's tests (models, damage, motion, plan, body, behaviour, death,
characters, melee), those with a body on both paths; bodies in a world (a heavy piece knocks a
body down, and the body feels the blow). `svx_game_tests` (`pedestrians:`): people walk the
sidewalks, wait and cross; replays the same; they come and go with the viewer in bounded memory;
the dead stay where they fell across the streaming, with their wounds; a car driven into someone
knocks them down; a rocket among people tears them apart into gibs and blood.

## 8. Known limits

- A deep body near an awake piece is solved at the world's substep (1/120 s) with the pieces
  (docs/MOTION.md §5): its muscles and limits are stiffer there than in its own fine steps.
- The shallow bodies meet the world's pieces only as obstacles (a car does not push a shallow
  body): the hybrid policy makes bodies near awake pieces deep for that.
- What the host does not know how to make again (a `restore` that returns nothing: a look it
  no longer has) stays in the world as a stranger's body (an articulation no one draws).
