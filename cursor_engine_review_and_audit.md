# Engine review and audit
_Exported on 30/09/2026 at 3:22:59 CEST from Cursor (3.20.17)_

---

**User**

see this engine. please assess the feasibility of the below prompt request - how much is there left to do?

"""
please now do a deep review and audit and clean up, debug and harden the engine after all the changes we've made. In particular, I want to make sure the architecture is clean and that the systems are well decoupled, but we also consolidate all shared systems into the core. The core of this engine is still a destruction and structural integrity physics engine, that also allows for basically a fully physicalized world where we can have physicalized, machines, drivable breakable and damageable entities (such as the cars), rigid bodies that can be broken and have structural integrity, fully interact with the physicalized and destructible world which has the full structural integrity properties, damageable machines and cars, crumple physics, all fully driven as much as possible by the same core physics engine etc. Effectively, I am going for a engine in the style of teardown, but with the added structural integrity features and full streaming capability, so that we can simulate for example a city with cars, cranes, other machinery, that also interacts with the destroyable environment with full structural integrity physics, rigid bodies etc. I want the core physics engine to not have any domain leakage w.r.t. to the cars, machinery, but rather have a generic core that has all the building blocks to simulate this streaming, potentially procedurally infinite physicalized world. We may later choose to use it for a different game, such as a doom-style fps, or perhaps a real time strategy game, a life simulator, a gta-style game with drivable cars, a minecraft style adventure game, anything basically, anything that is grounded in a fully physicalized infinite streaming world with structural integrity. We want the performance to be strong and optimized, no memory leaks and overall strong memory management, a strong and well founded generic domain independent core physics architecture with structural integrity, fully physicalized objects that can be damaged, machinery, systems supporting player interaction etc. For example the cars are authored using this system, the features that make cars i.e. damage, crumple, lose parts etc are still generic to the engine core and can also be used in other systems or contexts - as long as they fit in nicely in the generic physics core methodology. 

Rendering should be kept reasonably decoupled for now so that we can iterate on it at some later point. Also, the actual procedural generation system will later be replaced, so also keep this a reasonably decoupled module. We will later (not now) add entity management systems, AI systems, gameplay systems etc - these are all separately authored modules we will design later at nother point on top of this core engine.

The reference for the structural integrity behavior and performance is the "oriented_1" branch, but this current branch may have changed aspects of the core engine because we added car and driving physics, damage systems such as crumples, losing parts etc. These features, if not already, should be come also core mechanics of the core engine, if they fit in nicely into the core methodology. I want you to make sure we didn't regress any behavior we had in "oriented_1", especially regarding structural integrity and destruction and machinery physics. There may still be some bugs you find along the way, do fix them. This newer branch may even have added improvements, optimizations and so forth. Preserve these - in case it was necessary to introduce lossy optimization, make sure it is parametrizable so we can later still scale back to the better non-regressed quality behavior when we have stronger hardware. If the current branch "cars" is an actual improvement over "oriented_1", then of course do not regress back to "oriented_1". We essentially want this to be a full lossless improvement, refinement and hardening pass. The goal is to have a strong well architected core engine so that we can later still independently iterate on a specific domain. We will at some point also want to merge in the "euphoria_3" branch which is a euphoria style animation system for human npcs, which will populate the city alongside the cars. This should also cleanly integrate into the engine, and the animation system should also be a reasonably decoupled module so we can still iterate on it separately to improve animations in a later pass. It is improtant that the core physics didn't regress vs "oriented_1", that any improvements this branch may introduced are kept and that also "euphoria_3" integrates cleanly and well. I am fine with any deep changes you make to the engine, you may even completely reprogram it from scratch if you deem it necessary, as long as we don't have any regressions relative to "oriented_1", and we want to make sure the driving and crashing physics also integrate nicely. As mentioned - the core of the engine should be domain agnostic, but have all the core machinery and building blocks in place for destruction, structural integrity, overall physical manipulation and interaction with the world and streaming for infinite procedural worlds. In case euphoria-style animation support for NPCs (and animation for any entities for that matter) also requires features in the core engine so it integrates well, do so. Cars, driving, the current animation style flavor for NPCs are all authored systems on top of our core engine which holds all the relevant core building blocks. The core engine should be clean, well architected, performant and methodologically clean and unified as far as possible, without hurting performance. Please fully implement this until everything I requested is integrated and the engine has become fully hardened and prudction-grade, so that we can start to later iterate on authored systems and content.
"""

