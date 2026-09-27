# svx_anim

The voxel character animation engine of structvox, documented in [../docs/ANIM.md](../docs/ANIM.md).

It covers:

- skeletons, IK, springs and keyframe curves;
- procedural locomotion with planted feet and personal gait styles;
- stances (kneeling, prone, sitting, on the ground, knocked down) and their transitions;
- actions: strikes, knife attacks, blocks, reloads, gestures, idle poses and fidgets;
- weapon handling (rifles, SMGs, machine guns, pistols, knives) and moods;
- hit reactions by where and how hard a blow lands, and fight choreography;
- procedural voxel soldiers and civilians;
- voxel-exact wounds and severing;
- ragdolls, gibs and blood;
- retro (Voxel Doom style) frames baked from the smooth animation.

It is TypeScript with no dependencies and doesn't depend on the physics engine or the
renderer. Hosts give it a `CollisionWorld` and draw its 20-byte vertices with the skin matrices
it computes.

```bash
npm install
npm test        # node --test (native type stripping)
npm run typecheck
```
