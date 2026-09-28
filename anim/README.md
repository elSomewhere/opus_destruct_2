# svx_anim

The voxel character animation engine of structvox, documented in [../docs/ANIM.md](../docs/ANIM.md).

It covers:

- skeletons, IK, springs and keyframe curves;
- a motion plan: procedural locomotion with planted feet and personal gait styles, stances and
  their transitions, actions (strikes, knife attacks, blocks, reloads, gestures, idle poses,
  fidgets), weapon handling and moods;
- a physical body in the manner of NaturalMotion's Euphoria: 16 rigid parts with anatomical
  joints and muscles (an XPBD solver), carrying the plan out;
- behaviours between the two: balance (capture point, stepping, staggering), bracing on
  walls, flinching, holding wounds, trips, falling and catching the fall, lying, writhing,
  getting up, dying; hits as impulses on the body; bodies colliding with each other;
- fight choreography;
- procedural voxel soldiers, civilians and thugs;
- voxel-exact wounds and severing;
- gibs and blood;
- retro (Voxel Doom style) frames baked from the smooth animation.

It is TypeScript with no dependencies and doesn't depend on the physics engine or the
renderer. Hosts give it a `CollisionWorld` and draw its 20-byte vertices with the skin matrices
it computes.

```bash
npm install
npm test        # node --test (native type stripping)
npm run typecheck
```