---

**Cursor**

I'll dig into the repo structure, branches, and coupling before judging feasibility.

## Bottom line

Most of what that prompt asks for is already done. This isn't a messy codebase that needs a rescue pass — it's a 34k-line C++20 core with enforced layering, zero `TODO`/`FIXME`/`HACK` markers, 177 passing tests across three suites, bit-identical determinism across threads/ARM/x86/WASM, and documented memory bounds per state kind. The prompt is written as if addressing a codebase with real rot, and it would hand the agent an open-ended license ("you may even completely reprogram it from scratch", "fully hardened and production-grade") that on *this* code is almost entirely downside risk.

The genuinely remaining work is a short, specific list of maybe 1–2 weeks — plus one item (euphoria_3) whose cost swings between one week and two months depending on what you actually mean by "integrates cleanly."

## Ask-by-ask status

| Request | State | Left to do |
|---|---|---|
| Clean architecture, decoupled systems | Largely done | Split the `World` god class |
| Consolidate shared systems into core | Largely done | Dedupe the TS mock engine |
| No car/machinery leakage in core | **Partial** | ~1–2 days of moves + renames |
| No regression vs `oriented_1` | Low risk, **unproven** | Build a cross-branch baseline |
| Lossy optimizations parametrizable | **Already done** | Nothing |
| Rendering decoupled | Already done | Nothing |
| Procgen decoupled | Already done | Optional: own CMake target |
| Performance + memory management | Strong | CI gates (ASan, soak, perf) |
| `euphoria_3` integration | **Not started** | 1 week to 2 months — see below |

## What's actually already there

The four-library split (`svx_core` → `svx_mesh`/`svx_env` → `svx_game` → web) is real and enforced, not aspirational. I grepped for downward includes and layering violations and found none: the core includes nothing from `game`/`env`/`mesh`, and no outer layer reaches into core private headers like `world_internal.hpp`. `svx_env` (fire, smoke, water) is built purely on the five public extension points — voxel layers, damage, loads, piece forces, and `WorldSystem` — which is exactly the "generic core with building blocks" pattern the prompt asks for. The core even installs as a standalone CMake package.

The "parametrizable lossy optimization" requirement is already satisfied. Every speed-up the `cars` branch added that trades quality is a named tunable you can dial back: `load_trigger_gap` (throttles re-solving a large structure under a creeping load), `crumple_check_gap` (fracture re-check cadence while a car folds), and `plastic_hinges`. And the optimizations that claim losslessness appear to actually be lossless — the fragment-labelling rewrite in `core/src/frag/fragments.cpp` precomputes Voronoi seeds into a table and prunes candidates on a conservative bound (`e2 > best`, valid because the seam noise is never negative), so labels come out bit-identical.

## The four real gaps

**Domain leakage is narrower than you'd fear, but it's there.** The clearest violation is in the core's material presets:

```41:50:core/include/svx/material/material.hpp
  SteelSection,  // a rolled steel section (HEB 200 as 2 x 2 voxels): 980 kg/m^3, 44 MPa, ductile
  Sheet,         // car body: pressed sheet steel and its stiffeners, smeared (crumples)
  CarFrame,      // a car's frame rails, floor pan and pillars (crumples, stiffer)
  Engine,        // an engine block and gearbox (solid, heavy: it does not crumple)
  Window,        // a car's glazing: 5 mm glass smeared over a voxel (light, shatters)
  Tyre,          // rubber on a rim (a wheel that came off)
  Plastic,       // bumpers, trim, lamps' housings (crumples, light)
  Asphalt,       // road surface (anchored ground)
  Paint,         // road markings (anchored ground)
  Lamp,          // head and tail lamps' glass (shatters)
```

These are mirrored in the C ABI as `SVXC_CAR_FRAME`, `SVXC_SHEET` and so on. Since materials are already registerable per-world (`register_material`, `World::materials`), these belong in `svx_game`'s startup, not in the core's standard preset enum. Moving them renumbers material ids, which breaks saved deltas — fine pre-release, but it needs a version bump.

