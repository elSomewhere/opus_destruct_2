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

Edited limbs may sit away from the nominal rig axis. In each enclosed limb
cross-section the interior pass retains a thin bone core nearest that axis,
without changing surface colours or adding cells. This avoids boneless calves
and forearms in offset sculpts. The engine and Foundry use the same rule.

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

Every deposited joule is accounted for, including temporary cavities, exit-hole
enlargement and crushed cells. Edge alignment changes the resistance paid per
removed cell; it does not discard unreported energy. Blunt pressure spreads inward
from the contact patch, with less energy farther from the contact. A zero-energy
command changes neither geometry nor physiology. The impulse delivered to a body
or attachment is incoming momentum minus the penetrator's remaining momentum,
including its deflected direction. Host bone hints cannot override the entry part
found by the actual path.

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

A stopped projectile or strike transfers its absorbed momentum to the prop's
physical anchor at the contact point. Retention sees that load immediately,
including between simulation ticks. A melee block also reacts on the attacking
limb or grip. Damage results report absorbed contact energy and momentum even
when the prop stops the entire strike and the body loses no health. Blast results
sum their fragment and crush contacts; the separate whole-body throw is excluded
from that contact-impulse field.

## Physiology and capability

Per-part state contains flesh integrity, muscle function, intact/fractured/shattered
bone, vessel damage, bleeding, pain and nerve function. Wounds persist without
the old 12-entry cap. Whole-body state tracks blood volume (initially 5 L), shock,
consciousness, breathing, adrenaline and cause of death. Bleeding clots over time;
arterial wounds clot more slowly. A hand actually pressing the selected wound
reduces that wound's bleeding. Adrenaline temporarily masks pain.

Fractures use energy deposited locally in the affected part, and require bone
in the contact volume. Penetrators count only energy deposited in bone; blunt
compression also transmits the local part's absorbed load. The authored fracture
threshold is 45 J for shins/forearms, 65 J for other extremities and 160 J for
the core. Shattering thresholds are 220/600 J for extremities/core; another
fracturing blow to an already fractured part can shatter it. A later weak hit
never restores a bone. These are gameplay coefficients, not medical thresholds.

Vital injuries likewise use energy deposited inside their own region. Removing
tissue there has different consequences from closed compression. A high-energy
source grazing an extremity cannot cause massive core trauma. Wound care compares
dimensionless urgency: active bleeding takes priority over transient pain, with
the largest clot-adjusted bleeding rate first. Dying support assists also respect
the legs' remaining capability.

Capabilities expose support, drive and control for each leg; strength, control
and grip for each arm; trunk and neck support; consciousness, vigor, pain,
mobility and a speed limit. Mobility is walk, limp, hobble, kneel, crawl or
immobile. Crawl variants use both arms, one arm or a seated scoot when some leg
drive remains. No usable limbs or insufficient consciousness makes the body
immobile. `health` is a derived compatibility summary.

Muscle gains, hand and foot assists, clearance, limp timing, trunk fold, sway,
guards and action rates read these values. Disabled limbs cannot pin themselves
to the plan with an assist. Two disabled legs request prone locomotion before
the recovery controller reads the host's stance. This also works when both lower
legs are severed and the host keeps requesting standing every tick. From the
back, the body rolls onto its front without passing through kneeling. Kneeling
uses the stronger leg for its forward foot. Incapacitated bodies stay down until
capability returns; they do not repeatedly attempt to get up.

During crawling, `Character::set_root` limits heading changes by usable arm/leg
drive, trunk support and consciousness (at most 0.9 rad/s). Forward translation
builds as the body turns towards the requested direction. This prevents a host
from snapping the prone body around after a recovery roll. Wound care reserves
the hands during recovery and moving crawls, and selects a usable free arm when
stationary. Weak arms hang, release loads and stop being selected for strikes.
Actions refuse unusable support legs or insufficient consciousness. Hosts clamp
their movement requests to the returned speed limit.

With one leg below 0.12 support and the other above 0.4, the foot planner uses a
single support cycle. The sound foot loads, pushes off and lands again; the
disabled foot stays out of the support polygon and balance-step selection.
Remaining control determines whether that leg tucks behind the body or drags.
The pelvis shifts over the supporting foot and follows one loading/flight cycle.
Foot pins are limited by both control and weight-bearing capability. Restoring
support through a tooling override places the foot before returning to walking.
Healthy two-leg gait uses the unchanged planning path.

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

Game command logs are version 5, with 33 numeric fields and an optional string.
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

### Projectile momentum and tissue displacement

A projectile spends energy on tissue removal and on quadratic drag integrated over each crossed cell's actual chord. The latter uses `E_out / E_in = exp(-rho Cd A length / mass)` before the removal and cavity budgets are combined. Expanding rounds have a larger effective area. These are calibrated game parameters, not a ballistics certification. The remaining energy determines exit momentum; the incoming-minus-exit impulse is applied once at the impact. Increasing speed therefore increases absorbed momentum instead of reducing it for an otherwise fixed channel. Root steering yields briefly to that impulse. Native and Foundry bridge regressions check speed response, finite energy and momentum accounting on both backends.


### Authored projectile recoil

`DamageDescriptor::impact_scale` controls additional gameplay recoil. A value of
1 uses deposited momentum alone. Higher values apply a stronger impulse at the
same hit point through the existing physical body and reflex paths, on either
backend. The total response is capped at 500 N·s. This is an authored animation
response; it adds no penetration energy or tissue damage. `WoundResult::impulse`
continues to report transferred physical momentum; `recoil_impulse` separately
reports the added impulse. Misses produce neither, and contacts whose impulse
was already delivered do not apply it twice.

The game web client, Foundry projectile controls, and thigh/femoral presets use
30× for a visible reaction comparable to Foundry's 60 N·s push. Individual
buckshot pellets and shotgun volleys keep 1×. Native descriptors, the legacy
`wound` wrapper, old API calls and old replay commands retain 1×. Foundry exposes
the multiplier as a slider and records it in sessions.

### Consumed joints and severing

Severing first removes tissue disconnected from the proximal joint, then tests
whether the remaining anchored tissue still supports each child joint. Support
cells come from the original model's geometry, including limbs offset from the
rig axis. If every supporting cell is consumed, the distal subtree detaches even
when no fragment remains at the wound. Empty parts also lose their physical limb
capability when the wound emitted no debris. This keeps repeated bullet channels
from leaving a hand or foot floating beyond a destroyed connection.
