# structvox v2: the physical method

v1 simulated structural integrity with a deformable per-voxel lattice (RBSM): implicit dynamics
inside event "bubbles", a telescoping composite with a linear far field, a compliance knob S_p = 9
and heavy mass damping for the XPBD "jelly" look. Detached pieces became rigid debris that could
not break any more, never touched each other, and faded away after a few seconds.

Play-testing showed where this stops working:

- **Towers bend instead of crumbling.** A structure spanned by one bubble is mostly coarse
  aggregates with linear kinematics. Failure is decided only on the few thousand fine cells that
  the nomination budget allows. The large rotation of a toppling tower is mechanically outside
  the model, and the damping (α = π) slows every tipping mode.
- **Falling parts never break.** A detached piece is an unbreakable rigid body, so a toppling
  tower, once released, lands in one piece and fades out.
- **There is no rubble.** Debris has no debris-debris contact, puts no weight on the structure,
  and is removed after about 3 s asleep, with a hard cap of 192 bodies.

v2 replaces the mechanics with one method that works the same way for standing structures and
for falling ones (the reference: Teardown's author's structural-integrity clips, 2026). A tower
that loses its ground columns now fails at its base, comes down storey by storey, breaks up in
the air and where it lands, and ends as a pile of slab plates, wall panels and blocks that
settles and sleeps.

## 1. Fragments and bonds

- **Voxels** (12.5 cm) are grouped into **fragments**, deterministically from the chunk content:
  a jittered Voronoi partition per material (seed spacing per material, e.g. 4.5 voxels for
  reinforced concrete, 3 x 2 x 1.6 for masonry), split into 6-connected components.
  - Fragments are pre-scored rubble pieces: the smallest rubble there is. A fragment never
    breaks; only carving removes voxels from it.
  - Fragments never cross chunk borders, so any chunk can be re-fragmented on its own
    (streaming, persistence).
- **Bonds** join adjacent fragments, one bond per fragment pair. Its section is the set of shared
  voxel faces projected on the mean face normal: area, centroid, second moments, extreme-fibre
  distances, lever arms from both fragments' centres.
  - Stiffness follows the rigid-body-spring model (RBSN): the two half-lengths in series, E and
    G per material. It only distributes load (an equilibrium solve); it is never seen as
    deformation.
  - Capacity comes from interface strengths: direct tension, flexural tension (the rebar reserve
    of reinforced concrete), crushing, and Mohr–Coulomb shear (cohesion + friction). A design
    strength class multiplies it (§3, design pass).
- **Persistent state is the voxel grid**: materials, a broken bit per voxel face, design classes.
  Fragments and bonds are caches derived from it.

## 2. Stress: one elastic equilibrium solve per body

Every connected set of fragments is either a **static structure** (it reaches an anchor, such
as ground or rock) or a **rigid piece** (free, simulated). Both get their stress from the same
linear-elastic equilibrium on the fragment graph:

    K u = f        (6 DOF per node: translation and rotation)

- **Static structures.** f = gravity, plus the contact forces of pieces resting on or striking
  them, plus blast impulses. Anchor bonds are fixed supports.
- **Pieces.** f = contact forces plus inertia, −m_i (a + α × r_i + ω × (ω × r_i)), where (a, α)
  is the piece's rigid acceleration under the same forces: self-equilibrated; one node (the one
  nearest the centre of mass) is pinned. A toppling tower fails in bending under the inertial
  load of its rotation; a landing slab under its contact forces.
- **Resolution.** Structures and pieces of more than 2,500 fragments are solved on clusters of
  fragments (chunk-local cells of 1 m, 2 m beyond 15,000). Their cracks follow cluster seams, and
  their parts break finer once they are small enough to be solved per fragment.
- **Solver.** Preconditioned conjugate gradients with smoothed-aggregation multigrid:
  rigid-body-mode prolongation, parallel Galerkin products, a hybrid Gauss–Seidel smoother over
  fixed 512-row blocks, a float32 SIMD V-cycle and a dense Cholesky on the coarsest level.
  - Static structures converge over ticks under a work budget (stress spreads through a building
    over a few frames) and are patched in place: removed bonds are subtracted from K, detached
    nodes retired, new fragments appended.
  - Pieces are solved within the substep (exactly, for small ones).