The wheel subsystem (`World::add_wheel`, `WheelDesc`, `WheelState`, `WheelDetached`, `core/src/phys/wheel.cpp`, `core/src/world/world_wheels.cpp`) is a different case. A cast-wheel-on-suspension constraint is a legitimate generic primitive — Bullet and PhysX both ship one — and it's genuinely useful for cranes and machinery, so I'd keep it in core. What leaks is vocabulary, not structure: the docs say "chassis" everywhere, and `WheelDesc::group`/`tag` are documented as "which vehicle it belongs to." Same for `world_crumple.cpp`, which is actually generic (it keys entirely off the material's `crush` pressure) but is written as a car-crash feature. This is a rename-and-redocument job, not a rewrite.

**The `World` class is the one substantial architectural debt.** It's 257 public and private methods, with all private state declared in a 1,122-line public header, implemented across eight files totalling ~8,500 lines — grids, structures, pieces, joints, wheels, crumpling, streaming, persistence, sessions, and queries all in one class. It's disciplined internally (sectioned by concern, one file per area) but it is a monolith, and it's the thing standing between you and "well decoupled systems." Breaking it apart is also the single highest-regression-risk change on the list, which is why I'd sequence it last and gate it on a baseline.

**Nothing proves the absence of regression against `oriented_1`.** The good news is that the history is linear — `git merge-base oriented_1 cars` is exactly `oriented_1`'s HEAD, so nothing diverged; `cars` is 16 commits strictly on top. But the core changed meaningfully in those commits (`rigid.cpp` +428 lines, `world.cpp` +282, `fragments.cpp` +102, `mesher.cpp` +119), and there's no pinned golden-hash or behavioural baseline. The tests check internal self-consistency (same hash across thread counts, across tight and loose budgets) but nothing compares *this* branch's tower collapse to *that* branch's. Building that harness — run the `svx_engine_demo` scenarios and `svx_env_bench` on both branches, diff piece counts, break counts, session hashes and tick timings — is maybe half a day and should be the first thing done, before any refactor.

**CI is thin relative to "production-grade."** It builds native + WASM and runs `ctest`. It does not run the web typecheck or tests, the browser smoke scripts (`drive-wasm.mjs`, `tower-wasm.mjs`, `machines-wasm.mjs` and friends all exist and are unused by CI), `svx_soak`, or any sanitizer — there's a `build/asan` directory but no ASan preset in `CMakePresets.json`. There's also no performance regression gate despite `svx_env_bench` printing exactly the numbers one would gate on. This is the cheapest high-value work on the list.

## euphoria_3 is the item that decides the estimate

This is where the prompt is quietly asking for something much bigger than it sounds. The facts:

`euphoria_3` forked at `9121a6a`, roughly 30 commits back — before the memory-bounding work, before materials, before fire/smoke/water. It touches **zero C++**. It adds ~22,700 lines of TypeScript: a standalone `anim/` package (its own `package.json`, its own tests) plus `web/src/actors/` and a lab app. Textual merge conflicts are limited to nine files, all under `web/` (`game.ts`, `weapons.ts`, `renderer.ts`, `occupancy.ts`, `hud.ts`, `settings.ts`, `styles.css`, two READMEs).

The catch is what's inside it. `anim/src/physics/rigid.ts` is a 1,274-line XPBD articulated-body solver with its own joints, muscles, contacts, and sleep, and `anim/README.md` states plainly that it "doesn't depend on the physics engine or the renderer" — hosts hand it a `CollisionWorld` interface. It is, in other words, a second physics engine.

So "integrates cleanly" resolves two very different ways. The shallow merge is about a week: resolve nine conflicts, implement `CollisionWorld` over the engine's `collide`/`raycast`, done. But then NPCs run in a separate solver, in JS float64 on the main thread, outside the core's determinism and replay hashes, invisible to the streaming archive, and unable to load a structure — an NPC can't push a wall over, and a collapsing slab only affects it through the ad-hoc `debris.ts` path.

The real integration means adding articulated-body support to the core and porting the ragdoll layer onto it. The core is perhaps 60% of the way there already: it has Ball, Hinge, Slider, Rope, Rod and Fixed joints with limits, drives, breakage, sleep, and jointed-wake propagation. What's missing is swing-cone-plus-twist limits on ball joints, angular servo drives with torque limits (today's drives are hinge/slider only), soft force-limited attachments and orienters, and per-substep external force and torque (`apply_force` is currently next-tick-only). Then port `anim/src/physics` plus `body/humanoid.ts` and `character.ts` — roughly 3,000 lines — onto those, leaving the motion-plan and behaviour layers (~7,000 lines) as the decoupled authored module you keep iterating on. That's 4–8 weeks, and it's a genuine research-flavoured risk: XPBD ragdolls and sequential-impulse voxel bodies have different stability characteristics, and the core's no-transcendentals determinism constraint applies.

