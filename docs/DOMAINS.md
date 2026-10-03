# Character test domains

`load_domain(Game&, DomainConfig)` builds a deterministic test world in the game library. It loads and bakes the voxel terrain before adding free objects. `Game::ensure_characters()` installs the normal character system and render host without requiring a city road network or spawning a pedestrian population.

The configuration contains a seed, terrain (`Flat`, `Stairs`, `Rough`, `Obstacles`, `Mixed`), difficulty in 0..1 and 0..32 loose objects. Courses use 12.5 cm cells, a clear starting area, and a finite 16 × 20 m footprint. Stairs have 50 cm treads and 12.5/25 cm risers. Rough patches have enough area for a sole. Mixed terrain includes a cycling native floor mover when objects are requested. Wooden crates enter through `Game::add_drop`, detach from their initial grids and become ordinary core pieces.

`anim::TravelState` is a small host helper for collision-aware root requests. It follows physical root motion during reactions, probes forward clearance and support, slows on risers, brakes before walls or unsupported edges, and limits prone turning. It is not a pathfinder. The motion plan still chooses individual footfalls. Its landing targets stop changing in the latter part of a swing, preventing a root crossing a riser from moving a foot's target abruptly.

The deep body is the correct choice for two-way contacts with dynamic world pieces. The shallow body is useful for comparing standalone animation and static-terrain contacts. The game still controls body policy; the helper does not silently change backends.

## Refinements exercised here

- Ground recovery retains bounded angular motion. Contact-patch torsion resists a grounded trunk spinning around its contact normal, while rising and prone targets blend at a bounded rate.
- Crawl recovery rechecks which side the body is lying on before choosing its roll, then waits for the physical trunk to turn over. Free palms take turns supporting the body at fixed surface points. A hip support does not push downward into a contact; this avoids pinning a one-arm crawl through added floor friction.
- Shallow position corrections obey the same rotation allowance as their reconstructed velocities. Joint dampers have finite muscle torque. A corrected wrist cannot change pose abruptly and then report a small spin.
- A prone foot lies along its instep instead of driving a vertical toe through the floor.
- Physical secondary grips also guide firearm support hands, except when an action deliberately moves that hand. A two-handed action retains its support. A persistently unreachable grip gives way or falls back to one hand where allowed.
- Projectile tissue drag uses the actual DDA chord and a finite energy budget. Root steering yields briefly after an impact instead of immediately cancelling the delivered momentum.

`tests/game/test_domain.cpp` covers roadless character ownership, stairs, physical drops, wall braking, prone turning and repeatable world hashes. The Foundry's `engine:fuzz` runs seeded terrain, actions, damage, recovery and world interactions on both bodies and records reproduction keys, motion metrics and tick cost. Its bridge streams the game's packed meshes to the game WebGPU renderer.

`tests/anim/test_crawl.cpp` checks alternating palm support, missing ground and physical shotgun recovery. The Foundry also runs shotgun recovery on all 35 authored builds in both backends and measures sustained two-arm/one-arm crawling through the native Game host (`engine:measure:ground`). These tests check actual trunk orientation and travel, not just a completed motion-plan transition. The randomized suite rejects sustained active crawling on the back.

The existing C API people test now aims through each rendered mesh's scaled head bounds. Its old unscaled rig-space brain point could miss a small or turned head after the stronger impact response. Death and API assertions remain unchanged.
