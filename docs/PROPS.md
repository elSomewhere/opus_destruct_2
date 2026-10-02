# Character props

`svx_anim` keeps the 23-bone pose and 16 physical body parts. Props have their own
transforms and persistent instance IDs. They do not extend either array.

## Data and ownership

`characters/props.hpp` defines immutable archetypes: stable string ID, display
name, voxel model, mass, centre of mass, diagonal inertia, dimensions, material,
tags, sockets, named points and contact features. `prop_catalog()` owns the shared
definitions. Code selects capabilities with `has()` and `satisfies()`.

The catalog contains rifle, SMG, LMG, pistol, knife, long dagger, machete, sword,
baton, bat, phone, bottle, briefcase, suitcase, backpack, shoulder bag and shopping
bag. The shopping bag is the extensibility example: one definition uses the
existing hanging-container path, with no new hold, action or damage branch.

`PropInstance` owns condition, strap integrity, reserved ammunition and contents,
an optional damaged model, location and velocities. `PropRegistry` allocates IDs
in deterministic order. A location is attached, loose or gone. Character and
motion-plan `weapon` members are compatibility views of the attachment slots;
neither owns another prop pointer.

```cpp
auto item = character.attachments().registry->create(prop_archetype("sword"));
character.attach(item, AttachPoint::LeftHand, "primary", WieldStyle::OneHand);
auto released = character.detach(AttachPoint::LeftHand, ReleaseReason::Voluntary);
character.swap(prop_archetype("backpack"), AttachPoint::Back, "strap", WieldStyle::Worn);
```

Attach, detach and swap work between ticks on shallow, deep and plan-only
characters. A failed swap preserves the old attachment. `Attachments::refusal`
explains refusal. `take_attachment_events()` reports the instance, point and
release reason. The future pickup controller can call this same `attach` API.

## Anchors and load

Attachment points are right hand, left hand, back, shoulder, hip, thigh, chest,
head and both arms. Head and both-arms placements are reserved. Archetypes list
their allowed points. Styles are one hand, two hands, reverse grip, hanging,
worn and stowed. A two-handed grip occupies both hands; its support hand uses
the secondary socket through the existing hand attachment and orienter.

Handedness and practice are character data. Mirrored actions transfer the same
instance between free hands and emit events. Pockets, crossed arms and similar
idle poses require free hands. Firearm holds retain their authored carry data;
blades and clubs use relaxed, ready and guard carries. A two-handed club can rest
on the shoulder. A phone's ready carry brings it to the ear, and the existing
`phone` pose remains available.

Mass and parallel-axis inertia are added to the anchor's original mass properties.
Joint gains are re-derived in the shallow body and copied to the deep articulation.
Detach restores the original properties. Offset mass recruits arm muscles, drops
a carrying shoulder, leans the trunk, shortens travel and widens the gait. No arm
swing is added to a carrying hand. Hanging loads swing about their handle; soft
attachments transmit damped angular loads. Rigid stowed attachments follow their
bone.

Retention is in newtons. Gravity, filtered acceleration, torque and contact
impulses contribute to the attachment load. Grip uses arm capability, muscle
tone, consciousness and vigor; straps use integrity. A two-handed grip can fall
back to a sound hand when the archetype permits it. The release reasons are:
voluntary, wrenched, grip failed, knocked out, death, breaking a fall, anchor lost,
strap cut, and hand or forearm damaged.

A strong severed hand remains in the loose item's geometry and mass. A weak
grip lets the hand and item fall separately. Loose items use a dedicated physical
debris pool without eviction or a kill plane. `nearby(point, radius)` returns
instances whose archetypes expose grasp sockets. Resetting a scene is explicit;
there is no pickup behavior or pathfinding.

## Actions and contacts

Action requirements name capability tags, free-hand occupancy, wield style and
minimum body capability. `MotionPlan::action_refusal` gives the reason an action
cannot start. Mass, practice and capability change its rate.

The new sets include reverse knife strikes; long-blade chops, slashes and thrusts;
one-handed club strikes; two-handed bat swings and jabs; guards, blocking,
overcommit recovery, fumbles and releasing a prop to break a fall. Every action
has a mirrored definition. `Brawler` chooses from these requirements, reach and
the character's capabilities.

Strike events name a contact feature. `StrikeTracker` samples its endpoints in
the physical pose, then traces the swept surface during the contact window.
Descriptors contain source identity, feature, impact class, point, direction,
relative speed, effective mass, energy, alignment and blocking state. A hit on
another prop consumes energy and loads its grip or strap before the body.

## Hosts and persistence

`CharacterDesc::loadout` supplies attachment entries. Pedestrian armed and civilian
shares default to zero and can be set with a logged command. Random choices use
the existing deterministic pedestrian seed.

The existing primary prop mesh/matrix slot is populated. Additional attachments
and loose items are one-matrix character views with the prop flag (64); they use
the same mesh and palette stream. Corpse records contain worn attachments.
`PropRegistry::record_loose()` saves loose geometry, state, IDs and velocities;
the game delta envelope stores this beside the core world delta. Older raw world
deltas still load. See [API.md](API.md) for wire formats and [WOUNDS.md](WOUNDS.md)
for damage.

The secondary grip is a point constraint between the two physical hands, with equal
and opposite impulses. Its orienter controls wrist alignment. Both solvers use the
same attachment path; core articulation records are version 2 and still read version 1.