## How I'd sequence it

Don't run the prompt as one task. It bundles a finished audit, a bounded cleanup, and an open-ended research project, and the "reprogram from scratch if necessary" clause invites an agent to destroy working code in pursuit of tidiness.

I'd split it into four passes: first the baseline harness and CI gates (~2 days, zero risk, and it protects everything after it); then the domain-leakage cleanup and CMake module split (~2–3 days, mechanical, verifiable against the baseline); then the euphoria merge at whichever depth you choose; and only then, if you still want it, the `World` decomposition.

One thing to flag about what I measured: the test binaries I ran are from Sep 29 04:35 and predate your last six commits, including "Car parts on breakable joints" from ~01:42 today. 103 core, 32 env, and 42 game tests passed on that build, but the sandbox is read-only so I couldn't rebuild at HEAD to confirm.

**Scoping the audit**

> **How deeply should euphoria_3 integrate? This is the single biggest driver of the estimate.**
> - Real integration (Recommended): add articulated bodies (cone/twist ball joints, angular muscle drives, per-substep forces) to the core, port the ragdoll onto it, keep motion/behaviour as a TS module — 4-8 weeks ✔
> - Shallow merge: keep the TS XPBD solver, adapt CollisionWorld over the engine, resolve the 9 web conflicts — ~1 week, but NPCs stay outside determinism, streaming and structural loading
> - Defer euphoria entirely for now; do the audit and cleanup passes first so the core is stable before merging

> **Do you want the `World` god-class decomposition (257 methods, ~8.5k lines, all private state in the public header) in this pass?**
> - Yes, but only after the oriented_1 baseline harness exists to prove no behaviour changed (Recommended) ✔
> - Yes, treat it as the headline task of the audit
> - No — it works and is well sectioned; leave it and spend the time on euphoria and CI hardening

> **How should I treat the prompt's 'you may reprogram it from scratch / fully hardened and production-grade' license?**
> - Drop it (Recommended): every change must be baseline-verified as behaviour-preserving, no speculative rewrites ✔
> - Keep it as written — allow deep restructuring where the agent judges it warranted

With those three choices, here's the answer to your original question: **roughly 6–10 weeks of work remains, and about three quarters of it is the euphoria integration.** The audit-and-cleanup part of the prompt — the part that reads like the main ask — is about a week and a half, because the engine is already in the shape the prompt is asking for.

Here's how the four passes fall out.

**Pass 1 — baseline and gates (~2 days, zero behavioural risk).** This has to come first because you chose the gated and conservative options, and everything downstream depends on being able to prove a change was behaviour-preserving. Build a harness that runs the `svx_engine_demo` scenarios (`tower/pillars`, `tower/side`, `tower/core`, `machines`, `angles`, `yard`) plus `svx_env_bench` on both `oriented_1` and `cars`, and diffs session hashes, piece counts, break counts, awake-speed profiles and tick timings. That gives you the regression evidence the branch currently lacks, and it doubles as the gate for passes 2 and 4. While you're there, wire the things CI already has but doesn't run: an ASan/UBSan preset in `CMakePresets.json`, `npm run typecheck && npm test` for the front end, the browser smoke scripts, `svx_soak`, and a perf threshold on `svx_env_bench`.

**Pass 2 — domain de-leaking (~2–3 days, mechanical).** Move the vehicle material presets (`Sheet` through `Lamp`) out of `MaterialId` and the `SVXC_*` mirror into `svx_game`'s startup via `register_material`, bump the delta format version for the id renumbering, and re-vocabulary the wheel and crumple subsystems from "chassis/car" to generic terms while leaving their structure in core. Optionally split `procgen.cpp`/`city.cpp`/`drive_city.cpp` into their own CMake target so the "procgen gets replaced later" plan is a target swap rather than a file surgery. Every step here is verifiable against pass 1's baseline, since none of it should change a single hash except where material ids appear.

