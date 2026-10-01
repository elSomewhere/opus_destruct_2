# Audit of the previous implementation (commit 68925a3)

The previous engine (~18k lines) had a reasonable intent — typed planning
layers, derived seeds, packed regions, a worker — but several structural
problems made it the wrong base for a 12.5 cm, fully explorable world:

1. **Inconsistent physical scale.** Voxels were nominally 25 cm, yet a local
   road was 8 voxels (2 m) wide while a story was 14 voxels (3.5 m). Every
   dimension was hard-coded in voxel units, so changing the voxel size meant
   touching almost every file.
2. **Region planning with padding.** Regions were planned on padded rectangles
   and features were filtered by an "owner region" afterwards; stitching only
   worked because roads were a global jittered lattice. There was no general
   rule preventing disagreement at seams.
3. **Road network.** A rectilinear jittered lattice with stubs; no curves, no
   elevation, no highways, no underground, crosswalks and markings painted per
   region.
4. **Interiors.** Floor plates with a corridor and a core; rooms were rects
   without a guarantee that doors lie on shared walls; "furniture" was single
   voxels stamped in corners; stairs were not walkable geometry; there was no
   voxel-level validation.
5. **Renderer.** Isometric only, no LOD, no transparency, no first-person
   exploration — interiors could not actually be visited.
6. **Framework plumbing.** A legacy "flat plan" adapter duplicated the artifact
   store; debug and runtime paths diverged.

## Decision

Rebuild on a new foundation rather than retrofit: metric units with a single
voxel-size knob, a pure-function generation hierarchy keyed by stable
structure (no padded regions), chunk writers that make every generator
LOD-agnostic, lazy interior planning on real label grids with validated
connectivity, and a streamed LOD viewer with walking. The old code remains in
git history.
