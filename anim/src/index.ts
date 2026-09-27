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