**Pass 3 — euphoria, the real version (4–8 weeks, the actual project).** Core work first: swing-cone-plus-twist limits on `JointType::Ball`, angular servo drives with torque limits (today `JointDrive` only serves hinges and sliders), soft force-limited attachments and orienters, and per-substep external force/torque to replace the next-tick-only `apply_force`. All of it has to respect the no-transcendentals rule that gives you bit-identical results across ARM, x86 and WASM — that constraint is the main source of schedule risk, along with the fact that `anim`'s XPBD solver and the core's sequential-impulse solver have genuinely different stability behaviour for long light-to-heavy chains. Then port `anim/src/physics`, `body/humanoid.ts` and `character.ts` (~3,000 lines) onto the core primitives, and keep the motion-plan and behaviour layers (~7,000 lines) as the decoupled TS module you go on iterating. The merge itself is the easy part: nine conflicting files, all under `web/`.

**Pass 4 — `World` decomposition (1–2 weeks, gated).** Extract streaming/persistence, joints and wheels, and pieces/fracture into collaborating subsystems behind a pimpl, so the public header stops carrying private state. Do it last, one subsystem at a time, with pass 1's hashes green after each step.

Two things worth deciding before pass 3 starts. First, whether NPCs need to participate in replay determinism and the streaming archive from day one or can land in a second increment — that choice moves the estimate several weeks by itself. Second, whether a character's limbs are voxel bodies (consistent with `anim/src/voxel`, and it gets you voxel-exact wounds and severing through the existing fracture path for free) or sphere-collider proxies bolted onto the core; I'd push for voxel bodies, but it's a real fork in the design.

I'm in Ask mode, so I've scoped rather than started. Say the word and I can switch to Agent mode and begin with pass 1's baseline harness.

---

**User**

in your view, will the deep euphoria port hurt performance? could it be better for performance to not consolidate them? what are the trade offs of deep vs shallow?

---

**Cursor**

Good question — let me pull the actual numbers from both solvers before answering, since the answer hinges on substep counts, threading, and where each runs.

## Short answer

Yes, the deep port will probably make frame times *worse* at today's scale, and there's a real performance case for leaving them separate. But that case has a ceiling, and you hit it at roughly 20–30 physical NPCs. Below that, shallow is faster; above it, shallow can't get there at all.

The numbers matter here, so let me start with them. From `docs/ANIM.md` on euphoria_3, measured on an M5 Pro: a physical body costs about **90 µs per frame** (plan, behaviours and 8 substeps of 16 rigid bodies), a plan-only character about **10 µs**, and 56 characters in the streamed city with about 20 of them physical, in full combat, averages **3.5–5.5 ms per frame**. That is already a quarter to a third of a 16.6 ms budget, on fast hardware, at only 20 physical bodies.

## Three reasons the deep port costs you performance

**They currently run on two threads in parallel, and consolidating serializes them.** The core runs in a WASM worker with pthreads; `anim` runs in TypeScript on the main thread. Those 3.5–5.5 ms are spent concurrently with the worker's physics tick, not added to it. Folding ragdolls into the core puts them behind structure solves and collapse physics in the same tick.

Your own code documents exactly why that's bad:

```1:8:web/src/game/occupancy.ts
/**
 * Client-side solid occupancy (protocol `occupancy` messages): one bit per voxel for every
 * resident chunk of the world grid and of the oriented grids (in their lattices, placed by their
 * frames: engine/gridframes.ts), and the rigid pieces' voxels where they are now
 * (engine/pieces.ts), so the player's collision sweeps run on the main thread with exactly the
 * engine's `collide` rules and never wait for a busy worker (structure solves and a collapse's
 * pieces can hold the worker for tens of milliseconds per tick).
 */
```

Today, when a tower collapse blows the worker's tick budget to tens of milliseconds, NPCs keep animating smoothly at display rate on the main thread. After consolidation they hitch with the physics — and it hitches worst precisely during the spectacular moments the engine exists for. That's a quality regression an average frame time won't reveal.

