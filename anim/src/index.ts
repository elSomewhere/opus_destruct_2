/**
 * svx_anim: a voxel character animation engine.
 *
 * Renderer-, physics- and host-agnostic: no DOM, no GPU, no engine protocol. A host feeds it
 * where characters are and what they do, plus a CollisionWorld (ground and contact queries),
 * and reads back skin matrices and meshes in a documented vertex format (voxel/mesh.ts).
 * See docs/ANIM.md.
 */
export * from './math/vec.ts';
export * from './math/quat.ts';
export * from './math/mat4.ts';
export * from './math/random.ts';
export * from './core/skeleton.ts';
export * from './voxel/model.ts';
export * from './voxel/sculpt.ts';
export * from './voxel/mesh.ts';
export * from './humanoid/rig.ts';
export * from './characters/palette.ts';
export * from './characters/humans.ts';
export * from './characters/props.ts';
export * from './core/ik.ts';
export * from './core/spring.ts';
export * from './locomotion/gait.ts';
export * from './physics/collision.ts';
export * from './humanoid/animator.ts';
export * from './voxel/damage.ts';
export * from './physics/ragdoll.ts';
export * from './humanoid/ragdoll.ts';
export * from './retro/bake.ts';
export * from './retro/sequences.ts';
export * from './character.ts';
export * from './physics/debris.ts';
export * from './core/curve.ts';
export * from './humanoid/style.ts';
export * from './humanoid/actions.ts';
export * from './humanoid/reactions.ts';
export * from './humanoid/stances.ts';
export * from './characters/furniture.ts';
export * from './humanoid/brawl.ts';