## 3. Failure of static structures

- A bond's utilization φ is the largest of the tension, crushing and shear checks on the fibre
  stresses N/A ± M/S, divided by the fragility knob.
- **Break rounds.** On a converged state, a round breaks the bonds with φ ≥ max(1, 0.85 · φ_max)
  and at least the worst quarter of those over strength (at most 256), then solves again: a
  cascade unfolds over ticks.
- **Sudden changes** (a carve, a blast, a break round) are judged with a dynamic increase factor
  on the change of bond force since the last converged state, F_old + DIF · ΔF.
- **Detachment.** After a round, what no longer reaches an anchor leaves the grid as a rigid
  piece, with its bonds (unbroken faces), at rest or with a blast's impulse.
- **Design pass** (bake): every structure is solved under its own weight, and members above a
  utilization of 0.45 are strengthened, so what stands at load time stands at rest.
- **Streamed worlds are designed on first touch.** Chunks fresh from the generator are
  undesigned. The first time a structure reaching them is extracted (a shot, a blast, a piece
  landing on it), the chunks around it are generated first (it must not stand on chunks that are
  not there yet); then, if it is intact, it is designed like the bake does it, and that state is
  the reference of later sudden changes. Events design what they will hit before they hit. An
  untouched chunk that is evicted and generated again is designed again when touched (the
  result is the same).

## 4. Rigid pieces

Each piece owns a voxel shape in its own grid-aligned frame, its fragments and its bond graph.

- **Contacts.** Surface samples (inset corners of exposed faces; more for larger pieces) are
  tested against the world grid and other pieces' shapes. Normals come from the face of least
  penetration leading to air. A pair keeps a manifold of 12 contacts, plus 8 per metre of a
  piece's radius (a large piece rests on a bearing surface, not on a few points).
- **Solver.** Sequential impulses (projected Gauss–Seidel): warm starting, Coulomb friction,
  restitution for fast impacts only, split-impulse position correction (the impulses stay true
  forces for the fracture layer). A squeeze guard keeps a light piece pinned between heavy ones
  from leaving faster than its partners.
- **Substeps.** Two a tick (10 velocity, 4 position iterations). *Busy* (more than 150 pieces
  faster than 2 m/s, the violent part of a collapse, or more than 6,000 contacts, a large pile
  settling): one substep, 6 and 2 iterations. Both are functions of the state alone.
- **Sleep.** A piece touching something sleeps once its smoothed speed stayed below the sleep
  speed for 0.25 s (0.15 m/s, or 1.2 g dt if more: what gravity adds in a substep is what an
  unconverged solve leaves). Rubble (pieces under 1.5 m) moving slower than 0.9 m/s loses 20 %
  of its speed per 1/120 s: rubble is rough, and piles settle within seconds. A large piece
  toppling slowly is not held. An awake piece moving near a sleeping one wakes it, as do carves
  and blasts nearby.
- **Coupling to structures.** Contact impulses on world voxels load the fragment they touch:
  impacts as sudden load cases (pancake collapse of floors), resting pieces as dead loads.
- **Budget.** Beyond 3,000 pieces the smallest sleeping ones fade out. Pieces that fall off the
  world are removed.

## 5. Fracture of pieces

A piece is checked when something happens to it, after its contact solve in the substep:

- **Triggers.**
  - *Impact*: a contact closes faster than rubble jostling (1.5 m/s; up to 4 m/s for pieces
    under 1.5 t) with forces over 1.8 × the piece's weight.
  - *Resting*: its support forces changed by half its weight or more (a first landing, rubble
    shifting under it).
  - *Spin*: centripetal load over 2 g at its rim.
- **Loads.**
  - A resting contact (closing no faster than gravity makes it within a substep or two) carries
    a steady force, J / dt: the piece's weight and what rests on it, in full.
  - An impact: a rigid contact stops a piece within one substep, but a real impact takes the time
    a stress wave (slowed by crushing) needs to cross it. Impact impulses load a piece over
    τ = max(dt, 2r / 400 m/s): large pieces crush progressively from where they hit instead of
    feeling a uniform deceleration of tens of g.
  - The rigid solver's contact forces are one of many statically admissible answers and may put
    a piece's whole weight on a few points. The check uses the elastic answer instead: the same
    net force and moment shared over the contact points as by a rigid body on equal springs
    (least squares, f_c = u + θ × r_c).