**The substep rates differ by 4x, and it's not an accident.** The core runs `substeps = 2` per 1/60 tick (so 120 Hz) with 10 velocity and 4 position iterations, dropping to one substep and 6 iterations in busy mode. The anim solver runs `maxSubstep = 1/480`, so 8 substeps per tick with a single constraint pass each. That's XPBD's whole premise: many tiny substeps rather than many iterations is what keeps a stiff chain with large mass ratios — a hand on an arm on a chest — from going soft or exploding. You have three ways out and none is free. Running ragdolls at the core's 120 Hz gives you softer, less stable bodies, which is the exact quality regression the prompt forbids. Raising the global substep rate to 480 Hz multiplies the cost of *all* destruction physics by four, which is a non-starter. The right answer is per-island substepping, with ragdoll islands at 1/480 and rubble at 1/120 — and that is the single hardest piece of the port, because the moment a falling slab lands on an NPC, they are one island spanning both rates.

**The core charges a determinism tax the TS version doesn't pay.** The core compiles with `-ffp-contract=off` and no fast-math, uses no transcendentals on the simulation path (it computes `ln x` via an atanh series in `phys_internal.hpp` rather than calling `std::log`), and schedules its parallel work deterministically. The anim solver calls `Math.sin` and `Math.cos` freely. Every muscle drive and cone limit you port pays that tax. Per operation it's modest, but it's a genuine slowdown relative to the TS original and a meaningful share of the engineering effort.

## What the deep port buys, and where the crossover sits

The offsetting win is that the core is native, SIMD-enabled, and multithreaded over a pthread pool, against single-threaded JavaScript. Per body it should be several times faster. Extrapolating your own measurement: 100 physical NPCs at 90 µs each is around 9 ms per frame single-threaded, which does not fit in a 60 fps budget alongside a renderer. The same load spread across four to eight worker threads with SIMD plausibly does.

So the crossover is somewhere around 20–30 physical bodies. A Teardown-style scene with a handful of NPCs is better off shallow. A GTA-style city crowd is only reachable deep.

## The part that isn't about performance at all

The stronger argument for deep was never speed. It's that today an NPC **cannot load a structure**. A crowd can't collapse a floor, a body can't be pinned under rubble that's then shot away, a ragdoll can't shove a car. `debris.ts` fakes the one direction that matters most — falling rubble striking people — and nothing goes the other way. NPCs are also outside `session_hash`, so replays and lockstep don't reproduce them, and they're outside the change archive, so they don't persist out of range the way vehicles do. If any of that is load-bearing for the game you're building, the performance question is secondary.

One correction to something I implied earlier: consolidating doesn't let you delete the client occupancy mirror, since the player's collision needs it regardless. That memory and bandwidth stays either way unless the player controller moves into the core too.

## What I'd actually recommend

There's a middle path that I think dominates both options you were choosing between. Add the articulated-body primitives to the core — cone and twist limits on ball joints, angular muscle drives with torque limits, per-substep forces — so NPCs *can* be core bodies, but keep the TypeScript path as a level-of-detail tier rather than replacing it. The system already has LOD built in: `physics = false` runs a calm character on its plan alone for 10 µs, and settled corpses sleep for free. So you'd run core articulated bodies for NPCs near the player and anything actually interacting with destruction, and the TS path for the crowd at distance.

That gets you bidirectional coupling exactly where it's visible, avoids paying the 4x substep problem for the majority of bodies, keeps the animation system independently iterable as you wanted, and lets pass 3 ship incrementally instead of as an eight-week big bang.

Whichever way you go, I'd spike per-island variable substepping first — about a week. If that doesn't work cleanly inside the core's sequential-impulse solver, the deep port's quality story collapses and you'd want to know that before committing six more weeks to it.

**Revising the euphoria approach**

> **Given the performance analysis, do you want to revise the euphoria plan from pass 3?**
> - Switch to the hybrid (Recommended): core articulated bodies as the near/interacting tier, TS anim retained as the distance LOD tier — incremental, keeps the parallelism where it pays
> - Keep the full deep port: all NPCs become core bodies, accept the worker serialization and solve per-island substepping properly
> - Drop back to shallow after all: merge as-is, accept that NPCs stay cosmetic w.r.t. destruction, revisit when crowd scale demands it

> **Should the per-island variable substepping spike (~1 week) run before committing to the port?**
> - Yes, spike it first and let the result decide the architecture (Recommended)
> - Yes, but only after passes 1 and 2 (baseline + de-leaking) are done
> - Skip the spike, commit to the design and solve it inline during the port

