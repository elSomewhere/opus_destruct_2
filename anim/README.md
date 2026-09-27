# svx_anim

The voxel character animation engine of structvox, documented in [../docs/ANIM.md](../docs/ANIM.md).

It covers:

- skeletons, IK and springs;
- procedural locomotion with planted feet;
- weapon handling, moods and reactions;
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