- **Break rounds: a progressive failure within the substep** (steps of a sequentially linear
  analysis).
  1. Solve, then break the worst bonds: those within 0.85 of φ_max and at least the worst
     quarter of the overloaded ones.
  2. Chips crushed off where the piece struck (parts under 4 % of its mass) pass their load on to
     the piece through the crack (crushed material in between still transmits it). They leave
     the solve, and the loads redistribute.
  3. Repeat, up to 8 rounds, until nothing is overloaded, the impact's energy is spent, or a
     part of some size comes apart. (Pieces of more than 800 fragments are solved on fragment
     clusters, 1 m cells; they break finer once smaller.)
- **Separation.** The parts of a piece broken in a collision do not touch each other for the rest
  of that substep: the failed interface carried its strength and then no more. The contact step
  is solved again with the new pieces (only they are collided afresh), so the part above a
  failed storey keeps falling and meets what is below in its own collision. Collapse and breakup
  proceed through collisions, storey by storey and crack by crack, instead of one overloaded
  solve pulverizing everything at once.
- **Energy.** An impact pays for its cracks from the kinetic energy its contacts take out of the
  collision (½ J v): a crack costs the material's fracture energy × area, crushing 20 × that.
  Rubble cannot grind itself down, and a hard landing shatters only what it overloads.
- **Dust.** Fragments on the lighter side of a bond that failed by crushing turn to dust (the
  space they held opens). Parts under 16 voxels become dust events (particles in the front end)
  instead of rigid pieces. Pieces under 8 fragments never break further.

## 6. Determinism and threads

Every parallel section splits its work into fixed chunks that do not depend on the thread count
and joins results in a fixed order:

- contacts per body and per pair, the broad-phase sweep in fixed runs;
- Gauss–Seidel by colouring pair manifolds: a colour's manifolds share no awake piece, so they
  are solved concurrently, bitwise the same in any order; manifolds beyond 24 colours are
  solved after them, in order;
- pieces' stress checks run concurrently, each touching only its piece; their stats, events and
  splits are applied in body order;
- the multigrid's parallel products and smoother blocks.

A session replays bit for bit on any thread count (tested on 1 and 4 threads).

Tower collapse (the engine demo's "pillars" scenario: 920k voxels, 200k of them coming down,
about 1,600 pieces at rest, asleep by ~12 s): rigid work at the peak about 10–14 ms a tick
natively on 8 threads; in the browser (WASM, 8 threads) the peak runs at 20–35 ticks a second
on a busy machine, so its heaviest seconds play slower than real time.

## 7. What is kept from v1

The voxel grid, chunks and streaming, procedural and Doom worlds, sector movers, meshing,
persistence, command logs, the C ABI and the WebGPU front end. Pieces use v1's pose protocol:
an event carries a piece's mesh, poses follow. A piece that splits is released and its parts are
announced as new pieces.

## 8. Knobs

Runtime (`EngineParams`, the front end's settings):

| Knob | Default | Meaning |
|---|---|---|
| fragility | 1 | divides every strength (more collapse) |
| impact | 1 | scales contact loads on structures and pieces (impact severity) |
| dynamic increase factor | 1.5 | overshoot of sudden load changes on structures |

Configuration (`EngineConfig`, `RigidParams`, the material table):

| Knob | Default | Meaning |
|---|---|---|
| fragment size | material table | rubble size and stress resolution |
| cluster_nodes | 2,500 | above: structures and pieces are solved on fragment clusters |
| max_breaks_per_round, break_band | 256, 0.85 | cascade pace of structures |
| stress_work | 4 M | solver work per tick (how fast stress spreads through structures) |
| impact_rounds, impact_round_fraction | 12, 0.15 | a piece's break rounds per check |
| impact_chip_fraction | 0.04 | parts lighter than this are crushed chips |
| impact_wave_speed | 400 m/s | how long an impact takes to load a piece |
| crush_energy | 20 | crushing cost relative to a crack |
| min_body_voxels, min_fracture_frags | 16, 8 | dust below; unbreakable rubble below |
| max_bodies | 3,000 | beyond: the smallest sleeping pieces fade |
| substeps, iterations | 2, 10 | rigid solver |
