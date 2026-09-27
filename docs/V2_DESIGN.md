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

v2 replaces the mechanics with a single method that works the same way for standing structures
and for falling ones, as in the reference clips (Teardown's author, 2026).

## 1. Fragments and bonds

- **Voxels** (12.5 cm) are grouped into **fragments** by a deterministic function of the chunk
  content: a jittered Voronoi partition per material, split into 6-connected components.
  - Fragments are pre-scored rubble pieces of about 8 to 60 voxels.
  - A fragment is rigid and never breaks. Only carving removes voxels from it.
  - Fragments never cross chunk borders, so any chunk can be re-fragmented on its own. That is
    what makes streaming and persistence work.
- **Bonds** join adjacent fragments, one bond per (fragment pair, axis). A bond's section is the
  set of shared voxel faces: area, centroid, second moments and extreme-fibre distances.
  - Stiffness follows the rigid-body-spring model (RBSM): each fragment's half-length is in
    series, with E, G and per-material values.
  - Capacity comes from material strengths: tension, flexural tension (the rebar reserve of
    reinforced concrete), compression, and Mohr–Coulomb shear with cohesion and friction.
  - A design strength class multiplies the capacity.
- **Persistent state is the voxel grid**: materials, a broken bit per voxel face, and design
  classes. Fragments and bonds are caches derived from it. A bond is broken when its faces are
  marked broken.

## 2. Stress: one elastic equilibrium solve per body

Every connected set of fragments is either a **static structure** (it reaches an anchor, such
as ground or rock, and does not move) or a **rigid body** (free, simulated). For both, stress
comes from the same linear-elastic equilibrium on the fragment graph:

    K u = f        (6 DOF per fragment: translation and rotation)

- **Static structures.**
  - f = gravity plus the contact forces of bodies resting on or hitting the structure, plus
    blast impulses.
  - Anchor bonds are fixed supports.
- **Rigid bodies.**
  - f = gravity, plus contact forces, plus inertia: −m_i (a + α × r_i + ω × (ω × r_i)), where
    (a, α) is the body's rigid acceleration under the same forces. This load is
    self-equilibrated, and one fragment is pinned.
  - This is how a toppling tower fails in bending (the inertial load of its rotation), and how
    a falling slab shatters where it lands (contact force = impulse / dt).
- **Solver.**
  - Preconditioned conjugate gradients with an aggregation multigrid: rigid-body-mode
    prolongation, block-Jacobi Chebyshev smoothing, and a dense coarse solve.
  - The solve is warm-started from the last state.
  - Large static structures converge over several ticks under a per-tick work budget, so
    stress spreads through a building over a few frames. Bonds are judged only on a converged
    state.

## 3. Failure

- A bond's utilization φ is the largest of the tension, compression and shear checks, using
  the fibre stresses N/A ± M/S.
  - A bond breaks at φ ≥ 1, the worst bonds first: those within 10% of the round's maximum,
    at most `max_breaks_per_round`.
  - Then the structure is solved again. A cascade therefore unfolds over ticks, not in one
    instant.
- **Sudden changes** (a carve, a blast, a break round) are judged with a dynamic increase
  factor on the change of bond force since the last converged state: F_old + DIF · ΔF.
  Without it, a statically equivalent structure would miss the overshoot of a suddenly
  released load (about 2× undamped).
- **Detachment.** After a round of breaks, a two-sided lockstep flood from the ends of each
  broken bond finds the side that no longer reaches an anchor, at a cost proportional to the
  smaller side.
  - That set of fragments leaves the grid and becomes a rigid body, which keeps its bonds.
  - The body starts from rest, or with the blast's impulse.

## 4. Rigid bodies

Each body owns a voxel shape in its own grid-aligned local frame, its fragments and its bond
graph.

- **Contacts.**
  - Surface sample points (inset voxel corners) are tested against the world grid and against
    other bodies' local grids.
  - Normals come from the face of least penetration that leads to air.
  - Contacts are reduced to a small manifold per pair.
- **Solver.** Sequential impulses (projected Gauss–Seidel) with warm starting from persistent
  contacts, Coulomb friction, small restitution, and split-impulse position correction, so
  that reported impulses are true forces.
- **Sleeping.** Islands sleep when slow, and wake on contact with a moving body, on a carve or
  blast nearby, or when the world under them changes.
- **Coupling to structures.** Impulses on world voxels are applied to the fragment they hit.
  - Impacts become impact load cases on the static structure: pancake collapse of floors.
  - Resting bodies become dead loads.
- **Splitting.** A body's stress is solved when its loads change markedly.
  - A body split by broken bonds becomes several bodies, each with the parent's velocity field
    (v + ω × r).
- **Rubble** stays as sleeping bodies. It is not faded out, within a body budget. Beyond the
  budget, the oldest small sleeping pieces far from the viewer are removed.

## 5. What is kept from v1

The voxel grid, chunks and streaming, procedural and Doom worlds, sector movers, meshing,
persistence, command logs, the C ABI, and the WebGPU front end. Detached rigid pieces already
had a pose protocol: events carrying a mesh, plus poses. A body that splits is released, and
its children are announced as new pieces.

## 6. Knobs (runtime)

| Knob | Default | Meaning |
|---|---|---|
| fragility | 1 | divides every strength (more collapse) |
| dynamic increase factor | 1.5 | overshoot of sudden load changes |
| impact factor | 1 | contact force = factor · impulse / dt |
| fragment size | material table | rubble size and stress resolution |
| break cap per round | 256 | cascade pace |
| stress work per tick | budget | how fast stress spreads through large structures |
