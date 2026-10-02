# Character wounds and motor capability

This is the character damage pipeline. [DAMAGE.md](DAMAGE.md) continues to describe
structural damage to the voxel world.

```
source → DamageDescriptor → voxel mechanics → physiology → Capabilities
                                                        → plan and muscles
                                                        → grips, AI and hosts
```

Only the damage module reads injuries, tissue and physiology to make decisions.
Motion, behaviors, props and hosts read `Capabilities`. Diagnostic tools may
inspect physiology and override capabilities without applying a wound.

## Sources and units

`damage/descriptor.hpp` declares projectile, edge, point, blunt, blast, crush and
thermal descriptors. Thermal is reserved. Units are metres, seconds, kilograms,
joules and pascals. Energy is `mass * speed² / 2`; momentum is `mass * speed`.
Projectiles include diameter and FMJ, expanding or buckshot construction. Edge
sources include endpoints, swept length, sharpness and alignment. Blunt sources
include contact area. Blasts include pressure, radius and fragment count.

`Character::damage()` is the entry point. `wound`, `melee` and `blast` remain
compatibility wrappers that construct descriptors. World collision impulses
produce crush descriptors with `impulse_delivered`, so the already-resolved
physical impulse is not applied twice. Melee contacts use the feature sweep in
[PROPS.md](PROPS.md).

## Anatomy and mechanics

Organic models have an outer skin/clothing layer, enclosed flesh, bones along the
rig, a skull, ribs and spine. The interior pass uses global occupancy so duplicate
joint cells receive the same material. It can be rerun after editing. Brain,
spinal cord, heart/great vessels, lungs, liver and major limb vessels are procedural
rig-space regions scaled with the build.

Projectile and point paths use DDA through each posed part's cells. Each visited
cell consumes energy according to tissue resistance. Projectile diameter and
construction affect the channel; remaining energy can open an exit. Bone can
stop or deterministically deflect the path. Fast projectiles record surrounding
tissue damage without deleting the whole temporary cavity. The character lattice
sets the smallest visible channel; it cannot display a sub-voxel bore.

Edges remove a thin plane swept along the edge. Alignment, sharpness, tissue
resistance and available energy limit the cut. A flat blade contact acts as blunt
force. Blunt hits bruise and can fracture or shatter bone without removing cells.
High-energy crushing can remove tissue, within its energy budget.

Blasts apply pressure, fragment channels and a throw. At close range they crush
the nearest extremities. They do not run the old whole-body gib branch. The body
remains present while coherent geometry remains.

Severing uses connectivity to the joint after actual tissue removal, including
bone. Voxel-count percentages no longer amputate limbs. Fractured but connected
parts stay attached with weakened muscles. Detached parts retain the exposed
flesh and bone of their cross-section.

Held and worn props intercept paths in distance order. Their authored material
resistance consumes energy and their instance geometry records holes or cuts.
Hits also load grips; an edge crossing an anchor can cut its strap. Breaking a
prop into separate objects is reserved.

## Physiology and capability

Per-part state contains flesh integrity, muscle function, intact/fractured/shattered
bone, vessel damage, bleeding, pain and nerve function. Wounds persist without
the old 12-entry cap. Whole-body state tracks blood volume (initially 5 L), shock,
consciousness, breathing, adrenaline and cause of death. Bleeding clots over time;
arterial wounds clot more slowly. A hand actually pressing the selected wound
reduces that wound's bleeding. Adrenaline temporarily masks pain.

Capabilities expose support, drive and control for each leg; strength, control
and grip for each arm; trunk and neck support; consciousness, vigor, pain,
mobility and a speed limit. Mobility is walk, limp, hobble, kneel, crawl or
immobile. Crawl variants use both arms, one arm or a seated scoot when some leg
drive remains. No usable limbs or insufficient consciousness makes the body
immobile. `health` is a derived compatibility summary.

Muscle gains, foot assists, clearance, limp timing, trunk fold, sway, guards and
action rates read these values. A disabled leg cannot pin itself to the plan with
an assist. Two disabled legs request prone locomotion without a host stance
command. Weak arms hang, release loads and stop being selected for strikes.
Actions refuse unusable support legs or insufficient consciousness. Hosts clamp
their movement requests to the returned speed limit.

These are tunable game mechanics expressed in physical units. Material and
physiology coefficients are authored approximations; the same descriptor need
not produce identical contact geometry in two differently posed bodies.

## Visuals, records and determinism

Changed parts increment their geometry version. Exposed interiors keep Flesh
and Bone slots; rims and soaking surface clothing use Blood and darker shades.
The existing bounded drop/stain system supplies entry/exit spray, arterial pulses,
drips, trails and pools under resting bodies. Persistent loose props have a
separate pool and do not compete with visual debris.

Character damage records use the `SVXD` marker and version 2. They contain cells
and shades, the 16-bit lost-part mask, fractures and physiology, blood/stains and
attached prop state. Version 1 and the older removal-only character records still
load. Parsing validates the complete record before mutation. Living populations
are remade by the host. Loose items are stored separately in the game `SVXG`
version 1 envelope; the contained core delta remains backward compatible. Articulation records are
version 2 for relative grip anchors; version 1 records remain readable.

Game command logs are version 4, with 32 numeric fields and an optional string.
Attach, detach, damage and population loadout commands are logged. Versions 1–3
remain readable. Geometry traversal, region ordering, instance IDs and fragment
allocation are deterministic. Replay tolerance for an unchanged Foundry build is
`1e-8`; the replay also checks item, capability and physiology state, wound geometry,
severed-part transforms, blood drops and stains.

## Checks and measurements

`tests/anim/test_props_wounds.cpp` exercises both physical backends, attachment
mass/release, anatomy presets, pressure, blood loss, structural severing and
record validation. Existing combat, recovery, model and corpse tests remain.
The model fixture hashes changed for the new interior materials; exterior counts
and palettes remain checked. Old tests expecting voxel-count amputations or
guaranteed whole-body blast gibbing now check structural cuts and coherent corpses.

`svx_people_bench --wounds` reports cost per projectile wound and character tick
for each backend. Character Foundry provides before/after gait, COM sway, falls,
crawl speed and grip-release measurements, plus frame sequences of actions and
damage scenarios. Its nine presets call `damage/scenarios.hpp`, the same descriptors
available to native tests and other hosts.
